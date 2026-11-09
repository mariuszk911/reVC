/*
 * C64 crunch boot-intro model unit test (src/core/C64Intro.h).
 *
 * The renderer (src/skel/amigaos/amiga_c64_intro.cpp) only runs on the target,
 * so the timing curves, 4:3 geometry, screen composition and bounds clamping
 * are checked here on the host and cross-linked for 68k.
 *
 * Integer assertions only (no %f: this libnix's float printing is broken on the
 * target). On AmigaOS writes DH11:c64intro.log.
 */

#include <cstdio>
#include <cstring>
#include <cmath>

#include "C64Intro.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

static bool
near(float got, float want, float eps)
{
	return std::fabs(got - want) <= eps;
}

// Copy a composed row into `out`, then trim leading/trailing spaces.
static void
RowTrimmed(const c64intro::Screen &scr, int row, char *out)
{
	int i, n = c64intro::kCols;

	for(i = 0; i < n; i++)
		out[i] = (char)scr.ch[row][i];
	out[n] = 0;
	while(n > 0 && out[n - 1] == ' ')
		out[--n] = 0;
	i = 0;
	while(out[i] == ' ')
		i++;
	if(i > 0)
		std::memmove(out, out + i, std::strlen(out + i) + 1);
}

static bool
contains(const char *haystack, const char *needle)
{
	return std::strstr(haystack, needle) != 0;
}

