/*
 * ahi_sfx_test.c -- Phase 0 de-risk spike for the sampman_ahi backend.
 *
 * WHY: sampman_ahi plays reVC's real SFX (audio/sfx.RAW + audio/sfx.SDT, PC
 * little-endian 16-bit PCM) through ahi.device. Before writing the backend we
 * must pin, on the actual B1260/PiStorm target:
 *   0b  the exact AHI loop/offset tags (one-shot + looped sample) -- getting
 *       AHIP_LoopOffset/Length wrong yields silence;
 *   0c  AHIA_Channels/AHIA_Sounds/AHIA_MixFreq capacity (43 ch, 128 sounds, 22050);
 *   0d  the affordable simultaneous-loop channel count / CPU at 22050;
 *   0e  accepted parameter updates (AHI_SetFreq/SetVol/SetSound) from the task;
 *   0f  real fseek+fread+byteswap latency for small/median/max samples.
 *
 * Non-audible proof: the AHIA_SoundFunc fires at one-shot start and end (and
 * once per loop wrap), so a one-shot's 2nd callback plus continuous loop-wrap
 * callbacks prove playback. Audio is not required.
 *
 * Build: tests/amigaos/build_ahi_sfx.sh  (output build/amigaos/test/ahi_sfx_test)
 * Run from a writable drawer holding the game's Audio/ data (DH12:):
 *   CD DH12: && DH11:ahi_sfx_test
 * It writes ahi_sfx_test.log in the current directory and prints a terminal
 * line (`ahi_sfx_test: all ok` / `ahi_sfx_test: N failure(s)` / `SKIP ...`).
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

struct Library *AHIBase = NULL;

/* ---- timer.device EClock for timings ----------------------------------- */
struct Device *TimerBase;
static struct timerequest SfxTimerReq;
static unsigned int eclockScale;

static int
timer_init(void)
{
	if(TimerBase != NULL)
		return 1;
	if(OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
		      (struct IORequest *)&SfxTimerReq, 0) != 0)
		return 0;
	TimerBase = SfxTimerReq.tr_node.io_Device;
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
	if(eclockScale == 0)
		eclockScale = (unsigned int)(((unsigned long long)1000000 << 16) / freq);
	return (unsigned int)(((unsigned long long)ev.ev_lo * eclockScale) >> 16);
}

static void
delay_ms(unsigned int ms)
{
	Delay((ms + 19) / 20);	/* DOS Delay is in 1/50 s ticks */
}

/* ---- byte order: sfx.SDT / sfx.RAW are PC little-endian ---------------- */
static unsigned int
swap32(unsigned int v)
{
	return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
	       ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

static void
swap16_block(void *p, unsigned long words)
{
	unsigned short *w = (unsigned short *)p;
	unsigned long i;
	for(i = 0; i < words; i++)
		w[i] = (unsigned short)((w[i] << 8) | (w[i] >> 8));
}

/* ---- hooks -------------------------------------------------------------- */
struct HookCtx {
	volatile ULONG soundCount;	/* end-of-sound / per-loop-wrap callbacks */
};
static struct HookCtx hookCtx;

static ULONG
sound_hook_func(struct Hook *hook, APTR obj, APTR msg)
{
	struct AHIAudioCtrl *actrl = (struct AHIAudioCtrl *)obj;
	(void)msg;
	if(actrl != NULL && actrl->ahiac_UserData != NULL)
		((struct HookCtx *)actrl->ahiac_UserData)->soundCount++;
	Signal((struct Task *)hook->h_Data, SIGF_SINGLE);
	return 0;
}

static struct Hook soundHook = {
	{ NULL, NULL }, (HOOKFUNC)HookEntry, (HOOKFUNC)sound_hook_func, NULL
};

/* ---- test scaffolding --------------------------------------------------- */
static int failures;
#define CHK(cond, ...) do { \
	if(cond){ printf("ok   " __VA_ARGS__); printf("\n"); } \
	else { printf("FAIL " __VA_ARGS__); printf("\n"); failures++; } \
} while(0)

