/*
 * AmigaOS 68k platform backend for reVC (RW_MINI / MiniGL).
 *
 * This replaces src/skel/glfw (and win/sdl2) on AmigaOS. minigl.library owns
 * the RTG screen/window: librw's MINI device calls mglCreateContext() from
 * DEVICEINIT,
 * so this file does not open an Intuition screen itself. It must instead:
 *   - pump the MiniGL window's IDCMP (MiniGL opens it with no IDCMP flags and
 *     expects the caller to ModifyIDCMP),
 *   - feed RsKeyboardEventHandler / CPad mouse state,
 *   - provide the ps* / _ps* platform contract and the process entry point.
 *
 * Deliberately does NOT include <mgl/gl.h>: it pulls a `Matrix` typedef that
 * would clash with the game's Matrix class. Only the two symbols we need are
 * forward-declared with C linkage.
 */

#if defined(AMIGAOS)

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "crossplatform.h"

#include "main.h"
#include "FileMgr.h"
#include "Text.h"
#include "Pad.h"
#include "Timer.h"
#include "DMAudio.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Game.h"
#include "CutsceneMgr.h"
#include "EnvFlags.h"
#include "AmigaFlags.h"
#include "PCSave.h"
#include "MemoryCard.h"
#include "Sprite2d.h"
#include "AmigaSplash.h"	// boot splash (see src/skel/amigaos/amiga_splash.cpp)
#include "AmigaC64Intro.h"	// C64 crunch intro (see src/skel/amigaos/amiga_c64_intro.cpp)
#include "AnimViewer.h"
#include "Streaming.h"
#include "Font.h"
#include "MemoryMgr.h"
#include "GitSHA1.h"
#include "FrameProfile.h"
#include "amiga_boot.h"
#include "mini/rwminihost.h"	// librw MINI host interface (options + services)

// ---------------------------------------------------------------------------
// AmigaOS system headers, deliberately LAST: their proto/inline macros (Read,
// Write, SetMode, DeleteFile, ...) collide with game identifiers, so they must
// not be visible while the game headers are parsed.
// ---------------------------------------------------------------------------

#ifdef DeleteFile
#undef DeleteFile
#endif

// The 68k NDK's <proto/timer.h> and <inline/timer.h> reference a global named
// TimerBase. Give this translation unit's timer.device base a private name so it
// cannot clash with a TimerBase from any linked library: the macro renames every
// reference below, including the inline ReadEClock().
#define TimerBase AmigaTimerBase

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <exec/interrupts.h>
#include <devices/timer.h>
#include <devices/serial.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>

#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Crash-safe, unbuffered startup log.
//
// The old `freopen("reVC.log","w",stdout)` left stdout block-buffered, so a
// Guru (which never flushes stdio) produced an empty log. Redirect both streams
// at the descriptor level, make them unbuffered, and also expose amiga_log()/
// STARTLOG() built on write() so the earliest lines and any crash diagnostics
// survive regardless of stdio state.
// ---------------------------------------------------------------------------

extern "C" void *rwMiniGetWindowHandle(void);
static struct Window *amigaAppWindow;
#ifdef AUDIO_AHI
// Defined in src/audio/sampman_ahi.cpp: probes ahi.device and (when present)
// runs the AHI audio-mode requester. Called from main() before RW/GL init so
// the requester is visible instead of hidden under the game screen.
extern "C" int amigaAhiEarlyModeSelect(void);
#endif

// Log sink: stdout (redirected with freopen(), so libnix owns and closes it) plus
// a separate dos handle used only for the per-line Flush(). Never redirect with
// dup2(): libnix's dup2 shallow-copies the descriptor, so one handle ends up on
// several fds and is closed twice (here + libnix exit) -> AN_FileReclosed/#8700000E.
static int amigaLogReady = 0;
static BPTR amigaLogFH = 0;

// ---------------------------------------------------------------------------
// Shared REVC_* flag reader (definitions below, after STARTLOG).
//
// Every switch goes through these so the accepted tokens (see EnvFlags.h) and
// the fail-fast behaviour are identical for every flag. An absent/empty value
// takes the per-flag default passed by the caller; a present but unrecognised
// token is fatal (see amigaEnvFatal). Forward-declared because the serial path
// (amigaSerialWanted) is defined before STARTLOG.
// ---------------------------------------------------------------------------
static int amigaEnvBool(const char *name, int def);
static int amigaEnvBoolEx(const char *name, int def, int *present);
static int amigaEnvInt(const char *name, int def, int min, int max);
static void amigaEnvFatal(const char *name, const char *val);

// All REVC_* gates, read once. Resolved lazily (not in main) because some hooks
// run during rsINITIALIZE, before main() parses the environment.
static AmigaFlags gAmigaFlags;
static int gAmigaFlagsReady;

const AmigaFlags &
amigaFlags(void)
{
	AmigaFlags &f = gAmigaFlags;

	if(gAmigaFlagsReady)
		return f;

	f.lockArrays = amigaEnvBool("REVC_LOCKARRAYS", 1);
	f.matFx = amigaEnvBool("REVC_MATFX", 1);
	f.lockWin = amigaEnvBool("REVC_LOCKWIN", 1);
	f.texUniq = amigaEnvBool("REVC_TEXUNIQ", 0);
	f.asyncStream = amigaEnvBool("REVC_ASYNC_STREAM", 1);
	f.texMemLog = amigaEnvBool("REVC_TEXMEMLOG", 0);
	f.profile = amigaEnvBool("REVC_PROFILE", 0);
	f.profileMs = amigaEnvInt("REVC_PROFILE_MS", 0, 0, 0x7fffffff);
	f.randomSplash = amigaEnvBool("REVC_RANDOMSPLASH", 0);
	f.clearColor = amigaEnvBool("REVC_CLEAR", 0);
	f.directFetch = amigaEnvBool("REVC_DIRECT", 0);
	f.streamStats = amigaEnvBool("REVC_STREAMSTATS", 0);
	f.streamStatsFrames = amigaEnvInt("REVC_STREAMSTATS_FRAMES", 150, 1, 0x7fffffff);
	// -1 = unset -> default 20; 0 is a real value (disable slow-frame lines).
	f.streamStatsSlowMs = amigaEnvInt("REVC_STREAMSTATS_SLOW_MS", -1, -1, 0x7fffffff);
	if(f.streamStatsSlowMs < 0)
		f.streamStatsSlowMs = 20;
	gAmigaFlagsReady = 1;
	return f;
}

// ---------------------------------------------------------------------------
// Serial mirror. Everything written to reVC.log is also emitted on the serial
// port through Exec's RawPutChar (private LVO -516) -- the ROM "kprintf" path,
// the same sink emu68-common uses for its serial debug backend. It needs no
// serial.device open, no IOExtSer and no flow control: one byte at a time at the
// system serial baud (default 9600 8N1), and anything that hooks the raw serial
// output (Sashimi, minicom behind a USB adapter) sees it. Default: off; set the
// AmigaDOS env var REVC_SERIAL to 1/yes/on to enable. Writes are synchronous,
// so heavy logging is baud-limited and slows loading.
// ---------------------------------------------------------------------------
static int amigaSerOn = -1;	// -1 = not resolved yet, 0 = off, 1 = on

static int
amigaSerialWanted(void)
{
	// Default off (set REVC_SERIAL=1/yes/on to enable).
	return amigaEnvBool("REVC_SERIAL", 0);
}

// Exec RawPutChar. There is no NDK prototype, so call the vector directly: it
// clobbers d0/d1/a0/a1 and expects a6 = SysBase (read from 4.w, as emu68-common
// does -- the ~1 ms baud-limited byte dwarfs the cost of the read).
static void
amigaSerialPutChar(unsigned char c)
{
	register ULONG ch asm("d0") = c;

	asm volatile("move.l 4.w,%%a6\n\t"
	             "jsr -516(%%a6)"
	             : "+d"(ch)
	             :
	             : "d1", "a0", "a1", "a6", "cc", "memory");
}

static void
amigaSerialWrite(const char *s, long n)
{
	long i;

	if(n <= 0)
		return;
	if(amigaSerOn < 0)
		amigaSerOn = amigaSerialWanted() ? 1 : 0;
	if(!amigaSerOn)
		return;
	for(i = 0; i < n; i++)
		amigaSerialPutChar((unsigned char)s[i]);
}

static void
amigaRawWrite(const char *s, long n)
{
	if(n <= 0)
		return;
	if(amigaLogReady){
		// stdout is unbuffered, so this shares printf()'s handle and order.
		fwrite(s, 1, (size_t)n, stdout);
		if(amigaLogFH)
			Flush(amigaLogFH);	// push dirty filesystem buffers before a guru can drop them
	}else
		write(1, s, n);

	amigaSerialWrite(s, n);
}

// Raw platform log sink for re3_debug(); mirrors to the serial port like every
// other reVC.log write.
extern "C" void
amiga_log_raw(const char *s)
{
	amigaRawWrite(s, (long)strlen(s));
}

static void
amiga_log(const char *s)
{
	amigaRawWrite(s, (long)strlen(s));
}

// Unbuffered boot-log writer (C linkage); shared with the librw MINI backend's
// rwminiHostLog() below and the profiler/overlay sinks.
extern "C" void
amiga_boot_log(const char *s)
{
	amiga_log(s);
	amiga_log("\n");
}

// --- librw MINI host interface (vendor/librw/src/mini/rwminihost.h) ----------
// Strong definitions; they override the weak defaults in librw's
// rwminihost.cpp, so the backend needs no application symbols of its own. A
// fresh fullscreen RTG screen may not be the front screen yet when the first
// display lock happens (Amiberry), so the backend retries the lock after
// bringing the minigl.library screen to front.
extern "C" void
rwminiHostLog(const char *s)
{
	static int on = -1;
	if(on < 0)
		on = amigaEnvBool("REVC_MINILOG", 0);
	if(on)
		amiga_boot_log(s);
}

extern "C" void
rwminiHostDelay(int ticks)
{
	if(ticks > 0)
		Delay(ticks);
}

extern "C" void
rwminiHostFrontScreen(void)
{
	struct Window *w = amigaAppWindow;

	if(w == nil)
		w = (struct Window *)rwMiniGetWindowHandle();
	if(w == nil)
		return;
	if(amigaAppWindow != nil){
		WindowToFront(w);
		ActivateWindow(w);
	}else if(w->WScreen != nil){
		ScreenToFront(w->WScreen);
	}
}

// Route librw's file access through reVC's case-correcting/normalising open.
extern "C" FILE *
rwminiHostFopen(const char *name, const char *mode)
{
	return _fcaseopen(name, mode);
}

// Per-pass render-stats clock (rwminihost.h): the same EClock microseconds the
// frame profiler uses. librw reads it only while stats collection is enabled.
extern "C" unsigned int
rwminiHostMicros(void)
{
	return amiga_boot_us();
}

// Milliseconds since boot (for the MINI backend's frame-time logging).
extern "C" unsigned int
amiga_boot_ms(void)
{
	return (unsigned int)psTimer();
}

