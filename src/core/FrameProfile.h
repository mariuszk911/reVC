#ifndef __GTA_FRAMEPROFILE_H__
#define __GTA_FRAMEPROFILE_H__

#include <stdlib.h>	// calloc/free for the lazily allocated ring (see fpEnable)

//
// Frame-time profiler core: outer phase slots, subset slots, a 30-frame
// aggregation window and a 600-frame ring, plus a slow-frame predicate and an
// integer-tenths formatter.
//
// This header is deliberately dependency-free (no librw, no stdio float) so the
// same code runs in the engine and in the host/68k unit test
// tests/amigaos/frameprofile_test.cpp.
//
// The state lives in one function-local static inside an external-linkage
// `inline` accessor, so every translation unit that includes this header shares
// the single profiler instance (a `static` function-local would duplicate it per
// TU). Everything is a no-op until fpEnable(true).
//
// %f / %g are broken in this libnix, so every value is formatted as integer
// tenths (see fpFmt10): assert on the rendered digit string, never on floats.
//

// On AmigaOS the timestamp comes from the platform layer (ReadEClock, us); the
// hook and its off-AmigaOS stub live in amiga_boot.h.
#include "amiga_boot.h"

namespace fp {

// Outer (non-overlapping) phase slots. The measured outer slots run from
// FP_TIMER to FP_PRESENT; FP_FRAME is the wall-clock inter-frame period and
// FP_OUTSIDE is the residual (FP_FRAME minus the outer slots). The subset slots
// below are measured but excluded from the residual.
enum {
	FP_FRAME = 0,
	FP_OUTSIDE,
	FP_TIMER,
	FP_PROCESS,
	FP_AUDIO,
	FP_RENDERLIST,
	FP_PRERENDER,
	FP_CLEAR,
	FP_SCENE,
	FP_DEBUG,
	FP_EFFECTS,
	FP_MBLUR,
	FP_2D,
	FP_MENUS,
	FP_FADE,
	FP_PRESENT,

	// Subset slots (inside FP_SCENE or FP_PROCESS; never added to FP_OUTSIDE).
	FP_LIT,			// CPU lighting inside the world draw pass
	FP_SKINCPU,		// CPU 4-weight vertex blend inside the skin pass
	FP_GAME_STREAM,
	FP_GAME_SCRIPT,
	FP_GAME_WORLD_PRE,
	FP_GAME_POP,
	FP_GAME_MISC,
	FP_GAME_WORLD,
	FP_GAME_SIM,
	FP_GAME_CARS,

