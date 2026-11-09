/*
 * Unit test for the frame profiler core (src/core/FrameProfile.h).
 *
 * The profiler is host-safe by design: it formats every value as integer tenths
 * (%f is broken in the target libnix) and its timestamp source can be replaced
 * with a synthetic clock, so the whole ring/window/slow-frame machinery is
 * exercised here without an emulator or a GPU.
 *
 * Covers: the skipped first frame after enable/reset, fpReset clearing the ring
 * and window, the 30-frame min/avg/max window, the ring wrap, the
 * explicit and auto (median) slow-frame predicates, the tenths formatter,
 * FP_OUTSIDE excluding the subset slots, the render-group push/pop mapping, the
 * line formatters, and the disabled no-op.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:frameprofile.log.
 */

#include <cstdio>
#include <cstring>

/* One TU, so the profiler can use its self-contained header-local state instead
 * of linking src/core/FrameProfile.cpp. */
#define FP_HEADER_ONLY
#include "FrameProfile.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Synthetic clock: g_now microseconds; g_now is advanced explicitly. */
static unsigned int g_now;
static unsigned int synthNow(void) { return g_now; }

#ifdef AMIGAOS
/* FrameProfile.h declares amiga_boot_us() extern "C" on AmigaOS; the test never
 * uses the real EClock, but the symbol must resolve for the link. */
extern "C" unsigned int amiga_boot_us(void) { return g_now; }
#endif

/* Force a genuine runtime read so gcc cannot constant-fold the calls. */
static unsigned int vi(unsigned int v) { volatile unsigned int t = v; return t; }

static void
fresh(void)
{
	fp::fpEnable(0);
	fp::fpSetSlowThresholdMs(0);
	fp::fpSetTimeFn(synthNow);
	fp::fpEnable(1);
	g_now = 0;
}

/* One begin/end cycle, advancing the clock by `us` before the begin. The first
 * cycle after a reset has no previous begin and is skipped by fpEndFrame. */
static void
cycle(unsigned int us)
{
	g_now += us;
	fp::fpBeginFrame();
	fp::fpEndFrame();
}

/* Run n recorded frames with the given period (drops the skipped first). */
static void
runFrames(int n, unsigned int us)
{
	int i;
	for (i = 0; i < n; i++)
		cycle(us);
}

static char g_capture[fp::FP_LINE_MAX];
static int g_captureCount;
static void captureSink(const char *line)
{
	if(g_captureCount == 0)
		std::strncpy(g_capture, line, sizeof(g_capture) - 1);
	else
		std::strncpy(g_capture, line, sizeof(g_capture) - 1);
	g_capture[sizeof(g_capture) - 1] = '\0';
	g_captureCount++;
}