// Microseconds since boot (frame profiler; see src/core/FrameProfile.h). The
// EClock tick counter (ev_lo, 32 bits) wraps after ~100 min at ~715 kHz, and the
// returned microsecond value after ~71 min; both are far longer than a frame, so
// callers only ever use `(unsigned)(end - start)` deltas. The Q16 reciprocal
// scale avoids a division (and any libm float call) on the per-mesh timing path:
//     us = (ticks_lo * (1000000 << 16) / freq) >> 16
extern "C" unsigned int
amiga_boot_us(void)
{
	struct EClockVal ev;
	ULONG freq;
	static unsigned int eclockScale = 0;	// Q16: 65536 * 1000000 / freq

	if(TimerBase == nil)
		return 0;
	freq = ReadEClock(&ev);
	if(freq == 0)
		return 0;
	if(eclockScale == 0)
		eclockScale = (unsigned int)(((unsigned long long)1000000 << 16) / freq);
	return (unsigned int)(((unsigned long long)ev.ev_lo * eclockScale) >> 16);
}

// Free physical memory (for the MINI backend's texture-load diagnostics).
extern "C" unsigned int
amiga_boot_avail(void)
{
	return (unsigned int)AvailMem(MEMF_ANY);
}

// Windowed-mode request for the librw MINI backend, from reVC.ini's
// [VideoMode] Windowed setting. In windowed mode the host owns a fixed-size
// Intuition window which MiniGL adopts as its display.
static int amigaWindowed = 0;
static bool amigaForcedFullscreen = false;

// Client-array render path for the librw MINI backend.
//
// minigl.library 29.x implements the indexed array pipeline
// (glVertexPointer/glColorPointer/glTexCoordPointer/glDrawElements), so it is on
// by default. REVC_VERTEXARRAY=0 forces immediate mode (the A/B baseline /
// rollback switch).
static int amigaVertexArray = 1;

extern "C" int
amiga_boot_vertexarray(void)
{
	return amigaVertexArray;
}

// glLockArrays/glUnlockArrays around the MINI client-array mesh loop (PiStorm3D).
extern "C" int
amiga_boot_lockarrays(void)
{
	return amigaFlags().lockArrays;
}

// MatFX ENVMAP reflection pass for vehicle bodies (MINI).
extern "C" int
amiga_boot_matfx(void)
{
	return amigaFlags().matFx;
}

// Locked <=1024-vertex draw windows for the PiStorm3D locked-gather path.
extern "C" int
amiga_boot_lockwin(void)
{
	return amigaFlags().lockWin;
}

// Distinct-texture-per-frame census (diagnostic).
extern "C" int
amiga_boot_texuniq(void)
{
	return amigaFlags().texUniq;
}


// REVC_SKIPINTRO=1 bypasses the frontend (starts a new game immediately) and
// auto-finishes every in-engine cutscene for the rest of the session. Set from
// the env var before the main loop; see the GS_INIT_ONCE and GS_PLAYING_GAME
// cases there. Parsed by main() through the shared amigaEnvBool() reader
// (src/core/EnvFlags.h).
static int amigaSkipIntro = 0;

// Background CdStream loader Process (amiga_stream.c) vs inline synchronous reads.
extern "C" int
amiga_boot_async_stream(void)
{
	return amigaFlags().asyncStream;
}

// Texture-memory diagnostic (MINI): allocator stats / gen-delete balance / AvailMem.
extern "C" int
amiga_boot_texmemlog(void)
{
	return amigaFlags().texMemLog;
}

// Random loading splash (REVC_RANDOMSPLASH): pick one of the LOADSCx images per
// load instead of always LOADSC0 (default off).
extern "C" int
amiga_boot_randomsplash(void)
{
	return amigaFlags().randomSplash;
}

// Frame profiler (src/core/FrameProfile.h).
extern "C" int
amiga_boot_profile(void)
{
	return amigaFlags().profile;
}

// Colour-clear the frame (REVC_CLEAR). Without it the previous frame's pixels
// stay where no geometry is drawn; enabling it exposes holes that otherwise
// hide, which is the point of the switch.
extern "C" int
amiga_boot_clearcolor(void)
{
	return amigaFlags().clearColor;
}

// Slow-frame threshold (ms); 0 = auto (2 x 30-frame median, floored at 20 ms).
extern "C" unsigned int
amiga_boot_profile_ms(void)
{
	return (unsigned int)amigaFlags().profileMs;
}

// Streaming diagnostics (src/core/StreamStats.*).
extern "C" int
amiga_boot_streamstats(void)
{
	return amigaFlags().streamStats;
}

extern "C" int
amiga_boot_streamstats_frames(void)
{
	return amigaFlags().streamStatsFrames;
}

extern "C" int
amiga_boot_streamstats_slow_ms(void)
{
	return amigaFlags().streamStatsSlowMs;
}

static void
STARTLOG(const char *fmt, ...)
{
	char buf[400];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	amiga_log("[boot] ");
	amiga_log(buf);
	amiga_log("\n");
}

// Shared REVC_* flag readers (forward-declared near the top of the file).
//
// A malformed switch is a configuration error, so fail fast: log the offending
// name/value and refuse to start, rather than silently guessing a path. Absent
// or empty values are not an error -- they take the caller's per-flag default.
static void
amigaEnvFatal(const char *name, const char *val)
{
	// Disable the serial mirror before logging: STARTLOG -> amigaRawWrite ->
	// amigaSerialWrite -> amigaSerialWanted -> amigaEnvBool("REVC_SERIAL"), so
	// an invalid REVC_SERIAL would re-enter this function before amigaSerOn is
	// cached and recurse until the stack overflows. The file log still gets the
	// line (amigaRawWrite writes it first), and we abort right after.
	amigaSerOn = 0;
	STARTLOG("FATAL: %s=\"%s\": expected boolean (1/y/yes/true/on or "
	         "0/n/no/off/false) or an integer", name, val);
	exit((int)RETURN_FAIL);
}

static int
amigaEnvBoolEx(const char *name, int def, int *present)
{
	char v[8];
	LONG n = GetVar((STRPTR)name, (STRPTR)v, sizeof(v) - 1, 0);
	int out = def;

	if(present != 0)
		*present = 0;
	if(n <= 0)
		return def;
	v[n] = '\0';
	if(!rfEnvParseBool(v, &out))
		amigaEnvFatal(name, v);
	if(present != 0)
		*present = 1;
	return out;
}

static int
amigaEnvBool(const char *name, int def)
{
	return amigaEnvBoolEx(name, def, 0);
}

static int
amigaEnvInt(const char *name, int def, int min, int max)
{
	char v[16];
	LONG n = GetVar((STRPTR)name, (STRPTR)v, sizeof(v) - 1, 0);
	int out = def;

	if(n <= 0)
		return def;
	v[n] = '\0';
	if(!rfEnvParseInt(v, &out))
		amigaEnvFatal(name, v);
	// A non-positive value for a positive-default threshold means "use the
	// default" (matches the old *_MS / MBLUR_INTERVAL behaviour); then clamp.
	if(out <= 0 && def > 0)
		out = def;
	if(out < min)
		out = min;
	if(out > max)
		out = max;
	return out;
}

static void
amigaLogOpen(void)
{
	char path[256];
	const char *name = "reVC.log";
	LONG n;

	// Optional REVC_LOG=<path> override (AmigaDOS env/assign).
	n = GetVar((STRPTR)"REVC_LOG", (STRPTR)path, sizeof(path) - 1, 0);
	if(n > 0){
		path[n] = '\0';
		name = path;
	}

	// Redirect stdout (printf/re3_debug go through plat::LogWrite but the game
	// and librw call printf directly). freopen() gives libnix its own handle,
	// closed exactly once at exit; the old open()+dup2() aliased one handle over
	// fds 1/2/3 and double-closed it (AN_FileReclosed, #8700000E).
	if(freopen(name, "w", stdout) == NULL){
		name = "PROGDIR:reVC.log";
		if(freopen(name, "w", stdout) == NULL){
			name = "T:reVC.log";
			if(freopen(name, "w", stdout) == NULL)
				name = 0;
		}
	}

	if(name != 0){
		// stderr too, so librw's RWERROR() (rwerror.h: fprintf(stderr,...)) lands
		// in the log instead of opening a console window.
		freopen(name, "a", stderr);
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);
		// Best-effort separate handle for the per-line Flush() (see amigaRawWrite);
		// it may not open alongside stdio on some filesystems.
		amigaLogFH = Open((STRPTR)name, MODE_OLDFILE);
	}

	amigaLogReady = (name != 0);
}

static void
amigaLogClose(void)
{
	if(amigaLogFH){
		Flush(amigaLogFH);
		Close(amigaLogFH);
		amigaLogFH = 0;
	}
	// stdout is libnix's; do NOT close it here (that would double-close the
	// handle when libnix's exit() closes it -> AN_FileReclosed, #8700000E).
	amigaLogReady = 0;
}

// The toolchain's generic libm *f functions are soft-float (result in d0)
// while every -mhard-float caller reads fp0; amiga_libfix.c routes each *f
// through the double (FPU) functions. If that object is ever dropped from a
// build, matrices go to ~1e15 and memory is corrupted, so probe the overrides
// at boot and say plainly in the log whether they are active. Inputs are
// volatile so gcc cannot constant-fold the calls, and the build defines
// -fno-builtin-sinf/-fno-builtin-cosf so it cannot turn them back into
// builtins either.
static void
amigaLogFloatLibfix(void)
{
	volatile float h = 0.5f, one = 1.0f, two = 2.0f, three = 3.0f, four = 4.0f;
	volatile float p27 = 2.7f, p25 = 2.5f, m25 = -2.5f, m27 = -2.7f, m1 = -1.0f;
	volatile float seven5 = 7.5f, ten = 10.0f, eight = 8.0f, ee = 2.7182818f, k1000 = 1000.0f;
	volatile float m275 = -2.75f, nzero = -0.0f;
	char bad[160];
	int n = 0;
	unsigned int ub;
	float v;
#define FLCK(name, expr, want, tol) do { \
	float g_ = (expr); v = g_ - (want); if(v < 0.0f) v = -v; \
	if(!(v <= (tol)) && n < (int)sizeof(bad) - 24) \
		n += snprintf(bad + n, sizeof(bad) - (size_t)n, "%s=%d ", name, (int)(g_ * 1000.0f)); \
	} while(0)

	bad[0] = '\0';
	FLCK("sinf", sinf(h), 0.479426f, 0.001f);
	FLCK("cosf", cosf(h), 0.877583f, 0.001f);
	FLCK("tanf", tanf(h), 0.546302f, 0.001f);
	FLCK("asinf", asinf(h), 0.523599f, 0.001f);
	FLCK("acosf", acosf(h), 1.047198f, 0.001f);
	FLCK("atanf", atanf(one), 0.785398f, 0.001f);
	FLCK("atan2f", atan2f(one, one), 0.785398f, 0.001f);
	FLCK("sqrtf", sqrtf(four), 2.0f, 0.001f);
	FLCK("floorf", floorf(m25), -3.0f, 0.0f);
	FLCK("ceilf", ceilf(m25), -2.0f, 0.0f);
	FLCK("rintf", rintf(p27), 3.0f, 0.0f);
	FLCK("roundf", roundf(p25), 3.0f, 0.0f);
	FLCK("truncf", truncf(m27), -2.0f, 0.0f);
	FLCK("fmodf", fmodf(seven5, two), 1.5f, 0.0f);
	FLCK("powf", powf(two, ten), 1024.0f, 0.01f);
	FLCK("expf", expf(one), 2.718282f, 0.001f);
	FLCK("logf", logf(ee), 1.0f, 0.001f);
	FLCK("log10f", log10f(k1000), 3.0f, 0.001f);
	FLCK("log2f", log2f(eight), 3.0f, 0.001f);
	FLCK("copysignf", copysignf(three, m1), -3.0f, 0.0f);
	FLCK("sinhf", sinhf(one), 1.175201f, 0.001f);
	FLCK("coshf", coshf(one), 1.543081f, 0.001f);
	FLCK("tanhf", tanhf(one), 0.761594f, 0.001f);
	FLCK("ldexpf", ldexpf(one, 10), 1024.0f, 0.0f);
	FLCK("fmaxf", fmaxf(two, three), 3.0f, 0.0f);
	FLCK("fminf", fminf(two, three), 2.0f, 0.0f);
	v = fabsf(nzero);
	memcpy(&ub, &v, sizeof(ub));
	if(ub != 0u && n < (int)sizeof(bad) - 24)
		n += snprintf(bad + n, sizeof(bad) - (size_t)n, "fabsf=%08x ", ub);
	{ float ip, fr = modff(m275, &ip);
	  if((!(fr > -0.751f && fr < -0.749f) || ip != -2.0f) && n < (int)sizeof(bad) - 32)
		n += snprintf(bad + n, sizeof(bad) - (size_t)n, "modff=%d/%d ",
		              (int)(fr * 1000.0f), (int)(ip * 1000.0f)); }
	{ int ex; float m = frexpf(eight, &ex);
	  if((m != 0.5f || ex != 4) && n < (int)sizeof(bad) - 32)
		n += snprintf(bad + n, sizeof(bad) - (size_t)n, "frexpf=%d/%d ",
		              (int)(m * 1000.0f), ex); }
