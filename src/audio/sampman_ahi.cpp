/*
 * sampman_ahi.cpp -- AmigaOS 68k AHI audio backend for reVC.
 *
 * AHI is not a 3D API: it mixes a fixed pool of channels to stereo and reads
 * each sound from a caller-owned sample pointer. This backend is therefore a
 * translation/state layer (AHI does the mixing):
 *
 *   reVC channel  ->  AHI channel
 *   sample slice  ->  a bounded RAM cache, bound to an AHI "sound" slot
 *   sample bank   ->  sfx.RAW on disk, read on demand (340 MB cannot be mapped)
 *
 * Design decisions for the AHI audio backend:
 *   - EXTERNAL_3D_SOUND stays on: the engine passes camera-relative positions,
 *     distances and an emitting volume; we reuse the public
 *     cAudioManager::ComputeVolume()/ComputePan() to produce a stereo gain/pan
 *     and let AHI mix. The MAX2DCHANNELS slot (police radio) passes volume/pan
 *     straight through.
 *   - sfx.SDT and each sfx.RAW slice are PC little-endian; they are byteswapped
 *     once to native 68k order (AMIGAOS only, so host-side builds stay identity).
 *   - AHI's SoundFunc was measured to fire on start *and* on every loop wrap
 *     (tests/amigaos/ahi_sfx_test.c), so it is not used to detect one-shot end;
 *     the backend tracks duration instead.
 *
 * The file always compiles the full cSampleManager surface; when ahi.device is
 * missing or REVC_AHI=0 every method silently no-ops, exactly like sampman_null.
 */

#include "common.h"
#include "Endian.h"

#ifdef AUDIO_AHI

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "Timer.h"
#include "EnvFlags.h"
#include "crossplatform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// AmigaOS system headers, deliberately LAST: their proto/inline macros and
// types collide with game identifiers (see src/skel/amigaos/amigaos.cpp).
// ---------------------------------------------------------------------------
#ifdef DeleteFile
#undef DeleteFile
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/ahi.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>
#include <proto/ahi.h>

// The NDK's <proto/ahi.h> declares `extern struct Library *AHIBase;`.
struct Library *AHIBase = NULL;

// mpega.library binding (streams). Declared `extern` by mpega_lib.h/mpega.h.
#include "mpega/mpega_lib.h"
struct Library *MPEGABase = NULL;

// ---------------------------------------------------------------------------
// Backend-owned symbols: exactly the tuple sampman_oal.cpp / sampman_miles.cpp
// define. sampman_null.cpp only defines the first four, which is not enough.
// ---------------------------------------------------------------------------
cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;

uint32 BankStartOffset[MAX_SFX_BANKS];

char SampleBankDescFilename[] = "audio/sfx.SDT";
char SampleBankDataFilename[] = "audio/sfx.RAW";

FILE *fpSampleDescHandle = NULL;
FILE *fpSampleDataHandle = NULL;
int8  gBankLoaded[MAX_SFX_BANKS];
int32 _nSampleDataEndOffset;   // sfx.RAW length; validates every SDT slice

#ifdef FIX_BUGS
uint32 gPlayerTalkSfx = UINT32_MAX;
void  *gPlayerTalkData = 0;
size_t gPlayerTalkDataSize = 0;
#endif

// ---------------------------------------------------------------------------
// Tunables / env
// ---------------------------------------------------------------------------
#define AHI_REQ_MIXFREQ_DEFAULT  22050
#define AHI_CACHE_KB_DEFAULT     8192   // 8 MB
#define AHI_CACHE_KB_FLOOR       2048   // 2 MB
#define AHI_NCACHE_SLOTS         128    // SFX cache-slot pool
#define AHI_MISSION_SLOT         MAX_PEDSFX          // slot 7
#define AHI_SFX_SLOT_BASE        (MAX_PEDSFX + 1)    // slot 8..
#define AHI_NSFX_SLOTS           (AHI_NCACHE_SLOTS - AHI_SFX_SLOT_BASE)
#define AHI_PAN_CENTRE           63      // pan is 0..MAX_VOLUME, 63 = centre
#define AHI_GAIN_Q15             32768u  // MAX_VOLUME maps here before scaling

// AmigaDOS env: libnix getenv() does not read ENV:, so use dos.library GetVar
// (the same mechanism the rest of the port uses in amigaos.cpp). A local
// buffer keeps this allocation-free and safe to call at init time.
static const char *
AhiEnvGet(const char *name, char *buf, int bufsize)
{
	LONG n;
	buf[0] = '\0';
	n = GetVar((STRPTR)name, (STRPTR)buf, bufsize - 1, 0);
	if (n < 0) {
		buf[0] = '\0';
		return NULL;
	}
	buf[bufsize - 1] = '\0';
	return buf;
}

static int
AhiEnvBool(const char *name, int def)
{
	char v[128];
	const char *s = AhiEnvGet(name, v, sizeof(v));
	int out = def;
	if (s != NULL && rfEnvParseBool(s, &out))
		return out;
	return def;
}

static int
AhiEnvInt(const char *name, int def, int min, int max)
{
	char v[128];
	const char *s = AhiEnvGet(name, v, sizeof(v));
	int out = def;
	if (s != NULL && rfEnvParseInt(s, &out)) {
		if (out < min) out = min;
		if (out > max) out = max;
		return out;
	}
	return def;
}

// Hex (0x-prefixed or bare) AHI audio ID from an env var, or `def`.
static ULONG
AhiEnvMode(const char *name, ULONG def)
{
	char v[128];
	const char *s = AhiEnvGet(name, v, sizeof(v));
	if (s == NULL || *s == '\0')
		return def;
	return (ULONG)strtoul(s, NULL, 16);
}

// ---------------------------------------------------------------------------
// AHI state
// ---------------------------------------------------------------------------
static struct MsgPort   *ahiPort = NULL;
static struct AHIRequest *ahiIO = NULL;
static struct AHIAudioCtrl *ahiCtrl = NULL;
static bool8 ahiSilent = TRUE;          // no device / REVC_AHI=0
static int   ahiTrace = 0;              // REVC_AHI_TRACE: dense per-op logging

// Early boot mode selection (amigaAhiEarlyModeSelect): the requester runs from
// main() before the GL context/screen exists, so its window is actually
// visible; cSampleManager::Initialise() later consumes the result instead of
// prompting again.
static ULONG ahiReqID = AHI_INVALID_ID; // requester choice, ~0 = none
static ULONG ahiReqMixFreq = 0;         // requester mix rate, 0 = use env/default
static bool8 ahiEarlyDone = FALSE;      // early select already ran (any outcome)
static bool8 ahiDeviceMissing = FALSE;  // no ahi.device: stay silent, no requester

#define AHITRACE(...) do { if (ahiTrace) debug(__VA_ARGS__); } while(0)
static ULONG ahiMixFreq = AHI_REQ_MIXFREQ_DEFAULT;
static ULONG ahiCacheBytes = AHI_CACHE_KB_DEFAULT * 1024;
static char  ahiModeName[80] = "silent";
static int   ahiNumSounds = AHI_NCACHE_SLOTS; // REVC_AHI_SOUNDS: SFX cache-slot count
static int   ahiNumBlocks = AHI_NSFX_SLOTS;   // usable SFX cache blocks

// ---------------------------------------------------------------------------
// N-channel software mixer (SNDDMA model).
//
// AHI halves the mix gain for every doubling of the *allocated* channel count
// (32 channels -> 1/32 ~= -30 dB), so we allocate ONE AHI channel per *bus* and
// mix all reVC voices / streams ourselves into a power-of-two stereo ring per
// bus, exactly like Sdursun/ReturnToCastleWolfenstein_Minigl68k
// (sp/amigaos/amiga_snd.c). AHI reports each bus's play position through a
// single AHIET_CHANNELINFO effect (one offset per channel); we keep each ring
// filled ahead of it.
//
// bus 0 = SFX voices (the pre-existing software mixer).
// bus 1 = streamed audio (radio / ambience / cutscene / mission speech).
//
// Two allocated channels make AHI apply 1/2 gain to *everything* (SFX too), so
// each bus carries a pre-gain (REVC_AHI_BUSBOOST, x2) with a saturating clamp,
// putting the two-bus output back at unity without ducking the SFX.
// See the software-mixer and radio-streaming entries in
// docs/PORT_STATUS.md.
// ---------------------------------------------------------------------------
#define AHI_MAX_BUSES 2
#define AHI_BUS_SFX    0
#define AHI_BUS_STREAM 1
#define AHI_LEAD_MS_DEFAULT 250      // mixed audio kept queued ahead of AHI (ms)

struct tAhiBus {
	int16 *ring;                 // interleaved stereo, native
	uint32 ringFrames;           // power of two
	uint32 ringMask;
	volatile ULONG readFrame;    // AHI play pos (frames), ISR-written
	uint32 writeFrame;           // mixer write cursor (frames)
	uint32 leadFrames;           // how far to stay ahead of AHI
	uint32 chan;                 // AHI channel index
	uint32 sound;                // AHI sound slot index
	bool8  idleSilent;           // ring already flushed to silence (no voices)
	uint32 pregain;              // 1 or 2 (per-bus boost)
};
static tAhiBus ahiBus[AHI_MAX_BUSES];
static int    ahiNumBuses = 2;

// ---- streamed audio (bus 1) shared state ----------------------------------
// Declared here because Initialise()/Terminate() touch it before the stream
// engine (further down) is defined.
enum {
	ST_STOPPED = 0,
	ST_PRELOADED,
	ST_PLAYING,
	ST_PAUSED,
	ST_DRAINING
};

#define STREAM_FIFO_FRAMES       32768    // power of two, source-rate stereo frames
#define STREAM_FIFO_MASK         (STREAM_FIFO_FRAMES - 1)
#define STREAM_LEAD_MS_DEFAULT   250
#define STREAM_DECODE_MAX_FRAMES 4096     // absolute cap on one decode call
#define STREAM_DECODE_SLICE_MS   40       // max decode work (~ms of PCM) per tick
#define STREAM_ADF_KEY           0x22     // .adf whole-file XOR obfuscation
#define STREAM_BITBUF            8192     // mpega bitstream buffer (multiple of 4)
#define STREAM_WAV_FMT_PCM       1        // WAVE_FORMAT_PCM
#define STREAM_WAV_FMT_IMA       0x11     // WAVE_FORMAT_IMA_ADPCM
#define STREAM_WAV_PCM_FRAMES    512      // source frames decoded per PCM block
#define STREAM_WAV_MAX_ALIGN     4096     // reject larger fmt blockAlign

// RIFF/WAVE source state (mission audio: cutscene dialogue, phone calls, ...).
// Unlike .mp3/.adf these are decoded here, not by mpega.library.
struct tAhiWav {
	uint16 fmt;
	uint16 channels;
	uint16 blockAlign;
	uint32 rate;
	uint32 dataOff, dataSize, dataLeft;
	uint32 blockFrames;               // source frames per decoded block
	uint8 *raw;                       // raw decode-block scratch
	int16 *pcm;                       // planar scratch, [0]=L, [blockFrames]=R
	int16 adpSample[2], adpStep[2];   // IMA ADPCM per-channel state
};

struct tAhiStream {
	uint8  state;
	bool8  wav;                       // source is RIFF/WAVE instead of MPEG
	struct tAhiWav wavInfo;
	bool8  loop;
	uint8  vol;                       // 0..127 raw (SetStreamedVolumeAndPan)
	uint8  pan;                       // 0..127
	bool8  effect;                    // use effects (not music) master volume
	FILE  *fp;                        // owned by the bitstream access hook
	uint32 xorByte;                   // 0 (plain .mp3) or STREAM_ADF_KEY (.adf)
	MPEGA_STREAM *mpds;
	MPEGA_CTRL ctrl;
	struct Hook bsHook;               // mpega bitstream access hook (h_Data = this)
	uint32 srcRate, srcCh;
	int16 *fifo;                      // interleaved stereo int16, source rate
	uint32 fifoHead, fifoCount;
	uint32 startMs;                   // position (ms) the stream started at
	uint32 consumed;                  // source frames consumed by the mixer
	uint32 mixFrac;                   // 16.16 fractional FIFO position
	uint32 stepW, stepF;              // source frames per output frame (16.16)
	uint32 gainL, gainR;
};
static tAhiStream ahiStreams[MAX_STREAMS];

static bool8  ahiStreamOn = TRUE;                 // REVC_AHI_MUSIC
static uint32 ahiStreamLeadMs = STREAM_LEAD_MS_DEFAULT;
static uint32 ahiMusicGain = 128;                 // REVC_AHI_MUSICVOL (128 = 1.0)
static uint32 ahiMusicQuality = MPEGA_QUALITY_HIGH; // REVC_AHI_MUSIC_QUALITY (0..2)
static uint32 ahiMusicDiv = 1;                    // REVC_AHI_MUSIC_DIV (1/2/4)
static uint32 ahiStreamLength[TOTAL_STREAMED_SOUNDS];
static uint8  ahiStreamLenKnown[TOTAL_STREAMED_SOUNDS];
static uint32 ahiStreamUnderruns;                 // PLAYING but FIFO empty at mix
static uint32 ahiStreamLastDump;                  // ms of last health line

// AHIET_CHANNELINFO effect: AHI calls the hook from its mixing interrupt with
// (a0=hook, a2=audioctrl, a1=AHIEffChannelInfo). One effect instance reports
// the play offset of every allocated channel.
struct tAhiChanInfo {
	struct AHIEffChannelInfo aeci;
	ULONG offset[AHI_MAX_BUSES];              // storage for ahieci_Offset[]
};
static struct tAhiChanInfo ahiChanInfo;
static struct Hook ahiChanInfoHook;

static ULONG
AhiChanInfoFunc(register struct Hook *hook __asm("a0"),
                register struct AHIAudioCtrl *actrl __asm("a2"),
                register struct AHIEffChannelInfo *aeci __asm("a1"))
{
	int i, n;
	(void)hook; (void)actrl;
	n = aeci->ahieci_Channels;
	if (n > AHI_MAX_BUSES)
		n = AHI_MAX_BUSES;
	for (i = 0; i < n; i++)
		ahiBus[i].readFrame = aeci->ahieci_Offset[i];
	return 0;
}

// ---------------------------------------------------------------------------
// Channel state
// ---------------------------------------------------------------------------
struct tAhiChannel {
	int32  sfx;              // sample index currently armed, -1 = none
	uint32 slot;             // cache block handle feeding `data`
	bool8  used;             // engine considers this channel playing
	bool8  looping;          // loop forever / finite (engine stops it)
	bool8  is2d;             // player comment / police radio: no 3D math
	uint32 freq;             // current playback rate (Hz)
	uint32 frames;           // sample length in frames
	uint32 loopStartFrames;
	uint32 loopLenFrames;
	uint32 loopEnd;          // loopStartFrames + loopLenFrames (cached)
	uint8  emittingVol;      // 0..127 (3D) or 2D volume
	uint8  pan;              // 2D pan (0..127)
	float  x, y, z;
	float  maxDist;
	uint32 startMs;
	/* Published to the mixer: the sample pointer, loop region and gains.
	   The mixer copies these under Forbid() (AhiSnapCopy). */
	const int16 *data;       // mono sample frames (native endian)
	uint32 stepW;            // whole frames advanced per output frame
	uint32 stepF;            // 0..65535 fractional advance per output frame
	uint32 gainL, gainR;     // 0..32768 per-side gains
	bool8  active;           // game thread: voice armed and producing samples
	uint32 epoch;            // bumped on every start/stop (pairs with doneEpoch)
};
static tAhiChannel ahiCh[NUM_CHANNELS];

// ---------------------------------------------------------------------------
// Decoupled SFX mixer state.
//
// The game thread alone owns ahiCh[] (loop points, gains, step, active,
// epoch). The mixer -- the background Process when REVC_AHI_MIXTASK is on,
// otherwise the game thread itself -- takes a short Forbid()-protected
// snapshot (mixSnap[]) at the start of every block and then owns mixAct[],
// mixVoice[] and the bus-0 write cursor for that block. It never touches
// ahiCh[] except inside AhiSnapCopy().
//
// One-shot completion is reported back by publishing ahiDoneEpoch[ch] = epoch
// after the ring read cursor has passed the mixed-out point; the game thread's
// GetChannelUsedFlag() retires `used` on the match. Keeping `used` set until
// then matches real playback completion (AL_PLAYING), not mix completion.
// ---------------------------------------------------------------------------
struct tMixSnap {
	const int16 *data;
	uint32 frames, loopStartFrames, loopLenFrames, loopEnd;
	uint32 stepW, stepF, gainL, gainR;
	uint32 epoch;
	bool8  looping;
};

