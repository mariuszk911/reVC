/*
 * H4/H5: texture decode / colour conversion / downscale unit test
 * (vendor/librw/src/mini/minidecode.h).
 *
 * The MINI backend decodes DXT1/3/5, PAL4/PAL8 and 16/24/32-bit TXD levels
 * itself (miniraster.cpp) and the shared raster.cpp conv_ / expandPal4 / copyPal8
 * helpers feed it. minidecode.h is the extracted, dependency-free copy of that
 * math, included by image.cpp/raster.cpp/miniraster.cpp and here so the test
 * exercises the exact production code.
 *
 * Covers: DXT1/3/5 against an independent per-texel reference decoder, explicit
 * known-answer cases (opaque/1-bit-alpha/midpoint, DXT3 4-bit alpha, 1555),
 * a little-endian KAT (the whole point of H4: a native multi-byte load creeping
 * back in is wrong on big-endian 68k), palette expand/copy/unpalettize, the
 * 2x2 box downscale, the raster row fast paths plus merged alpha scan used by
 * rasterFromImage (against the legacy conv_* bytes), and a real generic.txd
 * decode smoke (structure + dims + level-0 byte count + opaque alpha) or SKIP
 * when the asset is absent.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:texture_decode.log. No libm float calls (%f / %g formatting is broken in
 * this libnix, and CXX tests are not linked with libfix_math.o) -- integer
 * assertions only.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "minidecode.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

static void
checkBytes(const char *name, const unsigned char *got, const unsigned char *want,
           long n)
{
	for (long i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			std::printf("FAIL %s (byte %ld: got %u want %u)\n",
			            name, i, (unsigned)got[i], (unsigned)want[i]);
			failures++;
			return;
		}
	}
	std::printf("ok   %s\n", name);
}

/*
 * Raster row fast paths used by MINI rasterFromImage. The reference is the
 * legacy per-pixel conv_* function, so this is exactly the byte contract the
 * fused DXT writer will rely on.
 */
static void
testRowHelpers(void)
{
	static const int widths[] = { 1, 3, 4, 5, 64 };
	unsigned char src[64*4 + 16];
	unsigned char dst[64*4 + 16];
	unsigned char ref[64*4 + 16];

	for (int i = 0; i < (int)sizeof(src); i++)
		src[i] = (unsigned char)(i*37 + 11);

	for (int wi = 0; wi < (int)(sizeof(widths)/sizeof(widths[0])); wi++) {
		int w = widths[wi];

		/* RGBA8888 -> RGBA8888 */
		std::memset(dst, 0xA5, sizeof(dst));
		std::memset(ref, 0, sizeof(ref));
		for (int x = 0; x < w; x++)
			miniConvRGBA8888fromRGBA8888(ref + x*4, src + x*4);
		miniCopyRGBA8888Row(dst + 4, src, w);
		checkBytes("row RGBA->RGBA", dst + 4, ref, w*4);
		CHECK(dst[3] == 0xA5 && dst[4 + w*4] == 0xA5);

		/* RGB888 -> RGB888 */
		std::memset(dst, 0xA5, sizeof(dst));
		std::memset(ref, 0, sizeof(ref));
		for (int x = 0; x < w; x++)
			miniConvRGB888fromRGB888(ref + x*3, src + x*3);
		miniCopyRGB888Row(dst + 4, src, w);
		checkBytes("row RGB->RGB", dst + 4, ref, w*3);
		CHECK(dst[3] == 0xA5 && dst[4 + w*3] == 0xA5);

		/* opaque 32-bit -> RGB888: alpha must not be written past the row */
		std::memset(dst, 0xA5, sizeof(dst));
		std::memset(ref, 0, sizeof(ref));
		for (int x = 0; x < w; x++)
			miniConvRGB888fromRGB888(ref + x*3, src + x*4);
		miniPackRGB888fromRGBA8888Row(dst + 4, src, w);
		checkBytes("row RGBA->RGB", dst + 4, ref, w*3);
		CHECK(dst[3] == 0xA5 && dst[4 + w*3] == 0xA5);

		/* RGB888 -> RGBA8888: alpha is opaque */
		std::memset(dst, 0xA5, sizeof(dst));
		std::memset(ref, 0, sizeof(ref));
		for (int x = 0; x < w; x++)
			miniConvRGBA8888fromRGB888(ref + x*4, src + x*3);
		miniExpandRGBA8888fromRGB888Row(dst + 4, src, w);
		checkBytes("row RGB->RGBA", dst + 4, ref, w*4);
		CHECK(dst[3] == 0xA5 && dst[4 + w*4] == 0xA5);
	}
}

/* The old Image::hasAlpha()/imageHasSmoothAlpha() semantics, used as the
 * reference for the merged scan. */
static void
refAlphaFlags(const unsigned char *pixels, int w, int h, int bpp, int stride,
              int depth, const unsigned char *palette, int *any, int *smooth)
{
	*any = 0;
	*smooth = 0;
	if (depth == 32) {
		unsigned ret = 0xFF;
		for (int y = 0; y < h; y++) {
			const unsigned char *line = pixels + y*stride;
			for (int x = 0; x < w; x++) {
				unsigned a = line[3];
				ret &= a;
				if (a != 0 && a != 0xFF)
					*smooth = 1;
				line += bpp;
			}
		}
		*any = ret != 0xFF;
		if (!*any)
			*smooth = 0;
	} else if (depth == 16) {
		unsigned ret = 0xFF;
		for (int y = 0; y < h; y++) {
			const unsigned char *line = pixels + y*stride;
			for (int x = 0; x < w; x++) {
				ret &= line[1] & 0x80;
				line += bpp;
			}
		}
		*any = ret != 0x80;
	} else if (depth <= 8 && palette) {
		unsigned ret = 0xFF;
		for (int y = 0; y < h; y++) {
			const unsigned char *line = pixels + y*stride;
			for (int x = 0; x < w; x++) {
				ret &= palette[*line*4 + 3];
				line += bpp;
			}
		}
		*any = ret != 0xFF;
	}
}

