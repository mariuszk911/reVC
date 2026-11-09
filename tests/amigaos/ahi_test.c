/*
 * ahi_test.c -- AHI audio bring-up spike (standalone, render-agnostic).
 *
 * WHY: reVC's AmigaOS build currently ships the null audio backend
 * (sampman_null). Before writing a real sampman_ahi we must prove the AHI
 * client path actually works on the 68k targets: open ahi.device, enumerate
 * the audio modes, allocate the installed (Paula) driver, load a generated
 * sine, play it, and confirm the device really mixed it to completion.
 *
 * Non-audible proof: the sample is a one-shot (AHIP_LoopSound, AHI_NOSOUND),
 * so the AHIA_SoundFunc end-of-sound hook must fire. The hook increments a
 * counter and Signals the main task; the app waits for it within a bounded
 * EClock deadline. That proves the device mixed and completed the sample
 * without depending on hearing it (the emulator is muted).
 *
 * Only exec/dos/utility/ahi/timer are used -- no render, no MiniGL. Runs on
 * the B1260 Amiberry target and on real PiStorm3D HW.
 *
 * Build: tests/amigaos/build_ahi_test.sh  (output build/amigaos/test/ahi_test)
 * Run from a writable drawer (e.g. DH11:) so the log lands there:
 *   CD DH11: && ahi_test
 * It writes ahi_test.log in the current directory and prints a terminal line
 * (`ahi_test: all ok` / `ahi_test: N failure(s)` / `ahi_test: SKIP ...`).
 *
 * libnix %f/%g printing is broken on target; this app logs integers only.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <devices/ahi.h>
#include <devices/timer.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/ahi.h>

#include <stdio.h>
#include <math.h>

#define AHI_PI	3.14159265358979323846
#define AHI_TONE_HZ	1000.0	/* audible test tone */
#define LEN	80	/* string buffer size for mode attributes */

/* AHIBase lives here; proto/ahi.h's inline macros resolve through it. */
struct Library *AHIBase = NULL;

/* timer.device EClock -- for allocation/play timing and the wait deadline. */
struct Device *TimerBase;
static struct timerequest AhiTimerReq;
static unsigned int ahiEclockScale;

static int failures;
#define CHK(cond, ...) do { \
	if(cond){ printf("ok   " __VA_ARGS__); printf("\n"); } \
	else { printf("FAIL " __VA_ARGS__); printf("\n"); failures++; } \
} while(0)

static int
timer_init(void)
{
	if(TimerBase != NULL)
		return 1;
	if(OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
		      (struct IORequest *)&AhiTimerReq, 0) != 0)
		return 0;
	TimerBase = AhiTimerReq.tr_node.io_Device;
	return TimerBase != NULL;
}

static unsigned int
now_us(void)
{
	struct EClockVal ev;
	ULONG freq;

	if(TimerBase == NULL)
		return 0;
	freq = ReadEClock(&ev);
	if(freq == 0)
		return 0;
	if(ahiEclockScale == 0)
		ahiEclockScale = (unsigned int)(((unsigned long long)1000000 << 16) / freq);
	return (unsigned int)(((unsigned long long)ev.ev_lo * ahiEclockScale) >> 16);
}

/* ---- end-of-sound hook -------------------------------------------------- */

struct SoundCtx { volatile ULONG count; };
static struct SoundCtx soundCtx;

static ULONG
sound_hook_func(struct Hook *hook, APTR obj, APTR msg)
{
	struct AHIAudioCtrl *actrl = (struct AHIAudioCtrl *)obj;
	struct Task *task;

	(void)msg;

	if(actrl != NULL && actrl->ahiac_UserData != NULL){
		struct SoundCtx *ctx = (struct SoundCtx *)actrl->ahiac_UserData;
		ctx->count++;
	}
	task = (struct Task *)hook->h_Data;
	if(task != NULL)
		Signal(task, SIGF_SINGLE);	/* Signal() is interrupt-safe */
	return 0;
}

