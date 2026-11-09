/*
 * Boot-splash geometry + fade unit test (src/core/SplashScreen.h).
 *
 * The AmigaOS renderer (src/skel/amigaos/amiga_splash.cpp) can only run on the
 * target, so the two error-prone pieces are checked here instead, on the host
 * and cross-linked for 68k:
 *   - splash::FitCentred: aspect-preserving fit inside (screen - 2*margin) on
 *     both axes, centred on both axes, degenerate inputs -> zero rect.
 *   - splash::OverlayAlpha: black-overlay alpha 255->0 (fade in), hold 0, then
 *     0->255 (fade out) over exactly splash::kTotalMs.
 *
 * Integer assertions only (no %f: this libnix's float printing is broken on the
 * target). On AmigaOS writes DH11:splash.log.
 */

#include <cstdio>
#include <cmath>

#include "SplashScreen.h"

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

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:splash.log", "w", stdout);
#endif

	// --- Wide logo (the shipped asset: 1280x178) --------------------------
	{
		splash::Rect r = splash::FitCentred(1280.0f, 178.0f, 640.0f, 480.0f, splash::kMarginPx);
		// availW = 560 -> scale 0.4375; 178*0.4375 = 77.9 <= availH(400)
		CHECK(near(r.w, 560.0f, 0.01f));
		CHECK(near(r.h, 178.0f * 560.0f / 1280.0f, 0.01f));
		CHECK(near(r.x, splash::kMarginPx, 0.01f));                        // left margin
		CHECK(near(r.x + r.w, 640.0f - splash::kMarginPx, 0.01f));         // right margin
		CHECK(near(r.y, (480.0f - r.h) * 0.5f, 0.01f));                    // centred vertically
	}

	// --- Tall logo: height-limited, still centred -------------------------
	{
		splash::Rect r = splash::FitCentred(100.0f, 1000.0f, 640.0f, 480.0f, splash::kMarginPx);
		// availW=560 -> scale 5.6 -> 5600 > availH(400) -> height-fit scale 0.4
		CHECK(near(r.w, 40.0f, 0.01f));
		CHECK(near(r.h, 400.0f, 0.01f));
		CHECK(near(r.x, (640.0f - 40.0f) * 0.5f, 0.01f));
		CHECK(near(r.y, splash::kMarginPx, 0.01f));
	}

	// --- Centred on both axes, inside the margins, for several screens ----
	{
		const float screens[][2] = { {320.0f, 240.0f}, {640.0f, 480.0f},
		                             {1024.0f, 768.0f}, {1280.0f, 720.0f} };
		for (int i = 0; i < 4; i++) {
			float sw = screens[i][0], sh = screens[i][1];
			splash::Rect r = splash::FitCentred(1280.0f, 178.0f, sw, sh, splash::kMarginPx);
			CHECK(near(r.x, (sw - r.w) * 0.5f, 0.01f));
			CHECK(near(r.y, (sh - r.h) * 0.5f, 0.01f));
			CHECK(r.w <= sw - 2.0f * splash::kMarginPx + 0.01f);
			CHECK(r.h <= sh - 2.0f * splash::kMarginPx + 0.01f);
			CHECK(r.w > 0.0f && r.h > 0.0f);
		}
	}

	// --- Degenerate inputs -> zero rect ----------------------------------
	{
		splash::Rect a = splash::FitCentred(0.0f, 100.0f, 640.0f, 480.0f, splash::kMarginPx);
		splash::Rect b = splash::FitCentred(100.0f, 100.0f, 60.0f, 480.0f, splash::kMarginPx);
		splash::Rect c = splash::FitCentred(100.0f, 100.0f, 640.0f, 480.0f, 400.0f);
		CHECK(a.w == 0.0f && a.h == 0.0f);
		CHECK(b.w == 0.0f && b.h == 0.0f);
		CHECK(c.w == 0.0f && c.h == 0.0f);
	}

	// --- Fade curve endpoints and ramps ----------------------------------
	{
		const unsigned fi = splash::kFadeInMs, ho = splash::kHoldMs, fo = splash::kFadeOutMs;
		CHECK(splash::kTotalMs >= 3000u);                          // "at least 3 seconds"

		CHECK(splash::OverlayAlpha(0, fi, ho, fo) == 255);         // starts black
		CHECK(splash::OverlayAlpha(250, fi, ho, fo) == 127);       // mid fade-in
		CHECK(splash::OverlayAlpha(fi, fi, ho, fo) == 0);          // fully revealed
		CHECK(splash::OverlayAlpha(fi + ho / 2, fi, ho, fo) == 0); // hold
		CHECK(splash::OverlayAlpha(fi + ho - 1, fi, ho, fo) == 0); // last hold frame
		CHECK(splash::OverlayAlpha(fi + ho, fi, ho, fo) == 0);     // fade-out start
		CHECK(splash::OverlayAlpha(fi + ho + 250, fi, ho, fo) == 127); // mid fade-out
		CHECK(splash::OverlayAlpha(splash::kTotalMs, fi, ho, fo) == 255); // ends black
		CHECK(splash::OverlayAlpha(splash::kTotalMs + 5000, fi, ho, fo) == 255);

		// Monotonic: fade-in overlay never increases, fade-out never decreases.
		int prev = splash::OverlayAlpha(0, fi, ho, fo);
		for (unsigned t = 1; t <= fi; t++) {
			int a = splash::OverlayAlpha(t, fi, ho, fo);
			CHECK(a <= prev);
			prev = a;
		}
		prev = splash::OverlayAlpha(fi + ho, fi, ho, fo);
		for (unsigned t = fi + ho + 1; t <= splash::kTotalMs; t++) {
			int a = splash::OverlayAlpha(t, fi, ho, fo);
			CHECK(a >= prev);
			prev = a;
		}
	}

	if (failures) {
		std::printf("splash_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("splash_test: all ok\n");
	return 0;
}