struct tMixVoice {
	uint32 epoch;
	uint32 idx, frac;        // 32-bit cursor (idx + frac), same math as before
	uint32 doneAt;           // ring frame just past the voice's last audio
	bool8  done;             // mixed to end; stop producing audio
};

static tMixSnap   mixSnap[NUM_CHANNELS];
static tMixVoice  mixVoice[NUM_CHANNELS];
static uint32     mixAct[NUM_CHANNELS];
static uint32     mixActCount;
static volatile ULONG ahiDoneEpoch[NUM_CHANNELS];  // mixer -> game thread

static void AhiMixTaskEntry(void);   // background SFX mixer Process entry

static volatile bool8 ahiMixTaskOn = FALSE;    // mix Process is running
static volatile bool8 ahiMixTaskQuit = FALSE;  // ask it to exit
static volatile bool8 ahiMixTaskDone = TRUE;   // it has left its loop

static volatile uint32 ahiMixBlocks;     // AhiServiceSfxBus() calls (diagnostics)
static volatile uint32 ahiMixFlushes;    // SFX ring -> silence transitions
static volatile uint32 ahiMixResyncs;    // write cursor overtaken by AHI's read cursor
static uint32 ahiLastVoiceDump;   // last "still active" diagnostic dump (ms)
static uint32 ahiLastServiceMs;   // time of the previous Service() call
static uint32 ahiMixLastDump;     // last mix-counter diagnostic dump (ms)

// AHI's AHIP_LoopSound defaults to AHIP_Sound, so our streaming channels loop
// their ring buffers forever. That is what keeps playback gapless, but it also
// means: if a mixer stops refilling, AHI keeps replaying whatever is left in
// the ring -- the last sound mixed (a menu "doong", a gunshot tail, ...).
// Zero a ring and snap its write cursor to AHI's read cursor. This variant
// never logs: the SFX mixer runs in a background Process and must not touch
// DOS. Bus 1 (streams, game thread) uses AhiFlushSilenceBus() below.
static void
AhiRingSilence(tAhiBus *bus)
{
	if (bus->ring == NULL)
		return;
	memset(bus->ring, 0, (size_t)bus->ringFrames * 2 * sizeof(int16));
	bus->writeFrame = (uint32)bus->readFrame & bus->ringMask;
	bus->idleSilent = TRUE;
}

// Generic "bus went quiet" helper for the stream bus, called from the game
// thread, so it may log. The SFX bus has its own silent flush (AhiSfxFlush).
static void
AhiFlushSilenceBus(tAhiBus *bus)
{
	if (bus->ring == NULL)
		return;
	if (!bus->idleSilent)
		debug("[boot] ahi: ring %lu -> silence (idle/stall)\n",
		      (unsigned long)bus->chan);
	AhiRingSilence(bus);
}

// A finished one-shot has been fully played once the bus read cursor has
// passed the ring frame where its audio ended.
static bool8
AhiVoiceDrained(uint32 readFrame, uint32 doneAt, uint32 mask)
{
	return (((readFrame - doneAt) & mask) < ((mask + 1) >> 1));
}

// ---------------------------------------------------------------------------
// Sample cache: a bounded accounting cap over heap blocks, each bound to a
// dedicated AHI sound slot. LRU-evicted; pinned from InitialiseChannel until
// StopChannel so AHI never reads freed memory.
// ---------------------------------------------------------------------------
struct tSfxBlock {
	void  *data;
	uint32 size;
	int32  sfx;
	uint32 slot;
	int    pins;
	uint32 lastUse;
	uint32 idleMs;   // earliest time this block may be evicted (AHI tick grace)
};
// AHI applies set/stop changes at the next player tick; keep a buffer alive a
// little after its last channel stops so an eviction can't free memory the
// driver is still reading.
#define AHI_EVICT_GRACE_MS 250
static tSfxBlock ahiBlocks[AHI_NSFX_SLOTS];
static uint32 ahiArenaUsed = 0;
static uint32 ahiTick = 0;
static uint32 ahiHits = 0, ahiMisses = 0, ahiPins = 0, ahiEvicts = 0;
static uint32 ahiStarts = 0, ahiStops = 0;   // channel start/stop counters (diagnostics)

// Ped-comment and mission "player comment" resident buffers (see the plan).
static void  *pedArena = NULL;
static uint32 pedBlockSize = 0;
static int32  nPedSlotSfx[MAX_PEDSFX];
static uint8  nCurrentPedSlot = 0;

// ---------------------------------------------------------------------------
// Byte order: sfx.SDT / sfx.RAW are PC little-endian. Swap32/Swap16Array come
// from src/core/Endian.h (the array helper is byte-wise, i.e. alignment-safe).
// ---------------------------------------------------------------------------

// Read a raw byte range from sfx.RAW and byteswap it to native 16-bit.
static bool8
ReadRawBytes(uint32 offset, uint32 size, void *dst)
{
	if (fpSampleDataHandle == NULL || dst == NULL || size == 0)
		return FALSE;
	if (fseek(fpSampleDataHandle, (long)offset, SEEK_SET) != 0)
		return FALSE;
	if (fread(dst, 1, size, fpSampleDataHandle) != size)
		return FALSE;
	Swap16Array(dst, size / 2);
	return TRUE;
}

// ---------------------------------------------------------------------------
// Cache operations
// ---------------------------------------------------------------------------
static int32
FindSampleBlock(int32 sfx)
{
	int32 i;
	if (sfx < 0)
		return -1;
	for (i = 0; i < (uint32)ahiNumBlocks; i++)
		if (ahiBlocks[i].data != NULL && ahiBlocks[i].sfx == sfx)
			return i;
	return -1;
}

static void
FreeBlock(uint32 bi)
{
	if (ahiBlocks[bi].data == NULL)
		return;
	FreeVec(ahiBlocks[bi].data);
	ahiArenaUsed -= ahiBlocks[bi].size;
	ahiBlocks[bi].data = NULL;
	ahiBlocks[bi].size = 0;
	ahiBlocks[bi].sfx = -1;
	ahiBlocks[bi].pins = 0;
	ahiBlocks[bi].idleMs = 0;
}

// Stop the oldest playing channel so its block becomes evictable.
static void
StopOldestChannel(void)
{
	int32 best = -1;
	uint32 i, oldest = 0xffffffff;
	for (i = 0; i < NUM_CHANNELS; i++) {
		if (ahiCh[i].used && ahiCh[i].startMs <= oldest) {
			oldest = ahiCh[i].startMs;
			best = (int32)i;
		}
	}
	if (best >= 0) {
		AHITRACE("[boot] ahi:t stop-oldest ch=%ld (arena full)\n", (long)best);
		SampleManager.StopChannel((uint32)best);
	}
}

static bool8
EvictOne(void)
{
	int32 victim = -1;
	uint32 i, lru = 0xffffffff;
	uint32 now = CTimer::GetTimeInMilliseconds();
	for (i = 0; i < (uint32)ahiNumBlocks; i++) {
		if (ahiBlocks[i].data != NULL && ahiBlocks[i].pins == 0 &&
		    now >= ahiBlocks[i].idleMs && ahiBlocks[i].lastUse < lru) {
			lru = ahiBlocks[i].lastUse;
			victim = (int32)i;
		}
	}
	if (victim < 0)
		return FALSE;
	FreeBlock((uint32)victim);
	ahiEvicts++;
	return TRUE;
}

// Ensure sample `sfx` is resident; pin it and return its AHI sound slot.
// offset/size come from m_aSamples[sfx] (private, so passed by the caller).
static bool8
ResidentSample(int32 sfx, uint32 offset, uint32 size, uint32 *outSlot)
{
	int32 bi = FindSampleBlock(sfx);
	uint32 i;

	if (bi >= 0) {
		ahiBlocks[bi].pins++;
		ahiBlocks[bi].lastUse = ++ahiTick;
		ahiHits++;
		*outSlot = ahiBlocks[bi].slot;
		return TRUE;
	}

	ahiMisses++;
	if (size == 0 || size > ahiCacheBytes)
		return FALSE;

	// Make room.
	while (ahiArenaUsed + size > ahiCacheBytes) {
		if (!EvictOne()) {
			StopOldestChannel();
			if (!EvictOne())
				return FALSE;
		}
	}

	// Free slot?
	bi = -1;
	for (i = 0; i < (uint32)ahiNumBlocks; i++) {
		if (ahiBlocks[i].data == NULL) { bi = (int32)i; break; }
	}
	if (bi < 0) {
		if (!EvictOne())
			return FALSE;
		for (i = 0; i < (uint32)ahiNumBlocks; i++) {
			if (ahiBlocks[i].data == NULL) { bi = (int32)i; break; }
		}
		if (bi < 0)
			return FALSE;
	}

	ahiBlocks[bi].data = AllocVec(size, MEMF_PUBLIC);
	if (ahiBlocks[bi].data == NULL)
		return FALSE;
	if (!ReadRawBytes(offset, size, ahiBlocks[bi].data)) {
		FreeVec(ahiBlocks[bi].data);
		ahiBlocks[bi].data = NULL;
		return FALSE;
	}

	ahiBlocks[bi].size = size;
	ahiBlocks[bi].sfx = sfx;
	ahiBlocks[bi].pins = 1;
	ahiBlocks[bi].lastUse = ++ahiTick;
	ahiBlocks[bi].slot = AHI_SFX_SLOT_BASE + (uint32)bi;
	ahiArenaUsed += size;
	ahiPins++;
	AHITRACE("[boot] ahi:t load block=%ld sfx=%d slot=%ld size=%lu\n",
	         (long)bi, (int)sfx, (long)ahiBlocks[bi].slot, (unsigned long)size);

	// The mixer reads ahiBlocks[bi].data directly; no AHI_LoadSound needed.

	*outSlot = ahiBlocks[bi].slot;
	return TRUE;
}

