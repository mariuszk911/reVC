/*
 * In-game frame-profiler overlay and trace sinks.
 *
 * Drawn from Render2dStuffAfterFade (before CFont::DrawFonts) when REVC_PROFILE
 * is on. Three display states cycled by F10 (hidden -> compact -> detailed);
 * tracing continues while hidden so clean A/B numbers are possible. Only integer
 * formatting is used: libnix %f misrounds on target.
 *
 * The trace sinks are also installed here: the 30-frame aggregate line goes to
 * the boot log, each slow frame appends (and flushes) profile_slow.txt, and the
 * full ring is dumped to profile_frames.txt at clean shutdown, and each Shift+F10
 * snapshot goes to its own profile_frames_<n>.txt.
 */

#include "common.h"
#include "FrameProfile.h"
#include "Platform.h"
#include "Font.h"
#include "Sprite2d.h"

// RwMiniStats (the librw pull query the detailed panel prints). The AmigaOS
// skeleton is the only platform that builds this TU, so the librw seam header
// is available here without spreading it through the rest of src/core.
#include "mini/rwminihost.h"

#include <stdio.h>
#include <string.h>

#ifdef REVC_FP_LEAN
// Keep the detailed rows' conditions visible to the compiler but constant, so
// the lean build drops them without #ifdef-ing the surrounding formatting
// code.
#define REVC_FP_LEAN_BUILD 1
#else
#define REVC_FP_LEAN_BUILD 0
#endif

#ifdef AMIGAOS
#include "amiga_boot.h"
#endif

static FILE *fpSlowFile = 0;

// Overlay scratch: at most 13 lines (the detailed page is exactly 13), 320 chars
// each is enough for the widest detailed "phases:" row (15 slots) with 4-digit
// ms values; the row builder still bounds its writes (see fpDrawOverlay).
#define FP_OVL_MAXLINES 13
#define FP_OVL_LINELEN  320

static void
fpAggregateSink(const char *line)
{
#ifdef AMIGAOS
	amiga_boot_log(line);
#else
	fputs(line, stderr);
	fputc('\n', stderr);
#endif
}

static void
fpSlowSink(const char *line)
{
	// Write only the crash-safe profile_slow.txt trace; do NOT also mirror every
	// slow frame through amiga_boot_log (write()+dos Flush() x2 per line). The
	// sink runs from fpEndFrame, after the frame's slots are closed, so that I/O
	// is charged to the next frame's FP_FRAME/FP_OUTSIDE and perturbs the very
	// timing being measured. The slow-frame data is already in profile_slow.txt.
	if(fpSlowFile == 0)
		return;
	fputs(line, fpSlowFile);
	fputc('\n', fpSlowFile);
	fflush(fpSlowFile);
}

// Open (truncate) the slow-frame trace and route the aggregate line to the boot
// log. Called once from main() when REVC_PROFILE resolves true.
void
fpInstallSinks(void)
{
	fpSlowFile = fopen("profile_slow.txt", "w");
	fp::fpSetLineSink(fpAggregateSink, fpSlowSink);
}

// Write the whole ring to `path` (truncate). No per-line log mirroring: that
// would be ~2x write+Flush per ring entry (hundreds of DOS flushes) inside the
// Shift+F10 key handler / shutdown path. `tag` identifies the capture in the log.
static void
fpWriteRing(const char *tag, const char *path)
{
	FILE *f;
	int i, n;

	f = fopen(path, "w");
	n = fp::fpRingCount();
	for(i = 0; i < n; i++){
		const fp::FrameRecord *r = fp::fpRingAt(i);
		char buf[fp::FP_LINE_MAX];
		fp::fpFmtFrameLine(buf, (unsigned int)(i + 1), r);
		if(f){
			fputs(buf, f);
			fputc('\n', f);
		}
	}
	if(f)
		fclose(f);
#ifdef AMIGAOS
	{
		char msg[96];
		sprintf(msg, "prof: %s ring dumped to %s (%d frames)", tag, path, n);
		amiga_boot_log(msg);
	}
#endif
}

// Shift+F10 while profiling: snapshot the current ring immediately. Each snapshot
// gets its own file (profile_frames_1.txt, _2.txt, ...) so successive dumps do
// not overwrite each other.
static unsigned int fpDumpSeq = 0;

void
fpDumpRingNow(void)
{
	char path[48];
	char tag[32];
	fpDumpSeq++;
	sprintf(path, "profile_frames_%u.txt", fpDumpSeq);
	sprintf(tag, "Shift+F10 #%u", fpDumpSeq);
	fpWriteRing(tag, path);
}

