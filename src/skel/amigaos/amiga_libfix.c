/*
 * Workarounds for the AmigaOS 68k C library (libnix / bebbo's amiga-gcc).
 *
 * This file must be compiled with -fno-builtin (see build/amigaos/build.py):
 * without it gcc turns "(float)sin(x)" straight back into a call to sinf,
 * i.e. into infinite recursion.
 *
 * Float math. The toolchain's generic libm float functions (sqrtf, cosf,
 * sinf, atan2f, fmodf, ...) are soft-float code that returns the result in
 * d0, while every -mhard-float caller reads the result from fp0 and gets
 * whatever was left there. libnix only ships the double FPU functions
 * (libm881), so the *f calls fall through to the broken library. Symptoms
 * are garbage/stale values in anything that normalises or rotates: the 3D
 * view rotated by 90 degrees on the RTCW port, and here the entity/frame
 * matrices (librw Matrix::rotate) went to 1.59e15.
 *
 * Each float function is routed through the double version, which returns in
 * fp0. sqrtf goes through __builtin_sqrt so it is not turned into a recursive
 * sqrtf call. Only *f whose backing double is hard-float (libm881) is provided;
 * omitting one is deliberate -- routing it through a soft lib__m__ double is the
 * same bug one level down (see round/copysign). verify-libfix guards this.
 *
 * See tests/amigaos/floatmath_test.c: the current link reports 5 WRONG, and
 * the same test with these overrides reports 0 WRONG.
 */
#include <math.h>
#include <string.h>

/* log2/log2f are macros in this libnix <math.h>. */
#undef log2
#undef log2f

/* ---- basic ------------------------------------------------------------- */
float sqrtf(float x)           { return (float)__builtin_sqrt((double)x); }
float sinf(float x)            { return (float)sin(x); }
float cosf(float x)            { return (float)cos(x); }
float tanf(float x)            { return (float)tan(x); }
float asinf(float x)           { return (float)asin(x); }
float acosf(float x)           { return (float)acos(x); }
float atanf(float x)           { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float floorf(float x)          { return (float)floor(x); }
float ceilf(float x)           { return (float)ceil(x); }
float rintf(float x)           { return (float)rint(x); }
float fmodf(float x, float y)  { return (float)fmod(x, y); }
float powf(float x, float y)   { return (float)pow(x, y); }
float expf(float x)            { return (float)exp(x); }
float logf(float x)            { return (float)log(x); }
float log10f(float x)          { return (float)log10(x); }
float log2f(float x)           { return (float)log2((double)x); }
double log2(double x)          { return log(x) / 0.69314718055994530942; }

/* ---- rounding / sign --------------------------------------------------- */
/* libm881 lacks round() and copysign(), so those fall back to lib__m__ (soft
 * ABI: result in d0, hard-float callers read fp0). Define them here. */
double round(double x)
{
	return trunc(x + copysign(0.5, x));
}
double copysign(double x, double y)
{
	unsigned int ux[2], uy[2];
	memcpy(ux, &x, sizeof(ux));
	memcpy(uy, &y, sizeof(uy));
	ux[0] = (ux[0] & 0x7fffffffu) | (uy[0] & 0x80000000u);
	memcpy(&x, ux, sizeof(x));
	return x;
}
float roundf(float x)          { return (float)round(x); }
float truncf(float x)          { return (float)trunc(x); }
float copysignf(float x, float y) { return (float)copysign(x, y); }

/* fabsf must clear the sign bit: the naive `x < 0 ? -x : x` returns -0.0 for
 * -0.0 and leaves a NaN's sign set. Bit-clear instead (no libm call, exact). */
float fabsf(float x)
{
	unsigned int u;
	memcpy(&u, &x, sizeof(u));
	u &= 0x7fffffffu;
	memcpy(&x, &u, sizeof(x));
	return x;
}

/* ---- hyperbolic -------------------------------------------------------- */
float sinhf(float x)           { return (float)sinh(x); }
float coshf(float x)           { return (float)cosh(x); }
float tanhf(float x)           { return (float)tanh(x); }

/* ---- power / root / misc ---------------------------------------------- */
float ldexpf(float x, int e)   { return (float)ldexp(x, e); }

/* fmaxf/fminf: plain FPU compares, no libm (C99 NaN rules: a quiet NaN loses). */
float fmaxf(float x, float y)  { return x > y ? x : (x != x ? y : (y != y ? x : y)); }
float fminf(float x, float y)  { return x < y ? x : (x != x ? y : (y != y ? x : y)); }

/* modff/frexpf take an out-parameter. */
float modff(float x, float *ip)
{
	double t = trunc((double)x);
	*ip = (float)t;
	return (float)((double)x - t);
}

float frexpf(float x, int *e)
{
	int de;
	double dm = frexp((double)x, &de);
	*e = de;
	return (float)dm;
}
