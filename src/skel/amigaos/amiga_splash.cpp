// AmigaOS boot splash renderer; see AmigaSplash.h.
//
// Split out of amigaos.cpp so the whole feature (embedded logo -> texture,
// frame drawing, fade loop) lives in one place, and so amigaos.cpp stays a
// platform file: it just calls AmigaSplashShow() once. The geometry/timing
// math is pure and unit-tested separately (src/core/SplashScreen.h).

#include "common.h"

#include "main.h"		// DoRWStuffStartOfFrame, Scene, SCREEN_WIDTH/HEIGHT
#include "Sprite2d.h"		// CSprite2d
#include "platform.h"		// psCameraShowRaster
#include "amiga_boot.h"		// amiga_boot_log/ms/poll_events/should_quit

#include "mini/rwminihost.h"	// rwminiGetFrameStats (device texture cap)

#include "SplashScreen.h"	// splash::FitCentred / OverlayAlpha / timing
#include "AmigaSplash.h"

#include "lodepng/lodepng.h"	// in-memory PNG decode

// Logo bytes, generated into gen/splash_logo.cpp by build/amigaos/gen_splash.py.
namespace splash {
extern const unsigned char logoPng[];
extern const unsigned int logoPngSize;
}

namespace {

// ---------------------------------------------------------------------------
// Embedded logo -> GPU texture
// ---------------------------------------------------------------------------

// Owns the splash logo texture for its lifetime (RAII: destroyed on scope exit,
// so it never outlives the splash and never leaks into the game).
class LogoTexture {
public:
	LogoTexture(void) : mTexture(nil), mWidth(0), mHeight(0) {}
	~LogoTexture(void) { if(mTexture) RwTextureDestroy(mTexture); }

	// Decode the embedded PNG and upload it. Returns false (and logs) if the
	// device cannot hold the image, in which case the splash is skipped.
	bool Load(void);

	RwTexture *Texture(void) const { return mTexture; }
	float Width(void) const { return (float)mWidth; }
	float Height(void) const { return (float)mHeight; }

private:
	LogoTexture(const LogoTexture &);
	LogoTexture &operator=(const LogoTexture &);

	RwTexture *mTexture;
	int32 mWidth, mHeight;
};

bool
LogoTexture::Load(void)
{
	unsigned char *rgba = nil;
	unsigned int w = 0, h = 0;
	RwMiniStats stats;
	RwImage *image;
	RwRaster *raster;
	int32 rw = 0, rh = 0, depth = 0, format = 0;

	if(lodepng_decode32(&rgba, &w, &h, splash::logoPng, splash::logoPngSize) != 0){
		amiga_boot_log("splash: embedded logo failed to decode");
		return false;
	}

	// The MINI texture path does not downscale images it creates via
	// RwRasterSetFromImage, so one larger than the device cap would fail to
	// upload. Fail soft instead.
	rwminiGetFrameStats(&stats);
	if(stats.maxTextureSize > 0 &&
	   (w > stats.maxTextureSize || h > stats.maxTextureSize)){
		amiga_boot_log("splash: logo too large for this device, skipping");
		free(rgba);
		return false;
	}

	image = RwImageCreate((int32)w, (int32)h, 32);
	if(image == nil){
		amiga_boot_log("splash: out of memory for logo image");
		free(rgba);
		return false;
	}
	RwImageAllocatePixels(image);
	if(RwImageGetPixels(image) == nil){
		RwImageDestroy(image);
		free(rgba);
		amiga_boot_log("splash: out of memory for logo pixels");
		return false;
	}
	memcpy(RwImageGetPixels(image), rgba, (size_t)w * h * 4);
	free(rgba);

	if(!RwImageFindRasterFormat(image, rwRASTERTYPETEXTURE, &rw, &rh, &depth, &format)){
		RwImageDestroy(image);
		amiga_boot_log("splash: no raster format for logo");
		return false;
	}

	raster = RwRasterCreate(rw, rh, depth, format);
	if(raster != nil){
		RwRasterSetFromImage(raster, image);
		mTexture = RwTextureCreate(raster);
		if(mTexture != nil){
			RwTextureSetFilterMode(mTexture, rwFILTERLINEAR);
			RwTextureSetAddressing(mTexture, rwTEXTUREADDRESSCLAMP);
		}
	}
	RwImageDestroy(image);

	if(mTexture == nil){
		// RwTextureCreate owns the raster on success; on failure it does not,
		// so release it here rather than leaking it.
		RwRasterDestroy(raster);
		amiga_boot_log("splash: could not create the logo texture");
		return false;
	}
	mWidth = rw;
	mHeight = rh;
	return true;
}

// ---------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------

// Snapshots every render state the splash changes and restores it on scope
// exit, so the game's first frame sees exactly the state it would have without
// the splash. The unsupported-but-set alpha test is handled explicitly (it is
// not a RwRenderState).
class RenderStateGuard {
public:
	RenderStateGuard(void) : mCount(0)
	{
		Save(rwRENDERSTATETEXTURERASTER);
		Save(rwRENDERSTATETEXTUREADDRESS);
		Save(rwRENDERSTATETEXTUREFILTER);
		Save(rwRENDERSTATEZTESTENABLE);
		Save(rwRENDERSTATEZWRITEENABLE);
		Save(rwRENDERSTATEVERTEXALPHAENABLE);
		Save(rwRENDERSTATESRCBLEND);
		Save(rwRENDERSTATEDESTBLEND);
		mAlphaTestFunc = rw::GetRenderState(rw::ALPHATESTFUNC);
	}
	~RenderStateGuard(void)
	{
		for(int i = 0; i < mCount; i++)
			RwRenderStateSet(mStates[i].state, mStates[i].value);
		rw::SetRenderState(rw::ALPHATESTFUNC, mAlphaTestFunc);
	}

private:
	struct SavedState { RwRenderState state; void *value; };

