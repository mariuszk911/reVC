/*
 * AmigaOS CdStream backend.
 *
 * Two implementations selected at runtime by the REVC_ASYNC_STREAM env var
 * (read once in amigaos.cpp, exposed as amiga_boot_async_stream()):
 *
 *   - default (REVC_ASYNC_STREAM unset/1): a background Exec Process performs
 *     the reads (src/skel/amigaos/amiga_stream.c). The per-channel state machine
 *     lives in src/core/StreamChannel.h and is shared with the loader, so the
 *     CdStream contract seen by CStreaming is identical.
 *
 *   - REVC_ASYNC_STREAM=0: the original synchronous backend. CdStreamRead does
 *     lseek+read inline on the game thread; by the time ProcessLoadingChannel
 *     polls CdStreamGetStatus the data is already in the channel buffer. This is
 *     the rollback path.
 *
 * Scope 1 of the asynchronous CdStream backend.
 */

#if defined(AMIGAOS)

#include "common.h"
#include "crossplatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "CdStream.h"
#include "StreamChannel.h"
#include "AmigaStreamAsync.h"
#include "PathNormalize.h"
#include "MemoryMgr.h"

#define CDDEBUG(f, ...)   debug("%s: " f "\n", "cdvd_stream", ## __VA_ARGS__)
#define CDTRACE(f, ...)   debug("%s: " f "\n", "cdvd_stream trace", ## __VA_ARGS__)

/* AmigaOS boot hooks (0 unless the matching REVC_* flag is set). */
#include "amiga_boot.h"

/* S1 A/B diagnostic (logged at CdStreamShutdown): main-task milliseconds
 * blocked in the streaming path, and the read volume, so sync vs async can be
 * compared independent of scene content. Async blocking is measured in
 * CdStreamSync around AmStreamWait; sync blocking is the inline disk read. */
static uint32 gStatBlockMs;	// main task only
static uint32 gStatReads;
static uint32 gStatBytes;
static uint32 gStatErrors;

/* The loader's status values (StreamChannel.h) and capacity (amiga_stream.c)
 * live where CdStream.h/config.h are not visible. Pin them here, where both
 * are, so an edit to either cannot silently desync the loader. */
static_assert(STREAM_NONE == 0 && STREAM_SUCCESS == 1 &&
              STREAM_READING == 0xFF && STREAM_ERROR == 0xFE &&
              STREAM_WAITING == 0xFA,
              "StreamChannel.h STREAM_* values must match CdStream.h");
static_assert(MAX_CDCHANNELS <= 8, "AM_MAX_CHANNELS in amiga_stream.c too small");
static_assert(MAX_CDIMAGES <= 8, "AM_MAX_IMAGES in amiga_stream.c too small");

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

/* ---------------------------------------------------------------------------
 * Shared image bookkeeping (both backends)
 * ------------------------------------------------------------------------ */

char gCdImageNames[MAX_CDIMAGES+1][64];
int32 gNumImages;
int32 gNumChannels;

int32 gImgFiles[MAX_CDIMAGES];	// 0: unused, otherwise index+1 (never an fd here)
char *gImgNames[MAX_CDIMAGES];

int32 lastPosnRead;

static bool gAsync;

/* Async-only state: one StreamChannel per engine channel, plus the last request
 * per channel (POSIX-parity duplicate-issue check). */
static struct StreamChannel *gChannels;
static uint32 gReqOffset[MAX_CDCHANNELS];
static uint32 gReqSize[MAX_CDCHANNELS];

/* ===========================================================================
 * Synchronous backend (preserved; the REVC_ASYNC_STREAM=0 baseline)
 * ======================================================================== */

struct CdReadInfo
{
	int32 hFile;		// index+1 into gImgFiles, 0 = none
	int32 nSectorOffset;
	int32 nSectorsToRead;
	void *pBuffer;
	bool bReading;
	bool bLocked;
	int32 nStatus;		// one-shot status reported by CdStreamGetStatus
};

static CdReadInfo *gpReadInfo;

static void
CdStreamInitSync(int32 numChannels)
{
	CDDEBUG("synchronous init, %d channels", (int)numChannels);

	gpReadInfo = (CdReadInfo *)calloc(numChannels, sizeof(CdReadInfo));
	ASSERT(gpReadInfo != nil);

	for(int32 i = 0; i < numChannels; i++)
		gpReadInfo[i].nStatus = STREAM_NONE;
}

