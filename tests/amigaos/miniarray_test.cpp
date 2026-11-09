/*
 * Unit test for the MINI client-array submission policy
 * (vendor/librw/src/mini/miniarray.h).
 *
 * The policy lives in a dependency-free header included by both mini.cpp /
 * minipipe.cpp and this test, so the exact decision and chunk arithmetic the
 * production array path runs is exercised here without a GPU.
 *
 * Covers: the D2 triangle-only gate, the D7 size thresholds (separate for
 * indexed and non-indexed draws), the sequential-index materialiser used for
 * non-indexed triangle draws, and the triangle-list / strip chunk bounds shared
 * with drawIndexedChunks().
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:miniarray.log. Nothing is printed as a float (libnix %f is broken)
 * and CXX tests are not linked with libfix_math.o.
 */

#include <cstdio>

#include "miniarray.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Force a genuine runtime read so gcc cannot constant-fold the calls. */
static int vi(int v) { volatile int t = v; return t; }

int main()
{
#ifdef AMIGAOS
	freopen("DH11:miniarray.log", "w", stdout);
#endif

	/* D7: disabled toggle never selects arrays, no matter the size. */
	{
		CHECK(miniArrayEligible(vi(0), vi(1), vi(100000), vi(0)) == 0);
		CHECK(miniArrayEligible(vi(0), vi(1), vi(100000), vi(1)) == 0);
	}

	/* D2: non-triangle primitives never select arrays. */
	{
		CHECK(miniArrayEligible(vi(1), vi(0), vi(100000), vi(0)) == 0);
		CHECK(miniArrayEligible(vi(1), vi(0), vi(100000), vi(1)) == 0);
	}

	/* D7: non-indexed threshold is MINI_ARRAY_MIN_VERTICES. */
	{
		CHECK(miniArrayEligible(vi(1), vi(1), vi(MINI_ARRAY_MIN_VERTICES - 1), vi(0)) == 0);
		CHECK(miniArrayEligible(vi(1), vi(1), vi(MINI_ARRAY_MIN_VERTICES), vi(0)) == 1);
		CHECK(miniArrayEligible(vi(1), vi(1), vi(MINI_ARRAY_MIN_VERTICES + 1), vi(0)) == 1);
	}

	/* D7: indexed threshold is MINI_ARRAY_MIN_INDICES. */
	{
		CHECK(miniArrayEligible(vi(1), vi(1), vi(MINI_ARRAY_MIN_INDICES - 1), vi(1)) == 0);
		CHECK(miniArrayEligible(vi(1), vi(1), vi(MINI_ARRAY_MIN_INDICES), vi(1)) == 1);
		CHECK(miniArrayEligible(vi(1), vi(1), vi(MINI_ARRAY_MIN_INDICES + 1), vi(1)) == 1);
	}

	/* The two thresholds are independent: a count between 8 and 11 is big
	 * enough for a vertex draw but too small for an indexed draw. */
	{
		int mid = vi(MINI_ARRAY_MIN_VERTICES);
		CHECK(mid < MINI_ARRAY_MIN_INDICES);
		CHECK(miniArrayEligible(vi(1), vi(1), mid, vi(0)) == 1);
		CHECK(miniArrayEligible(vi(1), vi(1), mid, vi(1)) == 0);
	}

	/* Zero / negative counts are never eligible. */
	{
		CHECK(miniArrayEligible(vi(1), vi(1), vi(0), vi(0)) == 0);
		CHECK(miniArrayEligible(vi(1), vi(1), vi(0), vi(1)) == 0);
		CHECK(miniArrayEligible(vi(1), vi(1), vi(-5), vi(0)) == 0);
	}

	/* Fan size guard: fans are drawn in one call, so only fans that fit the
	 * triangle scratch may use arrays; non-fans are always allowed. */
	{
		CHECK(miniArrayPrimFits(vi(0), vi(1)) == 1);
		CHECK(miniArrayPrimFits(vi(0), vi(MINI_ARRAY_TRI_CHUNK)) == 1);
		CHECK(miniArrayPrimFits(vi(0), vi(MINI_ARRAY_TRI_CHUNK + 1)) == 1);
		CHECK(miniArrayPrimFits(vi(1), vi(8)) == 1);
		CHECK(miniArrayPrimFits(vi(1), vi(MINI_ARRAY_TRI_CHUNK)) == 1);
		CHECK(miniArrayPrimFits(vi(1), vi(MINI_ARRAY_TRI_CHUNK + 1)) == 0);
	}

	/* Absolute-index budget (PiStorm array scratch): non-indexed uses
	 * 0..count-1, indexed uses the caller's maximum. */
	{
		CHECK(miniArrayIndicesFit(0, vi(0)) == 0);
		CHECK(miniArrayIndicesFit(0, vi(-1)) == 0);
		/* non-indexed: count-1 must be <= MINI_ARRAY_MAX_INDEX */
		CHECK(miniArrayIndicesFit(0, vi(MINI_ARRAY_MAX_INDEX)) == 1);
		CHECK(miniArrayIndicesFit(0, vi(MINI_ARRAY_MAX_INDEX + 1)) == 1);
		CHECK(miniArrayIndicesFit(0, vi(MINI_ARRAY_MAX_INDEX + 2)) == 0);

		unsigned short small[3] = { 0, 1, 2 };
		unsigned short edge[3] = { 5, (unsigned short)MINI_ARRAY_MAX_INDEX, 1 };
		unsigned short over[3] = { 0, (unsigned short)(MINI_ARRAY_MAX_INDEX + 1), 2 };
		CHECK(miniArrayIndicesFit(small, vi(3)) == 1);
		CHECK(miniArrayIndicesFit(edge, vi(3)) == 1);
		CHECK(miniArrayIndicesFit(over, vi(3)) == 0);

		/* The max is measured wherever it appears, not just the last slot. */
		unsigned short maxFirst[3] = { (unsigned short)(MINI_ARRAY_MAX_INDEX + 1), 0, 0 };
		CHECK(miniArrayIndicesFit(maxFirst, vi(3)) == 0);
	}

	/* Sequential index materialiser: content and return value. */
	{
		unsigned short buf[16];
		int n = vi(0);
		CHECK(miniSeqIndices(buf, n) == 0);

		n = vi(1);
		CHECK(miniSeqIndices(buf, n) == 1);
		CHECK(buf[0] == 0);

		n = vi(8);
		CHECK(miniSeqIndices(buf, n) == 8);
		int ok = 1;
		for (int i = 0; i < 8; i++)
			if (buf[i] != (unsigned short)i) ok = 0;
		CHECK(ok);

		n = vi(16);
		CHECK(miniSeqIndices(buf, n) == 16);
		ok = 1;
		for (int i = 0; i < 16; i++)
			if (buf[i] != (unsigned short)i) ok = 0;
		CHECK(ok);
	}

	/* Triangle-list chunk: cap is a multiple of 3 and never exceeds 4092. */
	{
		CHECK(miniTriChunkLen(vi(3), vi(0)) == 3);
		CHECK(miniTriChunkLen(vi(6), vi(0)) == 6);
		CHECK(miniTriChunkLen(vi(4092), vi(0)) == 4092);
		CHECK(miniTriChunkLen(vi(4093), vi(0)) == 4092);
		CHECK(miniTriChunkLen(vi(4095), vi(0)) == 4092);
		CHECK(miniTriChunkLen(vi(6000), vi(0)) == 4092);

		int rem = vi(6000) - vi(4092);
		CHECK(rem == 1908);
		CHECK(miniTriChunkLen(vi(6000), vi(4092)) == 1908);

		/* Non-multiple-of-3 remainder is truncated to a whole number of
		 * triangles (the immediate path chunks differently; this is the
		 * array path's documented behaviour, shared with drawIndexedChunks). */
		CHECK(miniTriChunkLen(vi(100), vi(0)) == 99);
		CHECK(miniTriChunkLen(vi(100), vi(99)) == 0);
		CHECK(miniTriChunkLen(vi(2), vi(0)) == 0);
	}

	/* Triangle-strip chunk: cap 1024, remainder passes through untouched. */
	{
		CHECK(miniStripChunkLen(vi(1), vi(0)) == 1);
		CHECK(miniStripChunkLen(vi(500), vi(0)) == 500);
		CHECK(miniStripChunkLen(vi(1024), vi(0)) == 1024);
		CHECK(miniStripChunkLen(vi(1025), vi(0)) == 1024);
		CHECK(miniStripChunkLen(vi(5000), vi(0)) == 1024);
		/* The cap still applies to later chunks; only a near-end start
		 * yields the (shorter) remainder. */
		CHECK(miniStripChunkLen(vi(5000), vi(1024)) == 1024);
		CHECK(miniStripChunkLen(vi(5000), vi(4100)) == 900);
		/* Seam overlap: the next chunk starts at cnt-2, so the last two
		 * verts of a full chunk are re-submitted in the next one. */
		CHECK(miniStripChunkLen(vi(5000), vi(1022)) == 1024);
	}

	/* Immediate-mode chunk: lists advance by cnt, strips by cnt-2 so the two
	 * vertices shared with the next chunk are re-submitted (seam), and a strip
	 * tail that cannot form a triangle stops. */
	{
		CHECK(miniImmediateChunkLen(vi(500), vi(0), vi(0)) == MINI_IMMEDIATE_CHUNK);
		CHECK(miniImmediateChunkLen(vi(500), vi(400), vi(0)) == 100);
		CHECK(miniImmediateChunkLen(vi(500), vi(400), vi(1)) == 100);
		CHECK(miniImmediateChunkLen(vi(500), vi(499), vi(0)) == 1);
		CHECK(miniImmediateChunkLen(vi(500), vi(499), vi(1)) == 0);
		CHECK(miniImmediateChunkNext(vi(0), vi(MINI_IMMEDIATE_CHUNK), vi(0)) == MINI_IMMEDIATE_CHUNK);
		CHECK(miniImmediateChunkNext(vi(0), vi(MINI_IMMEDIATE_CHUNK), vi(1)) == MINI_IMMEDIATE_CHUNK - 2);

		/* Every strip chunk must advance (no infinite loop) and the final
		 * chunk must reach the end. */
		int n = vi(1000);
		int first = vi(0);
		int chunks = 0, ok = 1;
		while (first < n) {
			int cnt = miniImmediateChunkLen(n, first, 1);
			if (cnt == 0) break;
			chunks++;
			if (first + cnt >= n) break;
			int next = miniImmediateChunkNext(first, cnt, 1);
			if (next <= first) { ok = 0; break; }
			first = next;
		}
		CHECK(ok == 1);
		CHECK(chunks > 1);
	}

	/* Chunk invariants across a range of sizes. */
	{
		int ok = 1, i;
		for (i = 0; i < 6000; i += 7) {
			int first = vi(i);
			int t = miniTriChunkLen(vi(6000), first);
			if (t > MINI_ARRAY_TRI_CHUNK) ok = 0;
			if (t < 0) ok = 0;
			if (t > 0 && (t % 3) != 0) ok = 0;
			int s = miniStripChunkLen(vi(6000), first);
			if (s > MINI_ARRAY_STRIP_CHUNK) ok = 0;
			if (i < 6000 && s < 1) ok = 0;
		}
		CHECK(ok);
	}

	/* Depth-state mapping: ztest wins, but zwrite without ztest must still
	 * write depth (CRadar::DrawRadarMask's "stamp a depth mask" state), so it
	 * maps to depth-test-on + GL_ALWAYS, not depth-test-off. */
	{
		CHECK(miniDepthMode(vi(1), vi(1)) == MINI_DEPTH_LEQUAL);
		CHECK(miniDepthMode(vi(1), vi(0)) == MINI_DEPTH_LEQUAL);
		CHECK(miniDepthMode(vi(0), vi(1)) == MINI_DEPTH_ALWAYS);
		CHECK(miniDepthMode(vi(0), vi(0)) == MINI_DEPTH_NONE);
		/* The radar mask state is exactly ztest=off, zwrite=on. */
		CHECK(miniDepthMode(vi(0), vi(1)) != MINI_DEPTH_NONE);
	}

	if (failures) {
		std::printf("miniarray_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("miniarray_test: all ok\n");
	return 0;
}