	void Save(RwRenderState state)
	{
		mStates[mCount].state = state;
		RwRenderStateGet(state, &mStates[mCount].value);
		mCount++;
	}

	SavedState mStates[8];
	int mCount;
	uint32 mAlphaTestFunc;
};

// Configure the pipeline for a straight alpha-blended 2D blit of the logo: no
// depth, no alpha test (so the anti-aliased edges blend against the black
// clear instead of being clipped).
void
PrepareLogoState(void)
{
	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
	RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
}

// Draw one splash frame: black background, the centred logo, then a full-screen
// black overlay of `overlayAlpha` (0 = logo fully visible, 255 = fully black).
// Returns false if the frame could not be presented (the camera is not ready to
// begin an update yet), so the caller can back off instead of spinning.
bool
DrawSplashFrame(const LogoTexture &logo, unsigned overlayAlpha)
{
	if(!DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255))
		return false;

	{
		RenderStateGuard guard;
		PrepareLogoState();

		splash::Rect r = splash::FitCentred(logo.Width(), logo.Height(),
		                                    SCREEN_WIDTH, SCREEN_HEIGHT, splash::kMarginPx);
		CRGBA white(255, 255, 255, 255);

		CSprite2d::SetVertices(CRect(r.x, r.y, r.x + r.w, r.y + r.h), white, white, white, white);
		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, RwTextureGetRaster(logo.Texture()));
		RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::GetVertices(), 4);

		if(overlayAlpha > 0)
			CSprite2d::DrawRect(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT),
			                    CRGBA(0, 0, 0, (uint8)overlayAlpha));
	}

	RwCameraEndUpdate(Scene.camera);
	psCameraShowRaster(Scene.camera);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bool
AmigaSplashShow(void)
{
	LogoTexture logo;
	unsigned start, elapsed;

	if(!logo.Load())
		return false;

	amiga_boot_log("splash: showing");
	start = amiga_boot_ms();

	// The MINI backend flips on the VBL (psCameraShowRaster -> mglSwitchDisplay),
	// so a presented frame paces the loop. If a frame cannot be presented yet
	// (the camera is not ready to begin an update), yield instead of spinning.
	for(;;){
		amiga_boot_poll_events();
		if(amiga_boot_should_quit())
			break;

		elapsed = amiga_boot_ms() - start;
		if(elapsed >= splash::kTotalMs){
			// One last fully-faded frame, then hand over to the game.
			DrawSplashFrame(logo, 255);
			break;
		}

		if(!DrawSplashFrame(logo,
		     splash::OverlayAlpha(elapsed, splash::kFadeInMs, splash::kHoldMs, splash::kFadeOutMs)))
			amiga_boot_yield();
	}

	amiga_boot_log("splash: done");
	return true;
}
