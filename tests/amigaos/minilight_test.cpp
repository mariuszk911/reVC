/*
 * CPU per-vertex lighting unit test (vendor/librw/src/mini/minilight.h).
 *
 * The MINI backend has no lighting path, so LIGHT-without-prelight geometry
 * (GTA:VC buildings) is shaded on the CPU, mirroring librw's GL3 default vertex
 * shader:
 *     color = clamp(base + ambient*surfAmbient
 *                       + sum(dir) max(0,dot(N,-dir))*lightColor*surfDiffuse)
 * minilight.h is the extracted, dependency-free copy included by minipipe.cpp
 * and here, so this exercises the exact production math.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:minilight.log. Assert on the numeric value only (no %f/%g: broken in
 * this libnix and CXX tests are not linked with libfix_math.o).
 */

#include <cstdio>

#include "minilight.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

static float vf(float v) { volatile float t = v; return t; }

static double dabs(double x) { return x < 0.0 ? -x : x; }

static bool
close3(const float *out, double a, double b, double c)
{
	return dabs(out[0]-a) < 1e-5 && dabs(out[1]-b) < 1e-5 && dabs(out[2]-c) < 1e-5;
}

int
main(void)
{
#ifdef AMIGAOS
	freopen("DH11:minilight.log", "w", stdout);
#endif

	/* Ambient only: colour is the ambient term. */
	{
		MiniLightState ls = { 0.5f, 0.25f, 0.1f, 0 };
		float n[3] = { 0.0f, 0.0f, 1.0f };
		float base[3] = { 0.0f, 0.0f, 0.0f };
		float out[3];
		miniLightVertex(n, &ls, 1.0f, 1.0f, base, out);
		CHECK(close3(out, 0.5, 0.25, 0.1));
	}

	/* Directional facing the surface: ndl = 1, adds the full colour. */
	{
		MiniLightState ls = { 0.0f, 0.0f, 0.0f, 1 };
		float n[3] = { 0.0f, 0.0f, 1.0f };
		float base[3] = { 0.0f, 0.0f, 0.0f };
		float out[3];
		/* shines along -z, so the direction to the light is +z. */
		ls.dir[0].dx = vf(0.0f); ls.dir[0].dy = vf(0.0f); ls.dir[0].dz = vf(-1.0f);
		ls.dir[0].r = vf(1.0f); ls.dir[0].g = vf(0.5f); ls.dir[0].b = vf(0.0f);
		miniLightVertex(n, &ls, 1.0f, 1.0f, base, out);
		CHECK(close3(out, 1.0, 0.5, 0.0));
	}

	/* Directional behind the surface: max(0,...) drops it, ambient remains. */
	{
		MiniLightState ls = { 0.3f, 0.3f, 0.3f, 1 };
		float n[3] = { 0.0f, 0.0f, 1.0f };
		float base[3] = { 0.0f, 0.0f, 0.0f };
		float out[3];
		ls.dir[0].dx = vf(0.0f); ls.dir[0].dy = vf(0.0f); ls.dir[0].dz = vf(1.0f);
		ls.dir[0].r = vf(1.0f); ls.dir[0].g = vf(1.0f); ls.dir[0].b = vf(1.0f);
		miniLightVertex(n, &ls, 1.0f, 1.0f, base, out);
		CHECK(close3(out, 0.3, 0.3, 0.3));
	}

	/* Clamp: ambient + full directional saturates to 1. */
	{
		MiniLightState ls = { 0.8f, 0.8f, 0.8f, 1 };
		float n[3] = { 0.0f, 0.0f, 1.0f };
		float base[3] = { 0.0f, 0.0f, 0.0f };
		float out[3];
		ls.dir[0].dx = vf(0.0f); ls.dir[0].dy = vf(0.0f); ls.dir[0].dz = vf(-1.0f);
		ls.dir[0].r = vf(1.0f); ls.dir[0].g = vf(1.0f); ls.dir[0].b = vf(1.0f);
		miniLightVertex(n, &ls, 1.0f, 1.0f, base, out);
		CHECK(close3(out, 1.0, 1.0, 1.0));
	}

	/* surfAmbient/surfDiffuse scale their terms independently. */
	{
		MiniLightState ls = { 0.5f, 0.5f, 0.5f, 1 };
		float n[3] = { 0.0f, 0.0f, 1.0f };
		float base[3] = { 0.0f, 0.0f, 0.0f };
		float out[3];
		ls.dir[0].dx = vf(0.0f); ls.dir[0].dy = vf(0.0f); ls.dir[0].dz = vf(-1.0f);
		ls.dir[0].r = vf(1.0f); ls.dir[0].g = vf(1.0f); ls.dir[0].b = vf(1.0f);
		miniLightVertex(n, &ls, 0.5f, 0.5f, base, out);
		CHECK(close3(out, 0.25+0.5, 0.25+0.5, 0.25+0.5));
	}

	/* Prelit base colour is added before ambient/dynamic. */
	{
		MiniLightState ls = { 0.1f, 0.2f, 0.3f, 0 };
		float n[3] = { 0.0f, 0.0f, 1.0f };
		float base[3] = { 0.2f, 0.2f, 0.2f };
		float out[3];
		miniLightVertex(n, &ls, 1.0f, 1.0f, base, out);
		CHECK(close3(out, 0.3, 0.4, 0.5));
	}

	/* Two directionals sum; a 45-degree normal gets ndl = cos45. */
	{
		MiniLightState ls = { 0.0f, 0.0f, 0.0f, 2 };
		float s = vf(0.70710678f);
		float n[3] = { s, s, 0.0f };
		float base[3] = { 0.0f, 0.0f, 0.0f };
		float out[3];
		ls.dir[0].dx = vf(-1.0f); ls.dir[0].dy = vf(0.0f); ls.dir[0].dz = vf(0.0f);
		ls.dir[0].r = vf(1.0f); ls.dir[0].g = vf(0.0f); ls.dir[0].b = vf(0.0f);
		ls.dir[1].dx = vf(0.0f); ls.dir[1].dy = vf(-1.0f); ls.dir[1].dz = vf(0.0f);
		ls.dir[1].r = vf(0.0f); ls.dir[1].g = vf(1.0f); ls.dir[1].b = vf(0.0f);
		miniLightVertex(n, &ls, 1.0f, 1.0f, base, out);
		CHECK(dabs(out[0]-0.70710678) < 1e-4 && dabs(out[1]-0.70710678) < 1e-4 && out[2] == 0.0f);
	}

	if (failures) {
		std::printf("minilight_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("minilight_test: all ok\n");
	return 0;
}