static void
checkAlphaCase(const char *name, const unsigned char *pixels, int w, int h,
               int bpp, int stride, int depth, const unsigned char *palette,
               int wantAny, int wantSmooth)
{
	int any, smooth, rAny, rSmooth;
	miniImageAlphaFlags(pixels, w, h, bpp, stride, depth, palette,
	                    &any, &smooth);
	refAlphaFlags(pixels, w, h, bpp, stride, depth, palette, &rAny, &rSmooth);
	if (any != wantAny || smooth != wantSmooth ||
	    any != rAny || smooth != rSmooth)
		std::printf("FAIL alpha %s: got any=%d smooth=%d want any=%d smooth=%d "
		            "(ref any=%d smooth=%d)\n",
		            name, any, smooth, wantAny, wantSmooth, rAny, rSmooth);
	else
		std::printf("ok   alpha %s (any=%d smooth=%d)\n",
		            name, any, smooth);
	if (any != wantAny || smooth != wantSmooth ||
	    any != rAny || smooth != rSmooth)
		failures++;
}

/* Reference alpha scan of a decoded 32-bit RGBA image. */
static void
scanLegacyAlpha(const unsigned char *rgba, int n, int *any, int *smooth)
{
	*any = 0;
	*smooth = 0;
	for (int i = 0; i < n; i++) {
		unsigned a = rgba[i*4 + 3];
		if (a != 0xFF) *any = 1;
		if (a != 0 && a != 0xFF) *smooth = 1;
	}
}

static unsigned int dxtRng = 0x13579BDFu;
static unsigned char
dxtRand(void)
{
	dxtRng = dxtRng*1664525u + 1013904223u;
	return (unsigned char)(dxtRng >> 16);
}

static void
testDXTAlphaFlags(void)
{
	unsigned char src1[16*8];
	unsigned char src3[16*16];
	unsigned char src5[16*16];
	unsigned char legacy[16*16*4];
	int any, smooth, wantAny, wantSmooth;

	/* Structured DXT1: opaque, transparent-mode/index 3, 4-color mode. */
	{
		static const unsigned char opaque[8] =
			{ 0xFF,0xFF, 0x00,0x00, 0x00,0x00,0x00,0x00 };
		static const unsigned char transp[8] =
			{ 0x00,0x00, 0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF };
		static const unsigned char four[8] =
			{ 0xFF,0xFF, 0x00,0x00, 0xFF,0xFF,0xFF,0xFF };
		miniDXTAlphaFlags(1, opaque, 4, 4, &any, &smooth);
		CHECK(any == 0 && smooth == 0);
		miniDXTAlphaFlags(1, transp, 4, 4, &any, &smooth);
		CHECK(any == 1 && smooth == 0);
		miniDXTAlphaFlags(1, four, 4, 4, &any, &smooth);
		CHECK(any == 0 && smooth == 0);
	}

	/* Structured DXT3: opaque, 1-bit, smooth alpha. */
	{
		unsigned char b[16];
		std::memset(b, 0xFF, sizeof(b));
		miniDXTAlphaFlags(3, b, 4, 4, &any, &smooth);
		CHECK(any == 0 && smooth == 0);
		b[0] = 0x00;
		miniDXTAlphaFlags(3, b, 4, 4, &any, &smooth);
		CHECK(any == 1 && smooth == 0);
		b[0] = 0x77;
		miniDXTAlphaFlags(3, b, 4, 4, &any, &smooth);
		CHECK(any == 1 && smooth == 1);
	}

	/* Random DXT1: 16 blocks (16x16), probe must match the decoded scan. */
	for (int i = 0; i < (int)sizeof(src1); i++) src1[i] = dxtRand();
	miniDecompressDXT1(legacy, 16, 16, src1);
	scanLegacyAlpha(legacy, 16*16, &wantAny, &wantSmooth);
	miniDXTAlphaFlags(1, src1, 16, 16, &any, &smooth);
	CHECK(any == wantAny && smooth == wantSmooth);

	/* Random DXT3. */
	for (int i = 0; i < (int)sizeof(src3); i++) src3[i] = dxtRand();
	miniDecompressDXT3(legacy, 16, 16, src3);
	scanLegacyAlpha(legacy, 16*16, &wantAny, &wantSmooth);
	miniDXTAlphaFlags(3, src3, 16, 16, &any, &smooth);
	CHECK(any == wantAny && smooth == wantSmooth);

	/* Random DXT5 (none in the retail assets, but the probe supports it). */
	for (int i = 0; i < (int)sizeof(src5); i++) src5[i] = dxtRand();
	miniDecompressDXT5(legacy, 16, 16, src5);
	scanLegacyAlpha(legacy, 16*16, &wantAny, &wantSmooth);
	miniDXTAlphaFlags(5, src5, 16, 16, &any, &smooth);
	CHECK(any == wantAny && smooth == wantSmooth);
}

static void
checkRowPadding(const char *name, const unsigned char *row, int used,
                int stride)
{
	int ok = 1;
	for (int i = used; i < stride; i++)
		if (row[i] != 0xA5)
			ok = 0;
	CHECK(ok);
	(void)name;
}