static int32
CdStreamReadSync(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	ASSERT(channel < gNumChannels);
	ASSERT(buffer != nil);

	lastPosnRead = size + offset;

	ASSERT(_GET_INDEX(offset) < MAX_CDIMAGES);
	int32 hImage = gImgFiles[_GET_INDEX(offset)];
	ASSERT(hImage > 0);

	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT(pChannel != nil);

	if(hImage <= 0){
		pChannel->nStatus = STREAM_ERROR;
		gStatErrors++;
		return STREAM_ERROR;
	}

	int32 fd = hImage - 1;
	uint32 nBytes = size * CDSTREAM_SECTOR_SIZE;
	off_t pos = (off_t)_GET_OFFSET(offset) * (off_t)CDSTREAM_SECTOR_SIZE;

	pChannel->hFile = hImage;
	pChannel->nSectorOffset = _GET_OFFSET(offset);
	pChannel->nSectorsToRead = 0;
	pChannel->pBuffer = buffer;
	pChannel->bReading = true;
	pChannel->bLocked = false;

	if(lseek(fd, pos, SEEK_SET) == (off_t)-1){
		pChannel->nStatus = STREAM_ERROR;
		pChannel->bReading = false;
		gStatErrors++;
		return STREAM_ERROR;
	}

	{
		uint32 t0 = amiga_boot_ms();
		ssize_t got = read(fd, buffer, nBytes);
		gStatBlockMs += amiga_boot_ms() - t0;
		if(got != (ssize_t)nBytes){
			pChannel->nStatus = STREAM_ERROR;
			pChannel->bReading = false;
			gStatErrors++;
			return STREAM_ERROR;
		}
	}

	gStatReads++;
	gStatBytes += nBytes;

	pChannel->nStatus = STREAM_NONE;
	pChannel->bReading = false;

	return STREAM_SUCCESS;
}

static int32
CdStreamGetStatusSync(int32 channel)
{
	ASSERT(channel < gNumChannels);
	CdReadInfo *pChannel = &gpReadInfo[channel];

	if(pChannel->bReading)
		return STREAM_READING;

	if(pChannel->nStatus != STREAM_NONE){
		int32 status = pChannel->nStatus;
		pChannel->nStatus = STREAM_NONE;
		return status;
	}

	return STREAM_NONE;
}

static int32
CdStreamSyncSync(int32 channel)
{
	ASSERT(channel < gNumChannels);
	CdReadInfo *pChannel = &gpReadInfo[channel];

#ifdef FLUSHABLE_STREAMING
	flushStream[channel] = false;
#endif

	pChannel->bReading = false;
	pChannel->bLocked = false;

	/* Byte-for-byte the original synchronous behaviour: this path is the
	 * REVC_ASYNC_STREAM=0 A/B baseline / rollback. D5 (no clear) applies to
	 * the async CdStreamSync below, which uses SC_PeekStatus. */
	if(pChannel->nStatus != STREAM_NONE){
		int32 status = pChannel->nStatus;
		pChannel->nStatus = STREAM_NONE;
		return status;
	}

	return STREAM_NONE;
}

static bool
CdStreamAddImageSync(char const *path)
{
	char buf[512];
	int32 fd = -1;
	int variant;

	ASSERT(path != nil);
	ASSERT(gNumImages < MAX_CDIMAGES);

	/* Same normalisation + exact/lower/upper fallback as _fcaseopen and the
	 * async backend, so REVC_ASYNC_STREAM does not change image resolution. */
	for(variant = 0; variant < 3; variant++){
		if(AmigaPathVariant(buf, sizeof(buf), path, variant) == 0 && variant > 0)
			continue;
		fd = open(buf, O_RDONLY);
		if(fd != -1)
			break;
	}

	if(fd == -1){
		ASSERT(0);
		return false;
	}

	gImgFiles[gNumImages] = fd + 1;	// because 0 means unused
	gImgNames[gNumImages] = strdup(path);

	strncpy(gCdImageNames[gNumImages], path, sizeof(gCdImageNames[0]) - 1);
	gCdImageNames[gNumImages][sizeof(gCdImageNames[0]) - 1] = '\0';

	gNumImages++;

	return true;
}