#undef FLCK

	STARTLOG("float-libfix: %s%s", n ? "MISSING/BROKEN " : "ok", bad);
}

// Enumerate the global environment (ENV:) and log each variable, so a capture
// records exactly which REVC_* switches (and anything else) were in effect.
// Top-level entries only; names with '/' (ENV: subdirectories) are rare here.
static void
amigaLogEnvVars(void)
{
	BPTR lock = Lock((STRPTR)"ENV:", ACCESS_READ);
	struct FileInfoBlock *fib;
	char val[96];
	int shown = 0;

	if(lock == 0){
		STARTLOG("env: (cannot lock ENV:)");
		return;
	}

	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if(fib == NULL){
		UnLock(lock);
		STARTLOG("env: (cannot allocate FileInfoBlock)");
		return;
	}

	if(Examine(lock, fib)){
		while(ExNext(lock, fib)){
			LONG n = GetVar((STRPTR)fib->fib_FileName, (STRPTR)val,
			                sizeof(val) - 1, 0);
			if(n > 0){
				val[n] = '\0';
				STARTLOG("env: %s = %s", fib->fib_FileName, val);
				shown++;
			}
		}
	}

	FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	if(shown == 0)
		STARTLOG("env: (none)");
}

static void
amigaLogEnvironment(int argc, char **argv)
{
	struct Task *me = (struct Task *)FindTask(NULL);
	char cwd[256];

	cwd[0] = '\0';
	if(getcwd(cwd, sizeof(cwd)) == NULL)
		strcpy(cwd, "?");

	STARTLOG("reVC AmigaOS 68k - startup log");
	STARTLOG("build id: %s", g_GIT_SHA1);
	amigaLogFloatLibfix();
	STARTLOG("log: %s", amigaLogReady ? "file" : "console (open failed)");
	STARTLOG("kickstart: %d.%d", (int)SysBase->LibNode.lib_Version,
	         (int)SysBase->LibNode.lib_Revision);
	STARTLOG("cpu AttnFlags: 0x%04x", (unsigned)SysBase->AttnFlags);
	STARTLOG("cwd: %s", cwd);
	STARTLOG("argc: %d", argc);
	STARTLOG("avail mem: %u", (unsigned)AvailMem(MEMF_ANY));
	{
		// tc_SPReg is only reliably written on task switches, so read the real
		// stack pointer for the free-byte figure.
		void *sp = NULL;
		__asm__ volatile ("move.l %%sp,%0" : "=r"(sp));
		STARTLOG("stack: sp=0x%08x upper=0x%08x lower=0x%08x (free %u of %u)",
		         (unsigned)(ULONG)sp, (unsigned)(ULONG)me->tc_SPUpper,
		         (unsigned)(ULONG)me->tc_SPLower,
		         (unsigned)((ULONG)me->tc_SPUpper - (ULONG)sp),
		         (unsigned)((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower));
	}

	amigaLogEnvVars();
}

#define MAX_SUBSYSTEMS (16)

long _dwOperatingSystemVersion;
size_t _dwMemAvailPhys;
RwUInt32 gGameState;

rw::EngineOpenParams openParams;

static RwBool ForegroundApp = TRUE;
static RwBool RwInitialised = FALSE;

static RwSubSystemInfo GsubSysInfo[MAX_SUBSYSTEMS];
static RwInt32 GnumSubSystems = 0;
static RwInt32 GcurSel = 0, GcurSelVM = 0;
static RwChar **_VMList;
static RwVideoMode *_VMModes;
static RwInt32 _VMListCount = 0;
static RwBool useDefault;

static RwBool defaultFullscreenRes = TRUE;
static RwBool amigaWindowClosed = FALSE;

// Native RTG screen/window state (MiniGL owns the window; we only borrow the
// handle for IDCMP).
struct AmigaGlobalType {
	struct Window *window;
	RwV2d lastMousePos;
};
static AmigaGlobalType Amiga;

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

static struct timerequest TimerReq;
struct Device *TimerBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

// ---------------------------------------------------------------------------
// Application-owned window (windowed mode)
//
// Fixed-size window on the Workbench screen. MiniGL adopts it with
// mglCreateContextFromWindow; the render target and the visible client area
// are therefore the same size and no scaling/present pass is needed.
// ---------------------------------------------------------------------------

// CUSTOM_FRONTEND_OPTIONS reads/writes int32-backed booleans through an int8*,
// i.e. the first byte. On big-endian 68k that is the most significant byte, so
// the field must carry the boolean there. LOAD_INI_SETTINGS compiles out the
// gta_vc.set writer, keeping this representation confined to reVC.ini and the
// frontend.
#ifndef LOAD_INI_SETTINGS
#error "amigaIniBool expects LOAD_INI_SETTINGS to compile out gta_vc.set"
#endif
static RwInt32
amigaIniBool(int on)
{
	RwInt32 v = 0;
	((RwUInt8*)&v)[0] = on ? 1 : 0;
	return v;
}

static struct Screen *amigaAppScreen = nil;
static const char *amigaAppTitle = "reVC";
static int amigaWindowFocused = 0;

// Fullscreen has no application window to lose focus: the driver's input
// backdrop is always owned by the game. In windowed mode only the active
// window may feed controls.
static bool
amigaInputHasFocus(void)
{
	return !amigaWindowed || amigaWindowFocused != 0;
}

static int
amigaWindowClientW(struct Window *w)
{
	return (int)w->Width - (int)w->BorderLeft - (int)w->BorderRight;
}

static int
amigaWindowClientH(struct Window *w)
{
	return (int)w->Height - (int)w->BorderTop - (int)w->BorderBottom;
}

static void amigaWindowClose(void);

static struct Window *
amigaWindowOpenAt(int innerW, int innerH, int left, int top)
{
	struct TagItem tags[16];
	int n = 0;

	tags[n].ti_Tag = WA_PubScreen;	tags[n++].ti_Data = (ULONG)amigaAppScreen;
	tags[n].ti_Tag = WA_Left;	tags[n++].ti_Data = (ULONG)left;
	tags[n].ti_Tag = WA_Top;	tags[n++].ti_Data = (ULONG)top;
	tags[n].ti_Tag = WA_InnerWidth;	tags[n++].ti_Data = (ULONG)innerW;
	tags[n].ti_Tag = WA_InnerHeight;tags[n++].ti_Data = (ULONG)innerH;
	tags[n].ti_Tag = WA_AutoAdjust;tags[n++].ti_Data = TRUE;
	tags[n].ti_Tag = WA_Title;	tags[n++].ti_Data = (ULONG)amigaAppTitle;
	tags[n].ti_Tag = WA_DragBar;	tags[n++].ti_Data = TRUE;
	tags[n].ti_Tag = WA_DepthGadget;tags[n++].ti_Data = TRUE;
	tags[n].ti_Tag = WA_CloseGadget;tags[n++].ti_Data = TRUE;
	tags[n].ti_Tag = WA_Activate;	tags[n++].ti_Data = TRUE;
	tags[n].ti_Tag = WA_ReportMouse;tags[n++].ti_Data = TRUE;
	tags[n].ti_Tag = WA_IDCMP;
	tags[n++].ti_Data = IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
	                    IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
	                    IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW;
	tags[n].ti_Tag = WA_Flags;
	tags[n++].ti_Data = WFLG_ACTIVATE | WFLG_REPORTMOUSE | WFLG_RMBTRAP;
	tags[n].ti_Tag = TAG_DONE;	tags[n++].ti_Data = 0;

	return OpenWindowTagList(nil, tags);
}

static struct Window *
amigaWindowOpen(int logicalW, int logicalH, const char *title)
{
	int screenW, screenH, left, top;

	if(amigaAppWindow != nil)
		return amigaAppWindow;
	if(IntuitionBase == nil)
		return nil;

	amigaAppScreen = LockPubScreen(nil);
	if(amigaAppScreen == nil){
		STARTLOG("window: LockPubScreen failed");
		return nil;
	}

	amigaAppTitle = title ? title : "reVC";
	screenW = amigaAppScreen->Width;
	screenH = amigaAppScreen->Height;
	left = (screenW - logicalW) / 2;
	top = (screenH - logicalH) / 2;
	if(left < 0) left = 0;
	if(top < 0) top = 0;

	amigaAppWindow = amigaWindowOpenAt(logicalW, logicalH, left, top);
	if(amigaAppWindow == nil){
		STARTLOG("window: OpenWindowTagList failed");
		UnlockPubScreen(nil, amigaAppScreen);
		amigaAppScreen = nil;
		return nil;
	}
	amigaWindowClosed = FALSE;
	amigaWindowFocused = 1;
	WindowToFront(amigaAppWindow);
	ActivateWindow(amigaAppWindow);

	STARTLOG("window: client %dx%d at %d,%d (screen %dx%d, fixed size)",
	         amigaWindowClientW(amigaAppWindow), amigaWindowClientH(amigaAppWindow),
	         (int)amigaAppWindow->LeftEdge, (int)amigaAppWindow->TopEdge,
	         screenW, screenH);
	return amigaAppWindow;
}

static void
amigaWindowClose(void)
{
	if(amigaAppWindow != nil){
		CloseWindow(amigaAppWindow);
		amigaAppWindow = nil;
		Amiga.window = nil;
	}
	if(amigaAppScreen != nil){
		UnlockPubScreen(nil, amigaAppScreen);
		amigaAppScreen = nil;
	}
	amigaWindowFocused = 0;
}

extern "C" int
rwminiHostGetModes(int *widths, int *heights, int maxModes)
{
	ULONG modeID;
	int entries = 0;
	int n = 0;
	bool truncated = false;

	if(widths == nil || heights == nil || maxModes <= 0 || GfxBase == nil)
		return 0;

	// The graphics display database is the same list ScreenMode prefs walks.
	// It holds one entry per depth/format (and per board), so several entries
	// share a resolution. The MiniGL backend renders in 32-bit and keys its
	// synthetic modes by width x height, so collapse duplicates here;
	// otherwise they spend the caller's maxModes budget before the real list
	// is built.
	for(modeID = NextDisplayInfo(INVALID_ID); modeID != INVALID_ID;
	    modeID = NextDisplayInfo(modeID)){
		struct DisplayInfo di;
		struct DimensionInfo dim;
		int w, h, i;

		if(!GetDisplayInfoData(nil, (APTR)&di, sizeof(di), DTAG_DISP, modeID) ||
		   !GetDisplayInfoData(nil, (APTR)&dim, sizeof(dim), DTAG_DIMS, modeID))
			continue;

		w = (int)dim.Nominal.MaxX - (int)dim.Nominal.MinX + 1;
		h = (int)dim.Nominal.MaxY - (int)dim.Nominal.MinY + 1;
		if(!(di.PropertyFlags & DIPF_IS_FOREIGN) || w <= 0 || h <= 0 ||
		   (di.RedBits + di.GreenBits + di.BlueBits < 24 && dim.MaxDepth < 24))
			continue;

		entries++;

		for(i = 0; i < n; i++)
			if(widths[i] == w && heights[i] == h)
				break;
		if(i < n)
			continue;	// same resolution, already listed

		if(n >= maxModes){
			truncated = true;
			continue;
		}

		// Keep the returned list ascending by area, then width, so the
		// frontend and psSelectDevice() see a stable order regardless of how
		// the database enumerates its entries.
		i = n;
		while(i > 0 && (widths[i - 1] * heights[i - 1] > w * h ||
		                (widths[i - 1] * heights[i - 1] == w * h &&
		                 widths[i - 1] > w))){
			widths[i] = widths[i - 1];
			heights[i] = heights[i - 1];
			i--;
		}
		widths[i] = w;
		heights[i] = h;
		n++;
	}

	STARTLOG("displaydb: %d RTG truecolour mode%s, %d unique size%s%s",
	         entries, entries == 1 ? "" : "s",
	         n, n == 1 ? "" : "s",
	         truncated ? " (truncated)" : "");
	return n;
}

static void
amigaOpenTimer(void)
{
	if(TimerBase != nil)
		return;
	if(OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&TimerReq, 0) == 0)
		TimerBase = TimerReq.tr_node.io_Device;
}

