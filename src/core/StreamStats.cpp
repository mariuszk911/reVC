/*
 * StreamStats.cpp - REVC_STREAMSTATS streaming diagnostics (AmigaOS only).
 *
 * The engine streaming code is upstream reVC; all calls into this module are
 * behind #ifdef AMIGAOS in Streaming.cpp and are no-ops when the switch is off.
 * While enabled, hooks only do integer adds and a timestamp at the frame
 * boundary; the window dump (every REVC_STREAMSTATS_FRAMES updates) and the
 * slow-frame line are the only log writes.
 *
 * Logging uses amiga_boot_log(), not debug(): debug() is compiled in but
 * disabled unless REVC_DEBUGLOG is set, and the stats must not require that
 * switch.
 */
#if defined(AMIGAOS)

#include "common.h"

#include "StreamStats.h"
#include "Streaming.h"
#include "ModelInfo.h"
#include "TxdStore.h"
#include "ColStore.h"
#include "AnimManager.h"
#include "amiga_boot.h"
#include "AmigaStreamAsync.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SS_TOP		8
#define SS_SLOW_LOG_MAX	8
#define SS_MB		(1024u * 1024u)

struct SsTop
{
	int id[SS_TOP];
	unsigned int val[SS_TOP];
};

static int ssOn;
static int ssInited;

static unsigned int ssDumpFrames;
static unsigned int ssSlowMs;

static SsTop ssTopLoad;
static SsTop ssTopRemove;
static SsTop ssTopSlow;

/* Per-window per-resource counters (reset at each dump). NULL while off. */
static uint16 *ssWinLoad;
static uint16 *ssWinRemove;

/* Frame timer (ssFrameBegin/End bracket CStreaming::Update). */
static unsigned int ssFrameT0;
static unsigned int ssFrameSeq;

/* Current frame counters. */
static unsigned int ssCurReq;
static unsigned int ssCurList;
static unsigned int ssCurFiles;
static unsigned int ssCurBytes;
static unsigned int ssCurBatches;
static unsigned int ssCurBatchFiles;
static unsigned int ssCurConvMs;
static unsigned int ssCurFinMs;
static unsigned int ssCurMakeSpaceMs;
static unsigned int ssCurEvicts;
static unsigned int ssCurEvictFallback;
static unsigned int ssCurLoadAllCalls;
static unsigned int ssCurLoadAllFiles;
static unsigned int ssCurLoadAllBytes;
static unsigned int ssCurLoadAllMs;

/* Window counters. */
static unsigned int ssWinFrames;
static unsigned int ssWinStreamUs;
static unsigned int ssWinStreamMaxUs;
static unsigned int ssWinListMax;
static unsigned int ssWinReq;
static unsigned int ssWinFiles;
static unsigned int ssWinBytes;
static unsigned int ssWinBatches;
static unsigned int ssWinBatchFiles;
static unsigned int ssWinBatchBytes;
static unsigned int ssWinConvMs;
static unsigned int ssWinFinMs;
/* Conversion/finish CPU split by resource type (the slow-frame decision). */
static unsigned int ssWinConvDff, ssWinConvTxd, ssWinConvCol, ssWinConvAnim;
static unsigned int ssWinFilesDff, ssWinFilesTxd, ssWinFilesCol, ssWinFilesAnim;
static unsigned int ssWinMakeSpaceMs;
static unsigned int ssWinEvicts;
static unsigned int ssWinEvictFallback;
static unsigned int ssWinLoadAllCalls;
static unsigned int ssWinLoadAllFiles;
static unsigned int ssWinLoadAllBytes;
static unsigned int ssWinLoadAllMs;

/* Cumulative totals (shutdown line). */
static unsigned int ssTotFiles;
static unsigned int ssTotBytes;
static unsigned int ssTotBatches;
static unsigned int ssTotEvicts;
static unsigned int ssTotConvMs;
static unsigned int ssTotFinMs;
static unsigned int ssTotLoadAllMs;

/* Last CdStreamGetStats sample, for the per-window disk deltas. */
static unsigned long ssLastReads;
static unsigned long ssLastReadBytes;
static unsigned long ssLastErrors;
/* Cumulative CdStreamSync block time at the last window. */
static unsigned int ssLastBlockMs;
/* Slow-frame log budget for the current window. */
static unsigned int ssSlowLogged;
static unsigned int ssSlowSuppressed;

static void
ssLog(const char *fmt, ...)
{
	char buf[400];
	va_list va;

	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	amiga_boot_log(buf);
}