static void
UnpinSlot(uint32 slot)
{
	uint32 i;
	for (i = 0; i < (uint32)ahiNumBlocks; i++) {
		if (ahiBlocks[i].data != NULL && ahiBlocks[i].slot == slot) {
			if (ahiBlocks[i].pins > 0)
				ahiBlocks[i].pins--;
			if (ahiBlocks[i].pins == 0)
				ahiBlocks[i].idleMs = CTimer::GetTimeInMilliseconds() + AHI_EVICT_GRACE_MS;
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Per-voice stereo gains from the engine volume (0..127) and pan (0..127,
// 63 = centre). Shared by StartChannel and the per-tick Service() refresh so
// the two can never diverge.
// ---------------------------------------------------------------------------
static void ChannelGainPan(uint32 ch, uint8 fadeVol, uint8 masterVol,
                           uint32 *outVol, uint32 *outPan);

// Stereo gains for a 0..127 volume and 0..127 pan (63 = centre). Shared by the
// SFX voices and the stream bus so the two can never diverge.
static void
AhiVolPanGains(uint32 vol, uint32 pan, uint32 *outL, uint32 *outR)
{
	uint32 v, gLf, gRf;

	if (vol > MAX_VOLUME)
		vol = MAX_VOLUME;
	v = (vol * AHI_GAIN_Q15) / MAX_VOLUME;        // 0..32768
	if (pan > MAX_VOLUME)
		pan = MAX_VOLUME;
	if (pan <= AHI_PAN_CENTRE) {
		gLf = MAX_VOLUME;
		gRf = (pan * MAX_VOLUME) / AHI_PAN_CENTRE;
	} else {
		gLf = ((MAX_VOLUME - pan) * MAX_VOLUME) / (MAX_VOLUME - AHI_PAN_CENTRE);
		gRf = MAX_VOLUME;
	}
	*outL = (v * gLf) / MAX_VOLUME;
	*outR = (v * gRf) / MAX_VOLUME;
}

// 16.16 source frames per output frame for `rate` -> `mix`. Shared by the SFX
// voices and the stream bus.
static void
AhiRateStep(uint32 rate, uint32 mix, uint32 *outW, uint32 *outF)
{
	uint64 s;

	if (rate == 0)
		rate = 1;
	if (mix == 0)
		mix = AHI_REQ_MIXFREQ_DEFAULT;
	s = ((uint64)rate << 16) / mix;     // 16.16 frames per output frame
	*outW = (uint32)(s >> 16);
	*outF = (uint32)(s & 0xffff);
}

static void
AhiVoiceGains(uint32 ch, uint8 fadeVol, uint8 masterVol)
{
	uint32 vol, pan;

	ChannelGainPan(ch, fadeVol, masterVol, &vol, &pan);
	AhiVolPanGains(vol, pan, &ahiCh[ch].gainL, &ahiCh[ch].gainR);
}

// Recompute the resample step from the current playback rate. The engine
// changes SetChannelFrequency() every frame (engine revs, Doppler, time scale),
// so this must run per tick, not only at start.
static void
AhiVoiceStep(uint32 ch)
{
	AhiRateStep(ahiCh[ch].freq, ahiMixFreq ? ahiMixFreq : AHI_REQ_MIXFREQ_DEFAULT,
	            &ahiCh[ch].stepW, &ahiCh[ch].stepF);
}

// ---------------------------------------------------------------------------
// cSampleManager
// ---------------------------------------------------------------------------
cSampleManager::cSampleManager(void) {}
cSampleManager::~cSampleManager(void) {}

#ifdef EXTERNAL_3D_SOUND
void cSampleManager::SetSpeakerConfig(int32 nConfig) {}
uint32 cSampleManager::GetMaximumSupportedChannels(void) { return MAXCHANNELS; }
uint32 cSampleManager::GetNum3DProvidersAvailable(void) { return 1; }
void cSampleManager::SetNum3DProvidersAvailable(uint32 num) {}
char *cSampleManager::Get3DProviderName(uint8 id)
{
	static char name[64] = "AHI";
	return name;
}
void cSampleManager::Set3DProviderName(uint8 id, char *name) {}
int8 cSampleManager::GetCurrent3DProviderIndex(void) { return 0; }
int8 cSampleManager::SetCurrent3DProvider(uint8 nProvider) { return 0; }
#endif

bool8 cSampleManager::IsMP3RadioChannelAvailable(void) { return FALSE; }

void cSampleManager::ReleaseDigitalHandle(void) {}
void cSampleManager::ReacquireDigitalHandle(void) {}

// ---------------------------------------------------------------------------
// Interactive audio-mode requester (REVC_AHI).
//
// AHI's Function Interface (AHI_AllocAudio/AHI_Play) is opened with
// AHI_NO_UNIT, and AHI_DEFAULT_ID then resolves to the prefs page labelled
// "Music unit" -- not the "Unit 0..3" pages. To let the user pick exactly the
// mode they configured (e.g. the PiStorm AHI driver), show AHI's own mode
// requester at boot and use its result. Returns TRUE with the selection.
// ---------------------------------------------------------------------------
static bool8
AhiRunAudioRequester(ULONG initialID, ULONG initialMixFreq, ULONG *outID, ULONG *outMixFreq)
{
	struct AHIAudioModeRequester *req;
	bool8 ok = FALSE;

	req = AHI_AllocAudioRequest(
		AHIR_SleepWindow, TRUE,
		TAG_DONE);
	if (req == NULL) {
		debug("[boot] ahi: requester alloc failed\n");
		return FALSE;
	}

	if (AHI_AudioRequest(req,
			AHIR_TitleText,      (ULONG)"reVC: select AHI audio mode",
			AHIR_PositiveText,   (ULONG)"Use",
			AHIR_NegativeText,   (ULONG)"Cancel",
			AHIR_DoMixFreq,      TRUE,
			AHIR_DoDefaultMode,  TRUE,
			AHIR_InitialAudioID, initialID,
			AHIR_InitialMixFreq, initialMixFreq,
			TAG_DONE)) {
		*outID = req->ahiam_AudioID;
		*outMixFreq = req->ahiam_MixFreq;
		ok = TRUE;
		debug("[boot] ahi: requester selected id=0x%lx mix=%lu\n",
		      (unsigned long)*outID, (unsigned long)*outMixFreq);
	} else {
		debug("[boot] ahi: requester cancelled (IoErr=%ld)\n", (long)IoErr());
	}

	AHI_FreeAudioRequest(req);
	return ok;
}

// ---------------------------------------------------------------------------
// Early AHI mode selection -- called from main() BEFORE rsINITIALIZE /
// rsRWINITIALIZE, i.e. while the Workbench/default public screen is still
// frontmost and the game's fullscreen RTG screen does not exist yet. This is
// what makes the requester visible (running it later, from Initialise(), put
// it under the GL screen). It also probes ahi.device: when AHI is not
// installed no requester is shown and the backend stays silent.
// ---------------------------------------------------------------------------
extern "C" int
amigaAhiEarlyModeSelect(void)
{
	struct MsgPort    *port;
	struct AHIRequest *io;
	ULONG outID = AHI_INVALID_ID, outMix = 0;

	// REVC_AHI=0: silent, nothing to ask.
	if (!AhiEnvBool("REVC_AHI", 1)) {
		ahiEarlyDone = TRUE;
		return 0;
	}

	port = CreateMsgPort();
	io = port ? (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest)) : NULL;
	if (io == NULL) {
		if (port != NULL)
			DeleteMsgPort(port);
		debug("[boot] ahi: early: CreateIORequest failed\n");
		// Mark done so the Initialise() fallback (which would run after the
		// game screen exists, i.e. hidden) can never fire.
		ahiEarlyDone = TRUE;
		return 0;
	}

	io->ahir_Version = 4;
	if (OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)io, 0) != 0) {
		// No AHI installed: do NOT show a requester; the backend will fall
		// through to the silent (REVC_AHI=0) behaviour in Initialise().
		debug("[boot] ahi: early: no ahi.device -> REVC_AHI forced off (silent)\n");
		ahiDeviceMissing = TRUE;
		DeleteIORequest((struct IORequest *)io);
		DeleteMsgPort(port);
		return 0;
	}

	AHIBase = (struct Library *)io->ahir_Std.io_Device;

	// Query the user; only reachable when REVC_AHI is enabled (device opened).
	{
		int mix0 = AhiEnvInt("REVC_AHI_MIXFREQ", AHI_REQ_MIXFREQ_DEFAULT, 4000, 48000);
		ULONG initID = AhiEnvMode("REVC_AHI_MODE", AHI_DEFAULT_ID);
		debug("[boot] ahi: early requester (before GL init)\n");
		if (AhiRunAudioRequester(initID, (ULONG)mix0, &outID, &outMix)) {
			ahiReqID = outID;
			ahiReqMixFreq = outMix;
		}
	}

	CloseDevice((struct IORequest *)io);
	DeleteIORequest((struct IORequest *)io);
	DeleteMsgPort(port);
	AHIBase = NULL;
	ahiEarlyDone = TRUE;
	return 1;
}

// Common teardown when the software mixer cannot be brought up: leave
// the manager initialised but silent (same contract as "no ahi.device").
static void
AhiMixerInitFail(void)
{
	int b;
	for (b = 0; b < AHI_MAX_BUSES; b++) {
		if (ahiBus[b].ring != NULL) {
			FreeVec(ahiBus[b].ring);
			ahiBus[b].ring = NULL;
		}
	}
	if (ahiCtrl != NULL) {
		AHI_FreeAudio(ahiCtrl);
		ahiCtrl = NULL;
	}
	if (ahiIO != NULL) {
		CloseDevice((struct IORequest *)ahiIO);
		DeleteIORequest((struct IORequest *)ahiIO);
		ahiIO = NULL;
	}
	if (ahiPort != NULL) {
		DeleteMsgPort(ahiPort);
		ahiPort = NULL;
	}
	ahiSilent = TRUE;
	_bSampmanInitialised = TRUE;
}

bool8
cSampleManager::Initialise(void)
{
	if (_bSampmanInitialised)
		return TRUE;

	// SFX cache-slot count. AHI uses one channel per bus (see the mixer), so
	// there is no separate channel-count knob.
	ahiNumSounds = AhiEnvInt("REVC_AHI_SOUNDS", AHI_NCACHE_SLOTS, AHI_SFX_SLOT_BASE + 4, 512);
	ahiNumBlocks = ahiNumSounds - AHI_SFX_SLOT_BASE;
	if (ahiNumBlocks > AHI_NSFX_SLOTS)
		ahiNumBlocks = AHI_NSFX_SLOTS;

	{
		int32 i;
		for (i = 0; i < NUM_CHANNELS; i++) {
			memset(&ahiCh[i], 0, sizeof(ahiCh[i]));
			ahiCh[i].sfx = -1;
			ahiDoneEpoch[i] = 0xFFFFFFFF;
		}
		memset(mixVoice, 0, sizeof(mixVoice));
		mixActCount = 0;
		ahiMixBlocks = 0;
		ahiMixFlushes = 0;
		ahiMixResyncs = 0;
		ahiMixTaskOn = FALSE;
		ahiMixTaskQuit = FALSE;
		ahiMixTaskDone = TRUE;
		for (i = 0; i < AHI_NSFX_SLOTS; i++) {
			ahiBlocks[i].data = NULL;
			ahiBlocks[i].sfx = -1;
		}
		for (i = 0; i < MAX_PEDSFX; i++)
			nPedSlotSfx[i] = NO_SAMPLE;
	}

	{
		for (int32 i = 0; i < MAX_SFX_BANKS; i++)
			gBankLoaded[i] = LOADING_STATUS_NOT_LOADED;
	}

	{
		for (int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
			m_aSamples[i].nOffset = 0;
			m_aSamples[i].nSize = 0;
			m_aSamples[i].nFrequency = 22050;
			m_aSamples[i].nLoopStart = 0;
			m_aSamples[i].nLoopEnd = -1;
		}
		m_nEffectsVolume = MAX_VOLUME;
		m_nMusicVolume = MAX_VOLUME;
		m_nEffectsFadeVolume = MAX_VOLUME;
		m_nMusicFadeVolume = MAX_VOLUME;
		m_nMP3BoostVolume = MAX_VOLUME;
		m_nMonoMode = FALSE;
	}

	// Stream / music tunables. Bus 1 only matters when ahi.device + a control
	// came up; mpega.library is opened below and degrades to music-off if absent.
	ahiStreamOn   = AhiEnvBool("REVC_AHI_MUSIC", 1) ? TRUE : FALSE;
	ahiStreamLeadMs = (uint32)AhiEnvInt("REVC_AHI_DECODE_LEAD", STREAM_LEAD_MS_DEFAULT, 20, 2000);
	ahiMusicGain  = (uint32)AhiEnvInt("REVC_AHI_MUSICVOL", 128, 0, 128);
	ahiMusicQuality = (uint32)AhiEnvInt("REVC_AHI_MUSIC_QUALITY", MPEGA_QUALITY_HIGH, 0, 2);
	{
		int div = AhiEnvInt("REVC_AHI_MUSIC_DIV", 1, 1, 4);
		ahiMusicDiv = (div == 2 || div == 4) ? (uint32)div : 1;
	}
	{
		int i;
		for (i = 0; i < MAX_STREAMS; i++) {
			ahiStreams[i].mpds = NULL;
			ahiStreams[i].fp = NULL;
			ahiStreams[i].fifo = NULL;
			ahiStreams[i].wav = FALSE;
			ahiStreams[i].wavInfo.raw = NULL;
			ahiStreams[i].wavInfo.pcm = NULL;
			ahiStreams[i].loop = FALSE;
			ahiStreams[i].state = ST_STOPPED;
		}
	}

	ahiTrace = AhiEnvBool("REVC_AHI_TRACE", 0);
	if (!AhiEnvBool("REVC_AHI", 1)) {
		debug("[boot] ahi: silent (REVC_AHI=0)\n");
		ahiSilent = TRUE;
		_bSampmanInitialised = TRUE;
		return TRUE;
	}
	debug("[boot] ahi: backend ready (trace=%d)\n", ahiTrace);

	// ---- read the sample bank index (needed before AHI setup logging) ----
	debug("[boot] ahi: step banks\n");
	if (!InitialiseSampleBanks()) {
		debug("[boot] ahi: silent (no %s/%s)\n", SampleBankDescFilename, SampleBankDataFilename);
		ahiSilent = TRUE;
		_bSampmanInitialised = TRUE;
		return TRUE;
	}
	debug("[boot] ahi: step banks ok\n");

	// ---- open ahi.device ----
	ahiPort = CreateMsgPort();
	if (ahiPort != NULL)
		ahiIO = (struct AHIRequest *)CreateIORequest(ahiPort, sizeof(struct AHIRequest));
	if (ahiIO == NULL) {
		debug("[boot] ahi: silent (CreateIORequest failed)\n");
		if (ahiPort != NULL) { DeleteMsgPort(ahiPort); ahiPort = NULL; }
		ahiSilent = TRUE;
		_bSampmanInitialised = TRUE;
		return TRUE;
	}
	ahiIO->ahir_Version = 4;
	if (OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)ahiIO, 0) != 0) {
		// Never CloseDevice() a request that failed to open; release it here.
		debug("[boot] ahi: silent (no ahi.device)\n");
		DeleteIORequest((struct IORequest *)ahiIO);
		ahiIO = NULL;
		DeleteMsgPort(ahiPort);
		ahiPort = NULL;
		ahiSilent = TRUE;
		_bSampmanInitialised = TRUE;
		return TRUE;
	}
	AHIBase = (struct Library *)ahiIO->ahir_Std.io_Device;
	debug("[boot] ahi: step device ok v%lu.%lu\n",
	      (unsigned long)AHIBase->lib_Version, (unsigned long)AHIBase->lib_Revision);

	// ---- enumerate audio modes, then try candidates in order ----------------
	{
		ULONG id = AHI_INVALID_ID, firstRt = AHI_INVALID_ID, match = AHI_INVALID_ID, any = AHI_INVALID_ID;
		ULONG reqID = ahiReqID, reqMixFreq = ahiReqMixFreq;
		ULONG cand[6];
		int ncand = 0, modeCount = 0, i;

		while ((id = AHI_NextAudioID(id)) != AHI_INVALID_ID) {
			LONG rt = 0, maxCh = 0;
			UBYTE name[80], drv[80];
			name[0] = drv[0] = '\0';
			modeCount++;
			AHI_GetAudioAttrs(id, NULL,
				AHIDB_BufferLen, 80,
				AHIDB_Driver, (ULONG)drv,
				AHIDB_Name, (ULONG)name,
				AHIDB_Realtime, (ULONG)&rt,
				AHIDB_MaxChannels, (ULONG)&maxCh,
				TAG_DONE);
			debug("[boot] ahi: mode 0x%08lx rt=%d ch=%u %s\n",
			      (unsigned long)id, (int)rt, (unsigned)maxCh, name);
			if (rt && firstRt == AHI_INVALID_ID)
				firstRt = id;
			{
				char low[80];
				int k, paula = 0;
				for (k = 0; k < 79 && drv[k]; k++)
					low[k] = (drv[k] >= 'A' && drv[k] <= 'Z') ? (char)(drv[k] + 32) : (char)drv[k];
				low[k] = '\0';
				if (strstr(low, "paula") || strstr(low, "amiberry") || strstr(low, "uae"))
					paula = 1;
				if (paula) {
					if (any == AHI_INVALID_ID) any = id;
					if (rt && match == AHI_INVALID_ID) match = id;
				}
			}
		}

		// Mode selection normally ran early from main()
		// (amigaAhiEarlyModeSelect) while the screen was still visible; its
		// result is in ahiReqID/ahiReqMixFreq. Only if that never ran (e.g. a
		// harness calling Initialise() directly) fall back to prompting here.
		if (!ahiEarlyDone && !ahiDeviceMissing) {
			int mix0 = AhiEnvInt("REVC_AHI_MIXFREQ", AHI_REQ_MIXFREQ_DEFAULT, 4000, 48000);
			ULONG initID = AhiEnvMode("REVC_AHI_MODE", AHI_DEFAULT_ID);
			AhiRunAudioRequester(initID, (ULONG)mix0, &reqID, &reqMixFreq);
		}

		// Candidate order: requester choice, explicit override, then the
		// user's AHI-preferred device (AHI_DEFAULT_ID is the mode chosen in
		// AHI prefs -- the right pick on real hardware, where a "Paula" mode
		// may exist but be silent), then a Paula/UAE realtime mode, then any
		// Paula, then first realtime.
		if (reqID != AHI_INVALID_ID)
			cand[ncand++] = reqID;
		{
			ULONG want = AhiEnvMode("REVC_AHI_MODE", AHI_INVALID_ID);
			if (want != AHI_INVALID_ID && want != reqID) cand[ncand++] = want;
		}
		if (AHI_DEFAULT_ID != reqID) cand[ncand++] = (ULONG)AHI_DEFAULT_ID;
		if (match != AHI_INVALID_ID) cand[ncand++] = match;
		if (any != AHI_INVALID_ID) cand[ncand++] = any;
		if (firstRt != AHI_INVALID_ID) cand[ncand++] = firstRt;

		{
			int mixReq = AhiEnvInt("REVC_AHI_MIXFREQ", AHI_REQ_MIXFREQ_DEFAULT, 4000, 48000);
			int cacheKb = AhiEnvInt("REVC_AHI_CACHE", AHI_CACHE_KB_DEFAULT, AHI_CACHE_KB_FLOOR, 64 * 1024);
			if (reqMixFreq >= 4000 && reqMixFreq <= 48000)
				mixReq = (int)reqMixFreq;
			ahiCacheBytes = (ULONG)cacheKb * 1024;

			// Bus count: 2 = SFX + streams (default), 1 = SFX only with the
			// streams mixed into bus 0 (0 dB, no AHI channel-count cut).
			ahiNumBuses = AhiEnvInt("REVC_AHI_BUSES", 2, 1, AHI_MAX_BUSES);

			for (i = 0; i < ncand && ahiCtrl == NULL; i++) {
				debug("[boot] ahi: try id=0x%08lx (sounds=%d mix=%d)\n",
				      (unsigned long)cand[i], ahiNumSounds, mixReq);
				// One channel per bus: with 2 buses AHI's gain is 1/2, put
				// back per-bus by REVC_AHI_BUSBOOST. Mixing all reVC voices
				// and streams is our job (see the software-mixer block).
				ahiCtrl = AHI_AllocAudio(
					AHIA_AudioID, cand[i],
					AHIA_MixFreq, (ULONG)mixReq,
					AHIA_Channels, (ULONG)ahiNumBuses,
					AHIA_Sounds, (ULONG)ahiNumBuses,
					TAG_DONE);
				if (ahiCtrl != NULL) {
					id = cand[i];
					ahiMixFreq = (ULONG)mixReq;
					AHI_ControlAudio(ahiCtrl, AHIC_MixFreq_Query, (ULONG)&ahiMixFreq, TAG_DONE);
					if (ahiMixFreq == 0)
						ahiMixFreq = (ULONG)mixReq;
				}
			}

			if (ahiCtrl == NULL) {
				debug("[boot] ahi: silent (all %d modes failed; sounds=%d)\n",
				      ncand, ahiNumSounds);
				CloseDevice((struct IORequest *)ahiIO);
				DeleteIORequest((struct IORequest *)ahiIO);
				ahiIO = NULL;
				DeleteMsgPort(ahiPort);
				ahiPort = NULL;
				ahiSilent = TRUE;
				_bSampmanInitialised = TRUE;
				return TRUE;
			}
		}

		{
			UBYTE name[80];
			name[0] = '\0';
			AHI_GetAudioAttrs(id, NULL, AHIDB_BufferLen, 80, AHIDB_Name, (ULONG)name, TAG_DONE);
			strncpy(ahiModeName, (const char *)name, sizeof(ahiModeName) - 1);
			ahiModeName[sizeof(ahiModeName) - 1] = '\0';
		}
		debug("[boot] ahi: chosen id=0x%lx %s (modes=%d)\n",
		      (unsigned long)id, ahiModeName, modeCount);

		// The selected mode's MASTER OUTPUT volume is a separate control from
		// the per-channel AHIP_Vol: AHI_AllocAudio initialises it from the
		// mode's prefs, which can be well below full (the "very quiet even at
		// full in-game volume" report). Query the range, log it, and drive it
		// to the maximum so the only attenuation is the game's own volume.
		{
			LONG outNow = 0, outMin = 0, outMax = 0;
			// Always drive the mode's master output to full scale (Fixed
			// 1.0). Only raise it further if the driver advertises a higher
			// max; never lower it (a bogus min/max query must not attenuate).
			LONG setTo = 0x10000;
			AHI_ControlAudio(ahiCtrl, AHIC_OutputVolume_Query, (ULONG)&outNow, TAG_DONE);
			AHI_GetAudioAttrs(id, NULL,
				AHIDB_MinOutputVolume, (ULONG)&outMin,
				AHIDB_MaxOutputVolume, (ULONG)&outMax,
				TAG_DONE);
			if (outMax > setTo)
				setTo = outMax;
			debug("[boot] ahi: output volume now=%ld min=%ld max=%ld -> set %ld\n",
			      (long)outNow, (long)outMin, (long)outMax, (long)setTo);
			AHI_ControlAudio(ahiCtrl, AHIC_OutputVolume, (ULONG)setTo, TAG_DONE);
			AHI_ControlAudio(ahiCtrl, AHIC_OutputVolume_Query, (ULONG)&outNow, TAG_DONE);
			debug("[boot] ahi: output volume now=%ld\n", (long)outNow);
		}

		// ---- per-bus streaming rings (software-mixer output) --------------
		{
			ULONG q = 0;
			struct AHISampleInfo si;
			uint32 pregain;
			int leadMs;
			int b;

			// The driver may answer with its real clock; keep sane values.
			AHI_ControlAudio(ahiCtrl, AHIC_MixFreq_Query, (ULONG)&q, TAG_DONE);
			if (q >= 8000 && q <= 96000)
				ahiMixFreq = q;
			else if (ahiMixFreq < 8000 || ahiMixFreq > 96000)
				ahiMixFreq = AHI_REQ_MIXFREQ_DEFAULT;

			// Two allocated AHI channels => AHI applies 1/2 gain to the whole
			// mix. Pre-scale each bus by x2 so the two-bus output is unity.
			pregain = (ahiNumBuses > 1 && AhiEnvBool("REVC_AHI_BUSBOOST", 1)) ? 2 : 1;
			// Mixed audio kept queued ahead of AHI. Also the slack that rides
			// out a slow gameplay frame (bigger = more resilient, more latency).
			leadMs = AhiEnvInt("REVC_AHI_LEAD", AHI_LEAD_MS_DEFAULT, 20, 1000);

			for (b = 0; b < ahiNumBuses; b++) {
				tAhiBus *bus = &ahiBus[b];
				bus->chan = (uint32)b;
				bus->sound = (uint32)b;
				bus->pregain = pregain;
				bus->ringFrames = (ahiMixFreq > 32000) ? 32768 : 16384;
				bus->ringMask = bus->ringFrames - 1;
				bus->leadFrames = ((uint32)leadMs * ahiMixFreq) / 1000;
				if (bus->leadFrames < 512)
					bus->leadFrames = 512;
				if (bus->leadFrames > bus->ringFrames / 2)
					bus->leadFrames = bus->ringFrames / 2;
				bus->ring = (int16 *)AllocVec((ULONG)bus->ringFrames * 2 * (ULONG)sizeof(int16),
				                              MEMF_PUBLIC | MEMF_CLEAR);
				if (bus->ring == NULL) {
					debug("[boot] ahi: silent (ring %d alloc failed)\n", b);
					AhiMixerInitFail();
					return TRUE;
				}

				// Always 16-bit stereo: the mixer writes native-endian int16
				// pairs and AHI converts to the mode's real resolution.
				si.ahisi_Type = AHIST_S16S;
				si.ahisi_Address = bus->ring;
				si.ahisi_Length = bus->ringFrames;
				if (AHI_LoadSound(bus->sound, AHIST_DYNAMICSAMPLE, &si, ahiCtrl) != AHIE_OK) {
					debug("[boot] ahi: silent (AHI_LoadSound %d failed)\n", b);
					AhiMixerInitFail();
					return TRUE;
				}
			}

			AHI_ControlAudio(ahiCtrl, AHIC_Play, TRUE, TAG_DONE);
			for (b = 0; b < ahiNumBuses; b++) {
				tAhiBus *bus = &ahiBus[b];
				AHI_Play(ahiCtrl,
					AHIP_BeginChannel, bus->chan,
					AHIP_Freq, ahiMixFreq,
					AHIP_Vol, 0x10000,
					AHIP_Pan, 0x8000,
					AHIP_Sound, bus->sound,
					AHIP_EndChannel, bus->chan,
					TAG_DONE);
				bus->writeFrame = 0;
				bus->readFrame = 0;
			}

			// AHI pokes this hook from its mixing interrupt with the play
			// offset of every channel; each mixer keeps its ring filled ahead.
			ahiChanInfo.aeci.ahie_Effect = AHIET_CHANNELINFO;
			ahiChanInfo.aeci.ahieci_Func = &ahiChanInfoHook;
			ahiChanInfo.aeci.ahieci_Channels = (UWORD)ahiNumBuses;
			for (b = 0; b < AHI_MAX_BUSES; b++)
				ahiChanInfo.offset[b] = 0;
			ahiChanInfoHook.h_Entry = (ULONG (*)())AhiChanInfoFunc;
			ahiChanInfoHook.h_Data = &ahiChanInfo;
			AHI_SetEffect(&ahiChanInfo.aeci, ahiCtrl);

			debug("[boot] ahi: mixer on, buses=%d ring=%lu frames @ %luHz lead=%lu pre=%lu\n",
			      ahiNumBuses, (unsigned long)ahiBus[0].ringFrames,
			      (unsigned long)ahiMixFreq, (unsigned long)ahiBus[0].leadFrames,
			      (unsigned long)pregain);
		}
	}

	// ---- resident ped-comment + mission buffers ----
	{
		uint32 maxPed = PED_BLOCKSIZE;
		uint32 maxPlayer = 0;
		uint32 i;
		for (i = SAMPLEBANK_MAX; i < (uint32)PLAYER_COMMENTS_START && i < TOTAL_AUDIO_SAMPLES; i++)
			if (m_aSamples[i].nSize > maxPed)
				maxPed = m_aSamples[i].nSize;
		for (i = PLAYER_COMMENTS_START; i <= PLAYER_COMMENTS_END && i < TOTAL_AUDIO_SAMPLES; i++)
			if (m_aSamples[i].nSize > maxPlayer)
				maxPlayer = m_aSamples[i].nSize;

		// Sample sizes were validated against sfx.RAW, so these fit; the
		// uint64 guard just makes the multiply overflow-proof.
		debug("[boot] ahi: step buffers ped=%lu player=%lu\n",
		      (unsigned long)maxPed, (unsigned long)maxPlayer);
		if ((uint64)maxPed * MAX_PEDSFX <= 0xffffffffULL) {
			pedBlockSize = maxPed;
			pedArena = AllocVec((ULONG)((uint64)maxPed * MAX_PEDSFX), MEMF_PUBLIC | MEMF_CLEAR);
		} else {
			pedBlockSize = 0;
			pedArena = NULL;
		}
#ifdef FIX_BUGS
		gPlayerTalkData = AllocVec(maxPlayer, MEMF_PUBLIC | MEMF_CLEAR);
		gPlayerTalkDataSize = maxPlayer;
		gPlayerTalkSfx = UINT32_MAX;
#endif
	}
	ahiSilent = FALSE;

	// ---- mpega.library (streamed audio): optional, like ahi.device --------
	// Missing library => music/radio off, SFX unaffected.
	if (ahiStreamOn) {
		MPEGABase = OpenLibrary((STRPTR)"mpega.library", 2);
		if (MPEGABase == NULL) {
			ahiStreamOn = FALSE;
			debug("[boot] ahi: no mpega.library -> streams off (SFX ok)\n");
		} else {
			debug("[boot] ahi: mpega.library v%lu.%lu open, streams on\n",
			      (unsigned long)MPEGABase->lib_Version,
			      (unsigned long)MPEGABase->lib_Revision);
		}
	} else {
		debug("[boot] ahi: streams off (REVC_AHI_MUSIC=0)\n");
	}

	LoadSampleBank(SFX_BANK_0);

	// Optional startup self-test: exercise the exact MusicManager radio call
	// sequence (loop + nonzero start pos) without driving the game.
	// REVC_AHI_STREAMTEST=<track> (default -1 = off).
	if (ahiStreamOn) {
		int st = AhiEnvInt("REVC_AHI_STREAMTEST", -1, -1, (int)TOTAL_STREAMED_SOUNDS - 1);
		if (st >= 0) {
			SetStreamedFileLoopFlag(TRUE, 0);
			SetStreamedVolumeAndPan(MAX_VOLUME, AHI_PAN_CENTRE, FALSE, 0);
			if (StartStreamedFile((tTrack)st, 30000, 0))
				debug("[boot] ahi:s selftest started trk=%d\n", st);
			else
				debug("[boot] ahi:s selftest FAILED trk=%d\n", st);
		}
	}

	debug("[boot] ahi: %s @ %luHz, %d channels, %luKB cache\n",
	      ahiModeName, (unsigned long)ahiMixFreq, (int)NUM_CHANNELS,
	      (unsigned long)(ahiCacheBytes / 1024));

	// ---- decoupled SFX mixer Process (REVC_AHI_MIXTASK) -------------------
	// The AHI ring is a looping DMA buffer; refilling it only from the game
	// tick stalls whenever the engine blocks (menu init, texture/slot loads,
	// level load) and AHI then replays the last lap (the menu "doong"). A
	// dedicated Process keeps the SFX ring fed independently of the frame
	// loop. Streams stay on the game thread; REVC_AHI_BUSES=1 stays
	// synchronous, as does REVC_AHI_MIXTASK=0.
	{
		int wantMixTask = AhiEnvBool("REVC_AHI_MIXTASK", 1);
		if (ahiNumBuses >= 2 && wantMixTask) {
			struct Process *p;
			// Clear the "done" flag *before* creating the Process so a
			// Terminate() that runs before the child is first dispatched still
			// waits for it to enter and leave the loop (no stale mixer).
			ahiMixTaskDone = FALSE;
			p = CreateNewProcTags(
				NP_Entry, (ULONG)AhiMixTaskEntry,
				NP_Name, (ULONG)"reVC_ahimix",
				NP_StackSize, 8192,
				NP_Priority, 2,
				TAG_DONE);
			if (p == NULL) {
				debug("[boot] ahi: mix task create failed, synchronous SFX\n");
			} else {
				ahiMixTaskOn = TRUE;
				debug("[boot] ahi: mix task on (prio 2, block 20ms)\n");
			}
		}
	}

	_bSampmanInitialised = TRUE;
	return TRUE;
}