static void
runFusedCase(int type, int w, int h, const unsigned char *src,
             const unsigned char *legacy)
{
	const int stride4 = w*4 + 8;
	const int stride3 = w*3 + 8;
	const int guard = 16;
	unsigned char dst4[16*8*4 + 16 + 64] __attribute__((aligned(4)));
	unsigned char dst3[16*8*3 + 16 + 64];
	unsigned char ref24[16*8*3];

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			for (int c = 0; c < 3; c++)
				ref24[(y*w + x)*3 + c] = legacy[(y*w + x)*4 + c];

	std::memset(dst4, 0xA5, sizeof(dst4));
	std::memset(dst3, 0xA5, sizeof(dst3));
	if (type == 1) {
		miniDecompressDXT1To(dst4, stride4, 4, w, h, src);
		miniDecompressDXT1To(dst3, stride3, 3, w, h, src);
	} else {
		miniDecompressDXT3To(dst4, stride4, 4, w, h, src);
		miniDecompressDXT3To(dst3, stride3, 3, w, h, src);
	}

	for (int y = 0; y < h; y++) {
		checkBytes("dxt fused 32", dst4 + y*stride4,
		           legacy + y*w*4, w*4);
		checkRowPadding("dxt fused 32 padding", dst4 + y*stride4,
		                w*4, stride4);
		checkBytes("dxt fused 24", dst3 + y*stride3,
		           ref24 + y*w*3, w*3);
		checkRowPadding("dxt fused 24 padding", dst3 + y*stride3,
		                w*3, stride3);
	}
	CHECK(dst4[stride4*h + guard - 1] == 0xA5);
	CHECK(dst3[stride3*h + guard - 1] == 0xA5);
}

static void
testDXTFused(void)
{
	const int w = 16, h = 8;
	unsigned char src1[8*8];
	unsigned char src3[8*16];
	unsigned char legacy[16*8*4];
	int any, smooth, wantAny, wantSmooth;

	for (int i = 0; i < (int)sizeof(src1); i++) src1[i] = dxtRand();
	for (int i = 0; i < (int)sizeof(src3); i++) src3[i] = dxtRand();

	miniDecompressDXT1(legacy, w, h, src1);
	scanLegacyAlpha(legacy, w*h, &wantAny, &wantSmooth);
	miniDXTAlphaFlags(1, src1, w, h, &any, &smooth);
	CHECK(any == wantAny && smooth == wantSmooth);
	runFusedCase(1, w, h, src1, legacy);

	miniDecompressDXT3(legacy, w, h, src3);
	scanLegacyAlpha(legacy, w*h, &wantAny, &wantSmooth);
	miniDXTAlphaFlags(3, src3, w, h, &any, &smooth);
	CHECK(any == wantAny && smooth == wantSmooth);
	runFusedCase(3, w, h, src3, legacy);
}

static void
testAlphaFlags(void)
{
	unsigned char px[8*8*4];
	unsigned char pal[256*4];

	/* 32-bit, 4x3, stride 24 (8 bytes of row padding). */
	std::memset(px, 0, sizeof(px));
	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 4; x++) {
			unsigned char *p = px + y*24 + x*4;
			p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 255;
		}
	checkAlphaCase("rgba opaque", px, 4, 3, 4, 24, 32, 0, 0, 0);
	px[0*24 + 1*4 + 3] = 0;
	checkAlphaCase("rgba 1-bit alpha", px, 4, 3, 4, 24, 32, 0, 1, 0);
	px[0*24 + 1*4 + 3] = 128;
	checkAlphaCase("rgba smooth alpha", px, 4, 3, 4, 24, 32, 0, 1, 1);
	std::memset(px, 0, sizeof(px));
	checkAlphaCase("rgba all zero alpha", px, 4, 3, 4, 24, 32, 0, 1, 0);

	/* Padding must not contribute: only the first 2 pixels of each row are
	 * real, the rest of the 16-byte stride has alpha 0. */
	std::memset(px, 0, sizeof(px));
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 2; x++) {
			unsigned char *p = px + y*16 + x*4;
			p[3] = 255;
		}
	checkAlphaCase("rgba stride padding", px, 2, 2, 4, 16, 32, 0, 0, 0);

	/* 24-bit has no alpha. */
	std::memset(px, 0x7F, sizeof(px));
	checkAlphaCase("rgb24", px, 4, 3, 3, 12, 24, 0, 0, 0);

	/* 16-bit ARGB1555: all alpha bits set -> opaque; one clear -> alpha. */
	std::memset(px, 0, sizeof(px));
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 3; x++)
			px[y*8 + x*2 + 1] = 0x80;
	checkAlphaCase("1555 opaque", px, 3, 2, 2, 8, 16, 0, 0, 0);
	px[0*8 + 1*2 + 1] = 0x00;
	checkAlphaCase("1555 1-bit alpha", px, 3, 2, 2, 8, 16, 0, 1, 0);

	/* 8-bit palette: alpha comes from the palette entries actually used by
	 * pixels; unused entries do not matter. Smooth is 0 for this depth in the
	 * old path (unpalettize turns it into 32-bit before the real scan). */
	std::memset(px, 0, sizeof(px));
	std::memset(pal, 0xFF, sizeof(pal));
	px[0] = 0; px[1] = 1; px[2] = 2;
	px[6] = 0; px[7] = 1; px[8] = 2;
	checkAlphaCase("pal8 opaque", px, 3, 2, 1, 6, 8, pal, 0, 0);
	pal[2*4 + 3] = 128;
	checkAlphaCase("pal8 used smooth entry", px, 3, 2, 1, 6, 8, pal, 1, 0);
	pal[2*4 + 3] = 0xFF;
	pal[5*4 + 3] = 128;	/* unused */
	checkAlphaCase("pal8 unused alpha entry", px, 3, 2, 1, 6, 8, pal, 0, 0);
}

/* Deterministic byte source so both decoders see identical blocks. */
static unsigned int rngState = 0x12345678u;
static unsigned char
rndByte(void)
{
	rngState = rngState*1664525u + 1013904223u;
	return (unsigned char)(rngState >> 16);
}

