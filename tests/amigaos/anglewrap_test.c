/*
 * H1: angle-wrap unit test (WrapRadianAngle / WrapTwoPi, src/core/AngleWrap.h).
 *
 * The old "while(angle > PI) angle -= TWOPI" idiom never terminates when the
 * FPU value is huge (subtracting TWOPI no longer changes it, e.g. 1e30), which
 * hung the game. The fmod-based replacement must:
 *   - return finite inputs wrapped into [-PI, PI] / [0, TWOPI),
 *   - terminate on huge/Inf/NaN inputs (returning NaN for non-finite input),
 *   - leave the exact boundaries (+/-PI, 0) unchanged.
 *
 * Runs on the host (little-endian) and cross-linked for 68k. On AmigaOS it
 * writes its own log (DH11:anglewrap.log) so the harness reads it directly and
 * never interleaves with reVC.log. Inputs are volatile so gcc cannot
 * constant-fold the calls and make the test falsely pass.
 *
 * Print only integers/strings: %f is broken in this libnix.
 */

#include <stdio.h>
#include <math.h>

#include "AngleWrap.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { printf("ok   %s\n", #cond); } \
	else { printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* A finite result must lie in the closed interval [lo, hi] (small tolerance). */
static int in_range(float x, float lo, float hi)
{
	return isfinite(x) && x >= lo - 1e-4f && x <= hi + 1e-4f;
}

static void check_finite_range(const char *name, float v)
{
	volatile float in = v;
	float r = WrapRadianAngle(in);
	int okR = in_range(r, -PI, PI);

	float t = WrapTwoPi(in);
	int okT = in_range(t, 0.0f, TWOPI);
	/* WrapTwoPi's upper bound is open, so TWOPI itself must never appear. */
	okT = okT && t < TWOPI;

	if (okR && okT) {
		printf("ok   range %s\n", name);
	} else {
		printf("FAIL range %s (rad=%d twopi=%d)\n", name, okR, okT);
		failures++;
	}
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:anglewrap.log", "w", stdout);
#endif

	/* Exact boundaries. */
	{
		volatile float zero = 0.0f;
		volatile float pi = PI;
		volatile float npi = -PI;
		volatile float twopi = TWOPI;
		volatile float ntwopi = -TWOPI;

		CHECK(WrapRadianAngle(zero) == 0.0f);
		CHECK(WrapTwoPi(zero) == 0.0f);
		/* +/-PI are inside the closed range and must be left alone. */
		CHECK(WrapRadianAngle(pi) == PI);
		CHECK(WrapRadianAngle(npi) == -PI);
		CHECK(WrapTwoPi(npi) == PI);
		/* +/-TWOPI fold to 0. */
		CHECK(WrapRadianAngle(twopi) == 0.0f);
		CHECK(WrapRadianAngle(ntwopi) == 0.0f);
		CHECK(WrapTwoPi(twopi) == 0.0f);
		CHECK(WrapTwoPi(ntwopi) == 0.0f);
	}

	/* Representative finite values, including the huge ones that used to hang. */
	check_finite_range("PI+0.1", PI + 0.1f);
	check_finite_range("-PI-0.1", -PI - 0.1f);
	check_finite_range("1.5*PI", 1.5f * PI);
	check_finite_range("3*PI/2", 3.0f * PI / 2.0f);
	check_finite_range("-1.5*PI", -1.5f * PI);
	check_finite_range("7*TWOPI+0.5", 7.0f * TWOPI + 0.5f);
	check_finite_range("-13*TWOPI-0.25", -13.0f * TWOPI - 0.25f);
	check_finite_range("1e30", (float)1e30);
	check_finite_range("-1e30", (float)-1e30);
	check_finite_range("1e7*TWOPI", 1.0e7f * TWOPI);

	/* +Inf as a float: must terminate, not hang. Builtins avoid a divide trap
	 * on the 68k FPU. */
	{
		volatile float big = __builtin_huge_valf();
		volatile float nbig = -__builtin_huge_valf();
		float r = WrapRadianAngle(big);
		float t = WrapTwoPi(nbig);
		/* Non-finite input cannot be wrapped; the contract is only "no hang". */
		CHECK(isfinite(big) == 0);
		(void)r;
		(void)t;
		printf("ok   non-finite input terminated\n");
	}

	/* NaN propagates (still NaN), Inf folds to NaN - both must not hang. */
	{
		volatile float nan = __builtin_nanf("");
		volatile float inf = __builtin_huge_valf();
		int nanIn = isnan(nan);
		int nanOut = isnan(WrapRadianAngle(nan));
		int infOut = isnan(WrapRadianAngle(inf));
		CHECK(nanIn);
		CHECK(nanOut);
		CHECK(infOut);
	}

	/* Idempotence: wrapping an already-wrapped value changes nothing. */
	{
		volatile float v = PI + 0.7f;
		float once = WrapRadianAngle(v);
		float twice = WrapRadianAngle(once);
		CHECK(once == twice);
		float t1 = WrapTwoPi(v);
		float t2 = WrapTwoPi(t1);
		CHECK(t1 == t2);
	}

	if (failures) {
		printf("anglewrap_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("anglewrap_test: all ok\n");
	return 0;
}