/* ===========================================================================
 * Public API: runtime dispatch
 * ======================================================================== */

void
CdStreamInitThread(void)
{
	// Synchronous backend: no reader thread. Async uses AmStreamInit().
}

void
CdStreamInit(int32 numChannels)
{
	gNumImages = 0;
	gNumChannels = numChannels;
	ASSERT(gNumChannels != 0);

	gAsync = (amiga_boot_async_stream() != 0);

	if(gAsync){
		CDDEBUG("asynchronous init, %d channels", (int)numChannels);
		gChannels = (struct StreamChannel *)calloc(numChannels,
		                                           sizeof(struct StreamChannel));
		ASSERT(gChannels != nil);
		for(int32 i = 0; i < numChannels; i++){
			SC_Reset(&gChannels[i]);
			gReqOffset[i] = 0;
			gReqSize[i] = 0;
		}
		AmStreamInit();
		if(AmStreamIsRunning())
			CDDEBUG("async loader running, %d channels", (int)numChannels);
		else
			CDDEBUG("async loader FAILED to start; reads run inline");
	}else{
		CdStreamInitSync(numChannels);
	}
}

uint32
GetGTA3ImgSize(void)
{
	if(gAsync)
		return (uint32)AmStreamImageSize(0);

	struct stat statbuf;

	ASSERT(gImgFiles[0] > 0);
	if(gImgFiles[0] <= 0)
		return 0;

	if(stat(gImgNames[0], &statbuf) == -1){
		char *real = casepath(gImgNames[0], false);
		if(real == nil || stat(real, &statbuf) == -1){
			if(real)
				free(real);
			CDTRACE("can't get size of gta3.img");
			return 0;
		}
		free(real);
	}

	return (uint32)statbuf.st_size;
}

void
CdStreamShutdown(void)
{
	// Close any image handles still open (normally done earlier by
	// CdStreamRemoveImages; idempotent).
	CdStreamRemoveImages();

	if(gAsync){
		unsigned long reads = 0, errors = 0, bytes = 0;
		AmStreamGetStats(&reads, &errors, &bytes);
		CDDEBUG("stats: mode=async reads=%lu errors=%lu bytes=%lu mainBlockMs=%lu lastIoErr=%ld",
		        reads, errors, bytes, (unsigned long)gStatBlockMs, AmStreamLastError());
		AmStreamShutdown();
		if(gChannels){
			free(gChannels);
			gChannels = nil;
		}
	}else{
		CDDEBUG("stats: mode=sync reads=%lu bytes=%lu mainBlockMs=%lu",
		        (unsigned long)gStatReads, (unsigned long)gStatBytes,
		        (unsigned long)gStatBlockMs);
	}

	if(gpReadInfo){
		free(gpReadInfo);
		gpReadInfo = nil;
	}
}

/* REVC_STREAMSTATS: main-task milliseconds blocked in CdStreamSync. Both
 * backends accumulate here (async wait or the inline synchronous read); the
 * value is cumulative so CdStreamShutdown() can report it accurately. */
unsigned int
AmStreamBlockMs(void)
{
	return (unsigned int)gStatBlockMs;
}

/* Backend-neutral counters for REVC_STREAMSTATS. The async loader owns its
 * counters in amiga_stream.c; the synchronous backend owns gStat*. */
void
CdStreamGetStats(unsigned long *reads, unsigned long *errors,
                 unsigned long *bytes)
{
	if(gAsync){
		AmStreamGetStats(reads, errors, bytes);
	}else{
		if(reads)
			*reads = (unsigned long)gStatReads;
		if(errors)
			*errors = (unsigned long)gStatErrors;
		if(bytes)
			*bytes = (unsigned long)gStatBytes;
	}
}