// ---------------------------------------------------------------------------
// Independent reference decoders: per-texel addressing (boxes addressed by
// x>>2 / y>>2) instead of the production block-stream loop. Same formulas, so
// a byte-for-byte match is meaningful.
// ---------------------------------------------------------------------------

static unsigned int
rd16(const unsigned char *p) { return p[0] | ((unsigned int)p[1] << 8); }
static unsigned int
rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned long long
rd64(const unsigned char *p)
{
	unsigned long long v = 0;
	for (int k = 7; k >= 0; k--)
		v = (v << 8) | p[k];
	return v;
}

/* RGB565 endpoint expansion + 4-colour/1-bit-alpha or 4-colour interpolation. */
static void
refColors(unsigned int c0, unsigned int c1, int dxt1,
          unsigned int col[4][4])
{
	col[0][0] = ((c0>>11) & 0x1F)*0xFF/0x1F;
	col[0][1] = ((c0>> 5) & 0x3F)*0xFF/0x3F;
	col[0][2] = ( c0      & 0x1F)*0xFF/0x1F;
	col[0][3] = 0xFF;
	col[1][0] = ((c1>>11) & 0x1F)*0xFF/0x1F;
	col[1][1] = ((c1>> 5) & 0x3F)*0xFF/0x3F;
	col[1][2] = ( c1      & 0x1F)*0xFF/0x1F;
	col[1][3] = 0xFF;
	if (!dxt1 || c0 > c1) {
		for (int i = 0; i < 3; i++) {
			col[2][i] = (2*col[0][i] + 1*col[1][i])/3;
			col[3][i] = (1*col[0][i] + 2*col[1][i])/3;
		}
		col[2][3] = 0xFF;
		col[3][3] = 0xFF;
	} else {
		for (int i = 0; i < 3; i++)
			col[2][i] = (col[0][i] + col[1][i])/2;
		col[2][3] = 0xFF;
		col[3][0] = col[3][1] = col[3][2] = col[3][3] = 0;
	}
}

static void
refDXT1(unsigned char *dst, int w, int h, const unsigned char *src)
{
	int bw = w/4;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int off = ((y>>2)*bw + (x>>2))*8;
			unsigned int c0 = rd16(src+off), c1 = rd16(src+off+2);
			unsigned int col[4][4];
			refColors(c0, c1, 1, col);
			int t = (y&3)*4 + (x&3);
			unsigned int idx = (rd32(src+off+4) >> (2*t)) & 3;
			unsigned char *d = dst + ((long)y*w + x)*4;
			d[0] = col[idx][0]; d[1] = col[idx][1];
			d[2] = col[idx][2]; d[3] = col[idx][3];
		}
}

static void
refDXT3(unsigned char *dst, int w, int h, const unsigned char *src)
{
	int bw = w/4;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int off = ((y>>2)*bw + (x>>2))*16;
			unsigned int c0 = rd16(src+off+8), c1 = rd16(src+off+10);
			unsigned int col[4][4];
			refColors(c0, c1, 0, col);
			int t = (y&3)*4 + (x&3);
			unsigned int idx = (rd32(src+off+12) >> (2*t)) & 3;
			unsigned int a = ((rd64(src+off) >> (4*t)) & 0xF)*17;
			unsigned char *d = dst + ((long)y*w + x)*4;
			d[0] = col[idx][0]; d[1] = col[idx][1];
			d[2] = col[idx][2]; d[3] = a;
		}
}

static void
refDXT5(unsigned char *dst, int w, int h, const unsigned char *src)
{
	int bw = w/4;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int off = ((y>>2)*bw + (x>>2))*16;
			unsigned int c0 = rd16(src+off+8), c1 = rd16(src+off+10);
			unsigned int col[4][4];
			refColors(c0, c1, 1, col);
			unsigned int a0 = src[off+0], a1 = src[off+1];
			unsigned int a[8];
			a[0] = a0; a[1] = a1;
			if (a0 > a1) {
				a[2] = (6*a0 + 1*a1)/7;
				a[3] = (5*a0 + 2*a1)/7;
				a[4] = (4*a0 + 3*a1)/7;
				a[5] = (3*a0 + 4*a1)/7;
				a[6] = (2*a0 + 5*a1)/7;
				a[7] = (1*a0 + 6*a1)/7;
			} else {
				a[2] = (4*a0 + 1*a1)/5;
				a[3] = (3*a0 + 2*a1)/5;
				a[4] = (2*a0 + 3*a1)/5;
				a[5] = (1*a0 + 4*a1)/5;
				a[6] = 0;
				a[7] = 0xFF;
			}
			int t = (y&3)*4 + (x&3);
			unsigned int idx = (rd32(src+off+12) >> (2*t)) & 3;
			unsigned int ai = (rd64(src+off+2) >> (3*t)) & 7;
			unsigned char *d = dst + ((long)y*w + x)*4;
			d[0] = col[idx][0]; d[1] = col[idx][1];
			d[2] = col[idx][2]; d[3] = a[ai];
		}
}

static int
near(unsigned int a, unsigned int b) { return a > b ? a-b <= 1 : b-a <= 1; }

static void
checkPix(const char *name, const unsigned char *px, long i,
         unsigned int r, unsigned int g, unsigned int b, unsigned int a)
{
	const unsigned char *p = px + i*4;
	if (near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], a))
		std::printf("ok   %s\n", name);
	else {
		std::printf("FAIL %s (pixel %ld: got %u,%u,%u,%u want %u,%u,%u,%u)\n",
		            name, i, p[0], p[1], p[2], p[3], r, g, b, a);
		failures++;
	}
}