// Dump the whole ring at clean shutdown and close the slow trace. (The exit dump
// is only useful when the session ends in gameplay; at high menu fps even a
// 600-frame ring is a few seconds, so a Shift+F10 snapshot is the reliable path.)
void
fpShutdownDump(void)
{
	if(fpSlowFile){
		fclose(fpSlowFile);
		fpSlowFile = 0;
	}
	fpWriteRing("exit", "profile_frames.txt");
}

// --- overlay -----------------------------------------------------------------

static void
fpOverlayPrint(float x, float y, const char *s)
{
	wchar ustr[FP_OVL_LINELEN];
	AsciiToUnicode((char*)s, ustr);
	CFont::PrintString(x, y, ustr);
}

// Format microseconds with the profiler's single integer-tenths rule (fpFmt10,
// which rounds) into a rotating buffer, so the overlay and the `prof:` trace
// never disagree by a rounding step. Up to four values are consumed per line.
static const char *
F10(unsigned int us)
{
	enum { N = 16 };
	static char bufs[N][16];
	static unsigned int idx;
	char *b = bufs[idx];
	idx = (idx + 1) % N;
	fp::fpFmt10(us, b);
	return b;
}

static void
fpOverlayAdd(char text[FP_OVL_MAXLINES][FP_OVL_LINELEN], int *n, const char *s)
{
	if(*n >= FP_OVL_MAXLINES)
		return;
	strncpy(text[*n], s, FP_OVL_LINELEN - 1);
	text[*n][FP_OVL_LINELEN - 1] = '\0';
	(*n)++;
}