int main()
{
#ifdef AMIGAOS
	freopen("DH11:frameprofile.log", "w", stdout);
#endif

	/* --- first frame after enable/reset is skipped (no valid period) --- */
	{
		fresh();
		fp::fpBeginFrame();
		fp::fpEndFrame();
		CHECK(fp::fpRingCount() == 0);
		CHECK(fp::fpFramesTotal() == 0);

		cycle(vi(10000));
		CHECK(fp::fpRingCount() == 1);
		CHECK(fp::fpLastRecord() != 0);
		CHECK(fp::fpLastRecord()->frameUs == 10000u);
	}

	/* --- fpReset clears ring, counters and window --- */
	{
		fresh();
		runFrames(vi(6), vi(10000));	/* first cycle is skipped */
		CHECK(fp::fpRingCount() == 5);
		fp::fpReset();
		CHECK(fp::fpRingCount() == 0);
		CHECK(fp::fpFramesTotal() == 0);
		CHECK(fp::fpLastRecord() == 0);
	}

	/* --- window min / avg / max --- */
	{
		unsigned int mn, mx, av;
		fresh();
		cycle(0);	/* skipped */
		cycle(10000);
		cycle(20000);
		cycle(30000);
		cycle(40000);
		CHECK(fp::fpRingCount() == 4);
		fp::fpWindowFrameStats(&mn, &mx, &av);
		CHECK(mn == 10000u);
		CHECK(mx == 40000u);
		CHECK(av == 25000u);
	}

	/* --- ring wraps at FP_RING, keeping the newest --- */
	{
		int i, total = (int)fp::FP_RING + 5;
		fresh();
		/* cycle k (1-based) has period 1000 + (k-1); the first cycle is skipped,
		 * so record r (0-based) has frameUs 1000 + r + ... see below. */
		for(i = 0; i < total; i++)
			cycle(1000u + (unsigned int)i);
		CHECK(fp::fpRingCount() == fp::FP_RING);
		/* Last record is the final cycle (period 1000 + total - 1). */
		CHECK(fp::fpLastRecord()->frameUs == 1000u + (unsigned int)(total - 1));
		/* Oldest retained is the record after the dropped ones; with total cycles
		 * there are total-1 records and (total-1-FP_RING) are dropped. */
		CHECK(fp::fpRingAt(0)->frameUs == 1000u + (unsigned int)(total - fp::FP_RING));
		CHECK(fp::fpRingAt(fp::FP_RING - 1) == fp::fpLastRecord());
	}

	/* --- explicit slow-frame threshold (REVC_PROFILE_MS) --- */
	{
		fresh();
		fp::fpSetSlowThresholdMs(vi(50));
		cycle(40000);	/* skipped */
		cycle(40000);
		CHECK(fp::fpLastRecord()->slow == 0u);
		cycle(60000);
		CHECK(fp::fpLastRecord()->slow == 1u);
		CHECK(fp::fpSlowTotal() == 1u);
		CHECK(fp::fpLastSlowSeq() == 2u);
	}

	/* --- auto predicate: 2 x window median, floored at 20 ms --- */
	{
		fresh();
		runFrames(vi(10), vi(10000));	/* median 10 ms, floor 20 ms */
		cycle(15000);			/* below the floor: not slow */
		CHECK(fp::fpLastRecord()->slow == 0u);
		cycle(100000);			/* well above: slow */
		CHECK(fp::fpLastRecord()->slow == 1u);
	}

	/* --- tenths formatter: integer only, no libc float --- */
	{
		char b[16];
		fp::fpFmt10(0, b);
		CHECK(std::strcmp(b, "0.0") == 0);
		fp::fpFmt10(50, b);
		CHECK(std::strcmp(b, "0.1") == 0);
		fp::fpFmt10(1000, b);
		CHECK(std::strcmp(b, "1.0") == 0);
		fp::fpFmt10(118400, b);
		CHECK(std::strcmp(b, "118.4") == 0);
		fp::fpFmt10(999, b);
		CHECK(std::strcmp(b, "1.0") == 0);
		fp::fpFmt10(1000000, b);
		CHECK(std::strcmp(b, "1000.0") == 0);
	}

	/* --- FP_OUTSIDE sums only the outer slots --- */
	{
		fresh();
		cycle(0);	/* skipped */
		g_now += 100000;
		fp::fpBeginFrame();
		fp::fpBegin(fp::FP_PROCESS);
		g_now += 30000;
		fp::fpEnd(fp::FP_PROCESS);
		fp::fpBegin(fp::FP_SCENE);
		g_now += 20000;
		fp::fpEnd(fp::FP_SCENE);
		/* subset slot measured inside FP_SCENE: must NOT reduce FP_OUTSIDE */
		fp::fpAdd(fp::FP_LIT, 15000);
		fp::fpAdd(fp::FP_GAME_WORLD, 5000);
		fp::fpEndFrame();
		CHECK(fp::fpLastRecord()->slotUs[fp::FP_OUTSIDE] == 50000u);
		CHECK(fp::fpLastRecord()->slotUs[fp::FP_LIT] == 15000u);
		CHECK(fp::fpLastRecord()->slotUs[fp::FP_GAME_WORLD] == 5000u);
	}

	/* --- render-group push/pop maps the top-level passes --- */
	{
		fresh();
		cycle(0);	/* skipped */
		g_now += 100000;
		fp::fpBeginFrame();
		fp::fpPushGroup("RenderScene");
		g_now += 20000;
		fp::fpPushGroup("CClouds::Render");	/* unknown: no slot, nested */
		g_now += 5000;
		fp::fpPopGroup();
		g_now += 3000;
		fp::fpPopGroup();
		fp::fpPushGroup("Render2dStuff");
		g_now += 4000;
		fp::fpPopGroup();
		fp::fpEndFrame();
		CHECK(fp::fpLastRecord()->slotUs[fp::FP_SCENE] == 28000u);
		CHECK(fp::fpLastRecord()->slotUs[fp::FP_2D] == 4000u);
	}

	/* --- render-group overflow stays LIFO-balanced (dropped pushes are not
	 * popped against a parent entry) --- */
	{
		int k;
		fresh();
		cycle(0);	/* skipped */
		g_now += 100000;
		fp::fpBeginFrame();
		fp::fpPushGroup("RenderScene");
		for(k = 0; k < (int)fp::FP_GROUP_MAX + 5; k++)
			fp::fpPushGroup("UnnamedGroup");
		for(k = 0; k < (int)fp::FP_GROUP_MAX + 5; k++)
			fp::fpPopGroup();
		g_now += 7000;
		fp::fpPopGroup();	/* closes RenderScene */
		fp::fpEndFrame();
		CHECK(fp::fpLastRecord()->slotUs[fp::FP_SCENE] == 7000u);
	}

	/* --- line formatters produce parseable integer tenths --- */
	{
		char b[fp::FP_LINE_MAX];
		fresh();
		cycle(0);	/* skipped */
		g_now += 118400;
		fp::fpBeginFrame();
		fp::fpAdd(fp::FP_LIT, 55000);
		fp::fpEndFrame();
		fp::fpFmtFrameLine(b, vi(1), fp::fpLastRecord());
		CHECK(std::strstr(b, "prof: frame 1:") != 0);
		CHECK(std::strstr(b, "118.4 ms") != 0);
		CHECK(std::strstr(b, "lit 55.0") != 0);
		CHECK(std::strstr(b, "outside") != 0);

		fp::fpFmtAggregate(b, vi(30));
		CHECK(std::strstr(b, "prof: frames 30:") != 0);
		CHECK(std::strstr(b, "ms/frame min") != 0);
	}

	/* --- disabled is a no-op --- */
	{
		int ringBefore;
		unsigned int totalBefore;
		fresh();
		runFrames(vi(3), vi(10000));
		ringBefore = fp::fpRingCount();
		totalBefore = fp::fpFramesTotal();
		CHECK(ringBefore == 2);
		fp::fpEnable(0);
		g_now += 100000;
		fp::fpBeginFrame();
		fp::fpAdd(fp::FP_LIT, 999);
		fp::fpEndFrame();
		/* fpEnable(0) frees the ring; disabled calls add nothing. */
		CHECK(fp::fpRingCount() == 0);
		CHECK(fp::fpFramesTotal() == totalBefore);
	}

	/* --- sink routing: slow + aggregate lines reach the callbacks --- */
	{
		fresh();
		fp::fpSetSlowThresholdMs(vi(50));
		g_captureCount = 0;
		fp::fpSetLineSink(captureSink, captureSink);
		/* drive 30 frames so the aggregate is due, with one slow frame */
		cycle(0);	/* skipped */
		runFrames(vi(29), vi(10000));
		cycle(80000);
		CHECK(g_captureCount >= 2);
		CHECK(std::strstr(g_capture, "prof:") != 0);
	}

	if (failures) {
		std::printf("frameprofile_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("frameprofile_test: all ok\n");
	return 0;
}