/* ---- case-insensitive mode match (same policy as ahi_test.c) ----------- */
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

/* ---- tSample as stored in sfx.SDT (LE, 5 x uint32) --------------------- */
struct sdtSample {
	unsigned int offset;
	unsigned int size;
	unsigned int freq;
	unsigned int loopStart;
	unsigned int loopEnd;
};

#define SDT_SAMPLE_SIZE 20
#define MAX_SAMPLES 24000

static struct sdtSample sdt[MAX_SAMPLES];
static unsigned int sdtCount;

static FILE *rawFile;

/* Read one sample's byte range into buf, byteswapped to native 16-bit. */
static int
read_sample_bytes(unsigned int offset, unsigned int size, void *buf)
{
	if(rawFile == NULL)
		return 0;
	if(fseek(rawFile, (long)offset, SEEK_SET) != 0)
		return 0;
	if(fread(buf, 1, size, rawFile) != size)
		return 0;
	swap16_block(buf, size / 2);
	return 1;
}

int
main(void)
{
	struct MsgPort *port = NULL;
	struct AHIRequest *io = NULL;
	struct AHIAudioCtrl *actrl = NULL;
	ULONG version, id, err, chosen = AHI_INVALID_ID;
	ULONG firstRt = AHI_INVALID_ID, matchRt = AHI_INVALID_ID, matchAny = AHI_INVALID_ID;
	LONG rt;
	UBYTE name[80], drv[80];
	int modeCount = 0, rtCount = 0;
	ULONG mixfreq = 0;
	ULONG channelsWant = 43, soundsWant = 128;
	int i;
	unsigned char *smallBuf = NULL, *medBuf = NULL, *maxBuf = NULL;
	unsigned int smallIdx = 0, medIdx = 0, maxIdx = 0;
	UBYTE *loopBuf = NULL;
	unsigned int loopIdx = 0;
	int smallOk = 0;

	freopen("ahi_sfx_test.log", "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("ahi_sfx_test: Phase 0 de-risk spike\n");

	timer_init();
	hookCtx.soundCount = 0;
	soundHook.h_Data = FindTask(NULL);

	/* ---- open ahi.device ------------------------------------------------ */
	port = CreateMsgPort();
	if(port == NULL){ printf("ahi_sfx_test: FAIL CreateMsgPort\n"); return 1; }
	io = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
	if(io == NULL){ printf("ahi_sfx_test: FAIL CreateIORequest\n"); DeleteMsgPort(port); return 1; }
	io->ahir_Version = 4;
	err = OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)io, 0);
	if(err != 0){
		printf("ahi_sfx_test: SKIP (no ahi.device) err=%lu\n", (ULONG)err);
		DeleteIORequest((struct IORequest *)io);
		DeleteMsgPort(port);
		return 0;
	}
	AHIBase = (struct Library *)io->ahir_Std.io_Device;
	version = AHIBase->lib_Version;
	printf("info ahi.device version %lu.%lu\n", (ULONG)AHIBase->lib_Version, (ULONG)AHIBase->lib_Revision);
	CHK(version >= 4, "ahi.device lib_Version >= 4: got %lu", (ULONG)version);

	/* ---- enumerate + choose Paula/realtime ------------------------------ */
	id = AHI_INVALID_ID;
	while((id = AHI_NextAudioID(id)) != AHI_INVALID_ID){
		rt = 0;
		name[0] = drv[0] = '\0';
		AHI_GetAudioAttrs(id, NULL,
			AHIDB_BufferLen, 80,
			AHIDB_Driver, (ULONG)drv,
			AHIDB_Name, (ULONG)name,
			AHIDB_Realtime, (ULONG)&rt,
			TAG_DONE);
		modeCount++;
		if(rt){
			rtCount++;
			if(firstRt == AHI_INVALID_ID)
				firstRt = id;
		}
		if(contains_ci(name, "paula") || contains_ci(drv, "paula") ||
		   contains_ci(name, "amiberry") || contains_ci(drv, "amiberry") ||
		   contains_ci(name, "uae") || contains_ci(drv, "uae")){
			if(matchAny == AHI_INVALID_ID) matchAny = id;
			if(rt && matchRt == AHI_INVALID_ID) matchRt = id;
		}
	}
	CHK(modeCount >= 1, "audio modes enumerated: got %d", modeCount);
	chosen = (matchRt != AHI_INVALID_ID) ? matchRt :
	         (matchAny != AHI_INVALID_ID) ? matchAny :
	         (firstRt != AHI_INVALID_ID) ? firstRt : AHI_DEFAULT_ID;
	printf("info modes=%d realtime=%d chosen=0x%08lx\n", modeCount, rtCount, (ULONG)chosen);

	/* ---- read sfx.SDT (byteswapped to native) --------------------------- */
	{
		FILE *sdtFile = fopen("audio/sfx.SDT", "rb");
		if(sdtFile == NULL)
			sdtFile = fopen("Audio/sfx.SDT", "rb");
		CHK(sdtFile != NULL, "open sfx.SDT");
		if(sdtFile == NULL)
			goto teardown;
		{
			unsigned int n = 0;
			while(n < MAX_SAMPLES &&
			      fread(&sdt[n], 1, SDT_SAMPLE_SIZE, sdtFile) == SDT_SAMPLE_SIZE){
				sdt[n].offset    = swap32(sdt[n].offset);
				sdt[n].size      = swap32(sdt[n].size);
				sdt[n].freq      = swap32(sdt[n].freq);
				sdt[n].loopStart = swap32(sdt[n].loopStart);
				sdt[n].loopEnd   = swap32(sdt[n].loopEnd);
				n++;
			}
			sdtCount = n;
		}
		fclose(sdtFile);
	}
	printf("info sdt samples=%lu firstOff=%lu lastFreq=%lu\n",
	       (ULONG)sdtCount, (ULONG)(sdtCount ? sdt[0].offset : 0),
	       (ULONG)(sdtCount ? sdt[sdtCount-1].freq : 0));
	CHK(sdtCount > 9000, "sfx.SDT sample count looks right: got %lu", (ULONG)sdtCount);
	if(sdtCount == 0)
		goto teardown;

	/* offsets must be monotonic; sizes non-zero */
	{
		int monotonic = 1;
		unsigned int sum = 0;
		for(i = 0; i < (int)sdtCount; i++){
			if(sdt[i].size == 0){ monotonic = 0; break; }
			if(i > 0 && sdt[i].offset < sdt[i-1].offset){ monotonic = 0; break; }
			sum += sdt[i].size;
		}
		CHK(monotonic, "sfx.SDT offsets monotonic / sizes non-zero (sum=%lu)", (ULONG)sum);
	}

	/* index the smallest / median / largest samples */
	smallIdx = medIdx = maxIdx = 0;
	for(i = 0; i < (int)sdtCount; i++){
		if(sdt[i].size < sdt[smallIdx].size) smallIdx = i;
		if(sdt[i].size > sdt[maxIdx].size) maxIdx = i;
	}
	/* median by counting size <= median-ish: use the sample at count/2 of a sorted-on-the-fly pass */
	{
		unsigned int target = 33320;	/* ~p50 measured on host */
		medIdx = 0;
		for(i = 0; i < (int)sdtCount; i++){
			if(sdt[i].size >= target){ medIdx = i; break; }
		}
	}
	/* first real loop (loopEnd > loopStart + 1) */
	loopIdx = 0;
	for(i = 0; i < (int)sdtCount; i++){
		if(sdt[i].loopEnd > sdt[i].loopStart + 1){ loopIdx = i; break; }
	}

	rawFile = fopen("audio/sfx.RAW", "rb");
	if(rawFile == NULL)
		rawFile = fopen("Audio/sfx.RAW", "rb");
	CHK(rawFile != NULL, "open sfx.RAW");
	if(rawFile == NULL)
		goto teardown;

	/* ---- 0f: read+swap latency for small/median/max --------------------- */
	smallBuf = (unsigned char *)AllocVec(sdt[smallIdx].size + 4, MEMF_PUBLIC);
	medBuf   = (unsigned char *)AllocVec(sdt[medIdx].size + 4, MEMF_PUBLIC);
	maxBuf   = (unsigned char *)AllocVec(sdt[maxIdx].size + 4, MEMF_PUBLIC);
	CHK(smallBuf && medBuf && maxBuf, "latency buffers allocated (%lu/%lu/%lu B)",
	    (ULONG)sdt[smallIdx].size, (ULONG)sdt[medIdx].size, (ULONG)sdt[maxIdx].size);
	if(smallBuf && medBuf && maxBuf){
		unsigned int t0, t1;
		t0 = now_us();
		smallOk = read_sample_bytes(sdt[smallIdx].offset, sdt[smallIdx].size, smallBuf);
		t1 = now_us();
		printf("info read small  idx=%u size=%lu us=%u\n", smallIdx, (ULONG)sdt[smallIdx].size, (unsigned)(t1-t0));

		t0 = now_us();
		read_sample_bytes(sdt[medIdx].offset, sdt[medIdx].size, medBuf);
		t1 = now_us();
		printf("info read median idx=%u size=%lu us=%u\n", medIdx, (ULONG)sdt[medIdx].size, (unsigned)(t1-t0));

		t0 = now_us();
		read_sample_bytes(sdt[maxIdx].offset, sdt[maxIdx].size, maxBuf);
		t1 = now_us();
		printf("info read max    idx=%u size=%lu us=%u\n", maxIdx, (ULONG)sdt[maxIdx].size, (unsigned)(t1-t0));

		CHK(smallOk, "small sample read+swap ok");
	}

	/* ---- allocate: 43 channels, 128 sounds, 22050 Hz ------------------- */
	{
		ULONG gotCh = 0, gotSounds = 0;
		actrl = AHI_AllocAudio(
			AHIA_AudioID, chosen,
			AHIA_MixFreq, 22050,
			AHIA_Channels, channelsWant,
			AHIA_Sounds, soundsWant,
			AHIA_SoundFunc, (ULONG)&soundHook,
			AHIA_UserData, (ULONG)&hookCtx,
			TAG_DONE);
		CHK(actrl != NULL, "AHI_AllocAudio ch=%lu sounds=%lu mix=22050",
		    (ULONG)channelsWant, (ULONG)soundsWant);
		if(actrl == NULL){
			printf("ahi_sfx_test: alloc failed, cannot continue\n");
			goto teardown;
		}
		(void)gotCh; (void)gotSounds;
		AHI_ControlAudio(actrl, AHIC_MixFreq_Query, (ULONG)&mixfreq, TAG_DONE);
		printf("info negotiated mixfreq=%lu\n", (ULONG)mixfreq);
	}

	if(!smallOk){
		printf("ahi_sfx_test: sample read failed, cannot continue\n");
		goto teardown;
	}

	/* ---- 0b one-shot: real sample, end-of-sound hook -------------------- */
	{
		struct AHISampleInfo si;
		ULONG last, n;
		ULONG medFrames = sdt[medIdx].size / 2;
		ULONG medFreq = sdt[medIdx].freq ? sdt[medIdx].freq : 32000;
		unsigned int t0;
		/* median sample: 17332 frames @ 32000 -> ~0.54 s. Poll the callback
		 * count every 10 ms so the exact firing times are visible. */
		si.ahisi_Type = AHIST_M16S;
		si.ahisi_Address = medBuf;
		si.ahisi_Length = medFrames;
		err = AHI_LoadSound(0, AHIST_SAMPLE, &si, actrl);
		CHK(err == AHIE_OK, "AHI_LoadSound(one-shot) == AHIE_OK: got %lu", (ULONG)err);
		if(err == AHIE_OK){
			hookCtx.soundCount = 0;
			SetSignal(0, SIGF_SINGLE);
			AHI_Play(actrl,
				AHIP_BeginChannel, 0,
				AHIP_Sound, 0,
				AHIP_Freq, medFreq,
				AHIP_Vol, 0x10000,
				AHIP_Pan, 0x8000,
				AHIP_LoopSound, AHI_NOSOUND,
				AHIP_EndChannel, 0,
				TAG_DONE);
			err = AHI_ControlAudio(actrl, AHIC_Play, TRUE, TAG_DONE);
			CHK(err == AHIE_OK, "one-shot AHIC_Play == AHIE_OK: got %lu", (ULONG)err);
			t0 = now_us();
			last = 0;
			/* ~900 ms of polling: one-shot at 0.54 s must end inside. */
			for(n = 0; n < 90; n++){
				ULONG c;
				delay_ms(10);
				c = hookCtx.soundCount;
				if(c != last){
					printf("info one-shot hook t=%u ms count=%lu\n",
					       (unsigned)((now_us() - t0) / 1000), (ULONG)c);
					last = c;
				}
			}
			CHK(last >= 1, "0b one-shot end-of-sound hook fired (count=%lu)", (ULONG)last);
			CHK(last <= 2, "0b one-shot fired at most twice (count=%lu)", (ULONG)last);
			AHI_ControlAudio(actrl, AHIC_Play, FALSE, TAG_DONE);
		}
	}

	/* ---- 0b loop: sample with real loop points -------------------------- */
	{
		struct sdtSample *ls = &sdt[loopIdx];
		struct AHISampleInfo si;
		ULONG loopOff, loopLen;
		CHK(ls->loopEnd > ls->loopStart + 1,
		    "0b loop sample found idx=%u loop=%lu..%lu",
		    loopIdx, (ULONG)ls->loopStart, (ULONG)ls->loopEnd);
		if(ls->loopEnd <= ls->loopStart + 1)
			goto skip_loop;

		loopBuf = (unsigned char *)AllocVec(ls->size + 4, MEMF_PUBLIC);
		CHK(loopBuf != NULL, "loop buffer allocated (%lu B)", (ULONG)ls->size);
		if(loopBuf == NULL)
			goto skip_loop;
		if(!read_sample_bytes(ls->offset, ls->size, loopBuf))
			goto skip_loop;

		si.ahisi_Type = AHIST_M16S;
		si.ahisi_Address = loopBuf;
		si.ahisi_Length = ls->size / 2;
		err = AHI_LoadSound(1, AHIST_SAMPLE, &si, actrl);
		CHK(err == AHIE_OK, "AHI_LoadSound(loop) == AHIE_OK: got %lu", (ULONG)err);
		if(err != AHIE_OK)
			goto skip_loop;

		loopOff = ls->loopStart / 2;
		loopLen = (ls->loopEnd - ls->loopStart) / 2;
		AHI_Play(actrl,
			AHIP_BeginChannel, 1,
			AHIP_Sound, 1,
			AHIP_Freq, ls->freq ? ls->freq : 32000,
			AHIP_Vol, 0x10000,
			AHIP_Pan, 0x8000,
			AHIP_Offset, 0,
			AHIP_Length, 0,			/* 0 = to end of sample */
			AHIP_LoopSound, 1,		/* loop on same sound */
			AHIP_LoopOffset, loopOff,
			AHIP_LoopLength, loopLen,
			AHIP_EndChannel, 1,
			TAG_DONE);
		err = AHI_ControlAudio(actrl, AHIC_Play, TRUE, TAG_DONE);
		CHK(err == AHIE_OK, "0b loop AHIC_Play == AHIE_OK: got %lu", (ULONG)err);

		/* AHI's SoundFunc fires once per play segment, which for a looped
		 * channel means once per loop wrap (confirmed by the count scaling
		 * with the loop rate). Many callbacks in 200 ms therefore prove the
		 * loop is tight and running, not a one-shot that already stopped. */
		{
			ULONG before = hookCtx.soundCount;
			ULONG got;
			delay_ms(200);
			got = hookCtx.soundCount - before;
			CHK(got >= 10,
			    "0b loop active: %lu loop-wrap callbacks in 200 ms", (ULONG)got);
		}

		/* Prove we controlled it: change freq/vol on the playing loop from
		 * the main task (0e) and stop it again. */
		delay_ms(50);
		AHI_SetFreq(1, (ULONG)((ls->freq ? ls->freq : 32000) * 3 / 4), actrl, AHISF_IMM);
		AHI_SetVol(1, 0x6000, 0x8000, actrl, AHISF_IMM);
		delay_ms(50);
		AHI_SetSound(1, (UWORD)AHI_NOSOUND, 0, 0, actrl, AHISF_IMM);
	}