// ---------------------------------------------------------------------------
// Real TXD chunk walker (LE headers: type u32, size u32, version u32).
// ---------------------------------------------------------------------------

static int
findChunk(const unsigned char *d, long start, long end, unsigned int magic,
          long *dataOff, long *size)
{
	long o = start;
	while (o + 12 <= end) {
		unsigned int t = rd32(d + o);
		unsigned int sz = rd32(d + o + 4);
		if ((unsigned long)sz > (unsigned long)(end - o - 12))
			return 0;
		if (t == magic) {
			*dataOff = o + 12;
			*size = (long)sz;
			return 1;
		}
		o += 12 + (long)sz;
	}
	return 0;
}

static unsigned char *
loadFile(const char *path, long *outLen)
{
	std::FILE *f = std::fopen(path, "rb");
	unsigned char *buf;
	long len;
	if (!f) return NULL;
	std::fseek(f, 0, SEEK_END);
	len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (len <= 0) { std::fclose(f); return NULL; }
	buf = (unsigned char*)std::malloc((size_t)len);
	if (!buf) { std::fclose(f); return NULL; }
	if (std::fread(buf, 1, (size_t)len, f) != (size_t)len) {
		std::free(buf); std::fclose(f); return NULL;
	}
	std::fclose(f);
	*outLen = len;
	return buf;
}

static long
expectedLevel0(int w, int h, int depth, int comp, unsigned int fmt)
{
	/* DXT1 is 8 bytes/block; DXT2/3/4/5 are 16 bytes/block. */
	if (comp == 1) {
		long bw = (w + 3)/4, bh = (h + 3)/4;
		return bw*bh*8;
	}
	if (comp == 2 || comp == 3 || comp == 4 || comp == 5) {
		long bw = (w + 3)/4, bh = (h + 3)/4;
		return bw*bh*16;
	}
	/* The native D3D8/9 blob stores PAL4 as D3DFMT_P8: one index byte per
	 * pixel (not packed nibbles), so level 0 is w*h bytes. */
	if (fmt & 0x4000) return (long)w*h;		/* PAL4 */
	if (fmt & 0x2000) return (long)w*h;		/* PAL8 */
	if (depth == 16) return (long)w*h*2;
	if (depth == 24) return (long)w*h*3;
	if (depth == 32) return (long)w*h*4;
	return -1;
}

/* Decode one native level-0 blob into RGBA (32-bit). Returns 1 on handled. */
static int
decodeLevel0(unsigned char *rgba, int w, int h, int depth, int comp,
             unsigned int fmt, const unsigned char *pal,
             const unsigned char *data)
{
	long n = (long)w*h;
	if (comp >= 1 && comp <= 5) {
		int dxt = comp == 2 ? 3 : comp == 4 ? 5 : comp;
		if (dxt == 1) miniDecompressDXT1(rgba, w, h, data);
		else if (dxt == 3) miniDecompressDXT3(rgba, w, h, data);
		else miniDecompressDXT5(rgba, w, h, data);
		return 1;
	}
	/* PAL4/PAL8 native levels are one index byte per pixel (see
	 * expectedLevel0). NOTE: the MINI reader currently assumes PAL4 is packed
	 * nibbles (`data[k/2]`); that divergence is the documented open question
	 * and is not what this asset actually stores, so the smoke follows the
	 * real layout. `miniExpandPal4` is still covered by its own KAT. */
	if ((fmt & 0x4000) || (fmt & 0x2000)) {
		miniUnpalettizeRGBA(rgba, w*4, data, w, pal, w, h);
		return 1;
	}
	{
		const unsigned char *s = data;
		unsigned char *p = rgba;
		for (long k = 0; k < n; k++) {
			if (depth == 16) {
				miniConvRGBA8888fromARGB1555(p, s);
				s += 2;
			} else if (depth == 24) {
				miniConvBGRA8888fromRGB888(p, s);
				s += 3;
			} else {
				miniConvBGRA8888fromRGBA8888(p, s);
				s += 4;
			}
			p += 4;
		}
	}
	return 1;
}