static void
ssName(int id, char *buf, int n)
{
	buf[0] = '\0';
	if(id < 0 || id >= NUMSTREAMINFO)
		return;

	if(id < STREAM_OFFSET_TXD){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(id);
		if(mi)
			snprintf(buf, n, "%s.dff", mi->GetModelName());
	}else if(id < STREAM_OFFSET_COL){
		int slot = id - STREAM_OFFSET_TXD;
		TxdDef *def = CTxdStore::GetSlot(slot);
		if(def)
			snprintf(buf, n, "%s.txd", def->name);
	}else if(id < STREAM_OFFSET_ANIM){
		int slot = id - STREAM_OFFSET_COL;
		ColDef *def = CColStore::GetSlot(slot);
		if(def)
			snprintf(buf, n, "%s.col", def->name);
	}else{
		CAnimBlock *blk = CAnimManager::GetAnimationBlock(id - STREAM_OFFSET_ANIM);
		if(blk)
			snprintf(buf, n, "%s.ifp", blk->name);
	}
}

static void
ssTopClear(SsTop *t)
{
	int i;
	for(i = 0; i < SS_TOP; i++){
		t->id[i] = -1;
		t->val[i] = 0;
	}
}

static void
ssTopBubble(SsTop *t, int i)
{
	while(i > 0 && t->val[i] > t->val[i-1]){
		int id = t->id[i];
		unsigned int v = t->val[i];
		t->id[i] = t->id[i-1];
		t->val[i] = t->val[i-1];
		t->id[i-1] = id;
		t->val[i-1] = v;
		i--;
	}
}

/* Update/insert a unique id entry (load/remove counters). */
static void
ssTopSet(SsTop *t, int id, unsigned int v)
{
	int i;

	if(id < 0)
		return;
	for(i = 0; i < SS_TOP; i++)
		if(t->id[i] == id){
			t->val[i] = v;
			ssTopBubble(t, i);
			return;
		}
	for(i = 0; i < SS_TOP; i++)
		if(t->id[i] < 0){
			t->id[i] = id;
			t->val[i] = v;
			ssTopBubble(t, i);
			return;
		}
	if(v > t->val[SS_TOP-1]){
		t->id[SS_TOP-1] = id;
		t->val[SS_TOP-1] = v;
		ssTopBubble(t, SS_TOP-1);
	}
}

/* Insert a standalone sample (slow-file list allows duplicates). */
static void
ssTopAdd(SsTop *t, int id, unsigned int v)
{
	int i;

	for(i = 0; i < SS_TOP; i++)
		if(t->id[i] < 0){
			t->id[i] = id;
			t->val[i] = v;
			ssTopBubble(t, i);
			return;
		}
	if(v > t->val[SS_TOP-1]){
		t->id[SS_TOP-1] = id;
		t->val[SS_TOP-1] = v;
		ssTopBubble(t, SS_TOP-1);
	}
}

static void
ssDumpTop(const char *tag, const SsTop *t)
{
	char line[400];
	char name[40];
	char piece[80];
	int i, off;

	off = snprintf(line, sizeof(line), "streamstats top %s:", tag);
	if(off < 0)
		off = 0;
	for(i = 0; i < SS_TOP && t->id[i] >= 0; i++){
		ssName(t->id[i], name, sizeof(name));
		snprintf(piece, sizeof(piece), " '%s' %u", name, t->val[i]);
		if(off + (int)strlen(piece) >= (int)sizeof(line) - 1)
			break;
		strcpy(line + off, piece);
		off += (int)strlen(piece);
	}
	ssLog("%s", line);
}

static void
ssResetFrame(void)
{
	ssCurReq = 0;
	ssCurList = 0;
	ssCurFiles = 0;
	ssCurBytes = 0;
	ssCurBatches = 0;
	ssCurBatchFiles = 0;
	ssCurConvMs = 0;
	ssCurFinMs = 0;
	ssCurMakeSpaceMs = 0;
	ssCurEvicts = 0;
	ssCurEvictFallback = 0;
	ssCurLoadAllCalls = 0;
	ssCurLoadAllFiles = 0;
	ssCurLoadAllBytes = 0;
	ssCurLoadAllMs = 0;
}