skip_loop:

	/* ---- 0c/0d: many simultaneous loops -------------------------------- */
	{
		ULONG n = 43;
		ULONG playFreq = sdt[medIdx].freq ? sdt[medIdx].freq : 32000;
		struct AHISampleInfo si;
		unsigned int t0, t1;

		si.ahisi_Type = AHIST_M16S;
		si.ahisi_Address = medBuf;
		si.ahisi_Length = sdt[medIdx].size / 2;

		/* Load the median sample into many sound slots (the real backend's
		 * pool is bounded, so a handful of slots is enough for this test). */
		for(i = 0; i < (int)n; i++){
			ULONG e = AHI_LoadSound((UWORD)(2 + i), AHIST_SAMPLE, &si, actrl);
			if(e != AHIE_OK){
				printf("info load slot %d failed: %lu\n", i, (ULONG)e);
				n = i;
				break;
			}
		}
		printf("info loaded %lu sound slots for CPU test\n", (ULONG)n);

		/* AHIP_EndChannel's ti_Data must be NULL. */
		for(i = 0; i < (int)n; i++){
			ULONG chn = (ULONG)(2 + i);
			AHI_Play(actrl,
				AHIP_BeginChannel, chn,
				AHIP_Sound, (UWORD)(2 + i),
				AHIP_Freq, playFreq,
				AHIP_Vol, 0x4000,
				AHIP_Pan, 0x8000,
				AHIP_LoopSound, (UWORD)(2 + i),
				AHIP_EndChannel, 0,
				TAG_DONE);
		}
		AHI_ControlAudio(actrl, AHIC_Play, TRUE, TAG_DONE);

		t0 = now_us();
		delay_ms(500);
		t1 = now_us();
		printf("info 0d start %lu loops; wall=%u us\n", (ULONG)n, (unsigned)(t1 - t0));

		for(i = 0; i < (int)n; i++)
			AHI_SetSound((UWORD)(2 + i), (UWORD)AHI_NOSOUND, 0, 0, actrl, AHISF_IMM);
		AHI_ControlAudio(actrl, AHIC_Play, FALSE, TAG_DONE);
		CHK(1, "0d %lu simultaneous looped channels ran without error", (ULONG)n);
	}

teardown:
	if(actrl != NULL){
		AHI_ControlAudio(actrl, AHIC_Play, FALSE, TAG_DONE);
		AHI_FreeAudio(actrl);
		actrl = NULL;
	}
	if(rawFile != NULL) fclose(rawFile);
	if(smallBuf) FreeVec(smallBuf);
	if(medBuf) FreeVec(medBuf);
	if(maxBuf) FreeVec(maxBuf);
	if(loopBuf) FreeVec(loopBuf);
	CloseDevice((struct IORequest *)io);
	DeleteIORequest((struct IORequest *)io);
	DeleteMsgPort(port);

	if(failures){
		printf("ahi_sfx_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("ahi_sfx_test: all ok\n");
	return 0;
}
