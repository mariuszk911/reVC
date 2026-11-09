/*
 * H9: BMP decode unit test (vendor/librw/src/mini/minibmp.h).
 *
 * readBMP() reads the BMP/DIB headers and pixel/palette data itself. The
 * extracted minibmp.h is the dependency-free copy of that parse, included by
 * vendor/librw/src/bmp.cpp and here so the test exercises the exact production
 * code. The critical contract (part 52 of the bring-up log) is that every BMP
 * multi-byte field is read little-endian byte-wise: on big-endian 68k a native
 * load of width/height/offset/depth would mis-parse the file. The test builds
 * synthetic streams whose LE byte order is explicit, so it is meaningful on the
 * little-endian host too.
 *
 * Covers: 24-bit BGR->RGB + bottom-up rows + row padding; 32-bit BI_RGB (alpha
 * forced opaque) vs BI_BITFIELDS with 0xFF000000 (alpha preserved); 8-bit and
 * 4-bit paletted (BGR->RGB, nibble order, forced alpha); an explicit LE width/
 * height KAT; the BITMAPCOREHEADER (headerSize <= 16) compression override; and
 * rejection of bad magic / unsupported compression / wrong bitfield masks /
 * invalid depth / truncated data.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes DH11:bmp.log.
 * Integer assertions only (%f / %g formatting is broken in this libnix).
 */

#include <cstdio>
#include <cstring>

#include "minibmp.h"

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

// ---------------------------------------------------------------------------
// Synthetic BMP file builder. `fileLen` is volatile so the compiler cannot
// constant-fold the byte positions and the decoder cannot be evaluated at
// compile time.
// ---------------------------------------------------------------------------
static unsigned char file[4096];
static volatile unsigned int fileLen;

static void b8(unsigned int v) { file[fileLen] = (unsigned char)(v & 0xFF); fileLen++; }
static void b16(unsigned int v) { b8(v); b8(v >> 8); }
static void b32(unsigned int v) { b8(v); b8(v >> 8); b8(v >> 16); b8(v >> 24); }

static void
reset(void) { fileLen = 0; }

static void
fileHeader(unsigned int offset)
{
	b16(0x4D42);        // 'B','M' little-endian
	b32(0);             // file size (unused)
	b16(0); b16(0);     // reserved
	b32(offset);
}

// BITMAPINFOHEADER base (headerSize is 40, or 56 with the four bitfield
// masks appended by the caller).
static void
dibHdr(unsigned int headerSize, int w, int h, int depth, unsigned int compression)
{
	b32(headerSize);
	b32((unsigned int)w);
	b32((unsigned int)h);
	b16(1);                       // planes
	b16((unsigned int)depth);
	b32(compression);
	b32(0);                       // imgSize
	b32(2835); b32(2835);         // resolution
	b32(0); b32(0);               // paletteLen, numImportant
}