void
cSampleManager::Terminate(void)
{
	uint32 i;
	int b;

	// Stop the decoupled SFX mixer before any AHI/ring teardown: it must never
	// observe a freed ring or a closed ahi.device.
	if (ahiMixTaskOn) {
		ahiMixTaskQuit = TRUE;
		for (i = 0; i < 100 && !ahiMixTaskDone; i++)
			Delay(1);
		ahiMixTaskOn = FALSE;
		if (!ahiMixTaskDone)
			debug("[boot] ahi: mix task did not stop\n");
	}

	// Release every open stream first (own file handles / FIFOs).
	for (i = 0; i < MAX_STREAMS; i++)
		StopStreamedFile((uint8)i);

	if (ahiCtrl != NULL) {
		// Cancel the channel-info effect and stop every streaming channel.
		ahiChanInfo.aeci.ahie_Effect = AHIET_CHANNELINFO | AHIET_CANCEL;
		AHI_SetEffect(&ahiChanInfo.aeci, ahiCtrl);
		AHI_ControlAudio(ahiCtrl, AHIC_Play, FALSE, TAG_DONE);
		AHI_FreeAudio(ahiCtrl);
		ahiCtrl = NULL;
	}
	for (b = 0; b < AHI_MAX_BUSES; b++) {
		if (ahiBus[b].ring != NULL) {
			FreeVec(ahiBus[b].ring);
			ahiBus[b].ring = NULL;
		}
		ahiBus[b].ringFrames = 0;
		ahiBus[b].ringMask = 0;
		ahiBus[b].writeFrame = 0;
		ahiBus[b].readFrame = 0;
		ahiBus[b].idleSilent = FALSE;
	}
	mixActCount = 0;
	memset(mixVoice, 0, sizeof(mixVoice));
	if (ahiIO != NULL) {
		CloseDevice((struct IORequest *)ahiIO);
		DeleteIORequest((struct IORequest *)ahiIO);
		ahiIO = NULL;
	}
	if (ahiPort != NULL) {
		DeleteMsgPort(ahiPort);
		ahiPort = NULL;
	}
	AHIBase = NULL;

	if (MPEGABase != NULL) {
		CloseLibrary(MPEGABase);
		MPEGABase = NULL;
	}
	ahiStreamOn = FALSE;

	for (i = 0; i < AHI_NSFX_SLOTS; i++) {
		if (ahiBlocks[i].data != NULL) {
			FreeVec(ahiBlocks[i].data);
			ahiBlocks[i].data = NULL;
			ahiBlocks[i].sfx = -1;
		}
	}
	ahiArenaUsed = 0;

	if (pedArena != NULL) {
		FreeVec(pedArena);
		pedArena = NULL;
	}
#ifdef FIX_BUGS
	if (gPlayerTalkData != NULL) {
		FreeVec(gPlayerTalkData);
		gPlayerTalkData = NULL;
		gPlayerTalkDataSize = 0;
		gPlayerTalkSfx = UINT32_MAX;
	}
#endif

	if (fpSampleDataHandle != NULL) {
		fclose(fpSampleDataHandle);
		fpSampleDataHandle = NULL;
	}

	_bSampmanInitialised = FALSE;
	ahiSilent = TRUE;

	debug("[boot] ahi: terminated (starts=%lu stops=%lu hits=%lu miss=%lu pins=%lu evict=%lu)\n",
	      (unsigned long)ahiStarts, (unsigned long)ahiStops,
	      (unsigned long)ahiHits, (unsigned long)ahiMisses, (unsigned long)ahiPins,
	      (unsigned long)ahiEvicts);
}

bool8 cSampleManager::CheckForAnAudioFileOnCD(void) { return TRUE; }
char  cSampleManager::GetCDAudioDriveLetter(void) { return '\0'; }

void
cSampleManager::UpdateEffectsVolume(void)
{
	// Gains are recomputed from m_nEffectsVolume/m_nEffectsFadeVolume on every
	// Service() tick, so there is nothing to push here.
}

void cSampleManager::SetEffectsMasterVolume(uint8 nVolume)
{
	m_nEffectsVolume = nVolume;
	UpdateEffectsVolume();
}

void cSampleManager::SetMusicMasterVolume(uint8 nVolume) { m_nMusicVolume = nVolume; }
void cSampleManager::SetMP3BoostVolume(uint8 nVolume) { m_nMP3BoostVolume = nVolume; }

void cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
	m_nEffectsFadeVolume = nVolume;
	UpdateEffectsVolume();
}

void cSampleManager::SetMusicFadeVolume(uint8 nVolume) { m_nMusicFadeVolume = nVolume; }
void cSampleManager::SetMonoMode(bool8 nMode) { m_nMonoMode = nMode; }

// ---------------------------------------------------------------------------
// Sample bank
// ---------------------------------------------------------------------------
bool8
cSampleManager::LoadSampleBank(uint8 nBank)
{
	if (nBank >= MAX_SFX_BANKS)
		return FALSE;
	if (ahiSilent)
		return FALSE;
	gBankLoaded[nBank] = LOADING_STATUS_LOADED;
	return TRUE;
}

void
cSampleManager::UnloadSampleBank(uint8 nBank)
{
	if (nBank >= MAX_SFX_BANKS)
		return;
	gBankLoaded[nBank] = LOADING_STATUS_NOT_LOADED;
}

int8
cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	if (nBank >= MAX_SFX_BANKS)
		return LOADING_STATUS_NOT_LOADED;
	return gBankLoaded[nBank];
}

#ifdef FIX_BUGS
uint8
cSampleManager::IsMissionAudioLoaded(uint8 nSlot, uint32 nSample)
{
	if (nSlot != MISSION_AUDIO_PLAYER_COMMENT)
		return LOADING_STATUS_NOT_LOADED;
	return (gPlayerTalkSfx == nSample) ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
}

bool8
cSampleManager::LoadMissionAudio(uint8 nSlot, uint32 nSample)
{
	if (nSlot != MISSION_AUDIO_PLAYER_COMMENT || nSample >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	if (ahiSilent || gPlayerTalkData == NULL)
		return FALSE;
	if (m_aSamples[nSample].nSize == 0 || m_aSamples[nSample].nSize > gPlayerTalkDataSize)
		return FALSE;
	if (!ReadRawBytes(m_aSamples[nSample].nOffset, m_aSamples[nSample].nSize, gPlayerTalkData))
		return FALSE;
	/* gPlayerTalkData is read by the mixer directly; nothing to bind. */
	gPlayerTalkSfx = nSample;
	return TRUE;
}
#endif

uint8
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	if (nComment >= TOTAL_AUDIO_SAMPLES)
		return LOADING_STATUS_NOT_LOADED;
	{
		int32 i;
		for (i = 0; i < 3; i++) {
			int32 slot = (int32)nCurrentPedSlot - i - 1;
			if (slot < 0)
				slot += MAX_PEDSFX;
			if (nComment == (uint32)nPedSlotSfx[slot])
				return LOADING_STATUS_LOADED;
		}
	}
	return LOADING_STATUS_NOT_LOADED;
}

