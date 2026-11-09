/*
 * Host/68k unit test for the runtime texture atlas packer
 * (vendor/librw/src/mini/miniatlas.h).
 *
 * Pins the shelf packing, per-texture UV transform, multi-shelf/multi-page
 * wrapping, and the invalid/oversized/overflow rejections. Runs on the host and
 * cross-linked for 68k; on AmigaOS writes DH11:miniatlas.log.
 */

#include <cstdio>
#include <cstring>

#include "miniatlas.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

static bool
within(const MiniAtlasRect &r, int pageSize)
{
	return r.x >= 0 && r.y >= 0 && r.x + r.w <= pageSize && r.y + r.h <= pageSize;
}

// No two rects on the same page may overlap.
static bool
overlaps(const MiniAtlasRect &a, const MiniAtlasRect &b)
{
	if (a.page != b.page)
		return false;
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

int main()
{
#ifdef AMIGAOS
	freopen("DH11:miniatlas.log", "w", stdout);
#endif

	/* single shelf: four 4x4 in a 16x16 page, pad 0 */
	{
		int ws[4] = {4,4,4,4}, hs[4] = {4,4,4,4};
		MiniAtlasRect r[4];
		int pages = miniAtlasPack(ws, hs, 4, 16, 0, 4, r);
		CHECK(pages == 1);
		CHECK(r[0].page == 0 && r[0].x == 0 && r[0].y == 0);
		CHECK(r[1].x == 4 && r[2].x == 8 && r[3].x == 12);
		CHECK(r[0].u0 == 0.0f && r[0].v0 == 0.0f);
		CHECK(r[3].u0 == 0.75f && r[3].du == 0.25f && r[3].dv == 0.25f);
		CHECK(within(r[0],16) && within(r[1],16) && within(r[2],16) && within(r[3],16));
	}

	/* pad gutter wraps the next cell to a new shelf */
	{
		int ws[2] = {8,8}, hs[2] = {6,6};
		MiniAtlasRect r[2];
		int pages = miniAtlasPack(ws, hs, 2, 16, 1, 2, r);
		CHECK(pages == 1);
		CHECK(r[0].x == 0 && r[0].y == 0);
		/* 0 + 8 + pad(1) = 9; 9+8=17 > 16 -> new shelf at y=0+6+1=7 */
		CHECK(r[1].x == 0 && r[1].y == 7);
	}

	/* height overflow wraps to a new page */
	{
		int ws[4] = {16,16,16,16}, hs[4] = {16,16,16,16};
		MiniAtlasRect r[4];
		int pages = miniAtlasPack(ws, hs, 4, 16, 0, 4, r);
		CHECK(pages == 4);
		CHECK(r[0].page == 0 && r[1].page == 1 && r[2].page == 2 && r[3].page == 3);
		CHECK(r[2].x == 0 && r[2].y == 0);
	}

	/* two textures share a shelf then a taller one starts a new shelf */
	{
		int ws[3] = {8,8,16}, hs[3] = {4,4,8};
		MiniAtlasRect r[3];
		int pages = miniAtlasPack(ws, hs, 3, 16, 0, 2, r);
		CHECK(pages == 1);
		CHECK(r[0].x == 0 && r[1].x == 8 && r[1].y == 0);
		CHECK(r[2].x == 0 && r[2].y == 4);	/* new shelf below the 4-tall band */
	}

	/* rejection: too many pages, oversized cell, invalid dims */
	{
		int ws[2] = {16,16}, hs[2] = {16,16};
		MiniAtlasRect r[2];
		CHECK(miniAtlasPack(ws, hs, 2, 16, 0, 1, r) == -1);	/* needs 2 pages */
		ws[0] = 32;
		CHECK(miniAtlasPack(ws, hs, 2, 16, 0, 4, r) == -1);	/* > page */
		ws[0] = 0;
		CHECK(miniAtlasPack(ws, hs, 2, 16, 0, 4, r) == -1);	/* invalid */
		CHECK(miniAtlasPack(ws, hs, 0, 16, 0, 4, r) == -1);	/* n=0 */
	}

	/* full 1024 shelf of 128x128 cells: 8 per row, overlaps-free */
	{
		int i;
		int ws[64], hs[64];
		MiniAtlasRect r[64];
		int pages;
		for (i = 0; i < 64; i++) { ws[i] = 128; hs[i] = 128; }
		pages = miniAtlasPack(ws, hs, 64, 1024, 0, 4, r);
		CHECK(pages == 1);
		CHECK(r[7].x == 896 && r[7].y == 0);
		CHECK(r[8].x == 0 && r[8].y == 128);
		for (i = 0; i < 64; i++) {
			int j;
			if (!within(r[i], 1024)) { CHECK(false); break; }
			for (j = i + 1; j < 64; j++)
				if (overlaps(r[i], r[j])) { CHECK(false); break; }
		}
	}

	if (failures) {
		std::printf("miniatlas_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("miniatlas_test: all ok\n");
	return 0;
}