/* Real-asset smoke returns 0 = not found, 1 = decoded, -1 = found but skipped. */
static int
runTxdSmoke(const char *arg)
{
	const char *paths[6];
	int np = 0;
	if (arg && arg[0]) paths[np++] = arg;
	paths[np++] = "DH11:generic.txd";
	paths[np++] = "DH11:reVc_pistorm3d/models/generic.txd";
	paths[np++] = "gamefiles/models/generic.txd";
	paths[np++] = "../../gamefiles/models/generic.txd";

	unsigned char *d = NULL;
	long len = 0;
	for (int i = 0; i < np && !d; i++)
		d = loadFile(paths[i], &len);
	if (!d)
		return 0;

	std::printf("txd: %ld bytes\n", len);

	long dictOff, dictSize, structSize, tnOff, tnSize;
	long hdrOff, hdrSize;
	if (!findChunk(d, 0, len, 0x16, &dictOff, &dictSize) ||
	    !findChunk(d, dictOff, dictOff + dictSize, 0x01, &hdrOff, &structSize) ||
	    !findChunk(d, dictOff, dictOff + dictSize, 0x15, &tnOff, &tnSize) ||
	    !findChunk(d, tnOff, tnOff + tnSize, 0x01, &hdrOff, &hdrSize)) {
		std::printf("FAIL txd: chunk walk failed\n");
		failures++;
		std::free(d);
		return 1;
	}
	CHECK(structSize >= 4);
	CHECK(dictSize > 0 && tnSize > 0);

	{
		const unsigned char *hdr = d + hdrOff;
		char nm[33];
		std::memcpy(nm, hdr + 8, 32);
		nm[32] = 0;
		unsigned int platform = rd32(hdr);
		unsigned int fmt = rd32(hdr + 72);
		int width = rd16(hdr + 80), height = rd16(hdr + 82);
		int depth = hdr[84], numLevels = hdr[85], compression = hdr[87];
		int pallen = (fmt & 0x4000) ? 32 : (fmt & 0x2000) ? 256 : 0;
		long lvlOff = 88 + (long)pallen*4;
		long lvl0 = rd32(hdr + lvlOff);
		const unsigned char *pal = (const unsigned char*)hdr + 88;
		const unsigned char *data = hdr + lvlOff + 4;
		long want = expectedLevel0(width, height, depth, compression, fmt);

		std::printf("txd: first '%s' %dx%d depth=%d comp=%d fmt=0x%x level0=%ld\n",
		            nm, width, height, depth, compression, fmt, lvl0);
		CHECK(platform == 8 || platform == 9);
		CHECK(width > 0 && height > 0 && width <= 4096 && height <= 4096);
		CHECK(numLevels >= 1);
		CHECK(want > 0 && lvl0 == want);

		if (want <= 0 || lvl0 != want) {
			std::free(d);
			return 1;
		}
		if (lvlOff + 4 + lvl0 > hdrSize) {
			std::printf("FAIL txd: level 0 runs past native struct\n");
			failures++;
			std::free(d);
			return 1;
		}
		if (compression == 0 && !(fmt & 0x6000) &&
		    depth != 16 && depth != 24 && depth != 32) {
			std::printf("txd: SKIP first texture has unsupported format\n");
			std::free(d);
			return -1;
		}

		{
			long n = (long)width*height;
			unsigned char *rgba = (unsigned char*)std::malloc((size_t)n*4);
			if (rgba) {
				if (decodeLevel0(rgba, width, height, depth, compression,
				                 fmt, pal, data)) {
					long opaque = 0;
					for (long k = 0; k < n; k++)
						if (rgba[k*4+3] == 255) opaque++;
					std::printf("txd: opaque texels %ld/%ld\n", opaque, n);
					CHECK(opaque > 0);
				}
				std::free(rgba);
			}
		}
	}
	std::free(d);
	return 1;
}

