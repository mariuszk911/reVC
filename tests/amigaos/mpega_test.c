/*
 * mpega_test.c -- standalone validation of the mpega.library stream decoder
 * used by the AHI stream bus.
 *
 * Proves, on the real target, before any sampman_ahi stream wiring:
 *   a) OpenLibrary("mpega.library") and the MPEGA_CTRL/MPEGA_STREAM ABI
 *      (GCC-packed structs) actually work;
 *   b) the MPEGA_CTRL.bs_access bitstream hook is called and gets our FILE*;
 *   c) the .adf whole-file XOR-0x22 obfuscation is undone in the READ hook so
 *      the decoder sees a clean MPEG stream (WILD.adf -> MPEG1 L3 44100);
 *   d) MPEGA_decode_frame produces samples and MPEGA_seek() jumps;
 *   e) wall-clock decode rate (ms of audio decoded per wall second) to size the
 *      per-Service() decode budget.
 *
 * Build: tests/amigaos/build_mpega_test.sh  (output build/amigaos/test/mpega_test)
 * Run from the game drawer (holds Audio/):
 *   CD DH12: && DH11:mpega_test    -> writes DH12:mpega_test.log
 * Terminal line: `mpega_test: all ok` / `mpega_test: N failure(s)` /
 *                `mpega_test: SKIP (no mpega.library)`.
 *
 * libnix %f/%g printing is broken on target; this app logs integers only.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <utility/hooks.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdio.h>
#include <string.h>

#include "mpega_lib.h"

struct Library *MPEGABase = NULL;

/* ---- timer.device EClock for timings ----------------------------------- */
struct Device *TimerBase;
static struct timerequest TestTimerReq;
static unsigned int eclockScale;

static int
timer_init(void)
{
	if (TimerBase != NULL)
		return 1;
	if (OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
	               (struct IORequest *)&TestTimerReq, 0) != 0)
		return 0;
	TimerBase = TestTimerReq.tr_node.io_Device;
	return TimerBase != NULL;
}

static unsigned int
now_us(void)
{
	struct EClockVal ev;
	ULONG freq;

	if (TimerBase == NULL)
		return 0;
	freq = ReadEClock(&ev);
	if (freq == 0)
		return 0;
	if (eclockScale == 0)
		eclockScale = (unsigned int)(((unsigned long long)1000000 << 16) / freq);
	return (unsigned int)(((unsigned long long)ev.ev_lo * eclockScale) >> 16);
}

/* ---- bitstream access hook (XOR for .adf) ------------------------------ */
struct BsCtx {
	FILE *fp;
	int   xor;
	unsigned long reads, bytes;
};

static struct BsCtx gBs;