int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	if(!gAsync)
		return CdStreamReadSync(channel, buffer, offset, size);

	ASSERT(channel < gNumChannels);
	ASSERT(buffer != nil);

	lastPosnRead = size + offset;

	ASSERT(_GET_INDEX(offset) < MAX_CDIMAGES);
	int32 img = _GET_INDEX(offset);

	struct StreamChannel *pChannel = &gChannels[channel];
	ASSERT(pChannel != nil);

	if(SC_IsBusy(pChannel)){
		// POSIX parity (CdStream_posix.cpp:348-357): an in-flight read that
		// already covers this request is a no-op success, anything else is
		// refused. The image is encoded in the top bits of offset.
		if(gReqSize[channel] >= size &&
		   _GET_INDEX(gReqOffset[channel]) == (uint32)img &&
		   _GET_OFFSET(gReqOffset[channel]) == _GET_OFFSET(offset))
			return STREAM_SUCCESS;
		return STREAM_NONE;
	}

	uint32 byteOff = (uint32)_GET_OFFSET(offset) * (uint32)CDSTREAM_SECTOR_SIZE;
	uint32 byteLen = size * (uint32)CDSTREAM_SECTOR_SIZE;

	gReqOffset[channel] = offset;
	gReqSize[channel] = size;

	SC_Submit(pChannel, (int)size);
	AmStreamSubmit(channel, pChannel, img, buffer,
	               (unsigned long)byteOff, (unsigned long)byteLen);

	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	if(!gAsync)
		return CdStreamGetStatusSync(channel);

	ASSERT(channel < gNumChannels);
	return SC_GetStatus(&gChannels[channel]);
}

int32
CdStreamGetLastPosn(void)
{
	return lastPosnRead;
}

int32
CdStreamSync(int32 channel)
{
	if(!gAsync)
		return CdStreamSyncSync(channel);

	ASSERT(channel < gNumChannels);
	struct StreamChannel *pChannel = &gChannels[channel];

#ifdef FLUSHABLE_STREAMING
	flushStream[channel] = false;
#endif

	if(SC_IsBusy(pChannel)){
		uint32 t0 = amiga_boot_ms();
		AmStreamWait(channel, pChannel);
		gStatBlockMs += amiga_boot_ms() - t0;
	}

	/* D5: report the one-shot status without consuming it. */
	return SC_PeekStatus(pChannel);
}

void
AddToQueue(Queue *queue, int32 item)
{
	ASSERT(queue != nil);
	ASSERT(queue->items != nil);
	queue->items[queue->tail] = item;
	queue->tail = (queue->tail + 1) % queue->size;
}

int32
GetFirstInQueue(Queue *queue)
{
	ASSERT(queue != nil);
	if(queue->head == queue->tail)
		return -1;
	return queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
	ASSERT(queue != nil);
	if(queue->head == queue->tail)
		return;
	queue->head = (queue->head + 1) % queue->size;
}

bool
CdStreamAddImage(char const *path)
{
	if(!gAsync)
		return CdStreamAddImageSync(path);

	ASSERT(path != nil);
	ASSERT(gNumImages < MAX_CDIMAGES);

	int32 idx = AmStreamAddImage(path);
	if(idx < 0){
		ASSERT(0);
		return false;
	}

	// Mirror into the shared name table (the image handle itself lives in
	// amiga_stream.c; gImgFiles only marks the slot as used).
	gImgFiles[idx] = idx + 1;
	gImgNames[idx] = strdup(path);

	strncpy(gCdImageNames[idx], path, sizeof(gCdImageNames[0]) - 1);
	gCdImageNames[idx][sizeof(gCdImageNames[0]) - 1] = '\0';

	gNumImages++;

	return true;
}

char *
CdStreamGetImageName(int32 cd)
{
	ASSERT(cd < MAX_CDIMAGES);
	if(gImgFiles[cd] > 0)
		return gCdImageNames[cd];
	return nil;
}

void
CdStreamRemoveImages(void)
{
	for(int32 i = 0; i < gNumChannels; i++){
#ifdef FLUSHABLE_STREAMING
		flushStream[i] = false;
#endif
		CdStreamSync(i);
	}

	if(gAsync){
		// All channels are synced above, so the loader cannot touch a handle
		// that is about to close.
		AmStreamRemoveImages();
	}

	for(int32 i = 0; i < gNumImages; i++){
		if(!gAsync && gImgFiles[i] > 0)
			close(gImgFiles[i] - 1);
		if(gImgNames[i]){
			free(gImgNames[i]);
			gImgNames[i] = nil;
		}
		gImgFiles[i] = 0;
	}

	gNumImages = 0;
}

int32
CdStreamGetNumImages(void)
{
	return gNumImages;
}

#endif // AMIGAOS