static void
ssResetWindow(void)
{
	ssWinFrames = 0;
	ssWinStreamUs = 0;
	ssWinStreamMaxUs = 0;
	ssWinListMax = 0;
	ssWinReq = 0;
	ssWinFiles = 0;
	ssWinBytes = 0;
	ssWinBatches = 0;
	ssWinBatchFiles = 0;
	ssWinBatchBytes = 0;
	ssWinConvMs = 0;
	ssWinFinMs = 0;
	ssWinConvDff = 0;
	ssWinConvTxd = 0;
	ssWinConvCol = 0;
	ssWinConvAnim = 0;
	ssWinFilesDff = 0;
	ssWinFilesTxd = 0;
	ssWinFilesCol = 0;
	ssWinFilesAnim = 0;
	ssWinMakeSpaceMs = 0;
	ssWinEvicts = 0;
	ssWinEvictFallback = 0;
	ssWinLoadAllCalls = 0;
	ssWinLoadAllFiles = 0;
	ssWinLoadAllBytes = 0;
	ssWinLoadAllMs = 0;
	ssSlowLogged = 0;
	ssSlowSuppressed = 0;

	if(ssWinLoad)
		memset(ssWinLoad, 0, NUMSTREAMINFO * sizeof(uint16));
	if(ssWinRemove)
		memset(ssWinRemove, 0, NUMSTREAMINFO * sizeof(uint16));

	ssTopClear(&ssTopLoad);
	ssTopClear(&ssTopRemove);
	ssTopClear(&ssTopSlow);
}

static unsigned int
ssUsedMB(void)
{
	return (unsigned int)(CStreaming::ms_memoryUsed / SS_MB);
}

static void
ssTypeConvAdd(int streamId, unsigned int ms, int countFile)
{
	if(streamId < 0)
		return;
	if(streamId < STREAM_OFFSET_TXD){
		ssWinConvDff += ms;
		if(countFile)
			ssWinFilesDff++;
	}else if(streamId < STREAM_OFFSET_COL){
		ssWinConvTxd += ms;
		if(countFile)
			ssWinFilesTxd++;
	}else if(streamId < STREAM_OFFSET_ANIM){
		ssWinConvCol += ms;
		if(countFile)
			ssWinFilesCol++;
	}else{
		ssWinConvAnim += ms;
		if(countFile)
			ssWinFilesAnim++;
	}
}

static void
ssLogSlow(unsigned int us)
{
	ssLog("streamstats slow: frame=%u ms=%u req=%u list=%u files=%u bytes=%u "
	      "batches=%u bfiles=%u conv=%ums fin=%ums evict=%u fallback=%u "
	      "makespace=%ums loadall=%u/%ufiles/%ubytes/%ums mem=%u/%uMB",
	      ssFrameSeq, (us + 500) / 1000, ssCurReq, ssCurList,
	      ssCurFiles, ssCurBytes,
	      ssCurBatches, ssCurBatchFiles, ssCurConvMs, ssCurFinMs,
	      ssCurEvicts, ssCurEvictFallback,
	      ssCurMakeSpaceMs, ssCurLoadAllCalls,
	      ssCurLoadAllFiles, ssCurLoadAllBytes, ssCurLoadAllMs,
	      ssUsedMB(), (unsigned int)(CStreaming::ms_memoryAvailable / SS_MB));
}

static void
ssDumpWindow(void)
{
	unsigned int frames;
	unsigned long reads, errors, readBytes;
	unsigned long dReads, dBytes, dErrors;
	unsigned int blockMs, blockTotal;
	unsigned long ratio;

	CdStreamGetStats(&reads, &errors, &readBytes);
	dReads = reads - ssLastReads;
	dBytes = readBytes - ssLastReadBytes;
	dErrors = errors - ssLastErrors;
	ssLastReads = reads;
	ssLastReadBytes = readBytes;
	ssLastErrors = errors;

	/* Cumulative; keep the delta for the window and leave the shutdown
	 * diagnostic (CdStreamShutdown's mainBlockMs) intact. */
	blockTotal = AmStreamBlockMs();
	blockMs = blockTotal - ssLastBlockMs;
	ssLastBlockMs = blockTotal;

	frames = ssWinFrames ? ssWinFrames : 1;

	ssLog("streamstats: frames=%u stream avg=%u max=%u ms listmax=%u req=%u",
	      ssWinFrames, ssWinStreamUs / (frames * 1000),
	      (ssWinStreamMaxUs + 500) / 1000, ssWinListMax, ssWinReq);
	ssLog("streamstats: files=%u bytes=%u batches=%u bfiles=%u "
	      "bbytes=%u conv=%ums fin=%ums",
	      ssWinFiles, ssWinBytes, ssWinBatches, ssWinBatchFiles,
	      ssWinBatchBytes, ssWinConvMs, ssWinFinMs);
	ssLog("streamstats: conv type dff=%ums/%u txd=%ums/%u col=%ums/%u "
	      "anim=%ums/%u",
	      ssWinConvDff, ssWinFilesDff, ssWinConvTxd, ssWinFilesTxd,
	      ssWinConvCol, ssWinFilesCol, ssWinConvAnim, ssWinFilesAnim);
	ssLog("streamstats: evict=%u fallback=%u makespace=%ums "
	      "loadall=%u/%ufiles/%ubytes/%ums",
	      ssWinEvicts, ssWinEvictFallback, ssWinMakeSpaceMs,
	      ssWinLoadAllCalls, ssWinLoadAllFiles, ssWinLoadAllBytes,
	      ssWinLoadAllMs);
	ratio = ssWinBytes ? (unsigned long)((dBytes * 100ul) / ssWinBytes) : 0;
	ssLog("streamstats: disk reads=%lu bytes=%lu err=%lu block=%ums "
	      "read/load=%lu%%",
	      dReads, dBytes, dErrors, blockMs, ratio);
	ssLog("streamstats: mem used=%uMB avail=%uMB",
	      ssUsedMB(), (unsigned int)(CStreaming::ms_memoryAvailable / SS_MB));

	ssDumpTop("load", &ssTopLoad);
	ssDumpTop("rm", &ssTopRemove);
	ssDumpTop("slowms", &ssTopSlow);

	if(ssSlowSuppressed)
		ssLog("streamstats: %u slow frames suppressed (cap %u/window)",
		      ssSlowSuppressed, SS_SLOW_LOG_MAX);

	ssResetWindow();
}