static void
amigaCloseTimer(void)
{
	if(TimerBase != nil){
		CloseDevice((struct IORequest *)&TimerReq);
		TimerBase = nil;
	}
}

double
psTimer(void)
{
	struct EClockVal ev;
	ULONG freq;
	if(TimerBase == nil)
		return 0.0;
	freq = ReadEClock(&ev);
	if(freq == 0)
		return 0.0;
	// 64-bit tick count -> milliseconds
	{
		unsigned long long ticks = ((unsigned long long)ev.ev_hi << 32) | (unsigned long long)ev.ev_lo;
		return (double)ticks * 1000.0 / (double)freq;
	}
}

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------

void
_psCreateFolder(const char *path)
{
	BPTR lock = Lock((STRPTR)path, ACCESS_READ);
	if(lock != 0)
		UnLock(lock);
	else
		CreateDir((STRPTR)path);
}

const char *
_psGetUserFilesFolder()
{
	static char szUserFiles[256];
	strcpy(szUserFiles, "PROGDIR:userfiles");
	_psCreateFolder(szUserFiles);
	return szUserFiles;
}

RwBool
psInstallFileSystem(void)
{
	return TRUE;
}

RwMemoryFunctions *
psGetMemoryFunctions(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return &memFuncs;
#else
	return nil;
#endif
}

// ---------------------------------------------------------------------------
// Video modes
// ---------------------------------------------------------------------------

RwInt32
_psGetNumVideModes()
{
	return RwEngineGetNumVideoModes();
}

static RwBool
_psFreeVideoModeList()
{
	RwInt32 numModes, i;

	if(_VMList == nil)
		return TRUE;

	numModes = _VMListCount;
	for(i = 0; i < numModes; i++)
		if(_VMList[i])
			RwFree(_VMList[i]);

	RwFree(_VMList);
	_VMList = nil;
	RwFree(_VMModes);
	_VMModes = nil;
	_VMListCount = 0;

	return TRUE;
}

RwChar **
_psGetVideoModeList()
{
	RwInt32 numModes, i;

	if(_VMList != nil)
		return _VMList;

	numModes = RwEngineGetNumVideoModes();

	_VMList = (RwChar **)RwCalloc(numModes, sizeof(RwChar*));
	if(_VMList == nil)
		return nil;
	_VMListCount = numModes;
	_VMModes = (RwVideoMode *)RwCalloc(numModes, sizeof(RwVideoMode));

	for(i = 0; i < numModes; i++){
		RwVideoMode vm;
		RwEngineGetVideoModeInfo(&vm, i);
		if(_VMModes != nil)
			_VMModes[i] = vm;
		if(vm.flags & rwVIDEOMODEEXCLUSIVE){
			_VMList[i] = (RwChar*)RwCalloc(100, sizeof(RwChar));
			if(_VMList[i])
				rwsprintf(_VMList[i], "%d X %d X %d", vm.width, vm.height, vm.depth);
		}else
			_VMList[i] = nil;
	}

	return _VMList;
}

// Requested startup video size from reVC.ini (loaded by psInitialize).
// REVC_MODE=WxH is an explicit debug/recovery override in both modes.
static void
amigaRequestedSize(RwInt32 *w, RwInt32 *h)
{
	RwInt32 ow = FrontEndMenuManager.m_nPrefsWidth;
	RwInt32 oh = FrontEndMenuManager.m_nPrefsHeight;
	char m[32];
	LONG n;

	if(ow <= 0 || oh <= 0){
		ow = 640;
		oh = 480;
	}
	// REVC_MODE is an explicit debug/recovery override for both modes.
	n = GetVar((STRPTR)"REVC_MODE", (STRPTR)m, sizeof(m)-1, 0);
	if(n > 0){
		RwInt32 cw = 0, ch = 0;
		m[n] = '\0';
		if(sscanf(m, "%dx%d", &cw, &ch) == 2 && cw > 0 && ch > 0){
			ow = cw;
			oh = ch;
		}
	}
	*w = ow;
	*h = oh;
}

// Choose the window size and open the fixed window for windowed mode.
// videoMode < 0 means startup, where no frontend mode has been selected yet.
static bool
amigaWindowedPrepare(RwInt32 videoMode, RwInt32 *w, RwInt32 *h, const char *title)
{
	if(videoMode >= 0 && videoMode < _VMListCount && _VMModes != nil &&
	   _VMModes[videoMode].width > 0 && _VMModes[videoMode].height > 0){
		*w = _VMModes[videoMode].width;
		*h = _VMModes[videoMode].height;
		FrontEndMenuManager.m_nPrefsWidth = *w;
		FrontEndMenuManager.m_nPrefsHeight = *h;
	}else{
		amigaRequestedSize(w, h);
	}
	if(amigaWindowOpen((int)*w, (int)*h, title) == nil)
		return false;
	// Intuition may have fitted the window to the screen.
	*w = amigaWindowClientW(amigaAppWindow);
	*h = amigaWindowClientH(amigaAppWindow);
	FrontEndMenuManager.m_nPrefsWidth = *w;
	FrontEndMenuManager.m_nPrefsHeight = *h;
	return true;
}

// Prepare openParams for windowed mode, falling back to runtime-only
// fullscreen if the window cannot be created. Never rewrites the persisted
// Windowed preference on an automatic fallback.
static void
amigaPrepareWindowed(RwInt32 videoMode, rw::EngineOpenParams *params)
{
	RwInt32 w, h;

	if(amigaWindowedPrepare(videoMode, &w, &h, params->windowtitle)){
		params->width = w;
		params->height = h;
		params->window = amigaAppWindow;
		return;
	}

	STARTLOG("window: windowed open failed, falling back to runtime fullscreen");
	amigaWindowed = 0;
	amigaForcedFullscreen = true;
}

RwBool
psSelectDevice()
{
	RwVideoMode vm;
	RwInt32 subSysNum;
	RwInt32 winW = 640, winH = 480;

	STARTLOG("psSelectDevice: enter (useDefault=%d)", (int)useDefault);

	// The game's [VideoMode] Windowed setting is authoritative unless a
	// transient window failure forced runtime fullscreen.
	amigaWindowed = !amigaForcedFullscreen &&
	                FrontEndMenuManager.m_nPrefsWindowed != 0;

	if(!useDefault){
		GnumSubSystems = RwEngineGetNumSubSystems();
		STARTLOG("psSelectDevice: subsystems=%d", (int)GnumSubSystems);
		if(!GnumSubSystems)
			return FALSE;

		GnumSubSystems = (GnumSubSystems > MAX_SUBSYSTEMS) ? MAX_SUBSYSTEMS : GnumSubSystems;

		for(subSysNum = 0; subSysNum < GnumSubSystems; subSysNum++)
			RwEngineGetSubSystemInfo(&GsubSysInfo[subSysNum], subSysNum);

		GcurSel = RwEngineGetCurrentSubSystem();
	}

	if(!RwEngineSetSubSystem(GcurSel)){
		STARTLOG("psSelectDevice: RwEngineSetSubSystem(%d) failed", (int)GcurSel);
		return FALSE;
	}

	// Desired size: stored prefs, overridable by REVC_MODE=WxH. Needed in the
	// useDefault path too, where windowed mode still uses the synthetic mode 0.
	amigaRequestedSize(&winW, &winH);

	if(!useDefault){
		if(amigaWindowed){
			// Windowed: no fullscreen RTG mode; the host-owned Workbench window
			// is the drawable at winW x winH.
			GcurSelVM = 0;
			FrontEndMenuManager.m_nDisplayVideoMode = 0;
			FrontEndMenuManager.m_nPrefsVideoMode = 0;
			FrontEndMenuManager.m_nPrefsWindowed = amigaIniBool(1);
			STARTLOG("psSelectDevice: windowed %dx%d", (int)winW, (int)winH);
		}else{
			// MiniGL only exposes fullscreen RTG modes here. Pick the mode closest
			// to the desired size (largest that does not exceed it), otherwise
			// the first exclusive mode.
			RwInt32 first = -1;
			RwInt32 best = -1, bestDist = 0;
			RwInt32 numModes = RwEngineGetNumVideoModes();
			STARTLOG("psSelectDevice: video modes=%d", (int)numModes);
			for(GcurSelVM = 0; GcurSelVM < numModes; GcurSelVM++){
				RwInt32 dist;
				RwEngineGetVideoModeInfo(&vm, GcurSelVM);
				STARTLOG("psSelectDevice:   mode %d = %dx%dx%d flags=0x%x", (int)GcurSelVM,
				         (int)vm.width, (int)vm.height, (int)vm.depth, (unsigned)vm.flags);
				if(!(vm.flags & rwVIDEOMODEEXCLUSIVE))
					continue;
				if(first < 0)
					first = GcurSelVM;
				if(vm.width > winW || vm.height > winH)
					continue;
				dist = (winW - vm.width) + (winH - vm.height);
				if(best < 0 || dist < bestDist){
					best = GcurSelVM;
					bestDist = dist;
				}
			}
			GcurSelVM = (best >= 0) ? best : first;
			if(GcurSelVM < 0){
				STARTLOG("psSelectDevice: no exclusive mode found (numModes=%d)", (int)numModes);
				return FALSE;
			}
			FrontEndMenuManager.m_nDisplayVideoMode = GcurSelVM;
			FrontEndMenuManager.m_nPrefsVideoMode = GcurSelVM;
			if(!amigaForcedFullscreen)
				FrontEndMenuManager.m_nPrefsWindowed = amigaIniBool(0);
		}
	}

	if(amigaWindowed){
		// The synthetic mode 0 carries the size just parsed from the selected
		// mode; the fullscreen mode list is built later, so its indices cannot
		// be reused here.
		GcurSelVM = 0;
		FrontEndMenuManager.m_nDisplayVideoMode = 0;
		FrontEndMenuManager.m_nPrefsVideoMode = 0;
	}

	RwEngineGetVideoModeInfo(&vm, GcurSelVM);
	if(amigaWindowed){
		vm.width = winW;
		vm.height = winH;
	}
	STARTLOG("psSelectDevice: mode %d = %dx%dx%d", (int)GcurSelVM,
	         (int)vm.width, (int)vm.height, (int)vm.depth);

	if(!useDefault)
		FrontEndMenuManager.m_nCurrOption = 0;

	if(!RwEngineSetVideoMode(GcurSelVM)){
		STARTLOG("psSelectDevice: RwEngineSetVideoMode failed");
		return FALSE;
	}

	RsGlobal.maximumWidth = vm.width;
	RsGlobal.maximumHeight = vm.height;
	RsGlobal.width = vm.width;
	RsGlobal.height = vm.height;

	if(!amigaForcedFullscreen){
		FrontEndMenuManager.m_nPrefsWidth = vm.width;
		FrontEndMenuManager.m_nPrefsHeight = vm.height;
		FrontEndMenuManager.m_nPrefsDepth = vm.depth;
	}

	STARTLOG("psSelectDevice: done");
	return TRUE;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	if(!RwCameraBeginUpdate(Scene.camera)){
		ForegroundApp = FALSE;
		RsEventHandler(rsACTIVATE, (void *)FALSE);
		return FALSE;
	}
	return TRUE;
}