int32
cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	int32 i;
	for (i = 0; i < 3; i++) {
		int32 slot = (int32)nCurrentPedSlot - i - 1;
		if (slot < 0)
			slot += MAX_PEDSFX;
		if (nComment == (uint32)nPedSlotSfx[slot])
			return slot;
	}
	return -1;
}

bool8
cSampleManager::LoadPedComment(uint32 nComment)
{
	if (nComment >= TOTAL_AUDIO_SAMPLES)
		return FALSE;
	uint32 slot;
	// No ped talking while the game code is paused or during cutscenes (same
	// contract as the OAL backend); avoids a mid-cutscene disk read.
	if (CTimer::GetIsCodePaused())
		return FALSE;
	if (MusicManager.IsInitialised() && MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE)
		return FALSE;
	if (ahiSilent || pedArena == NULL)
		return FALSE;
	if (m_aSamples[nComment].nSize == 0 || m_aSamples[nComment].nSize > pedBlockSize)
		return FALSE;

	slot = nCurrentPedSlot;
	if (!ReadRawBytes(m_aSamples[nComment].nOffset, m_aSamples[nComment].nSize,
	                  (uint8 *)pedArena + (size_t)pedBlockSize * slot))
		return FALSE;

	/* ped-comment data lives in pedArena; nothing to bind (mixer reads it). */
	nPedSlotSfx[slot] = (int32)nComment;
	nCurrentPedSlot++;
	if (nCurrentPedSlot >= MAX_PEDSFX)
		nCurrentPedSlot = 0;
	return TRUE;
}

int32
cSampleManager::GetBankContainingSound(uint32 offset)
{
	if (offset >= BankStartOffset[SFX_BANK_PED_COMMENTS])
		return SFX_BANK_PED_COMMENTS;
	if (offset >= BankStartOffset[SFX_BANK_0])
		return SFX_BANK_0;
	return INVALID_SFX_BANK;
}

uint32 cSampleManager::GetSampleBaseFrequency(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return 0;
	return m_aSamples[nSample].nFrequency;
}
uint32 cSampleManager::GetSampleLoopStartOffset(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return 0;
	return m_aSamples[nSample].nLoopStart;
}
int32 cSampleManager::GetSampleLoopEndOffset(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return 0;
	return m_aSamples[nSample].nLoopEnd;
}
uint32 cSampleManager::GetSampleLength(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES)
		return 0;
	return m_aSamples[nSample].nSize / sizeof(uint16);
}

bool8 cSampleManager::UpdateReverb(void) { return FALSE; }
void  cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag) {}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------
bool8
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	if (nChannel >= NUM_CHANNELS)
		return FALSE;
	if (ahiSilent)
		return FALSE;
	if (nSfx >= TOTAL_AUDIO_SAMPLES)
		return FALSE;

	// Stop a live voice first: `used` may already be false while a finished
	// one-shot is still published to the mixer (`active` TRUE). StopChannel
	// bumps the epoch, so the mixer drops the stale voice on its next block.
	if (ahiCh[nChannel].used || ahiCh[nChannel].active)
		StopChannel(nChannel);

	// Disarm first: a failure below must not leave a stale `slot` that a later
	// StopChannel would unpin (that could unpin a block another channel owns).
	ahiCh[nChannel].sfx = -1;
	ahiCh[nChannel].slot = 0;
	ahiCh[nChannel].used = FALSE;
	ahiCh[nChannel].looping = FALSE;
	ahiCh[nChannel].is2d = FALSE;
	ahiCh[nChannel].freq = m_aSamples[nSfx].nFrequency;
	ahiCh[nChannel].frames = m_aSamples[nSfx].nSize / sizeof(uint16);
	ahiCh[nChannel].loopStartFrames = 0;
	ahiCh[nChannel].loopLenFrames = 0;

	{
		uint32 slot = 0;
		if (nSfx < SAMPLEBANK_MAX) {
			if (!IsSampleBankLoaded(nBank))
				return FALSE;
			if (!ResidentSample((int32)nSfx, m_aSamples[nSfx].nOffset,
			                    m_aSamples[nSfx].nSize, &slot))
				return FALSE;
		}
#ifdef FIX_BUGS
		else if (nSfx >= PLAYER_COMMENTS_START && nSfx <= PLAYER_COMMENTS_END) {
			if (!IsMissionAudioLoaded(MISSION_AUDIO_PLAYER_COMMENT, nSfx))
				return FALSE;
			slot = AHI_MISSION_SLOT;
			ahiCh[nChannel].is2d = TRUE;
		}
#endif
		else {
			int32 pslot = _GetPedCommentSlot(nSfx);
			if (pslot < 0)
				return FALSE;
			slot = (uint32)pslot;
		}

		ahiCh[nChannel].sfx = (int32)nSfx;
		ahiCh[nChannel].slot = slot;

		// The mixer reads the sample frames directly from the cache block /
		// arena the slot refers to (no AHI sound-slot binding any more).
		if (slot >= (uint32)AHI_SFX_SLOT_BASE)
			ahiCh[nChannel].data =
				(const int16 *)ahiBlocks[slot - (uint32)AHI_SFX_SLOT_BASE].data;
#ifdef FIX_BUGS
		else if (slot == (uint32)AHI_MISSION_SLOT)
			ahiCh[nChannel].data = (const int16 *)gPlayerTalkData;
#endif
		else
			ahiCh[nChannel].data =
				(const int16 *)((const uint8 *)pedArena + (size_t)pedBlockSize * slot);
	}
	return TRUE;
}

#ifdef EXTERNAL_3D_SOUND
void
cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	if (nChannel >= MAXCHANNELS)
		return;
	if (nVolume > MAX_VOLUME)
		nVolume = MAX_VOLUME;
	// Duck SFX during cutscenes so dialogue/music stays clear (matches OAL and
	// Miles); the finale track mutes them entirely.
	if (MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE) {
		if (MusicManager.GetCurrentTrack() == STREAMED_SOUND_CUTSCENE_FINALE)
			nVolume = 0;
		else
			nVolume >>= 2;
	}
	ahiCh[nChannel].emittingVol = (uint8)nVolume;
}

void
cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	if (nChannel >= MAXCHANNELS)
		return;
	ahiCh[nChannel].x = fX;
	ahiCh[nChannel].y = fY;
	ahiCh[nChannel].z = fZ;
}

void
cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	if (nChannel >= MAXCHANNELS)
		return;
	ahiCh[nChannel].maxDist = fMax;
	(void)fMin;   // min-distance rolloff is not modelled (see docs/plan)
}
#endif

void
cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	if (nChannel < MAXCHANNELS || nChannel >= NUM_CHANNELS)
		return;
	if (nVolume > MAX_VOLUME)
		nVolume = MAX_VOLUME;
	// Duck during cutscenes (matches OAL; the finale track mutes entirely).
	if (MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE) {
		if (MusicManager.GetCurrentTrack() == STREAMED_SOUND_CUTSCENE_FINALE)
			nVolume = 0;
		else
			nVolume >>= 2;
	}
	ahiCh[nChannel].emittingVol = (uint8)nVolume;
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	if (nChannel < MAXCHANNELS || nChannel >= NUM_CHANNELS)
		return;
	if (nPan > MAX_VOLUME)
		nPan = MAX_VOLUME;
	ahiCh[nChannel].pan = (uint8)nPan;
}

void
cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	if (nChannel >= NUM_CHANNELS)
		return;
	ahiCh[nChannel].freq = nFreq;
}

void
cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	if (nChannel >= NUM_CHANNELS)
		return;
	if (nLoopEnd > (int32)nLoopStart && nLoopEnd > 0) {
		ahiCh[nChannel].loopStartFrames = nLoopStart / (DIGITALBITS / 8);
		ahiCh[nChannel].loopLenFrames = ((uint32)nLoopEnd - nLoopStart) / (DIGITALBITS / 8);
	} else {
		ahiCh[nChannel].loopStartFrames = 0;
		ahiCh[nChannel].loopLenFrames = 0;
	}
}

void
cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	if (nChannel >= NUM_CHANNELS)
		return;
	// 1 = one-shot; 0 = loop forever; >1 = finite (the engine stops it).
	ahiCh[nChannel].looping = (nLoopCount != 1) ? TRUE : FALSE;
}

bool8
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	if (nChannel >= NUM_CHANNELS)
		return FALSE;
	if (ahiSilent)
		return FALSE;
	if (ahiCh[nChannel].used && ahiDoneEpoch[nChannel] == ahiCh[nChannel].epoch) {
		// One-shot fully played. Retire `used` so the engine frees the slot,
		// and `active` so the mixer stops snapshotting a dead voice (the old
		// mixer cleared `active` itself; now only the game thread may write it).
		ahiCh[nChannel].used = FALSE;
		ahiCh[nChannel].active = FALSE;
	}
	return ahiCh[nChannel].used;
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	if (nChannel >= NUM_CHANNELS)
		return;
	if (ahiSilent || ahiCtrl == NULL || ahiBus[AHI_BUS_SFX].ring == NULL)
		return;
	if (ahiCh[nChannel].data == NULL || ahiCh[nChannel].freq == 0)
		return;

	// Normalise the loop region: no/invalid points => loop the whole sample
	// (matches the previous AHI backend's fallback).
	if (ahiCh[nChannel].looping) {
		if (ahiCh[nChannel].loopLenFrames == 0 ||
		    ahiCh[nChannel].loopStartFrames + ahiCh[nChannel].loopLenFrames >
		        ahiCh[nChannel].frames) {
			ahiCh[nChannel].loopStartFrames = 0;
			ahiCh[nChannel].loopLenFrames = ahiCh[nChannel].frames;
		}
	}
	ahiCh[nChannel].loopEnd =
		ahiCh[nChannel].loopStartFrames + ahiCh[nChannel].loopLenFrames;

	// Do NOT reset the mixer cursors here: the epoch bump below makes the
	// mixer reset idx/frac/done on its next block.
	AhiVoiceStep(nChannel);
	AhiVoiceGains(nChannel, m_nEffectsFadeVolume, m_nEffectsVolume);
	ahiCh[nChannel].startMs = CTimer::GetTimeInMilliseconds();

	ahiStarts++;
	if (ahiStarts <= 4 || (ahiStarts % 256) == 0)
		debug("[boot] ahi: start %lu ch=%d sfx=%d loop=%d ev=%lu md=%lu fm=%u/%u gain=%lu freq=%lu step=%lu.%lu\n",
		      (unsigned long)ahiStarts, (int)nChannel, (int)ahiCh[nChannel].sfx,
		      (int)ahiCh[nChannel].looping,
		      (unsigned long)ahiCh[nChannel].emittingVol,
		      (unsigned long)(uint32)ahiCh[nChannel].maxDist,
		      (unsigned)m_nEffectsFadeVolume, (unsigned)m_nEffectsVolume,
		      (unsigned long)ahiCh[nChannel].gainL,
		      (unsigned long)ahiCh[nChannel].freq,
		      (unsigned long)ahiCh[nChannel].stepW,
		      (unsigned long)ahiCh[nChannel].stepF);

	// Publish atomically: every field the mixer reads is final, and the
	// epoch bump tells it to restart this voice from frame 0.
	Forbid();
	ahiCh[nChannel].active = TRUE;
	ahiCh[nChannel].used = TRUE;
	ahiCh[nChannel].epoch++;
	Permit();
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	if (nChannel >= NUM_CHANNELS)
		return;
	if (ahiSilent)
		return;
	AHITRACE("[boot] ahi:t stop ch=%d sfx=%d used=%d\n",
	         (int)nChannel, (int)ahiCh[nChannel].sfx, (int)ahiCh[nChannel].used);
	ahiCh[nChannel].active = FALSE;
	ahiCh[nChannel].used = FALSE;
	ahiCh[nChannel].epoch++;
	if (ahiCh[nChannel].sfx >= 0 && ahiCh[nChannel].slot >= AHI_SFX_SLOT_BASE)
		UnpinSlot(ahiCh[nChannel].slot);
	ahiCh[nChannel].sfx = -1;
	ahiCh[nChannel].data = NULL;
	ahiStops++;
}

// ---------------------------------------------------------------------------
// Streamed audio (bus 1): radio / ambience / cutscene / mission speech.
//
// The .adf radio files are MPEG-1 Layer III with every byte XOR-ed with 0x22
// (the "Rockstar header" is really whole-file obfuscation); .mp3 files are
// plain. Both are decoded by mpega.library (LIBS:mpega.library, installed like
// ahi.device) through a per-stream bitstream-access hook that undoes the XOR.
// Decoded source-rate stereo lands in a per-stream FIFO; the bus-1 mixer
// resamples that FIFO to ahiMixFreq. All media is CBR, so length comes from the
// first frame header (kbps*125 bytes/s) without a full decode/scan.
// ---------------------------------------------------------------------------

// MPEG audio frame header (CBR assumption: constant bitrate/rate). Only the
// bitrate is needed, for the file-size length estimate.
struct tMp3Frame {
	uint32 kbps;
};

static bool8
StreamIsAdf(const char *name)
{
	size_t l = strlen(name);
	return l >= 4 && name[l - 4] == '.' &&
	       (name[l - 3] == 'A' || name[l - 3] == 'a') &&
	       (name[l - 2] == 'D' || name[l - 2] == 'd') &&
	       (name[l - 1] == 'F' || name[l - 1] == 'f');
}

