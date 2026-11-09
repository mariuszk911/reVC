// AmigaOS C64 crunch boot-intro renderer; see AmigaC64Intro.h.
//
// The intro model (timing, geometry, 40x25 grid composition) is pure and lives
// in src/core/C64Intro.h, so it is unit-tested on the host. This file owns the
// 320x200 RGBA texture upload, the C64 border/raster bars, the drive LED, the
// fade overlay and the boot-time loop.

#include "common.h"

#include "main.h"		// DoRWStuffStartOfFrame, Scene, SCREEN_WIDTH/HEIGHT
#include "Sprite2d.h"		// CSprite2d
#include "platform.h"		// psCameraShowRaster
#include "amiga_boot.h"		// amiga_boot_log/ms/us/poll_events/should_quit/yield

#include "mini/rwminihost.h"	// rwminiGetFrameStats (device texture cap)

#include "GitSHA1.h"		// g_GIT_SHA1 (build ID line and seed)
#include "C64Intro.h"		// pure model: timing, geometry, Screen/State
#include "c64_font.h"		// kC64Font
#include "AmigaC64Intro.h"

namespace {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

// Classic saturated C64 palette, RGB. Index 6 (blue) is the screen background.
const unsigned char kPalette[16][3] = {
	{0,0,0},       {255,255,255}, {136,0,0},     {170,255,238},
	{204,68,204},  {0,204,85},    {0,0,170},     {238,238,119},
	{221,136,85},  {102,68,0},    {255,119,119}, {51,51,51},
	{119,119,119}, {170,255,102}, {0,136,255},   {187,187,187}
};

CRGBA
C64Colour(unsigned index)
{
	const unsigned char *c = kPalette[index & 0x0F];
	return CRGBA(c[0], c[1], c[2], 255);
}

// ---------------------------------------------------------------------------
// 320x200 character grid -> GPU texture
// ---------------------------------------------------------------------------

class C64ScreenTexture {
public:
	C64ScreenTexture(void) : mRaster(nil), mTexture(nil) {}
	~C64ScreenTexture(void)
	{
		if(mTexture)
			RwTextureDestroy(mTexture);	// owns the raster
		else if(mRaster)
			RwRasterDestroy(mRaster);
	}

	bool Create(void);
	bool Upload(const c64intro::Screen &scr);

	RwTexture *Texture(void) const { return mTexture; }

private:
	C64ScreenTexture(const C64ScreenTexture &);
	C64ScreenTexture &operator=(const C64ScreenTexture &);

	RwRaster *mRaster;
	RwTexture *mTexture;
};

bool
C64ScreenTexture::Create(void)
{
	RwMiniStats stats;

	rwminiGetFrameStats(&stats);
	if(stats.maxTextureSize > 0 &&
	   ((unsigned)c64intro::kScreenW > stats.maxTextureSize ||
	    (unsigned)c64intro::kScreenH > stats.maxTextureSize)){
		amiga_boot_log("c64intro: screen texture exceeds device cap, skipping");
		return false;
	}

	mRaster = RwRasterCreate((RwInt32)c64intro::kScreenW,
	                         (RwInt32)c64intro::kScreenH, 32,
	                         rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
	if(mRaster == nil){
		amiga_boot_log("c64intro: texture create failed, skipping");
		return false;
	}

	mTexture = RwTextureCreate(mRaster);
	if(mTexture == nil){
		// RwTextureCreate owns the raster only on success.
		RwRasterDestroy(mRaster);
		mRaster = nil;
		amiga_boot_log("c64intro: texture create failed, skipping");
		return false;
	}
	mRaster = nil;

	RwTextureSetFilterMode(mTexture, rwFILTERNEAREST);
	RwTextureSetAddressing(mTexture, rwTEXTUREADDRESSCLAMP);
	return true;
}

// Compose the 40x25 grid into RGBA8888 and upload it. Byte writes (R,G,B,255)
// are endian-independent, unlike packed RwUInt32 pixels whose channel order
// differs between the GL3 and MINI backends.
bool
C64ScreenTexture::Upload(const c64intro::Screen &scr)
{
	RwRaster *raster = RwTextureGetRaster(mTexture);
	uint8 *pixels;
	int32 stride;

	if(raster == nil)
		return false;
	pixels = (uint8*)RwRasterLock(raster, 0, rwRASTERLOCKWRITE);
	if(pixels == nil)
		return false;
	stride = raster->stride;

	for(int row = 0; row < c64intro::kRows; row++){
		for(int col = 0; col < c64intro::kCols; col++){
			const unsigned char *bg = kPalette[c64intro::kColBlue];
			const unsigned char *fg = kPalette[scr.col[row][col] & 0x0F];
			unsigned glyph = scr.ch[row][col];

			if(glyph > c64intro::kLastGlyph)
				glyph = (unsigned char)'?';

			for(int gy = 0; gy < 8; gy++){
				unsigned bits = kC64Font[glyph][gy];
				uint8 *dst = pixels +
				             (int32)(row * 8 + gy) * stride +
				             (int32)(col * 8) * 4;

				for(int gx = 0; gx < 8; gx++){
					const unsigned char *c =
						(bits & (0x80u >> gx)) ? fg : bg;
					dst[0] = c[0];
					dst[1] = c[1];
					dst[2] = c[2];
					dst[3] = 255;
					dst += 4;
				}
			}
		}
	}

	RwRasterUnlock(raster);

	// rasterUnlock() re-uploads storage and resets the per-raster filter cache;
	// re-assert nearest so the next bind does not fall back to LINEAR.
	RwTextureSetFilterMode(mTexture, rwFILTERNEAREST);
	return true;
}

// ---------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------

// Same contract as the logo splash: snapshot everything this feature changes
// and restore it on scope exit so the game's first frame is unaffected. The
// alpha-test function is not a RwRenderState and is handled explicitly.
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

// Straight alpha-blended 2D quads: no depth (the intro paints back-to-front),
// vertex alpha on, nearest filtering for the pixel-art screen.
void
PrepareState(void)
{
	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
	RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERNEAREST);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
}

// CSprite2d::DrawRect() leaves z-test/z-write enabled, so restore the 2D state
// before the textured screen quad.
void
PrepareQuadState(void)
{
	RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)rwTEXTUREADDRESSCLAMP);
	RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERNEAREST);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
}

