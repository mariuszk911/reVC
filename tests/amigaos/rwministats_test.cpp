/*
 * Unit test for the MINI per-frame render-stats aggregation
 * (vendor/librw/src/mini/rwministats.h).
 *
 * The backend keeps one MiniStatsAccum: the render passes add to `cur` while
 * collection is on and showRaster publishes it into `last` at the frame
 * boundary. That reset/publish arithmetic is dependency-free, so the exact code
 * the backend runs is exercised here without a GPU (like miniarray_test).
 *
 * Covers: frame clear preserving the static caps, publish (cur -> last + clear),
 * clear-all (zero-when-off) and the flat 60-byte RwMiniStats layout.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:rwministats.log. Nothing is printed as a float (libnix %f is broken).
 */

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "rwministats.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Static caps are not per-frame; the frame helpers must leave them intact. */
static void
setCaps(RwMiniStats *s, unsigned int v)
{
	s->maxTextureSize = v;
	s->vertexBufferSize = v + 1;
	s->textureBufferSize = v + 2;
	s->vertexArrays = 1;
}

static void
checkCaps(const RwMiniStats *s, unsigned int v)
{
	CHECK(s->maxTextureSize == v);
	CHECK(s->vertexBufferSize == v + 1);
	CHECK(s->textureBufferSize == v + 2);
	CHECK(s->vertexArrays == 1);
}

/* Number of unsigned-int words in the per-frame prefix (everything before the
 * static caps), so the helpers below track RwMiniStats instead of restating the
 * field list the production clear routine also derives from the layout. */
static std::size_t
frameWords(void)
{
	return offsetof(RwMiniStats, maxTextureSize) / sizeof(unsigned int);
}

static void
setFrame(RwMiniStats *s, unsigned int v)
{
	unsigned int *w = (unsigned int *)s;
	std::size_t i;
	for(i = 0; i < frameWords(); i++)
		w[i] = v + (unsigned int)i;
}

static bool
frameZero(const RwMiniStats *s)
{
	const unsigned int *w = (const unsigned int *)s;
	std::size_t i;
	for(i = 0; i < frameWords(); i++)
		if(w[i] != 0)
			return false;
	return true;
}

int main()
{
#ifdef AMIGAOS
	freopen("DH11:rwministats.log", "w", stdout);
#endif

	/* Layout: the flat all-unsigned-int contract the 68k ABI relies on (no
	 * -malign-int, no padding). */
	{
		CHECK(sizeof(RwMiniStats) == 60);
		CHECK(sizeof(MiniStatsAccum) == 2 * sizeof(RwMiniStats));
	}

	/* Frame clear zeroes every per-frame field and keeps the caps. */
	{
		RwMiniStats s;
		std::memset(&s, 0, sizeof(s));
		setCaps(&s, 4096);
		setFrame(&s, 100);
		miniStatsClearFrame(&s);
		CHECK(frameZero(&s));
		checkCaps(&s, 4096);
	}

	/* Publish copies cur into last, clears cur and keeps both caps. */
	{
		MiniStatsAccum a;
		std::memset(&a, 0, sizeof(a));
		setCaps(&a.cur, 2048);
		setCaps(&a.last, 2048);
		setFrame(&a.cur, 200);
		miniStatsPublish(&a);
		CHECK(a.last.drawUs == 200 && a.last.skinUs == 201 &&
		      a.last.im2dUs == 202 && a.last.im3dUs == 203);
		CHECK(a.last.meshes == 204 && a.last.skinMeshes == 205 &&
		      a.last.drawCalls == 206 && a.last.texBinds == 207);
		CHECK(a.last.stateChanges == 208 && a.last.chunks == 209 &&
		      a.last.verts == 210);
		CHECK(frameZero(&a.cur));
		checkCaps(&a.cur, 2048);
		checkCaps(&a.last, 2048);
	}

	/* Zero-when-off: clear-all drops both frames' per-frame data, keeps caps,
	 * so a query returns zeros while the caps stay valid. */
	{
		MiniStatsAccum a;
		std::memset(&a, 0, sizeof(a));
		setCaps(&a.cur, 512);
		setCaps(&a.last, 512);
		setFrame(&a.last, 9);
		miniStatsClearAll(&a);
		CHECK(frameZero(&a.cur));
		CHECK(frameZero(&a.last));
		checkCaps(&a.cur, 512);
		checkCaps(&a.last, 512);
	}

	if (failures) {
		std::printf("rwministats_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("rwministats_test: all ok\n");
	return 0;
}