// Parse one MPEG-1/2/2.5 Layer-III header. Returns FALSE if not a valid header.
static bool8
StreamParseHdr(const uint8 *p, tMp3Frame *out)
{
	static const uint32 sr_mpeg1[3] = { 44100, 48000, 32000 };
	static const uint32 sr_mpeg2[3] = { 22050, 24000, 16000 };
	static const uint32 sr_mpeg25[3] = { 11025, 12000, 8000 };
	static const uint32 br_v1l3[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
	static const uint32 br_v2l3[16] = { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0 };
	uint32 ver, layer, br, sri, pad, rate, kbps, samples, fb;

	if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
		return FALSE;
	ver = (p[1] >> 3) & 3;
	layer = (p[1] >> 1) & 3;
	if (ver == 1 || layer != 1)              // reserved version / not Layer III
		return FALSE;
	br = (p[2] >> 4) & 0xF;
	sri = (p[2] >> 2) & 3;
	pad = (p[2] >> 1) & 1;
	if (br == 0 || br == 15 || sri == 3)
		return FALSE;

	if (ver == 3) { rate = sr_mpeg1[sri]; kbps = br_v1l3[br]; samples = 1152; }
	else if (ver == 2) { rate = sr_mpeg2[sri]; kbps = br_v2l3[br]; samples = 576; }
	else { rate = sr_mpeg25[sri]; kbps = br_v2l3[br]; samples = 576; }
	if (rate == 0 || kbps == 0)
		return FALSE;

	fb = (samples / 8) * kbps * 1000 / rate + pad;
	if (fb < 8)
		return FALSE;
	out->kbps = kbps;
	return TRUE;
}

// Find the first valid frame in a buffer.
static bool8
StreamFindFrame(const uint8 *p, uint32 n, tMp3Frame *out)
{
	uint32 i;
	for (i = 0; i + 4 <= n; i++) {
		tMp3Frame f;
		if (!StreamParseHdr(p + i, &f))
			continue;
		out->kbps = f.kbps;
		return TRUE;
	}
	return FALSE;
}

// ---- mpega.library bitstream access hook ----------------------------------
// mpega.library calls this with (a0=hook, a2=handle, a1=MPEGA_ACCESS*). The
// handle is whatever OPEN returned; we hand back a FILE* and keep it in the
// stream. READ de-obfuscates the .adf 0x22 XOR so the decoder always sees a
// clean MP3 bitstream.
static ULONG
MpegaBsFunc(register struct Hook *hook __asm("a0"),
            register APTR handle __asm("a2"),
            register MPEGA_ACCESS *acc __asm("a1"))
{
	tAhiStream *s = (tAhiStream *)hook->h_Data;

	switch (acc->func) {
	case MPEGA_BSFUNC_OPEN: {
		FILE *fp = fcaseopen(acc->data.open.stream_name, "rb");
		if (fp == NULL)
			return 0;
		fseek(fp, 0, SEEK_END);
		acc->data.open.stream_size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		s->fp = fp;
		return (ULONG)fp;
	}
	case MPEGA_BSFUNC_CLOSE:
		if (handle != NULL) {
			fclose((FILE *)handle);
			if (s != NULL && (FILE *)handle == s->fp)
				s->fp = NULL;
		}
		return 0;
	case MPEGA_BSFUNC_READ: {
		uint32 got = (uint32)fread(acc->data.read.buffer, 1,
		                           (size_t)acc->data.read.num_bytes, (FILE *)handle);
		if (got > 0 && s != NULL && s->xorByte) {
			uint8 *p = (uint8 *)acc->data.read.buffer;
			uint32 i;
			for (i = 0; i < got; i++)
				p[i] ^= (uint8)s->xorByte;
		}
		return got;
	}
	case MPEGA_BSFUNC_SEEK:
		if (handle != NULL &&
		    fseek((FILE *)handle, (long)acc->data.seek.abs_byte_seek_pos, SEEK_SET) == 0)
			return 0;
		return 1;
	}
	return 0;
}

// ---- RIFF/WAVE bitstream --------------------------------------------------
// All header fields are little-endian, so they are decoded byte-wise (68k is
// big-endian). Only the formats the game ships are accepted: 16-bit PCM and
// IMA ADPCM (matched against sampman_oal.cpp's CWavFile).
static uint16
StreamRdLE16(const uint8 *p)
{
	return (uint16)(p[0] | (p[1] << 8));
}

static uint32
StreamRdLE32(const uint8 *p)
{
	return (uint32)p[0] | ((uint32)p[1] << 8) |
	       ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static bool8
StreamIsWav(const char *name)
{
	size_t l = strlen(name);
	return l >= 4 && name[l - 4] == '.' &&
	       (name[l - 3] == 'W' || name[l - 3] == 'w') &&
	       (name[l - 2] == 'A' || name[l - 2] == 'a') &&
	       (name[l - 1] == 'V' || name[l - 1] == 'v');
}

// Parse fmt/data and leave `fp` at the first sample. Returns FALSE on anything
// other than mono/stereo 16-bit PCM or IMA ADPCM.
static bool8
StreamWavParse(struct tAhiWav *w, FILE *fp)
{
	uint8 hdr[16];
	uint16 afmt = 0, ch = 0, align = 0, bits = 0;
	uint32 rate = 0, dataOff = 0, dataSize = 0;
	bool8 fmtSeen = FALSE;

	if (fread(hdr, 1, 12, fp) != 12 ||
	    memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
		return FALSE;

	while (fread(hdr, 1, 8, fp) == 8) {
		uint32 sz = StreamRdLE32(hdr + 4);
		if (sz > 0x7FFFFFFF)          // fseek takes a signed offset
			return FALSE;
		if (memcmp(hdr, "fmt ", 4) == 0) {
			uint8 f[16];
			if (sz < 16 || fread(f, 1, 16, fp) != 16)
				return FALSE;
			afmt  = StreamRdLE16(f);
			ch    = StreamRdLE16(f + 2);
			rate  = StreamRdLE32(f + 4);
			align = StreamRdLE16(f + 12);
			bits  = StreamRdLE16(f + 14);
			fmtSeen = TRUE;
			if (sz > 16)
				fseek(fp, sz - 16, SEEK_CUR);
		} else if (memcmp(hdr, "data", 4) == 0) {
			dataOff = (uint32)ftell(fp);
			dataSize = sz;
			break;
		} else {
			fseek(fp, sz, SEEK_CUR);
		}
		if (sz & 1)
			fseek(fp, 1, SEEK_CUR);
	}

	if (!fmtSeen || dataOff == 0 || dataSize == 0 || ch < 1 || ch > 2 ||
	    rate == 0 || align == 0 || align > STREAM_WAV_MAX_ALIGN)
		return FALSE;

	w->fmt = afmt;
	w->channels = ch;
	w->rate = rate;
	w->blockAlign = align;
	w->dataOff = dataOff;
	w->dataSize = dataSize;
	w->dataLeft = dataSize;

	if (afmt == STREAM_WAV_FMT_PCM) {
		if (bits != 16 || align != ch * 2)
			return FALSE;
		w->blockFrames = STREAM_WAV_PCM_FRAMES;
	} else if (afmt == STREAM_WAV_FMT_IMA) {
		// (blockAlign/ch - 4) nibble pairs must divide into whole 8-sample
		// groups, or the block decoder would overrun the scratch buffers.
		if (align / ch < 4 || (align / ch - 4) % 4 != 0)
			return FALSE;
		w->blockFrames = (align / ch - 4) * 2 + 1;
	} else {
		return FALSE;
	}

	fseek(fp, dataOff, SEEK_SET);
	return TRUE;
}

static const uint16 StreamImaStep[89] = {
	7, 8, 9, 10, 11, 12, 13, 14,
	16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411,
	1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};

static int16
StreamImaSample(struct tAhiWav *w, uint32 ch, uint8 nib)
{
	int32 step = StreamImaStep[w->adpStep[ch]];
	int32 idx = w->adpStep[ch];
	int32 delta = step >> 3;
	int32 v;

	if (nib & 1) delta += step >> 2;
	if (nib & 2) delta += step >> 1;
	if (nib & 4) delta += step;
	if (nib & 8) delta = -delta;

	v = w->adpSample[ch] + delta;
	if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
	w->adpSample[ch] = (int16)v;

	if (nib & 4) idx += ((nib & 3) + 1) * 2; else idx--;
	if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
	w->adpStep[ch] = (int16)idx;
	return (int16)v;
}

// One IMA block: a 4-byte header per channel then 4-byte groups per channel,
// each group decoding to 8 samples (low nibble first).
static void
StreamWavDecodeIma(struct tAhiWav *w)
{
	const uint8 *p = w->raw;
	int16 *out[2];
	uint32 i, s;

	out[0] = w->pcm;
	out[1] = w->pcm + w->blockFrames;
	for (i = 0; i < w->channels; i++) {
		w->adpSample[i] = (int16)StreamRdLE16(p);
		w->adpStep[i] = (int16)(p[2] > 88 ? 88 : p[2]);
		p += 4;
		*out[i]++ = w->adpSample[i];
	}
	for (s = 1; s < w->blockFrames; s += 8) {
		for (i = 0; i < w->channels; i++) {
			uint32 k;
			for (k = 0; k < 4; k++) {
				*out[i]++ = StreamImaSample(w, i, p[k] & 0xF);
				*out[i]++ = StreamImaSample(w, i, p[k] >> 4);
			}
			p += 4;
		}
	}
}

static void
StreamPushPcm(tAhiStream *s, const WORD *left, const WORD *right,
              uint32 frames, uint32 ch)
{
	uint32 i;
	if (frames > STREAM_FIFO_FRAMES - s->fifoCount)
		frames = STREAM_FIFO_FRAMES - s->fifoCount;
	for (i = 0; i < frames; i++) {
		int16 l = (int16)left[i];
		int16 r = (ch > 1) ? (int16)right[i] : l;
		uint32 idx = (s->fifoHead + s->fifoCount) & STREAM_FIFO_MASK;
		s->fifo[idx * 2 + 0] = l;
		s->fifo[idx * 2 + 1] = r;
		s->fifoCount++;
	}
}

// Read and decode one WAV block; returns the frames produced, or 0 at EOF.
static uint32
StreamWavBlock(tAhiStream *s)
{
	struct tAhiWav *w = &s->wavInfo;
	uint32 want, got;

	if (w->dataLeft == 0)
		return 0;
	want = (w->fmt == STREAM_WAV_FMT_IMA) ? w->blockAlign
	                                      : w->blockFrames * w->blockAlign;
	if (want > w->dataLeft)
		want = w->dataLeft;
	got = (uint32)fread(w->raw, 1, want, s->fp);
	if (got < want)
		got -= got % w->blockAlign;       // drop a truncated tail frame
	if (got == 0)
		return 0;
	w->dataLeft -= got;

	if (w->fmt == STREAM_WAV_FMT_IMA) {
		StreamWavDecodeIma(w);
		return w->blockFrames;
	}

	{
		uint32 frames = got / w->blockAlign;
		uint32 f, c;
		for (f = 0; f < frames; f++)
			for (c = 0; c < w->channels; c++)
				w->pcm[c * w->blockFrames + f] =
				    (int16)StreamRdLE16(w->raw + (f * w->channels + c) * 2);
		return frames;
	}
}

static bool8
StreamDecodeWav(tAhiStream *s, uint32 maxFrames)
{
	struct tAhiWav *w = &s->wavInfo;
	uint32 decoded = 0;

	while (decoded < maxFrames) {
		uint32 frames;
		if (STREAM_FIFO_FRAMES - s->fifoCount < w->blockFrames)
			break;                    // let the mixer drain the FIFO
		frames = StreamWavBlock(s);
		if (frames == 0)
			return FALSE;
		StreamPushPcm(s, (const WORD *)w->pcm,
		              (const WORD *)(w->pcm + w->blockFrames),
		              frames, w->channels);
		decoded += frames;
	}
	return TRUE;
}

// Decode up to `maxFrames` source frames into the FIFO. Returns FALSE at EOF.
static bool8
StreamDecodeSome(tAhiStream *s, uint32 maxFrames)
{
	WORD buf[2][MPEGA_PCM_SIZE];
	WORD *pcm[2];
	uint32 decoded = 0;
	int guard = 0;

	if (s->wav)
		return StreamDecodeWav(s, maxFrames);
	if (s->mpds == NULL)
		return FALSE;
	pcm[0] = buf[0];
	pcm[1] = buf[1];

	while (decoded < maxFrames && s->fifoCount < STREAM_FIFO_FRAMES) {
		LONG n = MPEGA_decode_frame(s->mpds, pcm);

		if (n == MPEGA_ERR_EOF || n == MPEGA_ERR_MEM)
			return FALSE;
		if (n == MPEGA_ERR_BADFRAME || n == MPEGA_ERR_NO_SYNC) {
			if (++guard > 8)
				return FALSE;
			continue;
		}
		if (n <= 0) {                 // skipped frame (CRC / not enough data)
			if (++guard > 256)
				return FALSE;
			continue;
		}
		guard = 0;
		s->srcRate = (uint32)(s->mpds->dec_frequency ? s->mpds->dec_frequency
		                                              : s->mpds->frequency);
		s->srcCh = (uint32)(s->mpds->dec_channels ? s->mpds->dec_channels
		                                          : s->mpds->channels);
		if (s->srcCh < 1) s->srcCh = 1;
		if (s->srcCh > 2) s->srcCh = 2;
		StreamPushPcm(s, pcm[0], pcm[1], (uint32)n, s->srcCh);
		decoded += (uint32)n;
	}
	return TRUE;
}

// Max source frames to decode in one call: enough for STREAM_DECODE_SLICE_MS of
// PCM at the stream's rate, but at least one MPEG frame. Bounds the per-tick
// burst on the main thread.
static uint32
StreamDecodeSlice(const tAhiStream *s)
{
	uint32 rate = s->srcRate ? s->srcRate : 22050;
	uint32 slice = (STREAM_DECODE_SLICE_MS * rate) / 1000;
	if (slice < 512)
		slice = 512;
	return slice;
}

// Position a WAV stream at `ms`, block-aligned for IMA (decoder state lives
// only at block starts).
static void
StreamWavSeek(tAhiStream *s, uint32 ms)
{
	struct tAhiWav *w = &s->wavInfo;
	uint64 frame = ((uint64)ms * w->rate) / 1000;
	uint32 bytes;

	if (w->fmt == STREAM_WAV_FMT_IMA) {
		// One whole block holds blockFrames frames, so seek by block, not by
		// frame (frac offset would be a byte offset into a block's samples).
		uint32 blocks = (uint32)(frame / w->blockFrames);
		uint32 total = w->dataSize / w->blockAlign;
		if (blocks > total)
			blocks = total;
		bytes = blocks * w->blockAlign;
	} else {
		if (frame > w->dataSize / w->blockAlign)
			frame = w->dataSize / w->blockAlign;
		bytes = (uint32)frame * w->blockAlign;
	}
	fseek(s->fp, w->dataOff + bytes, SEEK_SET);
	w->dataLeft = w->dataSize - bytes;
}

// Seek via the library (resets the decoded FIFO).
static void
StreamSeek(tAhiStream *s, uint32 ms)
{
	if (s->wav) {
		StreamWavSeek(s, ms);
	} else {
		LONG r;
		if (s->mpds == NULL)
			return;
		r = MPEGA_seek(s->mpds, ms);
		if (r != 0)
			debug("[boot] ahi:s seek %lums -> err %ld\n", (unsigned long)ms, (long)r);
	}
	s->fifoHead = s->fifoCount = 0;
	s->mixFrac = 0;
	s->startMs = ms;
	s->consumed = 0;
}

// Rewind to the start for looping, keeping the decoded FIFO intact.
static void
StreamRewind(tAhiStream *s)
{
	if (s->wav) {
		StreamWavSeek(s, 0);
	} else {
		if (s->mpds == NULL)
			return;
		MPEGA_seek(s->mpds, 0);
	}
	s->startMs = 0;
	s->consumed = 0;
}

static void
StreamClose(tAhiStream *s)
{
	if (s->mpds != NULL) {
		MPEGA_close(s->mpds);         // calls our CLOSE hook -> fclose
		s->mpds = NULL;
	}
	if (s->wavInfo.raw != NULL) {
		FreeVec(s->wavInfo.raw);
		s->wavInfo.raw = NULL;
	}
	if (s->wavInfo.pcm != NULL) {
		FreeVec(s->wavInfo.pcm);
		s->wavInfo.pcm = NULL;
	}
	s->wav = FALSE;
	if (s->fp != NULL) {              // safety net if CLOSE was not called
		fclose(s->fp);
		s->fp = NULL;
	}
	if (s->fifo != NULL) {
		FreeVec(s->fifo);
		s->fifo = NULL;
	}
	s->state = ST_STOPPED;
	s->fifoHead = s->fifoCount = 0;
	s->mixFrac = 0;
}

// Open `track` and position at `posMs`. Leaves the stream in ST_PRELOADED.
static bool8
StreamOpenAt(tAhiStream *s, tTrack track, uint32 posMs)
{
	const char *name;

	StreamClose(s);

	if (track >= TOTAL_STREAMED_SOUNDS || !ahiStreamOn)
		return FALSE;
	name = StreamedNameTable[track];
	if (!StreamIsWav(name) && MPEGABase == NULL)
		return FALSE;

	s->fifo = (int16 *)AllocVec(STREAM_FIFO_FRAMES * 2 * (ULONG)sizeof(int16),
	                            MEMF_PUBLIC | MEMF_CLEAR);
	if (s->fifo == NULL)
		return FALSE;

	s->xorByte = StreamIsAdf(name) ? STREAM_ADF_KEY : 0;
	// NB: do NOT reset s->loop here. MusicManager sets the loop flag
	// (SetStreamedFileLoopFlag) immediately before StartStreamedFile, and the
	// reference backends keep loop state separate from open; clearing it here
	// would stop radio/ambience from looping. It is initialised once per slot.
	s->vol = MAX_VOLUME;
	s->pan = AHI_PAN_CENTRE;
	s->effect = FALSE;
	s->fifoHead = s->fifoCount = 0;
	s->mixFrac = 0;
	s->consumed = 0;
	s->startMs = 0;

	if (StreamIsWav(name)) {
		uint32 rawSize;

		s->fp = fcaseopen(name, "rb");
		if (s->fp == NULL || !StreamWavParse(&s->wavInfo, s->fp)) {
			debug("[boot] ahi:s open FAIL '%s' (wav)\n", name);
			StreamClose(s);
			return FALSE;
		}
		rawSize = (s->wavInfo.fmt == STREAM_WAV_FMT_IMA)
		    ? s->wavInfo.blockAlign
		    : s->wavInfo.blockFrames * s->wavInfo.blockAlign;
		s->wavInfo.raw = (uint8 *)AllocVec(rawSize, MEMF_PUBLIC);
		s->wavInfo.pcm = (int16 *)AllocVec(
		    s->wavInfo.blockFrames * 2 * (ULONG)sizeof(int16), MEMF_PUBLIC);
		if (s->wavInfo.raw == NULL || s->wavInfo.pcm == NULL) {
			StreamClose(s);
			return FALSE;
		}
		s->wav = TRUE;
		s->srcRate = s->wavInfo.rate;
		s->srcCh = s->wavInfo.channels;
		s->state = ST_PRELOADED;
		if (posMs > 0)
			StreamSeek(s, posMs);
		else
			StreamDecodeSome(s, StreamDecodeSlice(s));
		debug("[boot] ahi:s open '%s' wav fmt=%lx ch=%lu rate=%lu align=%lu size=%lu pos=%lums\n",
		      name, (unsigned long)s->wavInfo.fmt, (unsigned long)s->srcCh,
		      (unsigned long)s->srcRate, (unsigned long)s->wavInfo.blockAlign,
		      (unsigned long)s->wavInfo.dataSize, (unsigned long)posMs);
		return TRUE;
	}

	memset(&s->ctrl, 0, sizeof(s->ctrl));
	s->ctrl.bs_access = &s->bsHook;
	s->ctrl.check_mpeg = 1;
	s->ctrl.stream_buffer_size = STREAM_BITBUF;
	s->ctrl.layer_1_2.mono.freq_div = (WORD)ahiMusicDiv;
	s->ctrl.layer_1_2.stereo.freq_div = (WORD)ahiMusicDiv;
	s->ctrl.layer_3.mono.freq_div = (WORD)ahiMusicDiv;
	s->ctrl.layer_3.stereo.freq_div = (WORD)ahiMusicDiv;
	s->ctrl.layer_1_2.mono.quality = (WORD)ahiMusicQuality;
	s->ctrl.layer_1_2.stereo.quality = (WORD)ahiMusicQuality;
	s->ctrl.layer_3.mono.quality = (WORD)ahiMusicQuality;
	s->ctrl.layer_3.stereo.quality = (WORD)ahiMusicQuality;
	s->bsHook.h_Entry = (ULONG (*)())MpegaBsFunc;
	s->bsHook.h_Data = s;

	s->mpds = MPEGA_open((char *)name, &s->ctrl);
	if (s->mpds == NULL) {
		debug("[boot] ahi:s open FAIL '%s' (xor=%lu)\n",
		      name, (unsigned long)s->xorByte);
		StreamClose(s);
		return FALSE;
	}

	s->srcRate = (uint32)(s->mpds->dec_frequency ? s->mpds->dec_frequency
	                                             : s->mpds->frequency);
	s->srcCh = (uint32)(s->mpds->dec_channels ? s->mpds->dec_channels
	                                          : s->mpds->channels);
	if (s->srcCh < 1) s->srcCh = 1;
	if (s->srcCh > 2) s->srcCh = 2;
	s->state = ST_PRELOADED;

	if (posMs > 0)
		StreamSeek(s, posMs);
	else
		StreamDecodeSome(s, StreamDecodeSlice(s));

	debug("[boot] ahi:s open '%s' norm=%d layer=%d rate=%lu ch=%lu xor=%lu dur=%ldms pos=%lums\n",
	      name, (int)s->mpds->norm, (int)s->mpds->layer,
	      (unsigned long)s->srcRate, (unsigned long)s->srcCh,
	      (unsigned long)s->xorByte, (long)s->mpds->ms_duration,
	      (unsigned long)posMs);
	return TRUE;
}

// Keep the FIFO topped up; handle EOF (loop or drain).
static void
StreamPump(tAhiStream *s)
{
	uint32 target;
	if (s->state != ST_PLAYING || (s->mpds == NULL && !s->wav))
		return;
	if (s->fifoCount >= STREAM_FIFO_FRAMES / 2)
		return;
	target = (ahiStreamLeadMs * (s->srcRate ? s->srcRate : 22050)) / 1000;
	if (target < 2048)
		target = 2048;
	if (target > STREAM_FIFO_FRAMES / 2)
		target = STREAM_FIFO_FRAMES / 2;
	if (s->fifoCount >= target)
		return;

	{
		// Decode up to the target fill (not a fixed per-tick slice): when the
		// game loop stalls and Service() is called late, we must catch up or the
		// FIFO starves and audio goes choppy. Bounded by STREAM_DECODE_MAX_FRAMES
		// so a single call cannot run away.
		uint32 deficit = target - s->fifoCount;
		uint32 budget = (deficit < STREAM_DECODE_MAX_FRAMES)
		                ? deficit : STREAM_DECODE_MAX_FRAMES;
		if (!StreamDecodeSome(s, budget)) {
			if (s->loop)
				StreamRewind(s);
			else
				s->state = ST_DRAINING;
		}
	}
}

static void
StreamStop(tAhiStream *s)
{
	StreamClose(s);
}

// Probe a streamed file's length (ms) from its first frame + file size.
static int32
StreamProbeLength(tTrack track)
{
	const char *name;
	FILE *fp;
	uint8 probe[4096];
	uint32 got, xorByte;
	tMp3Frame fh;
	long fsz;

	if (track >= TOTAL_STREAMED_SOUNDS)
		return 1;
	name = StreamedNameTable[track];
	if (StreamIsWav(name)) {
		struct tAhiWav w;
		uint64 samples;
		int32 ms;
		memset(&w, 0, sizeof(w));
		fp = fcaseopen(name, "rb");
		if (fp == NULL)
			return 1;
		if (!StreamWavParse(&w, fp)) {
			fclose(fp);
			return 1;
		}
		fclose(fp);
		samples = (w.fmt == STREAM_WAV_FMT_IMA)
		    ? (uint64)(w.dataSize / w.blockAlign) * w.blockFrames
		    : (uint64)(w.dataSize / w.blockAlign);
		ms = (int32)(samples * 1000 / w.rate);
		return ms > 0 ? ms : 1;
	}
	if (!StreamIsAdf(name)) {
		size_t l = strlen(name);
		char c1, c2;
		// Only .mp3/.adf are stream-decodable; anything else keeps `pos % len`
		// safe with a default.
		if (l < 4)
			return 1;
		c1 = name[l - 2];
		c2 = name[l - 1];
		if (c2 != '3' || (c1 != 'p' && c1 != 'P'))
			return 1;
	}
	fp = fcaseopen(name, "rb");
	if (fp == NULL)
		return 1;
	xorByte = StreamIsAdf(name) ? STREAM_ADF_KEY : 0;
	got = (uint32)fread(probe, 1, sizeof(probe), fp);
	fseek(fp, 0, SEEK_END);
	fsz = ftell(fp);
	fclose(fp);
	if (fsz <= 0)
		return 1;
	if (xorByte) {
		uint32 i;
		for (i = 0; i < got; i++)
			probe[i] ^= (uint8)xorByte;
	}
	if (!StreamFindFrame(probe, got, &fh) || fh.kbps == 0)
		return 1;
	return (int32)(((uint64)fsz * 8) / fh.kbps);
}

// ---------------------------------------------------------------------------
// Stream API
// ---------------------------------------------------------------------------
void cSampleManager::PreloadStreamedFile(tTrack nFile, uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return;
	if (ahiSilent)
		return;
	StreamOpenAt(&ahiStreams[nStream], nFile, 0);
}

void cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return;
	tAhiStream *s = &ahiStreams[nStream];
	if (nPauseFlag) {
		if (s->state == ST_PLAYING)
			s->state = ST_PAUSED;
	} else {
		if (s->state == ST_PAUSED)
			s->state = ST_PLAYING;
	}
}

void cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return;
	if (ahiSilent)
		return;
	tAhiStream *s = &ahiStreams[nStream];
	if (s->state == ST_PRELOADED) {
		StreamSeek(s, 0);
		s->state = ST_PLAYING;
	}
}

bool8
cSampleManager::StartStreamedFile(tTrack nFile, uint32 nPos, uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return FALSE;
	if (ahiSilent || ahiCtrl == NULL || !ahiStreamOn)
		return FALSE;
	debug("[boot] ahi:s start trk=%d '%s' pos=%lums loop=%d\n",
	      (int)nFile,
	      (nFile < TOTAL_STREAMED_SOUNDS) ? StreamedNameTable[nFile] : "?",
	      (unsigned long)nPos, (int)ahiStreams[nStream].loop);
	if (!StreamOpenAt(&ahiStreams[nStream], nFile, 0))
		return FALSE;
	ahiStreams[nStream].state = ST_PLAYING;
	if (nPos > 0)
		StreamSeek(&ahiStreams[nStream], nPos);
	return TRUE;
}

void cSampleManager::StopStreamedFile(uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return;
	StreamStop(&ahiStreams[nStream]);
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return 0;
	tAhiStream *s = &ahiStreams[nStream];
	if (s->fp == NULL || s->srcRate == 0)
		return 0;
	return (int32)(s->startMs +
	               (uint32)(((uint64)s->consumed * 1000) / s->srcRate));
}

void
#ifdef GTA_PS2
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nLRPan, uint8 nFRPan, bool8 nEffectFlag, uint8 nStream)
#else
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, bool8 nEffectFlag, uint8 nStream)
#endif
{
	if (nStream >= MAX_STREAMS)
		return;
	tAhiStream *s = &ahiStreams[nStream];
	if (nVolume > MAX_VOLUME)
		nVolume = MAX_VOLUME;
#ifndef GTA_PS2
	if (nPan > MAX_VOLUME)
		nPan = MAX_VOLUME;
	s->pan = nPan;
#else
	(void)nLRPan; (void)nFRPan;
	s->pan = AHI_PAN_CENTRE;
#endif
	s->vol = nVolume;
	s->effect = nEffectFlag ? TRUE : FALSE;
}