static void
build(const unsigned char *data, unsigned int len)
{
	memcpy(file, data, len);
	fileLen = len;
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:bmp.log", "w", stdout);
#endif

	MiniBMPHeader h;

	// --- 24-bit, 2x2: BGR -> RGB, bottom-up rows, 2-byte row padding ------
	{
		reset();
		fileHeader(54);
		dibHdr(40, 2, 2, 24, 0);
		// bottom row first: BL=blue, BR=white, +2 pad
		b8(0xFF); b8(0x00); b8(0x00);
		b8(0xFF); b8(0xFF); b8(0xFF);
		b8(0); b8(0);
		// top row: TL=red, TR=green, +2 pad
		b8(0x00); b8(0x00); b8(0xFF);
		b8(0x00); b8(0xFF); b8(0x00);
		b8(0); b8(0);

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.width == 2 && h.height == 2 && h.depth == 24);
		CHECK(h.bpp == 3 && h.stride == 6 && h.paletteCount == 0);
		CHECK(h.noalpha == 1);

		unsigned char px[16];
		memset(px, 0xAA, sizeof(px));
		miniReadBMPPixels(file, (unsigned int)fileLen, &h, 0, px);
		// top row: red, green; bottom row: blue, white
		const unsigned char want[12] = {
			255, 0, 0,   0, 255, 0,
			0, 0, 255,   255, 255, 255
		};
		checkBytes("bmp24 2x2 BGR->RGB bottom-up", px, want, 12);
	}

	// --- 32-bit BI_RGB, 40-byte header: alpha forced opaque ---------------
	{
		reset();
		fileHeader(54);
		dibHdr(40, 1, 1, 32, 0);
		b8(10); b8(20); b8(30); b8(40);   // B,G,R,A (A ignored)

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.bpp == 4 && h.stride == 4);
		CHECK(h.noalpha == 1);

		unsigned char px[4];
		miniReadBMPPixels(file, (unsigned int)fileLen, &h, 0, px);
		const unsigned char want[4] = { 30, 20, 10, 0xFF };
		checkBytes("bmp32 BI_RGB alpha forced opaque", px, want, 4);
	}

	// --- 32-bit BI_BITFIELDS, 56-byte header: alpha preserved -------------
	{
		reset();
		fileHeader(70);
		dibHdr(56, 1, 1, 32, 3);               // 56-byte header = BI_BITFIELDS
		b32(0xFF0000); b32(0x00FF00); b32(0x0000FF); b32(0xFF000000);
		b8(10); b8(20); b8(30); b8(40);   // B,G,R,A (A kept)

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.noalpha == 0);

		unsigned char px[4];
		miniReadBMPPixels(file, (unsigned int)fileLen, &h, 0, px);
		const unsigned char want[4] = { 30, 20, 10, 40 };
		checkBytes("bmp32 BI_BITFIELDS alpha preserved", px, want, 4);
	}

	// --- 8-bit paletted, 1x1: BGR palette -> RGBA, forced alpha -----------
	{
		reset();
		fileHeader(14 + 40 + 256 * 4);     // 1078
		dibHdr(40, 1, 1, 8, 0);
		// palette[0] = B=0x11, G=0x22, R=0x33, A=0x44; rest zero
		b8(0x11); b8(0x22); b8(0x33); b8(0x44);
		for (int i = 1; i < 256; i++) { b8(0); b8(0); b8(0); b8(0); }
		b8(0x00);                          // pixel index 0
		b8(0);                             // 1 byte row pad

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.depth == 8 && h.bpp == 1 && h.stride == 1);
		CHECK(h.paletteCount == 256);

		unsigned char pal[1024];
		unsigned char px[4];
		memset(pal, 0x5A, sizeof(pal));
		miniReadBMPPixels(file, (unsigned int)fileLen, &h, pal, px);
		const unsigned char wantPal[4] = { 0x33, 0x22, 0x11, 0xFF };
		checkBytes("bmp8 palette BGRA->RGBA forced alpha", pal, wantPal, 4);
		CHECK(px[0] == 0x00);
	}

	// --- 4-bit paletted, 2x1: high nibble first, forced alpha -------------
	{
		reset();
		fileHeader(14 + 40 + 16 * 4);      // 118
		dibHdr(40, 2, 1, 4, 0);
		// palette[1] = B=1, G=2, R=3, A=4
		for (int i = 0; i < 16; i++) {
			if (i == 1) { b8(1); b8(2); b8(3); b8(4); }
			else { b8(0); b8(0); b8(0); b8(0); }
		}
		b8(0x12);                          // pixel0 = high nibble 1, pixel1 = 2
		b8(0); b8(0);                      // 2 bytes row pad

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.depth == 4 && h.bpp == 1 && h.stride == 2);
		CHECK(h.paletteCount == 16);

		unsigned char pal[64];
		unsigned char px[2];
		miniReadBMPPixels(file, (unsigned int)fileLen, &h, pal, px);
		const unsigned char wantPal[4] = { 3, 2, 1, 0xFF };
		checkBytes("bmp4 palette", pal + 4, wantPal, 4);
		CHECK(px[0] == 1 && px[1] == 2);
	}

	// --- Explicit little-endian field KAT (catches a native load on 68k) --
	{
		reset();
		fileHeader(54);
		dibHdr(40, 0x00000102, 0x00000304, 24, 0);  // 258 x 772

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.width == 258);
		CHECK(h.height == 772);
		CHECK(h.stride == 258 * 3);
	}

	// --- BITMAPCOREHEADER (headerSize <= 16): compression forced to 0 -----
	{
		reset();
		fileHeader(54);
		b32(12);                          // headerSize
		b32(1); b32(1); b16(1); b16(24);
		b32(5);                           // nonzero "compression" must be ignored
		b32(0); b32(0); b32(0); b32(0);
		b32(0); b32(0); b32(0); b32(0);

		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 1);
		CHECK(h.depth == 24);
	}

	// --- Rejection cases --------------------------------------------------
	{
		reset();
		b16(0x0000); b32(0); b16(0); b16(0); b32(54);
		b32(40); b32(1); b32(1); b16(1); b16(24);
		b32(0); b32(0); b32(0); b32(0); b32(0); b32(0);
		b32(0); b32(0); b32(0); b32(0);
		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 0);

		reset();
		fileHeader(54);
		dibHdr(40, 1, 1, 8, 1);                // RLE8 compression: unsupported
		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 0);

		reset();
		fileHeader(70);
		dibHdr(40, 1, 1, 32, 3);
		b32(0xFF00); b32(0x00FF00); b32(0x0000FF); b32(0xFF000000);
		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 0);

		reset();
		fileHeader(54);
		dibHdr(40, 1, 1, 12, 0);               // invalid depth
		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 0);

		const unsigned char shortData[6] = { 'B', 'M', 0, 0, 0, 0 };
		build(shortData, sizeof(shortData));
		CHECK(miniReadBMPHeader(file, (unsigned int)fileLen, &h) == 0);
	}

	if (failures) {
		std::printf("bmp_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("bmp_test: all ok\n");
	return 0;
}
