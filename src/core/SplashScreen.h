#ifndef __GTA_SPLASHSCREEN_H__
#define __GTA_SPLASHSCREEN_H__

/*
 * Layout and fade timing for the boot splash.
 *
 * Deliberately free of any game/platform dependency (no librw, no AmigaOS) so
 * the geometry and fade can be exercised in the host unit test
 * (tests/amigaos/splash_test.cpp). The AmigaOS renderer that consumes this
 * lives in src/skel/amigaos/amiga_splash.cpp.
 */

namespace splash {

// On-screen time budget, milliseconds: the logo fades in from black, holds,
// then fades back out. The sum is the minimum the splash stays visible (3 s).
const unsigned kFadeInMs  = 500;
const unsigned kHoldMs    = 2000;
const unsigned kFadeOutMs = 500;
const unsigned kTotalMs   = kFadeInMs + kHoldMs + kFadeOutMs;

// Inset from the screen edges the logo is fitted within, in pixels.
const float kMarginPx = 40.0f;

struct Rect { float x, y, w, h; };

// Aspect-preserving, screen-centred rect for a texW x texH image, fitted inside
// (screenW - 2*margin) by (screenH - 2*margin) and centred on both axes.
// Degenerate inputs (zero/negative size, or a margin that leaves no room)
// yield an all-zero rect.
inline Rect
FitCentred(float texW, float texH, float screenW, float screenH, float margin)
{
	Rect r = { 0.0f, 0.0f, 0.0f, 0.0f };
	float availW, availH, scale;

	if(texW <= 0.0f || texH <= 0.0f || screenW <= 0.0f || screenH <= 0.0f)
		return r;
	availW = screenW - 2.0f * margin;
	availH = screenH - 2.0f * margin;
	if(availW <= 0.0f || availH <= 0.0f)
		return r;

	scale = availW / texW;
	if(texH * scale > availH)
		scale = availH / texH;

	r.w = texW * scale;
	r.h = texH * scale;
	r.x = (screenW - r.w) * 0.5f;
	r.y = (screenH - r.h) * 0.5f;
	return r;
}

// Alpha (0..255) of the black overlay drawn over the logo after `elapsedMs`:
//   fade-in  [0, fadeIn):              255 -> 0  (logo revealed from black)
//   hold     [fadeIn, fadeIn + hold):  0
//   fade-out [fadeIn + hold, total):   0 -> 255 (logo hidden into black)
// At or past the end the overlay stays fully opaque. A zero-length phase is
// treated as instantaneous.
inline unsigned
OverlayAlpha(unsigned elapsedMs, unsigned fadeInMs, unsigned holdMs, unsigned fadeOutMs)
{
	unsigned total = fadeInMs + holdMs + fadeOutMs;
	unsigned holdEnd;

	if(elapsedMs >= total)
		return 255;
	if(elapsedMs < fadeInMs)
		return fadeInMs ? (255u * (fadeInMs - elapsedMs)) / fadeInMs : 0u;

	holdEnd = fadeInMs + holdMs;
	if(elapsedMs < holdEnd)
		return 0u;
	if(fadeOutMs == 0)
		return 255u;
	return (255u * (elapsedMs - holdEnd)) / fadeOutMs;
}

} // namespace splash

#endif // __GTA_SPLASHSCREEN_H__