void
psCameraShowRaster(RwCamera *camera)
{
	// librw's MINI device maps showRaster to mglSwitchDisplay(); the flag is
	// ignored (MiniGL swaps on its own VBL).
	RwCameraShowRaster(camera, nil, rwRASTERFLIPWAITVSYNC);
}

RwImage *
psGrabScreen(RwCamera *pCamera)
{
	// MiniGL has no camera-raster readback path in the MINI backend
	// (rasterLock(LOCKREAD) returns an uninitialized buffer). Screenshots are
	// intentionally unsupported on this fork: return nil so RwGrabScreen()
	// reports failure and writes no file instead of emitting garbage pixels.
	(void)pCamera;
	return nil;
}

// ---------------------------------------------------------------------------
// Input: Amiga rawkey -> RsKeyCodes
// ---------------------------------------------------------------------------

// Canonical Amiga RAWKEY codes (hardware/rawkeys.h is not shipped with this
// NDK). The IDCMP_RAWKEY Code has bit 7 (IECODE_UP_PREFIX) set on key-up.
enum {
	AK_BACKQUOTE = 0x00, AK_1 = 0x01, AK_2 = 0x02, AK_3 = 0x03, AK_4 = 0x04,
	AK_5 = 0x05, AK_6 = 0x06, AK_7 = 0x07, AK_8 = 0x08, AK_9 = 0x09,
	AK_0 = 0x0A, AK_MINUS = 0x0B, AK_EQUAL = 0x0C, AK_BACKSLASH = 0x0D,
	AK_Q = 0x10, AK_W = 0x11, AK_E = 0x12, AK_R = 0x13, AK_T = 0x14,
	AK_Y = 0x15, AK_U = 0x16, AK_I = 0x17, AK_O = 0x18, AK_P = 0x19,
	AK_LBRACKET = 0x1A, AK_RBRACKET = 0x1B,
	AK_A = 0x20, AK_S = 0x21, AK_D = 0x22, AK_F = 0x23, AK_G = 0x24,
	AK_H = 0x25, AK_J = 0x26, AK_K = 0x27, AK_L = 0x28, AK_SEMI = 0x29,
	AK_QUOTE = 0x2A,
	AK_Z = 0x31, AK_X = 0x32, AK_C = 0x33, AK_V = 0x34, AK_B = 0x35,
	AK_N = 0x36, AK_M = 0x37, AK_COMMA = 0x38, AK_PERIOD = 0x39, AK_SLASH = 0x3A,
	AK_SPACE = 0x40, AK_BACKSPACE = 0x41, AK_TAB = 0x42, AK_NP_ENTER = 0x43,
	AK_RETURN = 0x44, AK_ESC = 0x45, AK_DEL = 0x46,
	AK_UP = 0x4C, AK_DOWN = 0x4D, AK_RIGHT = 0x4E, AK_LEFT = 0x4F,
	AK_F1 = 0x50, AK_F2 = 0x51, AK_F3 = 0x52, AK_F4 = 0x53, AK_F5 = 0x54,
	AK_F6 = 0x55, AK_F7 = 0x56, AK_F8 = 0x57, AK_F9 = 0x58, AK_F10 = 0x59,
	AK_LSHIFT = 0x60, AK_RSHIFT = 0x61, AK_CAPSLOCK = 0x62, AK_CTRL = 0x63,
	AK_LALT = 0x64, AK_RALT = 0x65
};

#define RAWKEY_TABLE_SIZE 0x80
static int keymap[RAWKEY_TABLE_SIZE];
// Keys currently held, so focus loss can synthesize the key-ups Intuition will
// not deliver once another window is active.
static bool amigaKeyDown[RAWKEY_TABLE_SIZE];

bool lshiftStatus = false;
bool rshiftStatus = false;

static void
initkeymap(void)
{
	int i;
	for(i = 0; i < RAWKEY_TABLE_SIZE; i++)
		keymap[i] = rsNULL;

	keymap[AK_SPACE] = ' ';
	keymap[AK_QUOTE] = '\'';
	keymap[AK_COMMA] = ',';
	keymap[AK_MINUS] = '-';
	keymap[AK_PERIOD] = '.';
	keymap[AK_SLASH] = '/';
	keymap[AK_0] = '0'; keymap[AK_1] = '1'; keymap[AK_2] = '2';
	keymap[AK_3] = '3'; keymap[AK_4] = '4'; keymap[AK_5] = '5';
	keymap[AK_6] = '6'; keymap[AK_7] = '7'; keymap[AK_8] = '8';
	keymap[AK_9] = '9';
	keymap[AK_SEMI] = ';';
	keymap[AK_EQUAL] = '=';
	keymap[AK_A] = 'A'; keymap[AK_B] = 'B'; keymap[AK_C] = 'C';
	keymap[AK_D] = 'D'; keymap[AK_E] = 'E'; keymap[AK_F] = 'F';
	keymap[AK_G] = 'G'; keymap[AK_H] = 'H'; keymap[AK_I] = 'I';
	keymap[AK_J] = 'J'; keymap[AK_K] = 'K'; keymap[AK_L] = 'L';
	keymap[AK_M] = 'M'; keymap[AK_N] = 'N'; keymap[AK_O] = 'O';
	keymap[AK_P] = 'P'; keymap[AK_Q] = 'Q'; keymap[AK_R] = 'R';
	keymap[AK_S] = 'S'; keymap[AK_T] = 'T'; keymap[AK_U] = 'U';
	keymap[AK_V] = 'V'; keymap[AK_W] = 'W'; keymap[AK_X] = 'X';
	keymap[AK_Y] = 'Y'; keymap[AK_Z] = 'Z';
	keymap[AK_LBRACKET] = '[';
	keymap[AK_BACKSLASH] = '\\';
	keymap[AK_RBRACKET] = ']';
	keymap[AK_BACKQUOTE] = '`';

	keymap[AK_ESC] = rsESC;
	keymap[AK_RETURN] = rsENTER;
	keymap[AK_NP_ENTER] = rsPADENTER;
	keymap[AK_TAB] = rsTAB;
	keymap[AK_BACKSPACE] = rsBACKSP;
	keymap[AK_DEL] = rsDEL;

	keymap[AK_RIGHT] = rsRIGHT;
	keymap[AK_LEFT] = rsLEFT;
	keymap[AK_DOWN] = rsDOWN;
	keymap[AK_UP] = rsUP;

	keymap[AK_CAPSLOCK] = rsCAPSLK;

	keymap[AK_F1] = rsF1; keymap[AK_F2] = rsF2; keymap[AK_F3] = rsF3;
	keymap[AK_F4] = rsF4; keymap[AK_F5] = rsF5; keymap[AK_F6] = rsF6;
	keymap[AK_F7] = rsF7; keymap[AK_F8] = rsF8; keymap[AK_F9] = rsF9;
	keymap[AK_F10] = rsF10;

	keymap[AK_LSHIFT] = rsLSHIFT;
	keymap[AK_RSHIFT] = rsRSHIFT;
	keymap[AK_CTRL] = rsLCTRL;
	keymap[AK_LALT] = rsLALT;
	keymap[AK_RALT] = rsRALT;
}

void
_InputTranslateShiftKeyUpDown(RsKeyCodes *rs)
{
	RsKeyboardEventHandler(lshiftStatus ? rsKEYDOWN : rsKEYUP, &(*rs = rsLSHIFT));
	RsKeyboardEventHandler(rshiftStatus ? rsKEYDOWN : rsKEYUP, &(*rs = rsRSHIFT));
}

static void
AmigaHandleRawKey(UWORD code)
{
	bool up = (code & IECODE_UP_PREFIX) != 0;
	int raw = code & 0x7F;
	RsKeyCodes ks;

	// Controls belong to the game only while its window is active. Intuition
	// normally stops delivering keys to an inactive window, but a key-up can
	// still be lost on the focus change; amigaReleaseInput() synthesizes those.
	if(!amigaInputHasFocus())
		return;

	// F10 cycles the profiler overlay display (hidden -> compact -> detailed)
	// only while profiling is enabled; tracing continues regardless of the
	// display state, so a hidden overlay gives overlay-free A/B numbers.
	// Shift+F10 snapshots the current 120-frame ring to profile_frames.txt
	// immediately (classic Amiga keyboards have no F11/F12, and the exit dump is
	// overwritten by the menu in under a second of ring time). The key-up is
	// consumed too (the game must not see an unmatched up), and when REVC_PROFILE
	// is off the key is forwarded unchanged.
	if(raw == AK_F10 && amiga_boot_profile()){
		if(!up){
			if(lshiftStatus || rshiftStatus)
				fpDumpRingNow();
			else
				fp::fpCycleOverlay();
		}
		STARTLOG("prof: F10%s up=%d -> overlay page %d",
		         (lshiftStatus || rshiftStatus) ? "+shift" : "",
		         (int)up, (int)fp::fpOverlayMode());
		return;
	}

	ks = (RsKeyCodes)keymap[raw];

	if(raw == AK_LSHIFT)
		lshiftStatus = !up;
	if(raw == AK_RSHIFT)
		rshiftStatus = !up;

	if(ks == rsNULL)
		return;

	amigaKeyDown[raw] = !up;
	if(up)
		RsKeyboardEventHandler(rsKEYUP, &ks);
	else
		RsKeyboardEventHandler(rsKEYDOWN, &ks);
}

void
_InputInitialiseJoys()
{
	// No joystick enumeration yet (M5).
}

void
joysChangeCB(int jid, int event)
{
	(void)jid; (void)event;
}