int main(int argc, char **argv)
{
#ifdef AMIGAOS
	freopen("DH11:texture_decode.log", "w", stdout);
#endif

	/* --- LE reader KAT (the H4 endianness contract) --- */
	{
		const unsigned char b2[2] = { 0x34, 0x12 };
		const unsigned char b4[4] = { 0x78, 0x56, 0x34, 0x12 };
		const unsigned char b8[8] = { 0xEF, 0xCD, 0xAB, 0x89,
		                              0x67, 0x45, 0x23, 0x01 };
		CHECK(miniReadLE16(b2) == 0x1234u);
		CHECK(miniReadLE32(b4) == 0x12345678u);
		CHECK(miniReadLE64(b8) == 0x0123456789ABCDEFull);
	}

	/* --- DXT1 KATs (single 4x4 block) --- */
	{
		unsigned char blk[8];
		unsigned char px[64];

		/* col0=0xFFFF > col1=0, all indices 0 -> opaque white. */
		std::memset(blk, 0, sizeof(blk));
		blk[0] = 0xFF; blk[1] = 0xFF;
		miniDecompressDXT1(px, 4, 4, blk);
		checkPix("dxt1 white col0", px, 0, 255, 255, 255, 255);
		checkPix("dxt1 white last", px, 15, 255, 255, 255, 255);

		/* col0=0x0000 <= col1=0xFFFF, all indices 3 -> transparent black. */
		std::memset(blk, 0, sizeof(blk));
		blk[2] = 0xFF; blk[3] = 0xFF;
		blk[4] = 0xFF; blk[5] = 0xFF; blk[6] = 0xFF; blk[7] = 0xFF;
		miniDecompressDXT1(px, 4, 4, blk);
		checkPix("dxt1 1bit index3", px, 0, 0, 0, 0, 0);

		/* col0=0x001F (blue) < col1=0xF800 (red): index 1 = red endpoint,
		 * index 2 = midpoint (127,0,127), index 3 = transparent black,
		 * index 0 = blue endpoint. indices = t0=1,t1=2,t2=3,t3=0 = 0x39. */
		std::memset(blk, 0, sizeof(blk));
		blk[0] = 0x1F; blk[1] = 0x00; blk[2] = 0x00; blk[3] = 0xF8;
		blk[4] = 0x39;
		miniDecompressDXT1(px, 4, 4, blk);
		checkPix("dxt1 index1 red", px, 0, 255, 0, 0, 255);
		checkPix("dxt1 index2 midpoint", px, 1, 127, 0, 127, 255);
		checkPix("dxt1 index3 transparent", px, 2, 0, 0, 0, 0);
		checkPix("dxt1 index0 blue", px, 3, 0, 0, 255, 255);

		/* Endianness KAT: bytes are explicit little-endian. A native
		 * multi-byte load on 68k would read col0 as 0x1F00 and col1 as
		 * 0x007C, producing a different colour. */
		std::memset(blk, 0, sizeof(blk));
		blk[0] = 0x1F; blk[1] = 0x00; blk[2] = 0x00; blk[3] = 0xF8;
		miniDecompressDXT1(px, 4, 4, blk);
		checkPix("dxt1 LE byte order", px, 0, 0, 0, 255, 255);
	}

	/* --- DXT3 KAT: 4-bit alpha *17, 4-colour interpolation --- */
	{
		unsigned char blk[16];
		unsigned char px[64];
		std::memset(blk, 0, sizeof(blk));
		/* alphas t0=0xF t1=0x8 t2=0x4 t3=0x0 */
		blk[0] = 0x8F; blk[1] = 0x04;
		/* col0=0x0000, col1=0xFFFF */
		blk[10] = 0xFF; blk[11] = 0xFF;
		/* indices t0=0 t1=1 t2=2 t3=3 = 0xE4 */
		blk[12] = 0xE4;
		miniDecompressDXT3(px, 4, 4, blk);
		checkPix("dxt3 c0 a255", px, 0, 0, 0, 0, 255);
		checkPix("dxt3 c1 a136", px, 1, 255, 255, 255, 136);
		checkPix("dxt3 c2 a68", px, 2, 85, 85, 85, 68);
		checkPix("dxt3 c3 a0", px, 3, 170, 170, 170, 0);
	}

	/* --- DXT1/3/5 vs independent per-texel reference (8x8 = 4 blocks) --- */
	{
		const int W = 8, H = 8;
		unsigned char *src = (unsigned char*)std::malloc(W*H*4);
		unsigned char *got = (unsigned char*)std::malloc((size_t)W*H*4);
		unsigned char *ref = (unsigned char*)std::malloc((size_t)W*H*4);
		if (src && got && ref) {
			int b, nb;

			/* DXT1: 4 blocks * 8 bytes. Force both col0>col1 and <=. */
			nb = (W/4)*(H/4);
			for (b = 0; b < nb*8; b++) src[b] = rndByte();
			for (b = 0; b < nb; b++) {
				unsigned char *blk = src + b*8;
				if (b & 1) {
					blk[0] = 0x1F; blk[1] = 0x00;	/* col0=0x001F */
					blk[2] = 0x00; blk[3] = 0xF8;	/* col1=0xF800 */
				} else {
					blk[0] = 0x00; blk[1] = 0xF8;	/* col0=0xF800 */
					blk[2] = 0x1F; blk[3] = 0x00;	/* col1=0x001F */
				}
			}
			miniDecompressDXT1(got, W, H, src);
			refDXT1(ref, W, H, src);
			checkBytes("dxt1 vs reference", got, ref, (long)W*H*4);

			/* DXT3: 4 blocks * 16 bytes. */
			for (b = 0; b < nb*16; b++) src[b] = rndByte();
			for (b = 0; b < nb; b++) {
				unsigned char *blk = src + b*16;
				if (b & 1) {
					blk[8] = 0x1F; blk[9] = 0x00;
					blk[10] = 0x00; blk[11] = 0xF8;
				} else {
					blk[8] = 0x00; blk[9] = 0xF8;
					blk[10] = 0x1F; blk[11] = 0x00;
				}
			}
			miniDecompressDXT3(got, W, H, src);
			refDXT3(ref, W, H, src);
			checkBytes("dxt3 vs reference", got, ref, (long)W*H*4);

			/* DXT5: 4 blocks * 16 bytes; force both alpha modes. */
			for (b = 0; b < nb*16; b++) src[b] = rndByte();
			for (b = 0; b < nb; b++) {
				unsigned char *blk = src + b*16;
				if (b & 1) {
					blk[0] = 0xC8; blk[1] = 0x20;	/* a0=200 > a1=32 */
					blk[8] = 0x1F; blk[9] = 0x00;
					blk[10] = 0x00; blk[11] = 0xF8;
				} else {
					blk[0] = 0x20; blk[1] = 0xC8;	/* a0=32 < a1=200 */
					blk[8] = 0x00; blk[9] = 0xF8;
					blk[10] = 0x1F; blk[11] = 0x00;
				}
			}
			miniDecompressDXT5(got, W, H, src);
			refDXT5(ref, W, H, src);
			checkBytes("dxt5 vs reference", got, ref, (long)W*H*4);
		} else {
			CHECK(!"alloc failed");
		}
		std::free(src); std::free(got); std::free(ref);
	}

	/* --- ARGB1555 -> RGBA8888 KAT --- */
	{
		unsigned char in[2], out[4];

		in[0]=0x00; in[1]=0x00;
		miniConvRGBA8888fromARGB1555(out, in);
		{ unsigned char w[4]={0,0,0,0}; checkBytes("argb1555 0000", out, w, 4); }

		in[0]=0x00; in[1]=0x7C;
		miniConvRGBA8888fromARGB1555(out, in);
		{ unsigned char w[4]={255,0,0,0}; checkBytes("argb1555 7C00", out, w, 4); }

		in[0]=0xE0; in[1]=0x03;
		miniConvRGBA8888fromARGB1555(out, in);
		{ unsigned char w[4]={0,255,0,0}; checkBytes("argb1555 03E0", out, w, 4); }

		in[0]=0x1F; in[1]=0x00;
		miniConvRGBA8888fromARGB1555(out, in);
		{ unsigned char w[4]={0,0,255,0}; checkBytes("argb1555 001F", out, w, 4); }

		in[0]=0x00; in[1]=0x80;
		miniConvRGBA8888fromARGB1555(out, in);
		{ unsigned char w[4]={0,0,0,255}; checkBytes("argb1555 8000", out, w, 4); }

		in[0]=0xFF; in[1]=0xFF;
		miniConvRGBA8888fromARGB1555(out, in);
		{ unsigned char w[4]={255,255,255,255}; checkBytes("argb1555 FFFF", out, w, 4); }
	}

	/* --- RGB/BGRA reorders --- */
	{
		unsigned char rgb[3] = { 10, 20, 30 };
		unsigned char bgra[4] = { 10, 20, 30, 40 };
		unsigned char out[4];

		miniConvRGBA8888fromRGB888(out, rgb);
		CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 255);

		/* BGRA input -> RGBA output (R/B swap): 30,20,10,40. */
		miniConvBGRA8888fromRGBA8888(out, bgra);
		CHECK(out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 40);

		/* 5551 <-> 1555 round-trip is stable for r=g=b. */
		{
			unsigned char a[2] = { 0xFF, 0xFF };	/* white opaque */
			unsigned char b[2], c[2];
			miniConvRGBA5551fromARGB1555(b, a);
			miniConvARGB1555fromRGBA5551(c, b);
			CHECK(c[0] == a[0] && c[1] == a[1]);
		}
	}

	/* --- Palette expand / copy / unpalettize --- */
	{
		unsigned char src[2] = { 0xAB, 0x00 };
		unsigned char dst[2];
		/* MINI order: even pixel = high nibble. */
		miniExpandPal4(dst, 2, src, 1, 2, 1);
		CHECK(dst[0] == 0x0A && dst[1] == 0x0B);
		/* raster.cpp order (documented divergence): even = low nibble. */
		miniExpandPal4Lo(dst, 2, src, 1, 2, 1);
		CHECK(dst[0] == 0x0B && dst[1] == 0x0A);

		{
			unsigned char s8[4] = { 7, 8, 9, 10 };
			unsigned char d8[4] = { 0, 0, 0, 0 };
			miniCopyPal8(d8, 2, s8, 2, 2, 2);
			CHECK(d8[0] == 7 && d8[1] == 8 && d8[2] == 9 && d8[3] == 10);
		}

		{
			/* 4-entry palette with alpha; indices {0,1,2,3}. */
			unsigned char pal[16] = {
				1,2,3,4,  5,6,7,8,  9,10,11,12,  13,14,15,16
			};
			unsigned char idx[4] = { 0, 1, 2, 3 };
			unsigned char out[16];
			miniUnpalettizeRGBA(out, 16, idx, 4, pal, 4, 1);
			unsigned char want[16] = {
				1,2,3,4,  5,6,7,8,  9,10,11,12,  13,14,15,16
			};
			checkBytes("unpalettize RGBA", out, want, 16);
		}
	}

	/* --- miniHalveRGBA box filter --- */
	{
		unsigned char src[64];
		unsigned char out[64];

		/* 2x2 -> 1x1 exact average. */
		src[0]=10; src[1]=20; src[2]=30; src[3]=40;
		src[4]=50; src[5]=60; src[6]=70; src[7]=80;
		src[8]=90; src[9]=100; src[10]=110; src[11]=120;
		src[12]=130; src[13]=140; src[14]=150; src[15]=160;
		miniHalveRGBA(src, 2, 2, out);
		CHECK(out[0]==70 && out[1]==80 && out[2]==90 && out[3]==100);

		/* 4x4 -> 2x2: each output is the avg of its 2x2 quadrant. */
		{
			unsigned char s[64];
			for (int i = 0; i < 16; i++) {
				s[i*4+0] = (unsigned char)(i*4+0);
				s[i*4+1] = (unsigned char)(i*4+1);
				s[i*4+2] = (unsigned char)(i*4+2);
				s[i*4+3] = (unsigned char)(i*4+3);
			}
			miniHalveRGBA(s, 4, 4, out);
			/* quadrant (0,0): texels 0,1,4,5 -> per-channel avg. */
			CHECK(out[0] == (0+4+16+20)/4);
			CHECK(out[1] == (1+5+17+21)/4);
			CHECK(out[2] == (2+6+18+22)/4);
			CHECK(out[3] == (3+7+19+23)/4);
			/* quadrant (1,0): texels 2,3,6,7 (channel 0). */
			CHECK(out[4] == (8+12+24+28)/4);
			/* quadrant (0,1): texels 8,9,12,13 (channel 0). */
			CHECK(out[8] == (32+36+48+52)/4);
			/* quadrant (1,1): texels 10,11,14,15 (channel 0). */
			CHECK(out[12] == (40+44+56+60)/4);
		}

		/* 3x3 -> 1x1: only the top-left 2x2 contributes (nw=nh=1). */
		{
			unsigned char s[36];
			for (int i = 0; i < 9; i++) {
				s[i*4+0] = (unsigned char)(i*10);
				s[i*4+1] = (unsigned char)(i*10);
				s[i*4+2] = (unsigned char)(i*10);
				s[i*4+3] = (unsigned char)(i*10);
			}
			miniHalveRGBA(s, 3, 3, out);
			/* texels 0,1,3,4 -> 0,10,30,40 -> avg 20. */
			CHECK(out[0] == 20 && out[3] == 20);
		}

		/* 1x1 -> 1x1: all four samples clamp to the single texel. */
		{
			unsigned char s[4] = { 11, 22, 33, 44 };
			miniHalveRGBA(s, 1, 1, out);
			CHECK(out[0]==11 && out[1]==22 && out[2]==33 && out[3]==44);
		}

		/* 1x2 -> 1x1: horizontal edge clamps (sx=1 -> 0). */
		{
			unsigned char s[8] = { 10,20,30,40,  50,60,70,80 };
			miniHalveRGBA(s, 1, 2, out);
			/* samples (0,0),(0,0),(0,1),(0,1) -> avg of rows. */
			CHECK(out[0]==30 && out[1]==40 && out[2]==50 && out[3]==60);
		}
	}

	/* --- Raster row fast paths and merged alpha scan --- */
	testDXTAlphaFlags();
	testDXTFused();
	testRowHelpers();
	testAlphaFlags();

	/* --- Real generic.txd smoke --- */
	{
		const char *arg = (argc > 1) ? argv[1] : NULL;
		int r = runTxdSmoke(arg);
		if (r == 0) {
			if (failures) {
				std::printf("texture_decode_test: %d failure(s)\n", failures);
				return 1;
			}
			std::printf("texture_decode_test: SKIP (generic.txd not found)\n");
			return 0;
		}
	}

	if (failures) {
		std::printf("texture_decode_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("texture_decode_test: all ok\n");
	return 0;
}