static void
ssInit(void)
{
	ssInited = 1;
	if(!amiga_boot_streamstats())
		return;

	ssDumpFrames = (unsigned int)amiga_boot_streamstats_frames();
	if(ssDumpFrames < 1)
		ssDumpFrames = 1;
	ssSlowMs = (unsigned int)amiga_boot_streamstats_slow_ms();

	ssWinLoad = (uint16 *)calloc(NUMSTREAMINFO, sizeof(uint16));
	ssWinRemove = (uint16 *)calloc(NUMSTREAMINFO, sizeof(uint16));
	if(ssWinLoad == nil || ssWinRemove == nil){
		if(ssWinLoad)
			free(ssWinLoad);
		if(ssWinRemove)
			free(ssWinRemove);
		ssWinLoad = nil;
		ssWinRemove = nil;
		ssLog("streamstats: disabled, cannot allocate %u bytes",
		      (unsigned int)(2 * NUMSTREAMINFO * sizeof(uint16)));
		return;
	}

	ssOn = 1;
	ssFrameSeq = 0;
	ssResetFrame();
	ssResetWindow();
	ssLastReads = 0;
	ssLastReadBytes = 0;
	ssLastErrors = 0;
	ssLastBlockMs = 0;

	ssLog("streamstats: on frames=%u slow=%ums async=%d",
	      ssDumpFrames, ssSlowMs, AmStreamIsRunning());
}

int
ssEnabled(void)
{
	if(!ssInited)
		ssInit();
	return ssOn;
}

void
ssFrameBegin(void)
{
	if(!ssEnabled())
		return;
	ssFrameT0 = amiga_boot_us();
}

void
ssFrameEnd(void)
{
	unsigned int us;

	if(!ssOn)
		return;

	us = amiga_boot_us() - ssFrameT0;
	ssFrameSeq++;
	ssWinFrames++;
	ssWinStreamUs += us;
	if(us > ssWinStreamMaxUs)
		ssWinStreamMaxUs = us;

	if(ssSlowMs && us >= ssSlowMs * 1000){
		if(ssSlowLogged < SS_SLOW_LOG_MAX){
			ssSlowLogged++;
			ssLogSlow(us);
		}else
			ssSlowSuppressed++;
	}

	ssResetFrame();

	if(ssWinFrames >= ssDumpFrames)
		ssDumpWindow();
}

void
ssRequestAdded(int streamId)
{
	(void)streamId;
	if(!ssEnabled())
		return;
	ssCurReq++;
	ssWinReq++;
}

void
ssListSize(int n)
{
	if(!ssEnabled())
		return;
	if(n < 0)
		n = 0;
	ssCurList = (unsigned int)n;
	if(ssCurList > ssWinListMax)
		ssWinListMax = ssCurList;
}

void
ssBatchIssued(int files, unsigned int bytes)
{
	if(!ssEnabled())
		return;
	ssCurBatches++;
	ssCurBatchFiles += (unsigned int)files;
	ssWinBatches++;
	ssWinBatchFiles += (unsigned int)files;
	ssWinBatchBytes += bytes;
	ssTotBatches++;
}