void
fpDrawOverlay(void)
{
	char text[FP_OVL_MAXLINES][FP_OVL_LINELEN];
	char line[FP_OVL_LINELEN];
	unsigned int fmin, fmax, favg, fps10, i;
	const fp::FrameRecord *r;
	float lh, x0, y0, pad;
	int n = 0, k;

	if(!fp::fpEnabled() || fp::fpOverlayMode() == 0)
		return;
	r = fp::fpLastRecord();
	if(r == 0)
		return;

#ifdef AMIGAOS
	{
		static int logged;
		if(!logged){
			logged = 1;
			amiga_boot_log("prof: overlay drawing");
		}
	}
#endif

	fp::fpWindowFrameStats(&fmin, &fmax, &favg);
	fps10 = favg ? (10000000u / favg) : 0;

	sprintf(line, "frame %s ms   fps %u.%u   min %s ms   max %s ms",
	        F10(r->frameUs), fps10 / 10u, fps10 % 10u, F10(fmin), F10(fmax));
	fpOverlayAdd(text, &n, line);

#ifndef REVC_FP_LEAN
	/* The per-slot rows are the first thing a lean build drops: they need the
	 * individual spans that REVC_FP_LEAN compiles out, so printing them would
	 * show zeros. */
	sprintf(line, "proc %s ms   rl %s ms   clear %s ms   scene %s ms",
	        F10(r->slotUs[fp::FP_PROCESS]), F10(r->slotUs[fp::FP_RENDERLIST]),
	        F10(r->slotUs[fp::FP_CLEAR]), F10(r->slotUs[fp::FP_SCENE]));
	fpOverlayAdd(text, &n, line);

	sprintf(line, "2d %s ms   pres %s ms",
	        F10(r->slotUs[fp::FP_2D]), F10(r->slotUs[fp::FP_PRESENT]));
	fpOverlayAdd(text, &n, line);
#endif

#ifdef AMIGAOS
	sprintf(line, "flags  TEXUNIQ%d  LOCKARRAYS%d  LOCKWIN%d",
	        amiga_boot_texuniq(),
	        amiga_boot_lockarrays(), amiga_boot_lockwin());
#else
	sprintf(line, "flags  (engine build)");
#endif
	fpOverlayAdd(text, &n, line);

	if(!REVC_FP_LEAN_BUILD && fp::fpOverlayMode() >= 2){
		int pos = snprintf(line, sizeof(line), "phases(ms):");
		for(i = fp::FP_OUTSIDE; i <= fp::FP_PRESENT; i++){
			const char *nm = fp::fpSlotName((int)i);
			int rem, w;
			if(nm[0] == '\0')
				continue;
			rem = (int)sizeof(line) - pos;
			if(rem <= 1)
				break;	// never write past the buffer (large slot values)
			w = snprintf(line + pos, (size_t)rem, " %s %s", nm, F10(r->slotUs[i]));
			if(w < 0)
				break;
			pos += (w >= rem) ? rem - 1 : w;
		}
		fpOverlayAdd(text, &n, line);

		sprintf(line, "game(ms) stream %s  script %s  pre %s  pop %s",
		        F10(r->slotUs[fp::FP_GAME_STREAM]), F10(r->slotUs[fp::FP_GAME_SCRIPT]),
		        F10(r->slotUs[fp::FP_GAME_WORLD_PRE]), F10(r->slotUs[fp::FP_GAME_POP]));
		fpOverlayAdd(text, &n, line);
		sprintf(line, "game(ms) misc %s  world %s  sim %s  cars %s",
		        F10(r->slotUs[fp::FP_GAME_MISC]), F10(r->slotUs[fp::FP_GAME_WORLD]),
		        F10(r->slotUs[fp::FP_GAME_SIM]), F10(r->slotUs[fp::FP_GAME_CARS]));
		fpOverlayAdd(text, &n, line);
#ifdef AMIGAOS
		sprintf(line, "slow #%u of %u    free %uKB",
		        fp::fpLastSlowSeq(), fp::fpSlowTotal(), amiga_boot_avail() / 1024u);
#else
		sprintf(line, "slow #%u of %u",
		        fp::fpLastSlowSeq(), fp::fpSlowTotal());
#endif
		fpOverlayAdd(text, &n, line);
	}

#ifdef AMIGAOS
	sprintf(line, "VA%d  ASYNC%d  %dx%d",
	        amiga_boot_vertexarray(), amiga_boot_async_stream(),
	        (int)RsGlobal.width, (int)RsGlobal.height);
#else
	sprintf(line, "page %u", (unsigned)fp::fpOverlayMode());
#endif
	fpOverlayAdd(text, &n, line);

	// Detailed-only librw block, pulled from the MINI backend (distinct
	// producer from the reVC rows above). Each value is unit-tagged: per-pass
	// times in us, submission counts in items, caps in texels/vertices/slots.
	if(!REVC_FP_LEAN_BUILD && fp::fpOverlayMode() >= 2){
		RwMiniStats ms;
		plat::MiniStats(ms);
		sprintf(line, "mini draw %uus  skin %uus  im2d %uus  im3d %uus",
		        ms.drawUs, ms.skinUs, ms.im2dUs, ms.im3dUs);
		fpOverlayAdd(text, &n, line);
		/* Two rows: one six-counter row does not fit a 640x480 panel. */
		sprintf(line, "mini mesh %uitms  draw %uitms  tex %uitms",
		        ms.meshes, ms.drawCalls, ms.texBinds);
		fpOverlayAdd(text, &n, line);
		sprintf(line, "mini st %uitms  chunk %uitms  vert %uitms",
		        ms.stateChanges, ms.chunks, ms.verts);
		fpOverlayAdd(text, &n, line);
		sprintf(line, "mini mode %s  vbuf %uverts  texbuf %uitms  maxtex %utex",
		        ms.vertexArrays ? "arrays" : "immediate",
		        ms.vertexBufferSize, ms.textureBufferSize, ms.maxTextureSize);
		fpOverlayAdd(text, &n, line);
	}

	/* Text metrics (prop on, same scale as the panel-relative print below). The
	 * panel spans the full screen width (see below) rather than hugging the
	 * widest line. */
	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetBackGroundOnlyTextOff();
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetJustifyOff();
	/* Wide wrap so the long detailed "phases:" line is clipped, not wrapped onto
	 * the next overlay line. */
	CFont::SetWrapx(SCREEN_WIDTH * 8.0f);
	CFont::SetFontStyle(FONT_STANDARD);
	CFont::SetScale(SCREEN_SCALE_X(0.55f), SCREEN_SCALE_Y(0.8f));

	x0 = SCREEN_SCALE_X(8.0f);
	y0 = SCREEN_SCALE_Y(8.0f);
	lh = SCREEN_SCALE_Y(11.0f);
	pad = SCREEN_SCALE_X(5.0f);

	/* Translucent gray gradient panel (~59% opaque at the top, ~90% at the
	 * bottom) behind white text, spanning the full screen width. Corner mapping
	 * of DrawAnyRect: (x1,y1)=bottom-left c0, (x2,y2)=bottom-right c1,
	 * (x3,y3)=top-left c2, (x4,y4)=top-right c3. */
	{
		float left = 0.0f;
		float right = SCREEN_WIDTH;
		float top = y0 - pad;
		float bottom = y0 + (float)(n - 1) * lh + SCREEN_SCALE_Y(11.0f) + pad;
		CRGBA cTop(34, 34, 34, 150);
		CRGBA cBot(34, 34, 34, 230);
		CSprite2d::DrawAnyRect(left, bottom, right, bottom, left, top, right, top,
		                       cBot, cBot, cTop, cTop);
	}

	CFont::SetColor(CRGBA(255, 255, 255, 255));
	CFont::SetDropColor(CRGBA(0, 0, 0, 220));
	CFont::SetDropShadowPosition(1);
	for(k = 0; k < n; k++)
		fpOverlayPrint(x0, y0 + (float)k * lh, text[k]);
}