// ---------------------------------------------------------------------------
// Relative mouse input
//
// An input.device handler accumulates the signed hardware movement
// (IECLASS_RAWMOUSE ie_position.ie_xy) and, while gameplay owns the mouse,
// zeroes it before Intuition can move the OS pointer. This gives unclamped
// relative mouse-look without needing a pointer warp, which AmigaOS does not
// provide. If input.device cannot be opened, the absolute IDCMP path remains
// the fallback (and always drives the menu's software cursor).
// ---------------------------------------------------------------------------

static struct MsgPort *amigaInputPort = 0;
static struct IOStdReq *amigaInputReq = 0;
static struct Interrupt amigaInputHandler;
static int amigaInputOpen = 0;		// 1 = input.device is open
static int amigaInputState = 0;		// 1 = handler installed

// Written by the input.device handler, drained once per frame by the main task.
// `int` (not WORD) so a long stall cannot wrap the accumulator.
static volatile int amigaRelMouseX = 0;
static volatile int amigaRelMouseY = 0;

static struct Window *amigaInputWindow = 0;
static volatile int amigaSuppressMouse = 0;

static void
amigaInputShutdown(void)
{
	if(amigaInputState != 0){
		amigaInputReq->io_Data = (void *)&amigaInputHandler;
		amigaInputReq->io_Command = IND_REMHANDLER;
		DoIO((struct IORequest *)amigaInputReq);
		amigaInputState = 0;
	}

	if(amigaInputOpen){
		CloseDevice((struct IORequest *)amigaInputReq);
		amigaInputOpen = 0;
	}
	if(amigaInputReq){
		DeleteStdIO(amigaInputReq);
		amigaInputReq = 0;
	}
	if(amigaInputPort){
		DeletePort(amigaInputPort);
		amigaInputPort = 0;
	}
	amigaInputWindow = 0;
	amigaSuppressMouse = 0;
	amigaRelMouseX = 0;
	amigaRelMouseY = 0;
}

// Runs in input.device's context (a0 = event chain, a1 = is_Data). Only integer
// bookkeeping here; no game calls, no allocation.
static struct InputEvent *
amigaInputHandlerCode(register struct InputEvent *events __asm("a0"),
                      register APTR data __asm("a1"))
{
	struct InputEvent *ev;
	struct Window *win = amigaInputWindow;

	(void)data;

	if(win == 0)
		return events;

	for(ev = events; ev != 0; ev = ev->ie_NextEvent){
		if(ev->ie_Class != IECLASS_RAWMOUSE)
			continue;

		// Ignore motion while the game window is not focused (input.device
		// delivers events for the whole machine).
		if(!(win->Flags & WFLG_WINDOWACTIVE))
			continue;

		amigaRelMouseX += ev->ie_position.ie_xy.ie_x;
		amigaRelMouseY += ev->ie_position.ie_xy.ie_y;

		// Keep the OS pointer still while the game reads relative motion, so
		// the deltas never stop at a screen edge. Only when the window is
		// frontmost and the pointer is inside it (same guard as the RTCW/
		// MorphOS input handler).
		if(amigaSuppressMouse &&
		   IntuitionBase != nil &&
		   win->WScreen == IntuitionBase->FirstScreen &&
		   win->MouseX > 0 && win->MouseY > 0){
			ev->ie_position.ie_xy.ie_x = 0;
			ev->ie_position.ie_xy.ie_y = 0;
		}
	}

	return events;
}

static void
amigaInputInit(void)
{
	if(amigaInputState != 0)
		return;

	amigaInputPort = CreatePort(0, 0);
	if(amigaInputPort == 0)
		goto fail;

	amigaInputReq = CreateStdIO(amigaInputPort);
	if(amigaInputReq == 0)
		goto fail;

	if(OpenDevice((STRPTR)"input.device", 0, (struct IORequest *)amigaInputReq, 0) != 0)
		goto fail;

	amigaInputOpen = 1;

	amigaInputHandler.is_Node.ln_Type = NT_INTERRUPT;
	amigaInputHandler.is_Node.ln_Pri = 100;
	amigaInputHandler.is_Node.ln_Name = (char *)"reVC relative mouse input handler";
	amigaInputHandler.is_Data = 0;
	amigaInputHandler.is_Code = (void (*)())amigaInputHandlerCode;
	amigaInputReq->io_Data = (void *)&amigaInputHandler;
	amigaInputReq->io_Command = IND_ADDHANDLER;
	if(DoIO((struct IORequest *)amigaInputReq) != 0)
		goto fail;

	amigaInputState = 1;
	STARTLOG("mouse: input.device relative handler up");
	return;

fail:
	STARTLOG("mouse: input.device open failed, using absolute IDCMP path");
	amigaInputShutdown();
}

static int
amigaMouseRelActive(void)
{
	return amigaInputState != 0;
}

long
_InputInitialiseMouse(bool exclusive)
{
	(void)exclusive;
	// MiniGL's fullscreen custom screen shows no pointer; the frontend draws a
	// software cursor. Re-arm the input.device relative-mouse handler here
	// (idempotent) since the frontend calls this on every menu open/close.
	amigaInputInit();
	return 0;
}

void
_InputShutdownMouse()
{
	// Called by the frontend on every menu open/close (not only on exit), so it
	// must NOT tear down the input.device handler; that happens once in main().
}

bool
_InputMouseNeedsExclusive()
{
	return true;
}

void
CapturePad(RwInt32 padID)
{
	(void)padID;
	// Keyboard/mouse only for the vertical slice.
}

// ---------------------------------------------------------------------------
// IDCMP pump
// ---------------------------------------------------------------------------

static CMouseControllerState amigaMouseAccum;

// Invert signs shared by the absolute (IDCMP) and relative (input.device) mouse
// paths, so the fallback and the normal path apply exactly the same rule. Only
// used for gameplay; the menu keeps its raw signs.
static void
amigaMouseSigns(float *signX, float *signY)
{
	*signX = MousePointerStateHelper.bInvertHorizontally ? -1.0f : 1.0f;
	*signY = MousePointerStateHelper.bInvertVertically ? -1.0f : 1.0f;
}

static void
AmigaHandleMouseButtons(UWORD code)
{
	if(!amigaInputHasFocus())
		return;
	switch(code){
	case SELECTDOWN: amigaMouseAccum.LMB = true; break;
	case SELECTUP:   amigaMouseAccum.LMB = false; break;
	case MENUDOWN:   amigaMouseAccum.RMB = true; break;
	case MENUUP:     amigaMouseAccum.RMB = false; break;
	case MIDDLEDOWN: amigaMouseAccum.MMB = true; break;
	case MIDDLEUP:   amigaMouseAccum.MMB = false; break;
	}
}

static void
AmigaHandleMouseMove(WORD mx, WORD my)
{
	float signX = 1.0f;
	float signY = 1.0f;

	// IDCMP coordinates include the window borders; the game works in the
	// client area, which is the render size in windowed mode.
	if(Amiga.window != nil){
		mx -= Amiga.window->BorderLeft;
		my -= Amiga.window->BorderTop;
	}

	if(!amigaInputHasFocus()){
		// Keep the anchor current so refocusing does not inject a jump.
		Amiga.lastMousePos.x = mx;
		Amiga.lastMousePos.y = my;
		return;
	}

	if(FrontEndMenuManager.m_bMenuActive){
		FrontEndMenuManager.m_nMouseTempPosX = mx;
		FrontEndMenuManager.m_nMouseTempPosY = my;
	}else{
		amigaMouseSigns(&signX, &signY);
	}

	// With the relative handler active the motion comes from input.device and
	// the IDCMP path must not also accumulate it; the absolute coordinates are
	// still tracked for the menu's software cursor.
	if(!(amigaMouseRelActive() && !FrontEndMenuManager.m_bMenuActive)){
		amigaMouseAccum.x += signX * (mx - Amiga.lastMousePos.x);
		amigaMouseAccum.y += signY * (my - Amiga.lastMousePos.y);
	}

	Amiga.lastMousePos.x = mx;
	Amiga.lastMousePos.y = my;
}

// Publish the motion + button state accumulated since the last consumer tick
// into CPad::NewMouseControllerState. This is called once per consumer tick
// (rsIDLE / rsFRONTENDIDLE), NOT per event poll: with the frame limiter on
// (default) the main loop polls input.device many times between two rendered
// frames, and committing on every poll overwrote (and thus dropped) every
// sub-frame delta except the last one, which made mouse-look feel erratic and
// far less sensitive than the settings imply.
static void
amigaPublishMouseState(void)
{
	if(!amigaInputHasFocus()){
		// The window lost focus: publish a neutral state so a held button or
		// half-frame delta cannot leak into the game.
		amigaMouseAccum.x = 0.0f;
		amigaMouseAccum.y = 0.0f;
		amigaMouseAccum.LMB = false;
		amigaMouseAccum.RMB = false;
		amigaMouseAccum.MMB = false;
	}
	CPad::PCTempMouseControllerState = amigaMouseAccum;
	CPad::OldMouseControllerState = CPad::NewMouseControllerState;
	CPad::NewMouseControllerState = CPad::PCTempMouseControllerState;
	amigaMouseAccum.x = 0.0f;
	amigaMouseAccum.y = 0.0f;
}

// Window focus was lost. Intuition will not deliver the key-ups/button-ups that
// were still outstanding, so synthesize them here; otherwise a held key (e.g.
// accelerate) stays stuck until the next press.
static void
amigaReleaseInput(void)
{
	int i;

	for(i = 0; i < RAWKEY_TABLE_SIZE; i++){
		RsKeyCodes ks;
		if(!amigaKeyDown[i])
			continue;
		amigaKeyDown[i] = false;
		ks = (RsKeyCodes)keymap[i];
		if(ks != rsNULL)
			RsKeyboardEventHandler(rsKEYUP, &ks);
	}
	lshiftStatus = false;
	rshiftStatus = false;

	amigaMouseAccum.LMB = false;
	amigaMouseAccum.RMB = false;
	amigaMouseAccum.MMB = false;
	amigaMouseAccum.x = 0.0f;
	amigaMouseAccum.y = 0.0f;

	Disable();
	amigaRelMouseX = 0;
	amigaRelMouseY = 0;
	Enable();
}

static void
AmigaPollEvents(void)
{
	struct Window *win = Amiga.window;

	if(win != nil){
		struct IntuiMessage *imsg;
		while((imsg = (struct IntuiMessage *)GetMsg(win->UserPort)) != nil){
			ULONG cls = imsg->Class;
			UWORD code = imsg->Code;
			WORD mx = imsg->MouseX;
			WORD my = imsg->MouseY;

			ReplyMsg((struct Message *)imsg);

			switch(cls){
			case IDCMP_RAWKEY:
				AmigaHandleRawKey(code);
				break;
			case IDCMP_MOUSEBUTTONS:
				AmigaHandleMouseButtons(code);
				break;
			case IDCMP_MOUSEMOVE:
				AmigaHandleMouseMove(mx, my);
				break;
			case IDCMP_ACTIVEWINDOW:
				amigaWindowFocused = 1;
				break;
			case IDCMP_INACTIVEWINDOW:
				amigaWindowFocused = 0;
				amigaReleaseInput();
				break;
			case IDCMP_REFRESHWINDOW:
				BeginRefresh(win);
				EndRefresh(win, TRUE);
				break;
			case IDCMP_CLOSEWINDOW:
				amigaWindowClosed = TRUE;
				RsGlobal.quit = TRUE;
				break;
			}
		}
	}

	// Drain the input.device relative deltas. Suppress the OS pointer while
	// gameplay owns the mouse; in the menu the pointer is left free and the
	// absolute IDCMP path drives the software cursor.
	if(amigaMouseRelActive()){
		float dx, dy;

		// The input.device handler may run between the load and the clear, so
		// drain atomically; otherwise a delta arriving in that window is lost.
		Disable();
		dx = (float)amigaRelMouseX;
		dy = (float)amigaRelMouseY;
		amigaRelMouseX = 0;
		amigaRelMouseY = 0;
		Enable();

		amigaSuppressMouse =
			(amigaInputHasFocus() && !FrontEndMenuManager.m_bMenuActive) ? 1 : 0;

		if(amigaInputHasFocus() && !FrontEndMenuManager.m_bMenuActive){
			float signX, signY;
			amigaMouseSigns(&signX, &signY);
			amigaMouseAccum.x += signX * dx;
			amigaMouseAccum.y += signY * dy;
		}
	}

	// NOTE: the mouse state is NOT committed here. AmigaPollEvents runs many
	// times per rendered frame (the frame limiter holds rsIDLE), so committing
	// here would overwrite every delta but the last. The main loop calls
	// amigaPublishMouseState() immediately before each rsIDLE/rsFRONTENDIDLE
	// consumer tick instead (CPad::UpdateMouse is a no-op on AMIGAOS).
}