void
ssConvertDone(int streamId, unsigned int ms)
{
	if(!ssEnabled())
		return;
	ssCurFiles++;
	ssCurConvMs += ms;
	ssWinFiles++;
	ssWinConvMs += ms;
	ssTotFiles++;
	ssTotConvMs += ms;
	ssTypeConvAdd(streamId, ms, 1);
	if(ms >= 2)
		ssTopAdd(&ssTopSlow, streamId, ms);
}

void
ssFinishDone(int streamId, unsigned int ms)
{
	if(!ssEnabled())
		return;
	ssCurFinMs += ms;
	ssWinFinMs += ms;
	ssTotFinMs += ms;
	ssTypeConvAdd(streamId, ms, 0);
	if(ms >= 2)
		ssTopAdd(&ssTopSlow, streamId, ms);
}

void
ssResourceLoaded(int streamId, unsigned int bytes)
{
	if(!ssEnabled())
		return;
	ssCurBytes += bytes;
	ssWinBytes += bytes;
	ssTotBytes += bytes;

	if(ssWinLoad && streamId >= 0 && streamId < NUMSTREAMINFO){
		if(ssWinLoad[streamId] != 0xFFFF)
			ssWinLoad[streamId]++;
		ssTopSet(&ssTopLoad, streamId, ssWinLoad[streamId]);
	}
}

void
ssResourceRemoved(int streamId)
{
	if(!ssEnabled())
		return;

	if(ssWinRemove && streamId >= 0 && streamId < NUMSTREAMINFO){
		if(ssWinRemove[streamId] != 0xFFFF)
			ssWinRemove[streamId]++;
		ssTopSet(&ssTopRemove, streamId, ssWinRemove[streamId]);
	}
}

unsigned int
ssMakeSpaceBegin(void)
{
	if(!ssEnabled())
		return 0;
	return amiga_boot_us() | 1;
}

void
ssMakeSpaceDone(unsigned int t0, int evicted, int fallback)
{
	unsigned int ms;

	if(!ssOn || t0 == 0)
		return;

	ms = (amiga_boot_us() - t0) / 1000;
	ssCurMakeSpaceMs += ms;
	ssWinMakeSpaceMs += ms;
	ssCurEvicts += (unsigned int)evicted;
	ssWinEvicts += (unsigned int)evicted;
	ssTotEvicts += (unsigned int)evicted;
	if(fallback){
		ssCurEvictFallback++;
		ssWinEvictFallback++;
	}
}

unsigned int
ssLoadAllBegin(void)
{
	if(!ssEnabled())
		return 0;
	ssCurLoadAllCalls++;
	ssWinLoadAllCalls++;
	return amiga_boot_us() | 1;
}

void
ssLoadAllDone(unsigned int t0, int files, unsigned int bytes)
{
	unsigned int ms;

	if(!ssOn || t0 == 0)
		return;

	ms = (amiga_boot_us() - t0) / 1000;
	ssCurLoadAllFiles += (unsigned int)files;
	ssCurLoadAllBytes += bytes;
	ssCurLoadAllMs += ms;
	ssWinLoadAllFiles += (unsigned int)files;
	ssWinLoadAllBytes += bytes;
	ssWinLoadAllMs += ms;
	ssTotLoadAllMs += ms;
}

void
ssShutdown(void)
{
	if(!ssInited)
		return;

	/*
	 * Do not call ssDumpWindow() here: CGame::ShutDown() calls
	 * CModelInfo::ShutDown() and CAnimManager::Shutdown() before
	 * CStreaming::Shutdown(), so the top-list name resolution would walk dead
	 * pools. The periodic in-game dumps already carry the names; at exit only
	 * numeric totals and the streaming memory are safe to print.
	 */
	if(ssOn){
		ssLog("streamstats: total frames=%u files=%u bytes=%u batches=%u "
		      "evicts=%u conv=%ums fin=%ums loadall=%ums",
		      ssFrameSeq, ssTotFiles, ssTotBytes, ssTotBatches,
		      ssTotEvicts, ssTotConvMs, ssTotFinMs, ssTotLoadAllMs);
		ssLog("streamstats: shutdown mem used=%uMB avail=%uMB",
		      ssUsedMB(),
		      (unsigned int)(CStreaming::ms_memoryAvailable / SS_MB));
	}

	if(ssWinLoad)
		free(ssWinLoad);
	if(ssWinRemove)
		free(ssWinRemove);
	ssWinLoad = nil;
	ssWinRemove = nil;
	ssOn = 0;
	ssInited = 0;
}

#endif /* AMIGAOS */