int32
cSampleManager::GetStreamedFileLength(tTrack nStream)
{
	if (nStream >= TOTAL_STREAMED_SOUNDS)
		return 1;                     // keep `pos % len` safe on a stray index
	if (ahiStreamLenKnown[nStream])
		return (int32)ahiStreamLength[nStream];
	ahiStreamLenKnown[nStream] = TRUE;
	ahiStreamLength[nStream] = (uint32)StreamProbeLength(nStream);
	return (int32)ahiStreamLength[nStream];
}

bool8
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return FALSE;
	tAhiStream *s = &ahiStreams[nStream];
	return (s->state == ST_PLAYING || s->state == ST_DRAINING) ? TRUE : FALSE;
}

void
cSampleManager::SetStreamedFileLoopFlag(bool8 nLoopFlag, uint8 nStream)
{
	if (nStream >= MAX_STREAMS)
		return;
	ahiStreams[nStream].loop = nLoopFlag ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// Per-frame service: auto-free finished one-shots, push changed params.
// ---------------------------------------------------------------------------

// Desired AHI volume (0..127, master/fade applied) and pan (0..127) for a
// channel. 2D channels pass their stored values through; 3D channels reuse the
// engine's ComputeVolume/ComputePan (the same functions the PS2 path uses).
static void
ChannelGainPan(uint32 ch, uint8 fadeVol, uint8 masterVol, uint32 *outVol, uint32 *outPan)
{
	uint32 vol, pan;

	if (ch >= MAXCHANNELS || ahiCh[ch].is2d) {
		vol = ahiCh[ch].emittingVol;
		pan = ahiCh[ch].pan;
	} else if (ahiCh[ch].maxDist <= 0.0f) {
		vol = 0;			// no distance set: treat as silent, avoid /0
		pan = AHI_PAN_CENTRE;
	} else {
		CVector pos;
		float dist;
		pos.x = ahiCh[ch].x;
		pos.y = ahiCh[ch].y;
		pos.z = ahiCh[ch].z;
		dist = sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
		vol = AudioManager.ComputeVolume(ahiCh[ch].emittingVol, ahiCh[ch].maxDist, dist);
		pan = (uint32)AudioManager.ComputePan(ahiCh[ch].maxDist, &pos);
	}

	vol = (fadeVol * vol * masterVol) >> 14;
	if (ahiCh[ch].freq == 0)
		vol = 0;
	*outVol = vol;
	*outPan = pan;
}

// ---- per-frame mixers -----------------------------------------------------
// Sum all active SFX voices for one output frame, reading the mixer-owned
// snapshot (mixSnap) and advancing the mixer-owned cursors (mixVoice). The
// game-thread tAhiChannel is never touched here. Same arithmetic as the
// original single-bus mixer. Returns TRUE if any voice produced a sample.
static bool8
AhiMixSfxFrame(int32 *outL, int32 *outR)
{
	uint32 n;
	bool8 got = FALSE;
	int32 l = 0, r = 0;
	// Ring frame this output frame will occupy; a voice that ends here is
	// audible until AHI's read cursor reaches one frame past it.
	uint32 wf = ahiBus[AHI_BUS_SFX].writeFrame;
	uint32 mask = ahiBus[AHI_BUS_SFX].ringMask;

	for (n = 0; n < mixActCount; n++) {
		uint32 ch = mixAct[n];
		tMixSnap *c = &mixSnap[ch];
		tMixVoice *v = &mixVoice[ch];
		uint32 idx;
		int32 s;

		// A Start/Stop on the game thread bumps the epoch; restart the
		// cursor from frame 0 for the new voice instance.
		if (v->epoch != c->epoch) {
			v->epoch = c->epoch;
			v->idx = 0;
			v->frac = 0;
			v->doneAt = 0;
			v->done = FALSE;
		}
		if (v->done)
			continue;

		idx = v->idx;
		if (c->looping) {
			if (idx >= c->loopEnd) {
				idx -= c->loopLenFrames;          // loopLen was validated > 0
				if (idx >= c->loopEnd)            // bad points / step > loop
					idx = c->loopStartFrames;
				v->idx = idx;
			}
		} else if (idx >= c->frames) {
			v->done = TRUE;
			v->doneAt = wf & mask;                // no audio this frame
			continue;
		}
		if (idx >= c->frames) {                    // hard clamp: never read OOB
			v->done = TRUE;
			v->doneAt = wf & mask;
			continue;
		}

		s = (int32)c->data[idx];
		l += (s * (int32)c->gainL) >> 15;
		r += (s * (int32)c->gainR) >> 15;
		got = TRUE;

		// Advance the 16.16 cursor with 32-bit add/carry (no 64-bit math
		// in the per-sample hot loop).
		v->frac += c->stepF;
		v->idx += c->stepW;
		if (v->frac & 0x10000) {
			v->frac &= 0xffff;
			v->idx++;
		}
		if (!c->looping && v->idx >= c->frames) {
			v->done = TRUE;
			v->doneAt = (wf + 1) & mask;          // last audio at wf
		}
	}
	*outL += l;
	*outR += r;
	return got;
}

// Sum all active streams for one output frame, resampling the source-rate FIFO
// to ahiMixFreq (linear, 8-bit fraction). Returns TRUE if any stream produced.
static bool8
AhiMixStreamFrame(int32 *outL, int32 *outR)
{
	int i;
	bool8 got = FALSE;

	for (i = 0; i < MAX_STREAMS; i++) {
		tAhiStream *s = &ahiStreams[i];
		uint32 p0, p1, tot, whole, f8;
		int32 al, ar, bl, br, sl, sr;

		if (s->state != ST_PLAYING && s->state != ST_DRAINING)
			continue;
		if (s->fifoCount < 2) {
			if (s->state == ST_DRAINING)
				s->state = ST_STOPPED;         // drained
			else
				ahiStreamUnderruns++;          // decoder could not keep up
			continue;
		}

		p0 = s->fifoHead & STREAM_FIFO_MASK;
		p1 = (p0 + 1) & STREAM_FIFO_MASK;
		al = s->fifo[p0 * 2 + 0]; ar = s->fifo[p0 * 2 + 1];
		bl = s->fifo[p1 * 2 + 0]; br = s->fifo[p1 * 2 + 1];
		f8 = s->mixFrac >> 8;
		sl = al + (((bl - al) * (int32)f8) >> 8);
		sr = ar + (((br - ar) * (int32)f8) >> 8);

		*outL += (sl * (int32)s->gainL) >> 15;
		*outR += (sr * (int32)s->gainR) >> 15;
		got = TRUE;

		tot = s->mixFrac + s->stepF;
		whole = s->stepW + (tot >> 16);
		s->mixFrac = tot & 0xffff;
		if (whole > 0) {
			if (whole <= s->fifoCount) {
				s->fifoHead = (s->fifoHead + whole) & STREAM_FIFO_MASK;
				s->fifoCount -= whole;
				s->consumed += whole;
			} else {
				s->consumed += s->fifoCount;
				s->fifoCount = 0;
			}
		}
	}
	return got;
}

// Fill `bus`'s ring up to its lead, mixing the requested sources. Applies the
// per-bus pre-gain and saturating clamp. When no source produces, the rest of
// the lead is written as silence (so a stalled ring can never replay stale
// audio -- the menu-"doong" loop).
static void
AhiFillRingCore(tAhiBus *bus, bool8 mixSfx, bool8 mixStream)
{
	uint32 read, ahead;

	read = (uint32)bus->readFrame & bus->ringMask;
	ahead = (bus->writeFrame - read) & bus->ringMask;
	if (ahead > bus->leadFrames) {
		// AHI reached or overtook our write cursor: resync. (a healthy queue
		// is <= leadFrames, so any value above that is an underrun/wrap.)
		bus->writeFrame = read;
		ahead = 0;
		if (bus == &ahiBus[AHI_BUS_SFX])
			ahiMixResyncs++;
	}

	while (ahead < bus->leadFrames) {
		int32 l = 0, r = 0;
		bool8 got = FALSE;
		int16 *out;

		if (mixSfx && AhiMixSfxFrame(&l, &r))
			got = TRUE;
		if (mixStream && AhiMixStreamFrame(&l, &r))
			got = TRUE;

		if (!got) {
			// No source this frame: fill the remaining lead with silence.
			while (ahead < bus->leadFrames) {
				out = &bus->ring[(bus->writeFrame & bus->ringMask) * 2];
				out[0] = 0;
				out[1] = 0;
				bus->writeFrame = (bus->writeFrame + 1) & bus->ringMask;
				ahead++;
			}
			break;
		}

		l *= (int32)bus->pregain;
		r *= (int32)bus->pregain;
		if (l < -32768) l = -32768; else if (l > 32767) l = 32767;
		if (r < -32768) r = -32768; else if (r > 32767) r = 32767;
		out = &bus->ring[(bus->writeFrame & bus->ringMask) * 2];
		out[0] = (int16)l;
		out[1] = (int16)r;
		bus->writeFrame = (bus->writeFrame + 1) & bus->ringMask;
		ahead++;
	}
}

// Bus 0 ring source. `mixStreamToo` is set only for the single-bus fallback
// (REVC_AHI_BUSES=1), where streams share the SFX ring.
static void
AhiFillRingSfx(tAhiBus *bus, bool8 mixStreamToo)
{
	AhiFillRingCore(bus, TRUE, mixStreamToo);
}

// Bus 1 ring source (streams only; game thread).
static void
AhiFillRingStream(tAhiBus *bus)
{
	AhiFillRingCore(bus, FALSE, TRUE);
}

// A stream is "busy" while it is playing, OR while it is draining (even with an
// empty FIFO: the bus-1 mixer must still run to retire ST_DRAINING -> ST_STOPPED,
// otherwise IsStreamPlaying() would stick TRUE and stall MusicManager).
static bool8
AhiStreamBusy(void)
{
	int i;
	for (i = 0; i < MAX_STREAMS; i++) {
		tAhiStream *s = &ahiStreams[i];
		if (s->state == ST_PLAYING || s->state == ST_DRAINING)
			return TRUE;
	}
	return FALSE;
}

// Stream indices that take the effects path without the effects fade (police /
// mission speech = 1, spare = 2). Mirrors sampman_oal.cpp's
// SetStreamedVolumeAndPan so the two backends stay greppably aligned.
static bool8
AhiStreamBypassesFade(int nStream)
{
	return (nStream == 1 || nStream == 2) ? TRUE : FALSE;
}

// Recompute per-stream gains (music vs effects master/fade) + resample step.
static void
AhiStreamGains(uint8 musicVol, uint8 musicFade, uint8 fxVol, uint8 fxFade)
{
	uint32 mix = ahiMixFreq ? ahiMixFreq : AHI_REQ_MIXFREQ_DEFAULT;
	int i;

	for (i = 0; i < MAX_STREAMS; i++) {
		tAhiStream *s = &ahiStreams[i];
		uint32 vol;

		if (s->state != ST_PLAYING && s->state != ST_DRAINING)
			continue;
		if (s->effect) {
			if (AhiStreamBypassesFade(i))
				vol = (128u * s->vol * fxVol) >> 14;
			else
				vol = (fxFade * s->vol * fxVol) >> 14;
		} else {
			vol = (musicFade * s->vol * musicVol) >> 14;
			vol = (vol * ahiMusicGain) >> 7;
		}
		AhiVolPanGains(vol, s->pan, &s->gainL, &s->gainR);
		AhiRateStep(s->srcRate ? s->srcRate : mix, mix, &s->stepW, &s->stepF);
	}
}

static void
AhiPumpStreams(void)
{
	int i;
	for (i = 0; i < MAX_STREAMS; i++)
		StreamPump(&ahiStreams[i]);
}

// Rate-limited (~2 s) stream health line: FIFO fill and underrun count since
// the last line. Underruns > 0 during playback mean the decoder could not keep
// the FIFO fed (raise REVC_AHI_DECODE_LEAD / lower the mix rate / check HW CPU).
static void
AhiStreamHealth(void)
{
	uint32 now = CTimer::GetTimeInMilliseconds();
	int st;
	if (now - ahiStreamLastDump < 2000)
		return;
	ahiStreamLastDump = now;
	for (st = 0; st < MAX_STREAMS; st++) {
		tAhiStream *s = &ahiStreams[st];
		if (s->mpds == NULL && !s->wav)
			continue;
		debug("[boot] ahi:s stream=%d state=%d fill=%lums under=%lu\n",
		      st, (int)s->state,
		      (unsigned long)(s->srcRate ? (s->fifoCount * 1000u) / s->srcRate : 0),
		      (unsigned long)ahiStreamUnderruns);
	}
	ahiStreamUnderruns = 0;
}

// Diagnostic: every ~2 s list the SFX voices still alive (with their age).
// "loop=0" persisting means a one-shot that never ended (mixer bug); "loop=1"
// is an engine loop the game has not stopped. Game thread only: the mixer
// Process must never log.
static void
AhiVoiceDump(void)
{
	uint32 now = CTimer::GetTimeInMilliseconds();
	uint32 k;
	if (now - ahiLastVoiceDump < 2000)
		return;
	ahiLastVoiceDump = now;
	for (k = 0; k < NUM_CHANNELS; k++) {
		tAhiChannel *c = &ahiCh[k];
		if (!c->active)
			continue;
		debug("[boot] ahi: active ch=%lu sfx=%d loop=%d age=%lums gain=%lu\n",
		      (unsigned long)k, (int)c->sfx, (int)c->looping,
		      (unsigned long)(now - c->startMs), (unsigned long)c->gainL);
	}
}

// ---------------------------------------------------------------------------
// SFX bus service (bus 0)
// ---------------------------------------------------------------------------
// Copy the game-thread voice state the mixer is allowed to see. Only field
// copies happen inside Forbid(): no calls, no DOS/AHI/allocation. The game
// thread never calls this; the mixer (task or synchronous fallback) does.
static void
AhiSnapCopy(void)
{
	uint32 i, n = 0;

	Forbid();
	for (i = 0; i < NUM_CHANNELS; i++) {
		tAhiChannel *c = &ahiCh[i];
		tMixSnap *s;
		if (!c->active)
			continue;
		s = &mixSnap[i];
		s->data = c->data;
		s->frames = c->frames;
		s->loopStartFrames = c->loopStartFrames;
		s->loopLenFrames = c->loopLenFrames;
		s->loopEnd = c->loopEnd;
		s->stepW = c->stepW;
		s->stepF = c->stepF;
		s->gainL = c->gainL;
		s->gainR = c->gainR;
		s->epoch = c->epoch;
		s->looping = c->looping;
		mixAct[n++] = i;
	}
	mixActCount = n;
	Permit();
}

// Publish completion for one-shots whose audio the ring has already played.
// The game thread polls ahiDoneEpoch in GetChannelUsedFlag().
static void
AhiPublishDone(const tAhiBus *bus)
{
	uint32 i, read, mask;

	if (bus->ringFrames == 0)
		return;
	read = (uint32)bus->readFrame;
	mask = bus->ringMask;
	for (i = 0; i < NUM_CHANNELS; i++) {
		tMixVoice *v = &mixVoice[i];
		if (!v->done || ahiDoneEpoch[i] == v->epoch)
			continue;
		if (AhiVoiceDrained(read, v->doneAt, mask))
			ahiDoneEpoch[i] = v->epoch;
	}
}

// TRUE while a mixed-out voice's audio has not been played by AHI yet.
static bool8
AhiSfxPending(void)
{
	uint32 i;
	for (i = 0; i < NUM_CHANNELS; i++)
		if (mixVoice[i].done && ahiDoneEpoch[i] != mixVoice[i].epoch)
			return TRUE;
	return FALSE;
}

// Blank the SFX ring after everything queued has played (or been discarded).
// Publishing every done epoch is safe here: the ring holds no more audio.
// Never logs -- this runs in the mixer Process. Counted instead.
static void
AhiSfxFlush(tAhiBus *bus)
{
	uint32 i;
	for (i = 0; i < NUM_CHANNELS; i++) {
		if (mixVoice[i].done)
			ahiDoneEpoch[i] = mixVoice[i].epoch;
	}
	AhiRingSilence(bus);
	ahiMixFlushes++;
}

// TRUE while any snapshotted voice can still produce audio (not mixed out).
// Active-but-done voices cannot: the game thread may be blocked for a whole
// load and never call GetChannelUsedFlag() to clear `active`, so the snapshot
// itself must decide whether the bus is idle. The epoch test keeps a channel
// that was restarted in this block from being mistaken for a finished one.
static bool8
AhiSfxLive(void)
{
	uint32 k;
	for (k = 0; k < mixActCount; k++) {
		uint32 ch = mixAct[k];
		if (mixVoice[ch].epoch != mixSnap[ch].epoch || !mixVoice[ch].done)
			return TRUE;
	}
	return FALSE;
}

// Service the SFX bus: snapshot the game-thread voices, retire finished
// one-shots, then keep the ring filled `leadFrames` ahead of AHI. Runs on the
// dedicated mixer Process when one exists, otherwise on the game thread.
static void
AhiServiceSfxBus(bool8 mixStreamToo)
{
	tAhiBus *bus = &ahiBus[AHI_BUS_SFX];
	bool8 busy;

	if (bus->ring == NULL)
		return;
	ahiMixBlocks++;

	AhiSnapCopy();
	AhiPublishDone(bus);

	// A done voice the read cursor has not passed yet still owns unplayed
	// audio in the ring, so the bus stays busy until it can be retired.
	if (!AhiSfxLive() && !AhiSfxPending() &&
	    !(mixStreamToo && AhiStreamBusy())) {
		if (!bus->idleSilent)
			AhiSfxFlush(bus);
		return;
	}

	bus->idleSilent = FALSE;
	AhiFillRingSfx(bus, mixStreamToo);

	// Re-check after mixing: this block may have retired the last voice.
	if (!AhiSfxLive() && !AhiSfxPending() &&
	    !(mixStreamToo && AhiStreamBusy()))
		AhiSfxFlush(bus);
}

// Bus 1 (streams) service; game thread only.
static void
AhiServiceBus(tAhiBus *bus)
{
	if (bus->ring == NULL)
		return;
	if (!AhiStreamBusy()) {
		if (!bus->idleSilent)
			AhiFlushSilenceBus(bus);
		return;
	}
	bus->idleSilent = FALSE;
	AhiFillRingStream(bus);
	if (!AhiStreamBusy())
		AhiFlushSilenceBus(bus);
}

// Background SFX mixer. No debug(), printf, DOS, file I/O, allocation, AHI or
// engine calls here: only AhiServiceSfxBus(), state, and Delay(). Priorities
// above ~2 starve input/loading; Delay(1) yields either way.
static void
AhiMixTaskEntry(void)
{
	ahiMixTaskDone = FALSE;
	for (;;) {
		if (ahiMixTaskQuit)
			break;
		AhiServiceSfxBus(FALSE);
		Delay(1);                      // ~20 ms
	}
	ahiMixTaskDone = TRUE;
}

void
cSampleManager::Service(void)
{
	uint32 i;
	uint32 nowSvc = CTimer::GetTimeInMilliseconds();

	if (ahiSilent || ahiCtrl == NULL || ahiBus[AHI_BUS_SFX].ring == NULL)
		return;

	// If the audio tick gapped (blocking load / hitch), the looping rings will
	// have replayed stale audio; blank them before resuming so the loop stops.
	// Bus 0 is the mixer's while the task runs -- never touch it from here.
	if (ahiLastServiceMs != 0 && (nowSvc - ahiLastServiceMs) > 250) {
		int b;
		for (b = 0; b < ahiNumBuses; b++) {
			if (b == AHI_BUS_SFX) {
				if (!ahiMixTaskOn && !ahiBus[b].idleSilent)
					AhiSfxFlush(&ahiBus[b]);
			} else {
				AhiFlushSilenceBus(&ahiBus[b]);
			}
		}
	}
	ahiLastServiceMs = nowSvc;

	// Refresh per-voice gains + resample step (freq/vol/pan/fade can change
	// between ticks). The mixer snapshots them under Forbid().
	for (i = 0; i < NUM_CHANNELS; i++) {
		if (!ahiCh[i].active)
			continue;
		AhiVoiceGains(i, m_nEffectsFadeVolume, m_nEffectsVolume);
		AhiVoiceStep(i);
	}

	AhiVoiceDump();

	if (ahiNumBuses >= 2) {
		AhiStreamGains(m_nMusicVolume, m_nMusicFadeVolume,
		               m_nEffectsVolume, m_nEffectsFadeVolume);
		AhiPumpStreams();
		AhiServiceBus(&ahiBus[AHI_BUS_STREAM]);
		if (!ahiMixTaskOn)
			AhiServiceSfxBus(FALSE);
	} else {
		AhiStreamGains(m_nMusicVolume, m_nMusicFadeVolume,
		               m_nEffectsVolume, m_nEffectsFadeVolume);
		AhiPumpStreams();
		AhiServiceSfxBus(TRUE);
	}

	if (ahiStreamOn)
		AhiStreamHealth();

	if (ahiTrace && nowSvc - ahiMixLastDump >= 2000) {
		ahiMixLastDump = nowSvc;
		debug("[boot] ahi: mix blocks=%lu flushes=%lu resyncs=%lu\n",
		      (unsigned long)ahiMixBlocks, (unsigned long)ahiMixFlushes,
		      (unsigned long)ahiMixResyncs);
	}
}

// ---------------------------------------------------------------------------
// Bank index
// ---------------------------------------------------------------------------
bool8
cSampleManager::InitialiseSampleBanks(void)
{
	uint32 i;

	fpSampleDescHandle = fcaseopen(SampleBankDescFilename, "rb");
	if (fpSampleDescHandle == NULL)
		return FALSE;

	fpSampleDataHandle = fcaseopen(SampleBankDataFilename, "rb");
	if (fpSampleDataHandle == NULL) {
		fclose(fpSampleDescHandle);
		fpSampleDescHandle = NULL;
		return FALSE;
	}

	fseek(fpSampleDataHandle, 0, SEEK_END);
	_nSampleDataEndOffset = ftell(fpSampleDataHandle);
	rewind(fpSampleDataHandle);

	if (fread(m_aSamples, sizeof(tSample), TOTAL_AUDIO_SAMPLES, fpSampleDescHandle) != TOTAL_AUDIO_SAMPLES) {
		fclose(fpSampleDescHandle);
		fpSampleDescHandle = NULL;
		return FALSE;
	}
	fclose(fpSampleDescHandle);
	fpSampleDescHandle = NULL;

	// sfx.SDT is PC little-endian; swap the 5 x uint32 fields to native 68k.
	for (i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		m_aSamples[i].nOffset    = Swap32(m_aSamples[i].nOffset);
		m_aSamples[i].nSize      = Swap32(m_aSamples[i].nSize);
		m_aSamples[i].nFrequency = Swap32(m_aSamples[i].nFrequency);
		m_aSamples[i].nLoopStart = Swap32(m_aSamples[i].nLoopStart);
		m_aSamples[i].nLoopEnd   = (int32)Swap32((uint32)m_aSamples[i].nLoopEnd);
	}

	// Every slice must lie inside sfx.RAW. A corrupt/truncated SDT would
	// otherwise let on-demand reads (and the ped/mission buffer sizing) run
	// past the file, so treat a bad slice as "no usable bank" (silent).
	for (i = 0; i < TOTAL_AUDIO_SAMPLES; i++) {
		uint32 off = m_aSamples[i].nOffset;
		uint32 size = m_aSamples[i].nSize;
		if (size == 0 || off > (uint32)_nSampleDataEndOffset ||
		    size > (uint32)_nSampleDataEndOffset - off) {
			debug("[boot] ahi: silent (bad sfx.SDT slice %lu: off=%lu size=%lu of %lu)\n",
			      (unsigned long)i, (unsigned long)off, (unsigned long)size,
			      (unsigned long)_nSampleDataEndOffset);
			fclose(fpSampleDataHandle);
			fpSampleDataHandle = NULL;
			return FALSE;
		}
	}

	return TRUE;
}

int8 cSampleManager::AutoDetect3DProviders() { return -1; }

#endif // AUDIO_AHI