static ULONG
bs_func(register struct Hook *hook __asm("a0"),
        register APTR handle __asm("a2"),
        register MPEGA_ACCESS *acc __asm("a1"))
{
	struct BsCtx *ctx = (struct BsCtx *)hook->h_Data;

	switch (acc->func) {
	case MPEGA_BSFUNC_OPEN: {
		FILE *fp = fopen(acc->data.open.stream_name, "rb");
		if (fp == NULL)
			return 0;
		fseek(fp, 0, SEEK_END);
		acc->data.open.stream_size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		ctx->fp = fp;
		return (ULONG)fp;
	}
	case MPEGA_BSFUNC_CLOSE:
		if (handle != NULL) {
			fclose((FILE *)handle);
			if ((FILE *)handle == ctx->fp)
				ctx->fp = NULL;
		}
		return 0;
	case MPEGA_BSFUNC_READ: {
		ULONG got = (ULONG)fread(acc->data.read.buffer, 1,
		                         (size_t)acc->data.read.num_bytes, (FILE *)handle);
		if (got > 0 && ctx->xor) {
			unsigned char *p = (unsigned char *)acc->data.read.buffer;
			ULONG i;
			for (i = 0; i < got; i++)
				p[i] ^= (unsigned char)ctx->xor;
		}
		ctx->reads++;
		ctx->bytes += got;
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

static struct Hook gBsHook = { { NULL, NULL }, NULL, NULL, NULL };

static MPEGA_STREAM *
open_stream(const char *name, int xor, MPEGA_CTRL *ctrl)
{
	gBs.fp = NULL;
	gBs.xor = xor;
	gBs.reads = 0;
	gBs.bytes = 0;
	gBsHook.h_Entry = (ULONG (*)())bs_func;
	gBsHook.h_Data = &gBs;

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->bs_access = &gBsHook;
	ctrl->check_mpeg = 1;
	ctrl->stream_buffer_size = 8192;      /* multiple of 4 */
	ctrl->layer_3.stereo.freq_div = 1;
	ctrl->layer_3.mono.freq_div = 1;
	ctrl->layer_1_2.stereo.freq_div = 1;
	ctrl->layer_1_2.mono.freq_div = 1;
	ctrl->layer_3.stereo.quality = MPEGA_QUALITY_HIGH;
	ctrl->layer_3.mono.quality = MPEGA_QUALITY_HIGH;
	ctrl->layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
	ctrl->layer_1_2.mono.quality = MPEGA_QUALITY_HIGH;

	return MPEGA_open((char *)name, ctrl);
}

static int failures;

#define CHK(cond, ...) do { \
	if (cond) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
	else { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* Decode up to `want` frames; return total per-channel samples and peak. */
static unsigned long
decode_frames(MPEGA_STREAM *mps, int want, unsigned int *out_us, int *out_peak)
{
	WORD b0[MPEGA_PCM_SIZE], b1[MPEGA_PCM_SIZE];
	WORD *pcm[2];
	unsigned long total = 0;
	unsigned int t0, t1;
	int got = 0, guard = 0, peak = 0, i;

	pcm[0] = b0;
	pcm[1] = b1;
	t0 = now_us();
	while (got < want) {
		LONG n = MPEGA_decode_frame(mps, pcm);
		if (n == MPEGA_ERR_EOF)
			break;
		if (n < 0) {
			if (++guard > 64)
				break;
			continue;
		}
		if (n == 0) {                 /* skipped frame */
			if (++guard > 256)
				break;
			continue;
		}
		for (i = 0; i < n; i++) {     /* sample content, not just count */
			int a = b0[i] < 0 ? -(int)b0[i] : (int)b0[i];
			int b = b1[i] < 0 ? -(int)b1[i] : (int)b1[i];
			if (a > peak) peak = a;
			if (b > peak) peak = b;
		}
		total += (unsigned long)n;
		got++;
	}
	t1 = now_us();
	if (out_us)
		*out_us = t1 - t0;
	if (out_peak)
		*out_peak = peak;
	return total;
}

int
main(void)
{
	MPEGA_CTRL ctrl;
	MPEGA_STREAM *mps;

	freopen("mpega_test.log", "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
	timer_init();

	/* Prefer the installed library (the real deployment path). Fall back to a
	 * copy sitting next to the test in DH11 so this can run without touching
	 * the shared system LIBS: partition. OpenLibrary needs the basename to
	 * match the library's node name ("mpega.library"). */
	{
		static const char *const where[] = {
			"mpega.library",
			"LIBS:mpega.library",
			"DH11:mpega.library",
			"DH11:mpega040.library",
			"DH11:mpega020.library",
			NULL
		};
		int k;
		for (k = 0; where[k] != NULL && MPEGABase == NULL; k++) {
			MPEGABase = OpenLibrary((STRPTR)where[k], 2);
			printf("info open '%s' -> %s\n", where[k], MPEGABase ? "ok" : "fail");
		}
	}
	if (MPEGABase == NULL) {
		printf("mpega_test: SKIP (no mpega.library)\n");
		return 0;
	}
	printf("mpega.library v%lu.%lu ok\n",
	       (unsigned long)MPEGABase->lib_Version,
	       (unsigned long)MPEGABase->lib_Revision);

	/* ---- A: .adf (XOR-0x22) radio, decode + seek ---------------------- */
	printf("--- WILD.adf (XOR 0x22) ---\n");
	mps = open_stream("Audio/WILD.adf", 0x22, &ctrl);
	CHK(mps != NULL, "A0 open Audio/WILD.adf (XOR 0x22)");
	if (mps != NULL) {
		unsigned long samples;
		unsigned int us;
		unsigned long dur, seek_ms;
		int peak = 0;

		printf("info norm=%d layer=%d mode=%d bitrate=%d freq=%ld ch=%d dur=%lu dec=%ld dch=%d\n",
		       (int)mps->norm, (int)mps->layer, (int)mps->mode,
		       (int)mps->bitrate, (long)mps->frequency, (int)mps->channels,
		       (unsigned long)mps->ms_duration, (long)mps->dec_frequency,
		       (int)mps->dec_channels);
		CHK(mps->frequency == 44100, "A1 frequency 44100 (got %ld)", (long)mps->frequency);
		CHK(mps->ms_duration > 60000, "A2 duration > 60s (got %lums)", (unsigned long)mps->ms_duration);
		CHK(gBs.reads > 0, "A3 bitstream hook READ called (%lu reads)", gBs.reads);

		/* Decode ~2 s of audio and time it. */
		samples = decode_frames(mps, 88, &us, &peak);
		dur = (unsigned long)mps->dec_frequency ? (unsigned long)mps->dec_frequency : 44100;
		CHK(samples >= 88 * 1152 - 1152, "A4 decoded ~88 frames (%lu samples)", samples);
		printf("info peak=%d (0 = silence)\n", peak);
		CHK(peak > 256, "A4b decoded PCM is audible (peak=%d)", peak);
		if (us > 0) {
			unsigned long audio_ms = (samples * 1000ul) / dur;
			printf("info decode: %lu ms audio in %u us wall (%lu x realtime)\n",
			       audio_ms, us, us ? (unsigned long)((unsigned long long)audio_ms * 1000000ul / us) : 0);
			CHK(audio_ms * 1000000ul / us >= 1000ul,
			    "A5 decode >= 1.0x realtime");
		}

		seek_ms = mps->ms_duration / 3;
		CHK(MPEGA_seek(mps, seek_ms) == 0, "A6 MPEGA_seek(%lums) ok", seek_ms);
		samples = decode_frames(mps, 8, NULL, &peak);
		CHK(samples >= 8 * 1152 - 1152, "A7 decoded after seek (%lu samples)", samples);
		printf("info peak-after-seek=%d\n", peak);
		CHK(peak > 256, "A7b PCM audible after seek (peak=%d)", peak);

		MPEGA_close(mps);
	}

	/* ---- B: small .mp3, full decode ----------------------------------- */
	printf("--- ambsil.mp3 (plain) ---\n");
	mps = open_stream("Audio/ambsil.mp3", 0, &ctrl);
	CHK(mps != NULL, "B0 open Audio/ambsil.mp3 (plain)");
	if (mps != NULL) {
		unsigned long samples = decode_frames(mps, 100000, NULL, NULL);
		CHK(samples > 0, "B1 decoded %lu samples from .mp3", samples);
		CHK(mps->ms_duration > 0, "B2 .mp3 duration %lums", (unsigned long)mps->ms_duration);
		MPEGA_close(mps);
	}

	/* ---- C: find_sync helper ------------------------------------------ */
	{
		FILE *fp = fopen("Audio/WILD.adf", "rb");
		CHK(fp != NULL, "C0 fopen WILD.adf");
		if (fp != NULL) {
			unsigned char buf[4096];
			unsigned long got, k;
			LONG pos;
			got = (unsigned long)fread(buf, 1, sizeof(buf), fp);
			fclose(fp);
			for (k = 0; k < got; k++)
				buf[k] ^= 0x22;
			pos = MPEGA_find_sync((BYTE *)buf, (LONG)got);
			CHK(pos == 0, "C1 find_sync finds frame at 0 (got %ld)", (long)pos);
		}
	}

	if (gBs.fp != NULL)
		fclose(gBs.fp);

	if (failures) {
		printf("mpega_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("mpega_test: all ok\n");
	return 0;
}