// --- model viewer ----------------------------------------------------------
// REVC_MODELVIEWER=1 replaces the game menu/gameplay with CAnimViewer. The
// normal game loop stays untouched; only this branch differs.
static void
AmigaInitialiseModelViewer(void)
{
	gbModelViewer = TRUE;
	LoadingScreen("Loading the ModelViewer", nil, GetRandomSplashScreen());
	CAnimViewer::Initialise();
	CTimer::Update();
	FrontEndMenuManager.m_bGameNotLoaded = false;
}

static void
AmigaModelViewerFrame(void)
{
	// The normal loop publishes mouse state immediately before rsIDLE. The
	// viewer branch bypasses that call, so publish here or CPad::GetMouseX/Y
	// stay at zero.
	amigaPublishMouseState();
	TheModelViewer();
	// Only force the load while the viewer has no target; otherwise rely on
	// CAnimViewer::Update's incremental UpdateForAnimViewer streaming.
	if(CAnimViewer::pTarget == nil)
		CStreaming::LoadAllRequestedModels(false);

	CPad *pad = CPad::GetPad(0);
	if(pad->GetEscapeJustDown() || pad->GetStartJustDown())
		RsGlobal.quit = TRUE;
}

// --- boot-splash services (see src/skel/amigaos/amiga_splash.cpp) -----------
// Shared through src/core/amiga_boot.h: the splash owns its render loop, these
// give it the two host affordances it needs without reaching into this file.

extern "C" void
amiga_boot_poll_events(void)
{
	AmigaPollEvents();
}

extern "C" int
amiga_boot_should_quit(void)
{
	return RsGlobal.quit || amigaWindowClosed;
}

extern "C" void
amiga_boot_yield(void)
{
	// Give other tasks a slice when the splash cannot present a frame.
	Delay(1);
}

void
HandleExit()
{
	// Events are pumped from the main loop.
}

void
psMouseSetPos(RwV2d *pos)
{
	FrontEndMenuManager.m_nMouseTempPosX = pos->x;
	FrontEndMenuManager.m_nMouseTempPosY = pos->y;
	FrontEndMenuManager.m_nMousePosX = pos->x;
	FrontEndMenuManager.m_nMousePosY = pos->y;
	FrontEndMenuManager.m_nMouseOldPosX = pos->x;
	FrontEndMenuManager.m_nMouseOldPosY = pos->y;
	Amiga.lastMousePos.x = pos->x;
	Amiga.lastMousePos.y = pos->y;
}

RwBool
psNativeTextureSupport(void)
{
	return TRUE;
}

RwBool
IsForegroundApp()
{
	return !!ForegroundApp;
}

// ---------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------

void
InitialiseLanguage()
{
	// No usable setlocale() under libnix; default to English/American. The
	// NASTY_GAME define already forces nastyGame on anyway.
	CGame::germanGame = false;
	CGame::frenchGame = false;
	CGame::noProstitutes = false;
	CGame::nastyGame = true;
	FrontEndMenuManager.m_PrefsAllowNastyGame = true;
	FrontEndMenuManager.OS_Language = LANG_ENGLISH;
	FrontEndMenuManager.m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;

	TheText.Unload();
	TheText.Load();
}

// ---------------------------------------------------------------------------
// RW device (re)selection
// ---------------------------------------------------------------------------

// Hand the librw MINI backend its render-path options. Called before every
// rsRWINITIALIZE because DEVICEOPEN reads them, including the windowed flag
// that decides whether the host window is adopted or a fullscreen screen is
// opened.
static void
amigaApplyRwOptions(void)
{
	const AmigaFlags &f = amigaFlags();
	RwMiniOptions o = {};
	o.vertexArrays = amigaVertexArray;
	o.lockArrays   = f.lockArrays;
	o.lockWin      = f.lockWin;
	o.texUniq      = f.texUniq;
	o.matfx        = f.matFx;
	o.windowed     = amigaWindowed;
	o.directFetch  = f.directFetch;
	rwminiSetOptions(&o);
	// Collect librw per-pass render stats only while the profiler runs; the
	// overlay pulls a snapshot each frame through plat::MiniStats. The librw
	// timers are the largest profiler-on cost, so a lean build never enables
	// them: it only reports the frame/fps numbers.
#ifdef REVC_FP_LEAN
	rwminiStatsEnable(0);
#else
	rwminiStatsEnable(fp::fpEnabled());
#endif
}

static void
psPostRWinit(void)
{
	STARTLOG("psPostRWinit: window handle");
	if(amigaWindowed && amigaAppWindow != nil)
		Amiga.window = amigaAppWindow;
	else
		Amiga.window = (struct Window *)rwMiniGetWindowHandle();
	STARTLOG("psPostRWinit: window %s", Amiga.window ? "ok" : "NULL");

	if(Amiga.window != nil)
		ModifyIDCMP(Amiga.window,
			IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
			IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW);
	// The input.device handler follows Amiga.window, so point it at the
	// (possibly new) window and start it on first use. It stays up across menu
	// open/close and video-mode changes; only the suppression flag changes.
	amigaInputWindow = Amiga.window;
	if(Amiga.window != nil)
		amigaInputInit();

	_InputInitialiseJoys();
	_InputInitialiseMouse(false);
	STARTLOG("psPostRWinit: done");

	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);
}

RwBool
_psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	RwInitialised = FALSE;

	// The input.device handler must not touch the window being torn down;
	// psPostRWinit repoints it after the new one exists.
	amigaInputWindow = 0;

	RsEventHandler(rsRWTERMINATE, nil);

	// The old context is gone. A windowed context must be recreated from a
	// fresh fixed-size window, so drop the old one before RW init; a switch to
	// fullscreen just drops it.
	amigaWindowClose();
	amigaForcedFullscreen = false;
	amigaWindowed = FrontEndMenuManager.m_nPrefsWindowed != 0;

	GcurSel = subSystem;
	GcurSelVM = videoMode;

	useDefault = TRUE;

	openParams.width = RsGlobal.maximumWidth;
	openParams.height = RsGlobal.maximumHeight;
	openParams.windowtitle = RsGlobal.appName;
	openParams.window = nil;

	if(amigaWindowed)
		amigaPrepareWindowed(videoMode, &openParams);
	amigaApplyRwOptions();

	if(RsEventHandler(rsRWINITIALIZE, &openParams) == rsEVENTERROR){
		amigaWindowClose();
		return FALSE;
	}

	RwInitialised = TRUE;
	useDefault = FALSE;

	RwRect r;
	r.x = 0;
	r.y = 0;
	r.w = RsGlobal.maximumWidth;
	r.h = RsGlobal.maximumHeight;

	RsEventHandler(rsCAMERASIZE, &r);

	psPostRWinit();
	_psFreeVideoModeList();

	return TRUE;
}

void
_psSelectScreenVM(RwInt32 videoMode)
{
	RwTexDictionarySetCurrent(nil);

	FrontEndMenuManager.UnloadTextures();

	if(!_psSetVideoMode(RwEngineGetCurrentSubSystem(), videoMode)){
		RsGlobal.quit = TRUE;
		printf("ERROR: Failed to select new screen resolution\n");
	}else
		FrontEndMenuManager.LoadAllTextures();
}

// ---------------------------------------------------------------------------
// Platform init / term
// ---------------------------------------------------------------------------

RwBool
psInitialize(void)
{
	STARTLOG("psInitialize: enter");

	setlocale(LC_ALL, "C");
	STARTLOG("psInitialize: setlocale(LC_ALL,C) done");

	Amiga.lastMousePos.x = Amiga.lastMousePos.y = 0.0f;
	Amiga.window = nil;

	RsGlobal.ps = &Amiga;

	STARTLOG("psInitialize: open timer.device");
	amigaOpenTimer();
	STARTLOG("psInitialize: timer.device %s", TimerBase ? "ok" : "FAILED");

	if(IntuitionBase == nil)
		IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
	if(GfxBase == nil)
		GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
	STARTLOG("psInitialize: intuition.library %s, graphics.library %s",
	         IntuitionBase ? "ok" : "FAILED", GfxBase ? "ok" : "FAILED");

	CFileMgr::Initialise();
	STARTLOG("psInitialize: CFileMgr::Initialise done");

	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());
	STARTLOG("psInitialize: save dir done");

	STARTLOG("psInitialize: InitialiseLanguage");
	InitialiseLanguage();
	STARTLOG("psInitialize: InitialiseLanguage done");

	gGameState = GS_START_UP;

	_dwOperatingSystemVersion = OS_WINXP; // To fool other classes

	_dwMemAvailPhys = (size_t)AvailMem(MEMF_ANY);
	debug("Available physical memory %u\n", (unsigned)_dwMemAvailPhys);

	STARTLOG("psInitialize: FrontEndMenuManager.LoadSettings");
	FrontEndMenuManager.LoadSettings();
	FrontEndMenuManager.m_nPrefsWindowed =
	    amigaIniBool(((RwUInt8*)&FrontEndMenuManager.m_nPrefsWindowed)[0] != 0);
	STARTLOG("psInitialize: FrontEndMenuManager.LoadSettings done");

	TheText.Unload();

	STARTLOG("psInitialize: done");
	return TRUE;
}