	FP_NUM_SLOTS
};

enum { FP_FIRST_OUTER = FP_TIMER, FP_LAST_OUTER = FP_PRESENT };

enum {
	FP_WINDOW = 30,		// aggregation window (frames)
	FP_RING = 600,		// ring capacity (frames); 120 was too short a history
	FP_GROUP_MAX = 32,	// render-group nesting depth
	FP_LINE_MAX = 768,	// formatting scratch
	FP_SLOW_FLOOR_US = 20000	// auto-predicate floor (20 ms)
};

struct FrameRecord {
	unsigned int frameUs;
	unsigned int slotUs[FP_NUM_SLOTS];
	unsigned int slow;
};

typedef void (*FpLineFn)(const char *line);
typedef unsigned int (*FpTimeFn)(void);

struct FpState {
	int enabled;
	int prevValid;
	unsigned int prevBeginUs;
	unsigned int acc[FP_NUM_SLOTS];
	unsigned int beginUs[FP_NUM_SLOTS];
	FrameRecord *ring;	// lazily allocated in fpEnable(true) (no ~94 KB .bss when off)
	int ringHead;
	int ringCount;
	unsigned int framesTotal;
	unsigned int slowTotal;
	unsigned int lastSlowSeq;
	unsigned int slowMs;		// 0 = auto (2 x window median, floored)
	unsigned char overlayMode;	// 0 hidden, 1 compact, 2 detailed
	FpLineFn aggregateSink;
	FpLineFn slowSink;
	FpTimeFn timeFn;		// nil = amiga_boot_us; set by the host test
	struct FpGroup { int slot; unsigned int startUs; } group[FP_GROUP_MAX];
	int groupTop;
	int groupDropped;	// pushes dropped at FP_GROUP_MAX (kept LIFO-symmetric)
};

// The single profiler instance.
//
// This toolchain (m68k gcc 6.5) does NOT merge the function-local static of an
// inline function across translation units: each TU that includes this header
// would get its own copy, so fpEnable() in amigaos.cpp and fpDrawOverlay() in
// FrameProfileOverlay.cpp would see different state (the overlay never draws and
// the ring dump is empty). The state is therefore a real global defined once in
// src/core/FrameProfile.cpp. The host/68k unit test compiles one TU only, so it
// defines FP_HEADER_ONLY to keep the self-contained inline local static.
#ifdef FP_HEADER_ONLY
inline FpState &
fpS(void)
{
	static FpState s;
	return s;
}
#else
FpState &fpS(void);
#endif

inline int
fpEnabled(void)
{
	return fpS().enabled;
}

inline void
fpReset(void)
{
	FpState &s = fpS();
	int i;

	for(i = 0; i < FP_NUM_SLOTS; i++){
		s.acc[i] = 0;
		s.beginUs[i] = 0;
	}
	s.prevValid = 0;
	s.prevBeginUs = 0;
	s.ringHead = 0;
	s.ringCount = 0;
	s.framesTotal = 0;
	s.slowTotal = 0;
	s.lastSlowSeq = 0;
	s.groupTop = 0;
	s.groupDropped = 0;
}

inline void
fpEnable(int on)
{
	FpState &s = fpS();
	if(on && !s.enabled){
		// Allocate the ring here rather than carrying ~94 KB of .bss for the
		// whole process even when profiling is off. Freed on fpEnable(false).
		if(s.ring == 0)
			s.ring = (FrameRecord *)calloc(FP_RING, sizeof(FrameRecord));
		if(s.ring == 0)
			return;	// out of memory: stay disabled rather than crash
		s.enabled = 1;
		fpReset();
		s.overlayMode = 1;	// start on the compact page
	}else if(!on){
		s.enabled = 0;
		if(s.ring){
			free(s.ring);
			s.ring = 0;
		}
		// Drop the ring bookkeeping with the storage so no reader can follow a
		// freed pointer (framesTotal/slowTotal are left intact).
		s.ringHead = 0;
		s.ringCount = 0;
		s.prevValid = 0;
	}
}

inline void
fpSetSlowThresholdMs(unsigned int ms)
{
	fpS().slowMs = ms;
}

inline void
fpSetLineSink(FpLineFn aggregate, FpLineFn slow)
{
	FpState &s = fpS();
	s.aggregateSink = aggregate;
	s.slowSink = slow;
}

// Override the timestamp source (the host test drives a synthetic clock; nil
// restores the platform amiga_boot_us).
inline void
fpSetTimeFn(FpTimeFn fn)
{
	fpS().timeFn = fn;
}

inline unsigned int
fpNow(void)
{
	FpTimeFn fn = fpS().timeFn;
	return fn ? fn() : amiga_boot_us();
}

inline void
fpBeginFrame(void)
{
	FpState &s = fpS();
	unsigned int now;
	int i;

	if(!s.enabled)
		return;

	now = fpNow();
	for(i = 0; i < FP_NUM_SLOTS; i++)
		s.acc[i] = 0;

	if(s.prevValid)
		s.acc[FP_FRAME] = now - s.prevBeginUs;
	s.prevBeginUs = now;
	s.prevValid = 1;
}

inline void
fpAdd(int slot, unsigned int us)
{
	FpState &s = fpS();
	if(!s.enabled || slot < 0 || slot >= FP_NUM_SLOTS)
		return;
	s.acc[slot] += us;
}

inline void
fpBegin(int slot)
{
	FpState &s = fpS();
	if(!s.enabled || slot < 0 || slot >= FP_NUM_SLOTS)
		return;
	s.beginUs[slot] = fpNow();
}

inline void
fpEnd(int slot)
{
	FpState &s = fpS();
	if(!s.enabled || slot < 0 || slot >= FP_NUM_SLOTS)
		return;
	s.acc[slot] += fpNow() - s.beginUs[slot];
}

inline int
fpIsSlow(unsigned int frameUs)
{
	FpState &s = fpS();
	unsigned int thr, tmp[FP_WINDOW];
	unsigned int n;
	int start, i, j;

	if(s.slowMs > 0)
		thr = s.slowMs * 1000u;
	else {
		n = (unsigned int)(s.ringCount < FP_WINDOW ? s.ringCount : FP_WINDOW);
		if(n < 1)
			thr = FP_SLOW_FLOOR_US;
		else {
			start = s.ringHead - (int)n;
			while(start < 0)
				start += FP_RING;
			for(i = 0; i < (int)n; i++)
				tmp[i] = s.ring[(start + i) % FP_RING].frameUs;
			for(i = 1; i < (int)n; i++){
				unsigned int v = tmp[i];
				for(j = i - 1; j >= 0 && tmp[j] > v; j--)
					tmp[j + 1] = tmp[j];
				tmp[j + 1] = v;
			}
			thr = tmp[n / 2] * 2u;
			if(thr < FP_SLOW_FLOOR_US)
				thr = FP_SLOW_FLOOR_US;
		}
	}
	return frameUs > thr;
}

inline int
fpAppendStr(char *out, int pos, const char *str)
{
	while(*str)
		out[pos++] = *str++;
	return pos;
}

inline int
fpAppendUInt(char *out, int pos, unsigned int v)
{
	char tmp[12];
	int n = 0;

	if(v == 0){
		out[pos++] = '0';
		return pos;
	}
	while(v){
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while(n)
		out[pos++] = tmp[--n];
	return pos;
}

// Format microseconds as tenths of a millisecond: 118400 -> "118.4".
inline int
fpFmt10(unsigned int us, char *out)
{
	unsigned int tenths = (us + 50u) / 100u;
	int pos = fpAppendUInt(out, 0, tenths / 10u);
	out[pos++] = '.';
	out[pos++] = (char)('0' + (tenths % 10u));
	out[pos] = '\0';
	return pos;
}

// Slot name for a slot index, or "" for slots without a log field (the subset
// slots are logged separately; FP_FRAME/FP_OUTSIDE handled by the caller).
inline const char *
fpSlotName(int slot)
{
	switch(slot){
	case FP_OUTSIDE:    return "outside";
	case FP_TIMER:      return "timer";
	case FP_PROCESS:    return "process";
	case FP_AUDIO:      return "audio";
	case FP_RENDERLIST: return "renderlist";
	case FP_PRERENDER:  return "prerender";
	case FP_CLEAR:      return "clear";
	case FP_SCENE:      return "scene";
	case FP_DEBUG:      return "debug";
	case FP_EFFECTS:    return "effects";
	case FP_MBLUR:      return "mblur";
	case FP_2D:         return "2d";
	case FP_MENUS:      return "menus";
	case FP_FADE:       return "fade";
	case FP_PRESENT:    return "present";
	case FP_LIT:        return "lit";
	case FP_SKINCPU:    return "skincpu";
	case FP_GAME_STREAM:    return "stream";
	case FP_GAME_SCRIPT:    return "script";
	case FP_GAME_WORLD_PRE: return "pre";
	case FP_GAME_POP:       return "pop";
	case FP_GAME_MISC:      return "misc";
	case FP_GAME_WORLD:     return "world";
	case FP_GAME_SIM:       return "sim";
	case FP_GAME_CARS:      return "cars";
	default:            return "";
	}
}

inline void
fpAppendSlot(char *out, int *pos, int slot, unsigned int us)
{
	*pos = fpAppendStr(out, *pos, fpSlotName(slot));
	out[(*pos)++] = ' ';
	*pos += fpFmt10(us, out + *pos);
	out[(*pos)++] = ' ';
}

// Appends the three slot partitions (outer, in-scene/in-process subsets, game
// split) for a value array. Shared by the per-frame and aggregate formatters so
// a slot added or reordered in the enum cannot make the two trace formats drift
// apart. The librw-render counters moved to the pull query (RwMiniStats).
inline void
fpAppendParts(char *out, int *pos, const unsigned int *slotUs)
{
	int i;

	*pos = fpAppendStr(out, *pos, "; ");
	for(i = FP_OUTSIDE; i <= FP_PRESENT; i++)
		fpAppendSlot(out, pos, i, slotUs[i]);
	*pos = fpAppendStr(out, *pos, "; ");
	for(i = FP_LIT; i <= FP_SKINCPU; i++)
		fpAppendSlot(out, pos, i, slotUs[i]);
	*pos = fpAppendStr(out, *pos, "; ");
	for(i = FP_GAME_STREAM; i <= FP_GAME_CARS; i++)
		fpAppendSlot(out, pos, i, slotUs[i]);
}

inline int
fpFmtFrameLine(char *out, unsigned int seq, const FrameRecord *r)
{
	int pos = 0;

	if(r == 0){
		out[0] = '\0';
		return 0;
	}
	pos = fpAppendStr(out, pos, "prof: frame ");
	pos = fpAppendUInt(out, pos, seq);
	pos = fpAppendStr(out, pos, ": ");
	pos += fpFmt10(r->frameUs, out + pos);
	pos = fpAppendStr(out, pos, " ms");
	fpAppendParts(out, &pos, r->slotUs);
	out[pos] = '\0';
	return pos;
}

inline int
fpFmtAggregate(char *out, unsigned int seq)
{
	FpState &s = fpS();
	unsigned int n, fmin, fmax, fsum, sums[FP_NUM_SLOTS];
	unsigned int avg[FP_NUM_SLOTS];
	int pos = 0, i, start;

	n = (unsigned int)(s.ringCount < FP_WINDOW ? s.ringCount : FP_WINDOW);
	if(n == 0){
		out[0] = '\0';
		return 0;
	}
	start = s.ringHead - (int)n;
	while(start < 0)
		start += FP_RING;

	fmin = fmax = fsum = 0;
	for(i = 0; i < FP_NUM_SLOTS; i++)
		sums[i] = 0;
	for(i = 0; i < (int)n; i++){
		const FrameRecord *r = &s.ring[(start + i) % FP_RING];
		int j;
		if(i == 0 || r->frameUs < fmin)
			fmin = r->frameUs;
		if(i == 0 || r->frameUs > fmax)
			fmax = r->frameUs;
		fsum += r->frameUs;
		for(j = 0; j < FP_NUM_SLOTS; j++)
			sums[j] += r->slotUs[j];
	}
	for(i = 0; i < FP_NUM_SLOTS; i++)
		avg[i] = sums[i] / n;

	pos = fpAppendStr(out, pos, "prof: frames ");
	pos = fpAppendUInt(out, pos, seq);
	pos = fpAppendStr(out, pos, ": ms/frame min ");
	pos += fpFmt10(fmin, out + pos);
	pos = fpAppendStr(out, pos, " max ");
	pos += fpFmt10(fmax, out + pos);
	pos = fpAppendStr(out, pos, " avg ");
	pos += fpFmt10(fsum / n, out + pos);
	fpAppendParts(out, &pos, avg);
	out[pos] = '\0';
	return pos;
}

inline void
fpEndFrame(void)
{
	FpState &s = fpS();
	unsigned int sum, outside, frameUs;
	FrameRecord *r;
	int slow, i;

	if(!s.enabled || !s.prevValid || s.ring == 0)
		return;
	frameUs = s.acc[FP_FRAME];
	if(frameUs == 0)
		return;	// first frame after enable/reset has no valid period

	sum = 0;
	for(i = FP_FIRST_OUTER; i <= FP_LAST_OUTER; i++)
		sum += s.acc[i];
	outside = frameUs > sum ? frameUs - sum : 0;
	s.acc[FP_OUTSIDE] = outside;

	slow = fpIsSlow(frameUs);

	r = &s.ring[s.ringHead];
	r->frameUs = frameUs;
	for(i = 0; i < FP_NUM_SLOTS; i++)
		r->slotUs[i] = s.acc[i];
	r->slow = (unsigned int)slow;
	s.ringHead = (s.ringHead + 1) % FP_RING;
	if(s.ringCount < FP_RING)
		s.ringCount++;
	s.framesTotal++;

	if(slow){
		s.slowTotal++;
		s.lastSlowSeq = s.framesTotal;
		if(s.slowSink){
			char buf[FP_LINE_MAX];
			fpFmtFrameLine(buf, s.framesTotal, r);
			s.slowSink(buf);
		}
	}
	if(s.aggregateSink && (s.framesTotal % FP_WINDOW) == 0){
		char buf[FP_LINE_MAX];
		fpFmtAggregate(buf, s.framesTotal);
		s.aggregateSink(buf);
	}
}

// --- render-group timing (RwHelper.cpp Push/PopRendergroup) -----------------

inline int
fpSlotFromGroupName(const char *name)
{
	if(name == 0)
		return -1;
	if(name[0] != 'R')
		return -1;
	if(__builtin_strcmp(name, "RenderScene") == 0 ||
	   __builtin_strcmp(name, "RenderScene_new") == 0)
		return FP_SCENE;
	if(__builtin_strcmp(name, "RenderDebugShit") == 0)
		return FP_DEBUG;
	if(__builtin_strcmp(name, "RenderEffects") == 0 ||
	   __builtin_strcmp(name, "RenderEffects_new") == 0)
		return FP_EFFECTS;
	if(__builtin_strcmp(name, "Render2dStuff") == 0)
		return FP_2D;
	if(__builtin_strcmp(name, "RenderMenus") == 0)
		return FP_MENUS;
	if(__builtin_strcmp(name, "Render2dStuffAfterFade") == 0)
		return FP_FADE;
	return -1;
}

inline void
fpPushGroup(const char *name)
{
	FpState &s = fpS();
	int slot;

	if(!s.enabled)
		return;
	if(s.groupTop >= FP_GROUP_MAX){
		// Cannot stack deeper; the matching pop must not consume a parent
		// entry, so record the drop and let fpPopGroup unwind it LIFO.
		s.groupDropped++;
		return;
	}
	slot = fpSlotFromGroupName(name);
	s.group[s.groupTop].slot = slot;
	s.group[s.groupTop].startUs = slot >= 0 ? fpNow() : 0;
	s.groupTop++;
}

inline void
fpPopGroup(void)
{
	FpState &s = fpS();

	if(!s.enabled)
		return;
	if(s.groupDropped > 0){
		s.groupDropped--;
		return;
	}
	if(s.groupTop <= 0)
		return;
	s.groupTop--;
	if(s.group[s.groupTop].slot >= 0)
		s.acc[s.group[s.groupTop].slot] += fpNow() - s.group[s.groupTop].startUs;
}

// --- overlay / trace accessors ---------------------------------------------

inline unsigned char
fpOverlayMode(void)
{
	return fpS().overlayMode;
}

inline void
fpCycleOverlay(void)
{
	FpState &s = fpS();
	s.overlayMode = (unsigned char)((s.overlayMode + 1) % 3);
}

inline const FrameRecord *
fpLastRecord(void)
{
	FpState &s = fpS();
	int i;

	if(s.ringCount == 0)
		return 0;
	i = s.ringHead - 1;
	if(i < 0)
		i += FP_RING;
	return &s.ring[i];
}

inline int
fpRingCount(void)
{
	return fpS().ringCount;
}

inline const FrameRecord *
fpRingAt(int i)
{
	FpState &s = fpS();
	int start;

	if(i < 0 || i >= s.ringCount)
		return 0;
	start = s.ringHead - s.ringCount;
	while(start < 0)
		start += FP_RING;
	return &s.ring[(start + i) % FP_RING];
}

inline void
fpWindowFrameStats(unsigned int *minUs, unsigned int *maxUs, unsigned int *avgUs)
{
	FpState &s = fpS();
	unsigned int n, lo = 0, hi = 0, sum = 0;
	int start, i;

	n = (unsigned int)(s.ringCount < FP_WINDOW ? s.ringCount : FP_WINDOW);
	if(n == 0){
		if(minUs) *minUs = 0;
		if(maxUs) *maxUs = 0;
		if(avgUs) *avgUs = 0;
		return;
	}
	start = s.ringHead - (int)n;
	while(start < 0)
		start += FP_RING;
	for(i = 0; i < (int)n; i++){
		unsigned int v = s.ring[(start + i) % FP_RING].frameUs;
		if(i == 0 || v < lo) lo = v;
		if(i == 0 || v > hi) hi = v;
		sum += v;
	}
	if(minUs) *minUs = lo;
	if(maxUs) *maxUs = hi;
	if(avgUs) *avgUs = sum / n;
}

inline unsigned int
fpFramesTotal(void)
{
	return fpS().framesTotal;
}

inline unsigned int
fpSlowTotal(void)
{
	return fpS().slowTotal;
}

inline unsigned int
fpLastSlowSeq(void)
{
	return fpS().lastSlowSeq;
}

} // namespace fp

// Implemented in src/core/FrameProfileOverlay.cpp. Declared here so main.cpp can
// call it from Render2dStuffAfterFade without a second header.
void fpDrawOverlay(void);
void fpInstallSinks(void);
void fpShutdownDump(void);
// Shift+F10 while profiling: write the current ring to profile_frames_<n>.txt
// immediately (the exit dump is useless if the session ends in a fast menu, which
// overwrites a whole gameplay run in <1 s of ring time).
void fpDumpRingNow(void);

#endif // __GTA_FRAMEPROFILE_H__