/* HookEntry bridges the register-based Amiga hook ABI to a normal C call. */
static struct Hook soundHook = {
	{ NULL, NULL },
	(HOOKFUNC)HookEntry,
	(HOOKFUNC)sound_hook_func,
	NULL				/* h_Data = FindTask(NULL), set below */
};

/* Bounded wait for the end-of-sound hook, using an EClock deadline. */
static int
wait_hook(unsigned int budget_us)
{
	unsigned int t0 = now_us();
	unsigned int ticks = 0;
	/* Delay(1) is ~20 ms; keep a tick-counted fallback so the wait is still
	 * bounded if timer.device failed to open (now_us() then returns 0). */
	unsigned int maxTicks = budget_us / 20000 + 1;

	for(;;){
		ULONG sig = SetSignal(0, SIGF_SINGLE);
		if(sig & SIGF_SINGLE)
			return 1;
		if(TimerBase != NULL){
			if((unsigned int)(now_us() - t0) > budget_us)
				return 0;
		}
		else if(ticks++ >= maxTicks){
			return 0;
		}
		Delay(1);	/* ~20 ms tick; sample is ~1 s */
	}
}

/* ---- case-insensitive substring match for driver/mode names ------------ */

static char
lower_ci(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int
contains_ci(const char *hay, const char *needle)
{
	if(hay == NULL || needle == NULL)
		return 0;
	while(*hay){
		const char *h = hay;
		const char *n = needle;
		while(*h && *n && lower_ci(*h) == lower_ci(*n)){ h++; n++; }
		if(*n == '\0')
			return 1;
		hay++;
	}
	return 0;
}

static int
mode_matches(const char *name, const char *drv)
{
	return contains_ci(name, "paula") || contains_ci(drv, "paula") ||
	       contains_ci(name, "amiberry") || contains_ci(drv, "amiberry") ||
	       contains_ci(name, "uae") || contains_ci(drv, "uae");
}

int
main(void)
{
	struct MsgPort *port = NULL;
	struct AHIRequest *io = NULL;
	struct AHIAudioCtrl *actrl = NULL;
	ULONG version, id, err;
	ULONG chosen = AHI_INVALID_ID;
	ULONG firstRt = AHI_INVALID_ID;
	ULONG matchRt = AHI_INVALID_ID;
	ULONG matchAny = AHI_INVALID_ID;
	LONG bits, ch, minf, maxf, rt;
	UBYTE name[LEN], drv[LEN];
	int modeCount = 0, rtCount = 0;
	ULONG mixfreq = 0;
	ULONG sampleLen;
	WORD *sample = NULL;
	struct AHISampleInfo si;
	unsigned int t0, t1;
	ULONG allocUs = 0, playUs = 0;

	freopen("ahi_test.log", "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("ahi_test: AHI bring-up spike\n");

	timer_init();

	/* ---- B1. open ahi.device ------------------------------------------ */
	port = CreateMsgPort();
	if(port == NULL){
		printf("ahi_test: FAIL CreateMsgPort\n");
		return 1;
	}
	io = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
	if(io == NULL){
		printf("ahi_test: FAIL CreateIORequest\n");
		DeleteMsgPort(port);
		return 1;
	}

	/* Version 4 is enough: we use 16-bit mono samples, not the V6 formats.
	 * Requesting 4 keeps us compatible with both V4 and V6 devices. */
	io->ahir_Version = 4;
	err = OpenDevice(AHINAME, AHI_NO_UNIT, (struct IORequest *)io, 0);
	if(err != 0){
		/* No device is a SKIP, not a failure: a manual-run success signal. */
		printf("ahi_test: SKIP (no ahi.device) err=%lu\n", (ULONG)err);
		DeleteIORequest((struct IORequest *)io);
		DeleteMsgPort(port);
		return 0;
	}
	AHIBase = (struct Library *)io->ahir_Std.io_Device;
	version = AHIBase->lib_Version;
	printf("info ahi.device version %lu.%lu\n",
	       (ULONG)AHIBase->lib_Version, (ULONG)AHIBase->lib_Revision);
	CHK(version >= 4, "ahi.device lib_Version >= 4: got %lu", (ULONG)version);

	/* ---- B2. enumerate audio modes ------------------------------------ */
	id = AHI_INVALID_ID;
	while((id = AHI_NextAudioID(id)) != AHI_INVALID_ID){
		bits = 0; ch = 0; minf = 0; maxf = 0; rt = 0;
		name[0] = '\0'; drv[0] = '\0';

		/* AHIDB_BufferLen must precede the string tags. */
		AHI_GetAudioAttrs(id, NULL,
			AHIDB_BufferLen, LEN,
			AHIDB_Driver, (ULONG)drv,
			AHIDB_Name, (ULONG)name,
			AHIDB_Bits, (ULONG)&bits,
			AHIDB_MaxChannels, (ULONG)&ch,
			AHIDB_MinMixFreq, (ULONG)&minf,
			AHIDB_MaxMixFreq, (ULONG)&maxf,
			AHIDB_Realtime, (ULONG)&rt,
			TAG_DONE);

		printf("mode 0x%08lx drv=\"%s\" name=\"%s\" bits=%ld ch=%ld "
		       "mix=%ld..%ld realtime=%ld\n",
		       (ULONG)id, (char *)drv, (char *)name,
		       (long)bits, (long)ch, (long)minf, (long)maxf, (long)rt);

		modeCount++;
		if(rt){
			rtCount++;
			if(firstRt == AHI_INVALID_ID)
				firstRt = id;
		}
		if(mode_matches((char *)name, (char *)drv)){
			if(matchAny == AHI_INVALID_ID)
				matchAny = id;
			if(rt && matchRt == AHI_INVALID_ID)
				matchRt = id;
		}
	}
	printf("info modes=%d realtime=%d\n", modeCount, rtCount);
	CHK(modeCount >= 1, "at least one audio mode enumerated: got %d", modeCount);
	CHK(rtCount >= 1, "at least one realtime mode: got %d", rtCount);

	/* ---- B3. choose a mode -------------------------------------------- */
	if(matchRt != AHI_INVALID_ID)
		chosen = matchRt;		/* Paula/Amiberry/uae, realtime */
	else if(matchAny != AHI_INVALID_ID)
		chosen = matchAny;
	else if(firstRt != AHI_INVALID_ID)
		chosen = firstRt;		/* first realtime mode */
	else
		chosen = AHI_DEFAULT_ID;
	printf("info chosen mode 0x%08lx\n", (ULONG)chosen);

	/* ---- B4. allocate ------------------------------------------------- */
	soundCtx.count = 0;
	soundHook.h_Data = FindTask(NULL);

	t0 = now_us();
	actrl = AHI_AllocAudio(
		AHIA_AudioID, chosen,
		AHIA_MixFreq, 44100,
		AHIA_Channels, 1,
		AHIA_Sounds, 1,
		AHIA_SoundFunc, (ULONG)&soundHook,
		AHIA_UserData, (ULONG)&soundCtx,
		TAG_DONE);
	t1 = now_us();
	allocUs = (ULONG)(t1 - t0);
	CHK(actrl != NULL, "AHI_AllocAudio succeeded");
	if(actrl == NULL){
		printf("ahi_test: alloc failed, aborting\n");
		goto teardown;
	}
	printf("info alloc %lu us\n", (ULONG)allocUs);

	AHI_ControlAudio(actrl, AHIC_MixFreq_Query, (ULONG)&mixfreq, TAG_DONE);
	if(mixfreq == 0)
		mixfreq = 44100;	/* fall back to the suggested rate */
	printf("info negotiated mixfreq %lu Hz\n", (ULONG)mixfreq);

	/* ---- B5. sample (1 s mono 16-bit 1 kHz sine) ---------------------- */
	sampleLen = mixfreq;		/* ~1 second at the negotiated rate */
	if(sampleLen < 1000)
		sampleLen = 1000;
	sample = (WORD *)AllocVec(sampleLen * sizeof(WORD), MEMF_PUBLIC | MEMF_CLEAR);
	CHK(sample != NULL, "sample buffer allocated (%lu frames)", (ULONG)sampleLen);
	if(sample == NULL)
		goto teardown;

	{
		/* Phase is kept wrapped in [0, 2*pi): libm sin is only good for
		 * wrapped arguments on target, and a 1 kHz tone over 1 s would
		 * otherwise reach ~6000 rad. */
		const double twoPi = 2.0 * AHI_PI;
		double step = twoPi * AHI_TONE_HZ / (double)mixfreq;
		double phase = 0.0;
		ULONG i;
		long peak = 0;
		for(i = 0; i < sampleLen; i++){
			long v = (long)(32000.0 * sin(phase));
			sample[i] = (WORD)v;
			if(v < 0)
				v = -v;
			if(v > peak)
				peak = v;
			phase += step;
			if(phase >= twoPi)
				phase -= twoPi;
		}
		printf("info sample peak %ld (tone %d Hz, %lu frames)\n",
		       peak, (int)AHI_TONE_HZ, (ULONG)sampleLen);
		CHK(peak > 30000, "sample peak near full scale: got %ld", peak);
	}

	/* ---- B6. arm + start ---------------------------------------------- */
	si.ahisi_Type = AHIST_M16S;
	si.ahisi_Address = sample;
	si.ahisi_Length = sampleLen;

	err = AHI_LoadSound(0, AHIST_SAMPLE, &si, actrl);
	CHK(err == AHIE_OK, "AHI_LoadSound == AHIE_OK: got %lu", (ULONG)err);
	if(err != AHIE_OK)
		goto teardown;

	/* One-shot: AHIP_LoopSound = AHI_NOSOUND so the end-of-sound hook fires. */
	AHI_Play(actrl,
		AHIP_BeginChannel, 0,
		AHIP_Sound, 0,
		AHIP_Freq, mixfreq,
		AHIP_Vol, 0x10000,
		AHIP_Pan, 0x8000,
		AHIP_LoopSound, AHI_NOSOUND,
		AHIP_EndChannel, 0,
		TAG_DONE);

	SetSignal(0, SIGF_SINGLE);
	t0 = now_us();
	err = AHI_ControlAudio(actrl, AHIC_Play, TRUE, TAG_DONE);
	CHK(err == AHIE_OK, "AHIC_Play TRUE == AHIE_OK: got %lu", (ULONG)err);

	/* ---- B7. confirm the device mixed and finished the sample -------- */
	if(err == AHIE_OK){
		int fired = wait_hook(3000000);	/* 3 s budget for a 1 s sample */
		t1 = now_us();
		playUs = (ULONG)(t1 - t0);
		CHK(fired && soundCtx.count > 0,
		    "end-of-sound hook fired (count=%lu)", (ULONG)soundCtx.count);
	}

teardown:
	/* ---- B8. tear down ------------------------------------------------ */
	if(actrl != NULL){
		AHI_ControlAudio(actrl, AHIC_Play, FALSE, TAG_DONE);
		AHI_FreeAudio(actrl);
		actrl = NULL;
	}
	if(sample != NULL)
		FreeVec(sample);
	CloseDevice((struct IORequest *)io);
	DeleteIORequest((struct IORequest *)io);
	DeleteMsgPort(port);

	printf("info timing: alloc=%lu us play=%lu us mixfreq=%lu\n",
	       (ULONG)allocUs, (ULONG)playUs, (ULONG)mixfreq);

	/* ---- B9. terminal line + return code ----------------------------- */
	if(failures){
		printf("ahi_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("ahi_test: all ok\n");
	return 0;
}