void
psTerminate(void)
{
	amigaCloseTimer();

	if(GfxBase != nil){
		CloseLibrary((struct Library *)GfxBase);
		GfxBase = nil;
	}
	if(IntuitionBase != nil){
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = nil;
	}
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
main(int argc, char *argv[])
{
	RwV2d pos;
	RwInt32 i;

	// Crash-safe, unbuffered startup log (replaces the old freopen); also
	// redirects stdout/stderr so re3_debug()/printf survive a Guru.
	amigaLogOpen();
	amigaLogEnvironment(argc, argv);

	// REVC_DEBUGLOG: 0 off, 1 debug(), 2 also debugv().
	{
		int dbg = amigaEnvInt("REVC_DEBUGLOG", 0, 0, 2);
		re3_debugEnabled = dbg >= 1;
		re3_debugVerbose = dbg >= 2;
	}
	STARTLOG("main: enter");

#ifdef AUDIO_AHI
	// AHI audio-mode selection must run before the GL context/fullscreen screen
	// is created (rsRWINITIALIZE below), otherwise the requester opens on the
	// Workbench screen underneath the game and is never seen. No-op unless
	// REVC_AHI=1; shows nothing (and stays silent) when ahi.device is absent.
	amigaAhiEarlyModeSelect();
#endif

	STARTLOG("main: rsINITIALIZE begin");
	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR){
		STARTLOG("main: rsINITIALIZE failed");
		amigaLogClose();
		return 1;
	}
	STARTLOG("main: rsINITIALIZE done");

	// reVC.ini prefs are now loaded (psInitialize). Seed the requested size into
	// RsGlobal so DEVICEOPEN sees it; REVC_MODE overrides it in both modes.
	{
		RwInt32 reqW, reqH;
		amigaWindowed = FrontEndMenuManager.m_nPrefsWindowed != 0;
		amigaRequestedSize(&reqW, &reqH);
		RsGlobal.maximumWidth = reqW;
		RsGlobal.maximumHeight = reqH;
		STARTLOG("main: requested video size %dx%d", (int)reqW, (int)reqH);
	}

	for(i = 1; i < argc; i++)
		RsEventHandler(rsPREINITCOMMANDLINE, argv[i]);

	openParams.width = RsGlobal.maximumWidth;
	openParams.height = RsGlobal.maximumHeight;
	openParams.windowtitle = RsGlobal.appName;
	openParams.window = nil;

	// The MINI backend reads these at DEVICEOPEN.
	amigaVertexArray = amigaEnvBoolEx("REVC_VERTEXARRAY", amigaVertexArray, nil);
	amigaSkipIntro = amigaEnvBool("REVC_SKIPINTRO", 0);

	// REVC_PROFILE / REVC_PROFILE_MS (frame profiler). fpEnable latches the
	// profiler before the first Idle(); the sinks open profile_slow.txt and
	// route the 30-frame aggregate line to the boot log.
	{
		int prof = amiga_boot_profile();
		if(prof){
			fp::fpSetSlowThresholdMs(amiga_boot_profile_ms());
			fpInstallSinks();
		}
		fp::fpEnable(prof);
	}

	// Windowed mode needs the Intuition window to exist before DEVICEINIT:
	// MiniGL adopts it with mglCreateContextFromWindow.
	if(amigaWindowed)
		amigaPrepareWindowed(-1, &openParams);

	// Hand the librw MINI backend its render-path options before DEVICEOPEN:
	// librw reads these instead of the amiga_boot_* toggles (see rwminihost.h).
	amigaApplyRwOptions();

	// Effective REVC_* configuration, one line.
	{
		const AmigaFlags &f = amigaFlags();
		STARTLOG("main: flags windowed=%d vertexArray=%d skipIntro=%d ahi=%d "
		         "async=%d "
		         "texuniq=%d lockwin=%d lockarrays=%d matfx=%d "
		         "direct=%d clear=%d "
		         "texmemlog=%d profile=%d/%ums "
		         "streamstats=%d/%u/%ums",
		         (int)amigaWindowed, (int)amigaVertexArray, amigaSkipIntro,
		         amigaEnvBool("REVC_AHI", 1),
		         f.asyncStream, f.texUniq, f.lockWin,
		         f.lockArrays, f.matFx,
		         f.directFetch, f.clearColor,
		         f.texMemLog, f.profile, f.profileMs,
		         f.streamStats, (unsigned int)f.streamStatsFrames,
		         (unsigned int)f.streamStatsSlowMs);
	}

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	STARTLOG("main: rsRWINITIALIZE begin");
	if(rsEVENTERROR == RsEventHandler(rsRWINITIALIZE, &openParams)){
		STARTLOG("main: rsRWINITIALIZE failed");
		RsEventHandler(rsRWTERMINATE, nil);
		amigaWindowClose();
		RsEventHandler(rsTERMINATE, nil);
		amigaLogClose();
		return 1;
	}
	STARTLOG("main: rsRWINITIALIZE done");

	psPostRWinit();
	STARTLOG("main: psPostRWinit done");

	ControlsManager.InitDefaultControlConfigMouse(MousePointerStateHelper.GetMouseSetUp());

	for(i = 1; i < argc; i++)
		RsEventHandler(rsCOMMANDLINE, argv[i]);

	{
		RwRect r;
		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;
		RsEventHandler(rsCAMERASIZE, &r);
	}

	{
		CFileMgr::SetDirMyDocuments();

		int32 gta3set = CFileMgr::OpenFile("gta_vc.set", "r");
		if(gta3set){
			ControlsManager.LoadSettings(gta3set);
			CFileMgr::CloseFile(gta3set);
		}

		CFileMgr::SetDir("");

#ifdef LOAD_INI_SETTINGS
		int connectedPadButtons = ControlsManager.ms_padButtonsInited;
		LoadINIControllerSettings();
		if(connectedPadButtons != 0)
			ControlsManager.InitDefaultControlConfigJoyPad(connectedPadButtons);
		SaveINISettings();
		SaveINIControllerSettings();
#endif
	}

	STARTLOG("main: entering main loop");
	initkeymap();

	// Boot intros: the GL context exists and the window is up, so show the
	// procedural C64 loading screen first, then the PiStorm3D logo, before the
	// game's own loading screens, cutscenes and intros.
	if(amigaEnvBool("REVC_NOSPLASH", 0)){
		STARTLOG("splash: disabled by REVC_NOSPLASH");
	}else{
		if(amigaEnvBool("REVC_C64INTRO", 1))
			AmigaC64IntroShow();
		else
			STARTLOG("splash: C64 intro disabled by REVC_C64INTRO");
		AmigaSplashShow();
	}

	if(amigaEnvBool("REVC_MODELVIEWER", 0))
		AmigaInitialiseModelViewer();

	while(1){
		RwInitialised = TRUE;
		STARTLOG("main: loop iteration (gGameState=%d)", (int)gGameState);

		pos.x = RsGlobal.maximumWidth * 0.5f;
		pos.y = RsGlobal.maximumHeight * 0.5f;
		RsMouseSetPos(&pos);

		while(!RsGlobal.quit && !FrontEndMenuManager.m_bWantToRestart && !amigaWindowClosed){
			AmigaPollEvents();

			if(gbModelViewer){
				AmigaModelViewerFrame();
				continue;
			}

			if(ForegroundApp){
				switch(gGameState){
				case GS_START_UP:
#ifdef NO_MOVIES
					gGameState = GS_INIT_ONCE;
#else
					gGameState = GS_INIT_LOGO_MPEG;
#endif
					break;

				case GS_INIT_LOGO_MPEG:
					gGameState = GS_LOGO_MPEG;
					break;

				case GS_LOGO_MPEG:
					++gGameState;
					break;

				case GS_INIT_INTRO_MPEG:
					gGameState = GS_INTRO_MPEG;
					break;

				case GS_INTRO_MPEG:
					++gGameState;
					break;

				case GS_INIT_ONCE:
					LoadingScreen(nil, nil, "loadsc0");
					if(!CGame::InitialiseOnceAfterRW())
						RsGlobal.quit = TRUE;
					if(amigaSkipIntro){
						STARTLOG("main: skip-intro -> start new game (frontend bypassed)");
						gGameState = GS_INIT_PLAYING_GAME;
					}else{
						gGameState = GS_INIT_FRONTEND;
					}
					break;

				case GS_INIT_FRONTEND:
					LoadingScreen(nil, nil, "loadsc0");
					FrontEndMenuManager.m_bGameNotLoaded = true;
					FrontEndMenuManager.m_bStartUpFrontEndRequested = true;

					if(defaultFullscreenRes){
						defaultFullscreenRes = FALSE;
						FrontEndMenuManager.m_nPrefsVideoMode = GcurSelVM;
						FrontEndMenuManager.m_nDisplayVideoMode = GcurSelVM;
					}

					gGameState = GS_FRONTEND;
					break;

				case GS_FRONTEND:
					amigaPublishMouseState();
					RsEventHandler(rsFRONTENDIDLE, nil);

					if(!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad){
						gGameState = GS_INIT_PLAYING_GAME;
					}

					if(FrontEndMenuManager.m_bWantToLoad){
						InitialiseGame();
						FrontEndMenuManager.m_bGameNotLoaded = false;
						gGameState = GS_PLAYING_GAME;
						STARTLOG("main: GAMEPLAY_BEGIN");
					}
					break;

				case GS_INIT_PLAYING_GAME:
					InitialiseGame();
					// CGame::Initialise runs CCutsceneMgr::Initialise, so arm the
					// cutscene skip after it (Initialise does not clear ms_skip, but
					// this keeps the ordering explicit).
					CCutsceneMgr::SetSkip(amigaSkipIntro != 0);
					FrontEndMenuManager.m_bGameNotLoaded = false;
					gGameState = GS_PLAYING_GAME;
					STARTLOG("main: GAMEPLAY_BEGIN");
					break;

				case GS_PLAYING_GAME:
				{
					float ms = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerMillisecond();
					if(RwInitialised){
						if(!FrontEndMenuManager.m_PrefsFrameLimiter || (1000.0f / (float)RsGlobal.maxFPS) < ms){
							// Publish once per rendered frame, so deltas that
							// arrived while the frame limiter held are kept.
							amigaPublishMouseState();
							RsEventHandler(rsIDLE, (void *)TRUE);
						}
					}
					// Cutscenes are finished by the engine-level skip in
					// CCutsceneMgr::LoadCutsceneData() (armed via SetSkip above),
					// so no per-frame finish latch is needed here.
					break;
				}
				}
			}else{
				if(RwCameraBeginUpdate(Scene.camera)){
					RwCameraEndUpdate(Scene.camera);
					ForegroundApp = TRUE;
					RsEventHandler(rsACTIVATE, (void *)TRUE);
				}
			}
		}

		RwInitialised = FALSE;

		FrontEndMenuManager.UnloadTextures();

		if(!FrontEndMenuManager.m_bWantToRestart)
			break;

		CPad::ResetCheats();
		CPad::StopPadsShaking();

		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);

		CTimer::Stop();

		if(FrontEndMenuManager.m_bWantToLoad){
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
		}else{
			if(gbModelViewer)
				CAnimViewer::Shutdown();
			else if(gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();

			CTimer::Stop();

			if(FrontEndMenuManager.m_bFirstTime == true){
				gGameState = GS_INIT_FRONTEND;
			}else{
				gGameState = GS_INIT_PLAYING_GAME;
			}
		}

		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
	}

	if(gbModelViewer)
		CAnimViewer::Shutdown();
	else if(gGameState == GS_PLAYING_GAME)
		CGame::ShutDown();

	DMAudio.Terminate();

	_psFreeVideoModeList();

	// True shutdown (not the frontend's menu-toggle call): remove the
	// input.device handler before IntuitionBase is closed by psTerminate.
	amigaInputShutdown();

	_InputShutdownMouse();
	if(fp::fpEnabled())
		fpShutdownDump();
	RsEventHandler(rsRWTERMINATE, nil);
	// DeleteContext leaves an adopted window open; close it before psTerminate
	// closes IntuitionBase.
	amigaWindowClose();
	RsEventHandler(rsTERMINATE, nil);

	// The platform terminate (RsRwTerminate/RsTerminate) is deliberately NOT
	// forced here: those are the skeleton's default branch for unhandled events,
	// and librw's Engine guards reject the calls at exit regardless. The OS
	// reclaims minigl/timer/Intuition on process teardown.

	amigaLogClose();
	return 0;
}

#endif // AMIGAOS