// Tick an initialised model forward through every 12 fps frame up to and
// including `elapsedMs`, so a check sees the same history the renderer would.
static void
RunTo(c64intro::State &st, c64intro::Screen &scr, unsigned elapsedMs)
{
	for(unsigned t = 0; t <= elapsedMs; t += c64intro::kFrameMs)
		c64intro::Tick(st, scr, t);
	if(elapsedMs % c64intro::kFrameMs)
		c64intro::Tick(st, scr, elapsedMs);
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:c64intro.log", "w", stdout);
#endif

	// --- Progress curve ---------------------------------------------------
	{
		unsigned prev = 0;
		bool mono = true;

		CHECK(c64intro::ProgressAt(0) == 0);
		CHECK(c64intro::ProgressAt(c64intro::kProgressEndMs) == 1000);
		CHECK(c64intro::ProgressAt(c64intro::kProgressEndMs + 1) == 1000);
		CHECK(c64intro::ProgressAt(1000000) == 1000);

		for(unsigned t = 0; t <= c64intro::kTotalMs; t++){
			unsigned p = c64intro::ProgressAt(t);
			if(p < prev || p > 1000)
				mono = false;
			prev = p;
		}
		CHECK(mono);
		// Midway through the first segment: 60 * 90 / 180 = 30.
		CHECK(c64intro::ProgressAt(90) == 30);
	}

	// --- Frame index ------------------------------------------------------
	{
		CHECK(c64intro::FrameIndex(0) == 0);
		CHECK(c64intro::FrameIndex(c64intro::kFrameMs - 1) == 0);
		CHECK(c64intro::FrameIndex(c64intro::kFrameMs) == 1);
		CHECK(c64intro::FrameIndex(c64intro::kFrameMs * 2) == 2);
	}

	// --- Fade -------------------------------------------------------------
	{
		unsigned prev = 0;
		bool mono = true;

		CHECK(c64intro::FadeAlpha(0) == 0);
		CHECK(c64intro::FadeAlpha(c64intro::kFadeStartMs) == 0);
		CHECK(c64intro::FadeAlpha(c64intro::kFadeStartMs - 1) == 0);
		CHECK(c64intro::FadeAlpha(4375) == 127);      // halfway through the 250 ms fade
		CHECK(c64intro::FadeAlpha(c64intro::kTotalMs) == 255);
		CHECK(c64intro::FadeAlpha(c64intro::kTotalMs + 5000) == 255);

		for(unsigned t = 0; t <= c64intro::kTotalMs; t++){
			unsigned a = c64intro::FadeAlpha(t);
			if(a < prev)
				mono = false;
			prev = a;
		}
		CHECK(mono);
	}

	// --- 4:3 fit ----------------------------------------------------------
	{
		c64intro::Rect r;

		// 16:9 -> pillarboxed 4:3.
		r = c64intro::FitAspect(1280.0f, 720.0f, 4.0f / 3.0f, 0.0f);
		CHECK(near(r.w, 960.0f, 0.01f));
		CHECK(near(r.h, 720.0f, 0.01f));
		CHECK(near(r.x, 160.0f, 0.01f));
		CHECK(near(r.y, 0.0f, 0.01f));

		// 4:3 -> fills the screen.
		r = c64intro::FitAspect(640.0f, 480.0f, 4.0f / 3.0f, 0.0f);
		CHECK(near(r.x, 0.0f, 0.01f));
		CHECK(near(r.y, 0.0f, 0.01f));
		CHECK(near(r.w, 640.0f, 0.01f));
		CHECK(near(r.h, 480.0f, 0.01f));

		// Tall screen -> letterboxed.
		r = c64intro::FitAspect(480.0f, 640.0f, 4.0f / 3.0f, 0.0f);
		CHECK(near(r.w, 480.0f, 0.01f));
		CHECK(near(r.h, 360.0f, 0.01f));
		CHECK(near(r.x, 0.0f, 0.01f));
		CHECK(near(r.y, 140.0f, 0.01f));

		// Degenerate inputs -> zero rect.
		r = c64intro::FitAspect(0.0f, 480.0f, 4.0f / 3.0f, 0.0f);
		CHECK(r.w == 0.0f && r.h == 0.0f);
		r = c64intro::FitAspect(-640.0f, 480.0f, 4.0f / 3.0f, 0.0f);
		CHECK(r.w == 0.0f && r.h == 0.0f);
		r = c64intro::FitAspect(640.0f, 480.0f, 0.0f, 0.0f);
		CHECK(r.w == 0.0f && r.h == 0.0f);
		r = c64intro::FitAspect(640.0f, 480.0f, -1.0f, 0.0f);
		CHECK(r.w == 0.0f && r.h == 0.0f);
		r = c64intro::FitAspect(640.0f, 480.0f, 4.0f / 3.0f, 400.0f);
		CHECK(r.w == 0.0f && r.h == 0.0f);

		// Centred on both axes for an arbitrary screen.
		r = c64intro::FitAspect(1024.0f, 768.0f, 4.0f / 3.0f, 10.0f);
		CHECK(near(r.x, (1024.0f - r.w) * 0.5f, 0.01f));
		CHECK(near(r.y, (768.0f - r.h) * 0.5f, 0.01f));
	}

	// --- 320x200 inset ----------------------------------------------------
	{
		c64intro::Rect outer = { 0.0f, 0.0f, 384.0f, 272.0f };
		c64intro::Rect r = c64intro::InnerScreen(outer);

		CHECK(near(r.x, 32.0f, 0.01f));
		CHECK(near(r.y, 36.0f, 0.01f));
		CHECK(near(r.w, 320.0f, 0.01f));
		CHECK(near(r.h, 200.0f, 0.01f));

		// Same ratios at another scale/offset.
		outer.x = 10.0f;
		outer.y = 20.0f;
		outer.w = 768.0f;
		outer.h = 544.0f;
		r = c64intro::InnerScreen(outer);
		CHECK(near(r.x, 10.0f + 768.0f * 32.0f / 384.0f, 0.01f));
		CHECK(near(r.y, 20.0f + 544.0f * 36.0f / 272.0f, 0.01f));
		CHECK(near(r.w, 768.0f * 320.0f / 384.0f, 0.01f));
		CHECK(near(r.h, 544.0f * 200.0f / 272.0f, 0.01f));
	}

	// --- Seed hash --------------------------------------------------------
	{
		CHECK(c64intro::HashSeed("") != 0);
		CHECK(c64intro::HashSeed(0) != 0);
		CHECK(c64intro::HashSeed("abc") == c64intro::HashSeed("abc"));
		CHECK(c64intro::HashSeed("abc") != c64intro::HashSeed("abd"));
	}

	// --- Build ID and initial field --------------------------------------
	{
		c64intro::State st;
		c64intro::Screen scr;

		c64intro::Init(st, scr, 1234u, "abc123");
		CHECK(std::strcmp(st.buildId, "ABC123") == 0);
		CHECK(st.frame == 0);

		c64intro::Init(st, scr, 1234u, 0);
		CHECK(std::strcmp(st.buildId, "DEV") == 0);

		c64intro::Init(st, scr, 1234u, "");
		CHECK(std::strcmp(st.buildId, "DEV") == 0);

		c64intro::Init(st, scr, 1234u, "0123456789");
		CHECK(std::strcmp(st.buildId, "01234567") == 0);

		// Every initial glyph is inside the font table.
		{
			bool inRange = true;
			for(int row = 0; row < c64intro::kRows; row++)
				for(int col = 0; col < c64intro::kCols; col++)
					if(scr.ch[row][col] > c64intro::kLastGlyph)
						inRange = false;
			CHECK(inRange);
		}
	}

	// --- Font/garbage bounds ---------------------------------------------
	{
		{
			bool inRange = true;
			for(unsigned r = 0; r < 10000u; r++)
				if(c64intro::GarbageGlyph(r) > c64intro::kLastGlyph)
					inRange = false;
			CHECK(inRange);
		}

		// Every fixed string fits one 40-column row.
		CHECK(c64intro::TextLength(c64intro::kTextNowLoading) <= c64intro::kCols);
		CHECK(c64intro::TextLength(c64intro::kTextCredit) <= c64intro::kCols);
		CHECK(c64intro::TextLength(c64intro::kTextCopyright) <= c64intro::kCols);
		CHECK(c64intro::TextLength(c64intro::kTextUnpacking) <= c64intro::kCols);
		CHECK(c64intro::TextLength(c64intro::kTextPleaseWait) <= c64intro::kCols);
		CHECK(c64intro::TextLength(c64intro::kTextComplete) <= c64intro::kCols);
		CHECK(c64intro::TextLength(c64intro::kTextReady) <= c64intro::kCols);
	}

	// --- Bar --------------------------------------------------------------
	{
		CHECK(c64intro::BarFilled(0) == 0);
		CHECK(c64intro::BarFilled(1000) == 24);
		CHECK(c64intro::BarFilled(1001) == 24);
		CHECK(c64intro::PercentAt(0) == 0);
		CHECK(c64intro::PercentAt(1000) == 100);

		unsigned prev = 0;
		bool mono = true;
		for(unsigned p = 0; p <= 1000; p++){
			unsigned f = c64intro::BarFilled(p);
			if(f < prev || f > 24)
				mono = false;
			prev = f;
		}
		CHECK(mono);
	}

	// --- Composition timeline --------------------------------------------
	{
		c64intro::State st;
		c64intro::Screen scr;
		char row[64];

		c64intro::Init(st, scr, c64intro::HashSeed("test-build"), "test-build");

		// Typewriter has started at 300 ms but not finished, and the bar is
		// not composed before its start time.
		c64intro::Tick(st, scr, 300);
		RowTrimmed(scr, c64intro::kRowNowLoading, row);
		CHECK(std::strcmp(row, "NO") == 0);
		RowTrimmed(scr, c64intro::kRowBar, row);
		CHECK(row[0] == 0);

		// Full line once enough characters have been typed; bar visible by 900.
		c64intro::Tick(st, scr, 900);
		RowTrimmed(scr, c64intro::kRowNowLoading, row);
		CHECK(std::strcmp(row, c64intro::kTextNowLoading) == 0);
		RowTrimmed(scr, c64intro::kRowBar, row);
		CHECK(row[0] == '[');
		CHECK(contains(row, "%"));

		// The unpacking line is complete by 1500 ms.
		c64intro::Tick(st, scr, 1500);
		RowTrimmed(scr, c64intro::kRowUnpacking, row);
		CHECK(std::strcmp(row, c64intro::kTextUnpacking) == 0);

		// Completion: bar full, READY., build line present.
		c64intro::Tick(st, scr, c64intro::kProgressEndMs);
		RowTrimmed(scr, c64intro::kRowUnpacking, row);
		CHECK(std::strcmp(row, c64intro::kTextComplete) == 0);
		RowTrimmed(scr, c64intro::kRowPleaseWait, row);
		CHECK(std::strcmp(row, c64intro::kTextReady) == 0);
		RowTrimmed(scr, c64intro::kRowBar, row);
		CHECK(contains(row, "100%"));
		RowTrimmed(scr, c64intro::kRowTrack, row);
		CHECK(std::strcmp(row, "TRACK 09   SECTOR 05") == 0);
		RowTrimmed(scr, c64intro::kRowBuild, row);
		CHECK(contains(row, "PISTORM3D  BUILD TEST-BUI"));
		CHECK(!c64intro::LedOn(c64intro::kProgressEndMs));

		// Every glyph across the whole run stays inside the table.
		{
			bool inRange = true;
			for(unsigned t = 0; t <= c64intro::kTotalMs; t += c64intro::kFrameMs){
				c64intro::Tick(st, scr, t);
				for(int r2 = 0; r2 < c64intro::kRows; r2++)
					for(int c = 0; c < c64intro::kCols; c++)
						if(scr.ch[r2][c] > c64intro::kLastGlyph)
							inRange = false;
			}
			CHECK(inRange);
		}
	}

	// --- Determinism ------------------------------------------------------
	{
		c64intro::State a, b;
		c64intro::Screen sa, sb;

		c64intro::Init(a, sa, 0x1234567u, "build");
		c64intro::Init(b, sb, 0x1234567u, "build");
		RunTo(a, sa, 2500);
		RunTo(b, sb, 2500);
		CHECK(std::memcmp(&sa, &sb, sizeof(sa)) == 0);

		c64intro::Init(b, sb, 0x7654321u, "build");
		c64intro::Tick(b, sb, 2500);
		CHECK(std::memcmp(&sa, &sb, sizeof(sa)) != 0);
	}

	// --- Boundary writes never leave the grid ----------------------------
	{
		c64intro::Screen scr;

		std::memset(&scr, 0, sizeof(scr));
		c64intro::PutText(scr, 0, 0, "ABC", c64intro::kColWhite);
		CHECK(scr.ch[0][0] == 'A');
		CHECK(scr.ch[0][1] == 'B');
		CHECK(scr.ch[0][2] == 'C');
		CHECK(scr.ch[0][3] == 0);

		c64intro::PutText(scr, c64intro::kRows - 1, c64intro::kCols - 2, "XYZ",
		                  c64intro::kColWhite);
		CHECK(scr.ch[c64intro::kRows - 1][c64intro::kCols - 2] == 'X');
		CHECK(scr.ch[c64intro::kRows - 1][c64intro::kCols - 1] == 'Y');

		// Negative coordinates and rows past the grid are ignored.
		c64intro::PutGlyph(scr, -1, 0, 'A', c64intro::kColWhite);
		c64intro::PutGlyph(scr, c64intro::kRows, 0, 'A', c64intro::kColWhite);
		c64intro::PutGlyph(scr, 0, -1, 'A', c64intro::kColWhite);
		c64intro::PutGlyph(scr, 0, c64intro::kCols, 'A', c64intro::kColWhite);
		c64intro::PutText(scr, 1, -2, "ABC", c64intro::kColWhite);
		CHECK(scr.ch[1][0] == 'C');
	}

	if (failures) {
		std::printf("c64_intro_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("c64_intro_test: all ok\n");
	return 0;
}