// ---------------------------------------------------------------------------
// Border, raster bars, LED, fade
// ---------------------------------------------------------------------------

void
DrawBorder(const c64intro::Rect &outer, const c64intro::Rect &inner,
           unsigned elapsedMs)
{
	static const unsigned char topCycle[4] = {
		c64intro::kColRed, c64intro::kColYellow,
		c64intro::kColGreen, c64intro::kColLightGreen
	};
	static const unsigned char bottomCycle[4] = {
		c64intro::kColCyan, c64intro::kColLightBlue,
		c64intro::kColPurple, c64intro::kColBlue
	};
	const CRect outside(outer.x, outer.y, outer.x + outer.w, outer.y + outer.h);
	float topHeight = inner.y - outer.y;
	float bottomHeight = (outer.y + outer.h) - (inner.y + inner.h);
	unsigned cycle = elapsedMs / 120;

	// The screen border background (also fills the corners).
	CSprite2d::DrawRect(outside, C64Colour(c64intro::kColBlue));

	// Four equal-height bands above and below the screen, cycling colours.
	if(topHeight > 0.0f){
		for(int band = 0; band < 4; band++){
			float y0 = outer.y + topHeight * (float)band / 4.0f;
			float y1 = outer.y + topHeight * (float)(band + 1) / 4.0f;
			unsigned idx = (cycle + (unsigned)band * 2u) % 4u;
			CSprite2d::DrawRect(CRect(outer.x, y0, outer.x + outer.w, y1),
			                    C64Colour(topCycle[idx]));
		}
	}
	if(bottomHeight > 0.0f){
		for(int band = 0; band < 4; band++){
			float y0 = inner.y + inner.h + bottomHeight * (float)band / 4.0f;
			float y1 = inner.y + inner.h + bottomHeight * (float)(band + 1) / 4.0f;
			unsigned idx = (cycle + (unsigned)band * 2u) % 4u;
			CSprite2d::DrawRect(CRect(outer.x, y0, outer.x + outer.w, y1),
			                    C64Colour(bottomCycle[idx]));
		}
	}
}

