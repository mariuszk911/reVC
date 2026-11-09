/*
 * AmigaOS implementation of LoadingScreen(); see src/core/main.cpp.
 *
 * The shared implementation keeps the upstream DISABLE_LOADING_SCREEN fast
 * path and the compile-time RANDOMSPLASH choice. On AmigaOS both are runtime:
 *   - the progress bar is always drawn;
 *   - REVC_RANDOMSPLASH selects a random LOADSCx splash and the PS2 bar
 *     geometry; otherwise LOADSC0 with the lower/wider PC geometry (equal
 *     left/right margins) is used.
 *   - when the splash image actually changes (random mode), fade the old one
 *     to black, load the new one, then fade it in.
 *
 * The dev-only CHATTYSPLASH step text is deliberately not compiled here.
 */

#include "common.h"
#include "main.h"
#include "Sprite2d.h"
#include "Font.h"
#include "RwHelper.h"
#include "amiga_boot.h"

#include <string.h>

// Owned by src/core/main.cpp (ResetLoadingScreenBar()).
extern float NumberOfChunksLoaded;

// Original loading-bar calibration (src/core/main.cpp). Kept local so the
// shared file stays untouched.
#define TOTALNUMCHUNKS 95.0f

// Total fade time (old -> black -> new), split evenly across the two halves.
// Paced from the clock so slow frames cannot shorten it below ~0.3 s.
#define LOAD_FADE_MS 300

static char loadSplashName[32];   // currently loaded splash ("" = none yet)

// Progress bar at the current fill level. Drawn after any fade overlay, so it
// stays fully visible while the background image fades.
static void
LoadDrawBar(int ps2Bar)
{
	// Random splash (PS2 data) keeps the PS2 bar; LOADSC0 uses the PC bar:
	// lower (below the splash text) and symmetric margins.
	float hpos = ps2Bar ? SCREEN_STRETCH_X(40) : SCREEN_SCALE_X(40);
	float length = ps2Bar ? SCREEN_STRETCH_X(440)
	                      : SCREEN_WIDTH - SCREEN_SCALE_X(80);
	float top = ps2Bar ? SCREEN_STRETCH_Y(407.4f - 7.0f/3.0f)
	                   : SCREEN_HEIGHT - SCREEN_SCALE_Y(14);
	float bottom = ps2Bar ? SCREEN_STRETCH_Y(407.4f + 7.0f/3.0f)
	                      : top + SCREEN_SCALE_Y(5);

	CSprite2d::DrawRect(CRect(hpos-1.0f, top-1.0f, hpos+length+1.0f, bottom+1.0f), CRGBA(40, 53, 68, 255));

	CSprite2d::DrawRect(CRect(hpos, top, hpos+length, bottom), CRGBA(155, 50, 125, 255));

	length *= NumberOfChunksLoaded/TOTALNUMCHUNKS;
	CSprite2d::DrawRect(CRect(hpos, top, hpos+length, bottom), CRGBA(255, 150, 225, 255));
}

// One splash-only frame with an optional black overlay (0 = none, 255 = black).
static void
LoadFadeFrame(CSprite2d *splash, int blackAlpha, bool drawBar, int ps2Bar)
{
	if(!DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255))
		return;
	CSprite2d::SetRecipNearClip();
	CSprite2d::InitPerFrame();
	DefinedState();
	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
	splash->Draw(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT), CRGBA(255, 255, 255, 255));
	if(blackAlpha > 0)
		CSprite2d::DrawRect(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT),
		                    CRGBA(0, 0, 0, blackAlpha));
	if(drawBar)
		LoadDrawBar(ps2Bar);
	DoRWStuffEndOfFrame();
}

// Fade the overlay alpha from `fromAlpha` to `toAlpha` over `ms`, drawing one
// frame per ~20 ms. Alpha comes from elapsed time, so a slow frame cannot
// shorten the transition. Returns FALSE if the game is quitting.
static bool
LoadFadeRun(CSprite2d *splash, int fromAlpha, int toAlpha, uint32 ms,
            bool drawBar, int ps2Bar)
{
	uint32 start = amiga_boot_ms();

	for(;;){
		uint32 elapsed = amiga_boot_ms() - start;
		int alpha;

		if(elapsed >= ms)
			alpha = toAlpha;
		else
			alpha = fromAlpha +
			        (int)(((int32)(toAlpha - fromAlpha) * (int32)elapsed) / (int32)ms);

		amiga_boot_poll_events();
		if(RsGlobal.quit || amiga_boot_should_quit())
			return false;
		LoadFadeFrame(splash, alpha, drawBar, ps2Bar);
		if(elapsed >= ms)
			return true;
		amiga_boot_yield();
	}
}

// Fade `oldSplash` out, load `want`, fade the new image in. Returns FALSE if
// the game is quitting, so the caller can stop without drawing the bar.
static bool
LoadSplashTransition(CSprite2d *oldSplash, const char *want,
                     bool drawBar, int ps2Bar)
{
	CSprite2d *splash;

	if(!LoadFadeRun(oldSplash, 0, 255, LOAD_FADE_MS / 2, drawBar, ps2Bar))
		return false;

	splash = LoadSplash(want);

	return LoadFadeRun(splash, 255, 0, LOAD_FADE_MS / 2, drawBar, ps2Bar);
}

void
LoadingScreen(const char *str1, const char *str2, const char *splashscreen)
{
	CSprite2d *splash;
	int ps2Bar = amiga_boot_randomsplash();
	const char *want = ps2Bar ? splashscreen : "LOADSC0";
	bool drawBar = (str1 != nil);

	// Advance the bar before any fade, so the static bar during the transition
	// already shows this step.
	if(drawBar)
		NumberOfChunksLoaded += 1;

	splash = LoadSplash(nil);   // current image (untouched)
	if(want != nil && loadSplashName[0] != '\0' &&
	   strcmp(want, loadSplashName) != 0 && splash->m_pTexture != nil){
		if(!LoadSplashTransition(splash, want, drawBar, ps2Bar))
			return;
		splash = LoadSplash(nil);
	}else{
		splash = LoadSplash(want);
	}
	if(want != nil){
		strncpy(loadSplashName, want, sizeof(loadSplashName) - 1);
		loadSplashName[sizeof(loadSplashName) - 1] = '\0';
	}

	if(RsGlobal.quit)
		return;

	if(DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255)){
		CSprite2d::SetRecipNearClip();
		CSprite2d::InitPerFrame();
		CFont::InitPerFrame();
		DefinedState();
		RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
		splash->Draw(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT), CRGBA(255, 255, 255, 255));

		if(drawBar)
			LoadDrawBar(ps2Bar);

		CFont::DrawFonts();
		DoRWStuffEndOfFrame();
	}
}