void
DrawLed(const c64intro::Rect &outer, unsigned elapsedMs)
{
	const float size = 8.0f;
	const float pad = 12.0f;
	float x = outer.x + outer.w - pad;
	float y = outer.y + outer.h - pad;

	CSprite2d::DrawRect(CRect(x - 1.0f, y - 1.0f, x + size + 1.0f, y + size + 1.0f),
	                    C64Colour(c64intro::kColDarkGrey));
	CSprite2d::DrawRect(CRect(x, y, x + size, y + size),
	                    C64Colour(c64intro::LedOn(elapsedMs)
	                              ? c64intro::kColRed
	                              : c64intro::kColDarkGrey));
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

// Draw one intro frame. Returns false if the camera is not ready to begin an
// update, so the caller can yield instead of spinning.
bool
DrawFrame(const C64ScreenTexture &tex, unsigned elapsedMs)
{
	c64intro::Rect outer, inner;
	CRect screen;
	unsigned fade;
	CRGBA white(255, 255, 255, 255);

	if(!DoRWStuffStartOfFrame(0, 0, 0, 0, 0, 0, 255))
		return false;

	{
		RenderStateGuard guard;
		PrepareState();

		outer = c64intro::FitAspect(SCREEN_WIDTH, SCREEN_HEIGHT, 4.0f / 3.0f, 0.0f);
		inner = c64intro::InnerScreen(outer);
		screen = CRect(inner.x, inner.y, inner.x + inner.w, inner.y + inner.h);

		DrawBorder(outer, inner, elapsedMs);
		DrawLed(outer, elapsedMs);

		// Textured 320x200 screen. CSprite2d::DrawRect() above re-enabled the
		// depth test, so put the 2D state back first.
		PrepareQuadState();
		CSprite2d::SetVertices(screen, white, white, white, white);
		RwRenderStateSet(rwRENDERSTATETEXTURERASTER,
		                 RwTextureGetRaster(tex.Texture()));
		RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::GetVertices(), 4);

		fade = c64intro::FadeAlpha(elapsedMs);
		if(fade > 0)
			CSprite2d::DrawRect(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT),
			                    CRGBA(0, 0, 0, (uint8)fade));
	}

	RwCameraEndUpdate(Scene.camera);
	psCameraShowRaster(Scene.camera);
	return true;
}

// amiga_boot_log() takes a plain string; format the one numeric summary by
// hand (no snprintf on the target).
void
LogUploadSummary(unsigned count, unsigned averageUs)
{
	char line[64];
	char digits[11];
	const char *prefix = "c64intro: done, upload avg ";
	int n = 0, d = 0;

	if(count == 0)
		averageUs = 0;
	while(prefix[n] != 0){
		line[n] = prefix[n];
		n++;
	}
	for(;;){
		digits[d++] = (char)('0' + averageUs % 10);
		averageUs /= 10;
		if(averageUs == 0)
			break;
	}
	while(d > 0)
		line[n++] = digits[--d];
	line[n++] = ' ';
	line[n++] = 'u';
	line[n++] = 's';
	line[n] = 0;
	amiga_boot_log(line);
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bool
AmigaC64IntroShow(void)
{
	C64ScreenTexture texture;
	c64intro::Screen screen;
	c64intro::State state;
	const char *buildId = g_GIT_SHA1;
	unsigned start, lastFrame = ~0u;
	unsigned uploadTotalUs = 0, uploadCount = 0;

	if(!texture.Create())
		return false;

	c64intro::Init(state, screen, c64intro::HashSeed(buildId), buildId);
	amiga_boot_log("c64intro: start");

	start = amiga_boot_ms();
	// The MINI backend flips on the VBL, so a presented frame paces the loop.
	// The model is ticked only when the 12 fps frame index changes; the border
	// and LED are redrawn every presented frame at the display refresh.
	for(;;){
		unsigned elapsed, frame;
		unsigned t0;

		amiga_boot_poll_events();
		if(amiga_boot_should_quit())
			break;

		elapsed = amiga_boot_ms() - start;	// wrap-safe
		if(elapsed >= c64intro::kTotalMs){
			// One last fully-faded frame, then the logo splash follows.
			DrawFrame(texture, c64intro::kTotalMs);
			break;
		}

		frame = c64intro::FrameIndex(elapsed);
		if(frame != lastFrame){
			c64intro::Tick(state, screen, elapsed);
			t0 = amiga_boot_us();
			if(!texture.Upload(screen)){
				amiga_boot_log("c64intro: raster lock failed, skipping");
				return false;
			}
			uploadTotalUs += amiga_boot_us() - t0;
			uploadCount++;
			lastFrame = frame;
		}

		if(!DrawFrame(texture, elapsed))
			amiga_boot_yield();
	}

	LogUploadSummary(uploadCount, uploadCount ? uploadTotalUs / uploadCount : 0);
	return true;
}
