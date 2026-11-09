/*
 * Standalone 68k unit test for the libnix float-return ABI bug.
 *
 * On this toolchain, -mhard-float code returns float values in fp0, but the
 * float math functions (sqrtf/cosf/sinf/...) are pulled from the soft-float
 * generic libm020/lib__m__.a and return in d0. Every float call therefore
 * reads stale fp0 -> garbage. Double functions come from libnix's FPU
 * libm881 and are correct.
 *
 * Part of the target suite: cross-built with libfix_math.o, logs
 * DH11:floatmath_test.log and ends "floatmath_test: all ok" (or N failure(s)).
 * The negative control is built separately by build_floatmath.sh:
 *   floatmath_buggy: just this file + -lm            -> expect WRONG lines
 *   floatmath_fixed: this file + libfix_math.o -lm   -> expect all ok
 *
 * Inputs are volatile so gcc cannot constant-fold the calls (that is exactly
 * why the earlier matrix_test "HARDCHECK" falsely passed).
 *
 * On the target the log goes to DH11: (build/amigaos/test on the host); on the
 * host (no AMIGAOS) it prints to stdout so `make run` exercises it too.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef LOGFILE
#define LOGFILE "DH11:floatmath_test.log"
#endif

static int failures;

static void checkf(const char *what, float got, float want)
{
	float d = got - want;
	int ok = d < 1e-3f && d > -1e-3f;

	printf("%-16s = %14.6f  want %14.6f  %s\n", what, (double)got,
	       (double)want, ok ? "ok" : "WRONG");
	if (!ok)
		failures++;
}

static void checkd(const char *what, double got, double want)
{
	double d = got - want;
	int ok = d < 1e-6 && d > -1e-6;

	printf("%-16s = %14.6f  want %14.6f  %s\n", what, got, want,
	       ok ? "ok" : "WRONG");
	if (!ok)
		failures++;
}

/* Seeds gcc cannot fold. */
static volatile float half = 0.5f;
static volatile float two = 2.0f;
static volatile float nine = 9.0f;
static volatile float sevenfive = 7.5f;
static volatile float ten = 10.0f;
static volatile double dhalf = 0.5;

/* librw Matrix::makeRotation, verbatim apart from the name. */
typedef struct { float x, y, z; } V3d;
typedef struct {
	V3d right; unsigned flags; V3d up; unsigned p1; V3d at; unsigned p2; V3d pos; unsigned p3;
} Matrix;

static float dotv(V3d a, V3d b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

static void makeRotation(Matrix *m, V3d axis, float angle)
{
	float len = dotv(axis, axis);
	V3d v;
	float s, c, t;

	if (len != 0.0f)
		len = 1.0f / sqrtf(len);
	v.x = axis.x * len; v.y = axis.y * len; v.z = axis.z * len;
	angle = angle * 3.14159265358979323846f / 180.0f;
	s = sinf(angle);
	c = cosf(angle);
	t = 1.0f - c;
	m->right.x = c + v.x*v.x*t; m->right.y = v.x*v.y*t + v.z*s; m->right.z = v.z*v.x*t - v.y*s;
	m->up.x    = v.x*v.y*t - v.z*s;  m->up.y = c + v.y*v.y*t;    m->up.z = v.y*v.z*t + v.x*s;
	m->at.x    = v.z*v.x*t + v.y*s;  m->at.y = v.y*v.z*t - v.x*s; m->at.z = c + v.z*v.z*t;
}

static int matrixGarbage(const Matrix *m)
{
	float q[9];
	int i;
	q[0]=m->right.x; q[1]=m->right.y; q[2]=m->right.z;
	q[3]=m->up.x;    q[4]=m->up.y;    q[5]=m->up.z;
	q[6]=m->at.x;    q[7]=m->at.y;    q[8]=m->at.z;
	for (i = 0; i < 9; i++)
		if (!(q[i] > -1000.0f && q[i] < 1000.0f))
			return 1;
	return 0;
}

int main(void)
{
	float in = half;
	V3d axis;
	Matrix m;

#ifdef AMIGAOS
	freopen(LOGFILE, "w", stdout);
#endif

	printf("float ABI test  (calls into libm, volatile args)\n\n");

	/* The failing set: float-returning libm calls. */
	checkf("sinf(0.5)",  sinf(in),       0.4794255f);
	checkf("cosf(0.5)",  cosf(in),       0.8775826f);
	checkf("sqrtf(9)",   sqrtf(nine),    3.0000000f);
	checkf("atan2f(1,1)",atan2f(two/two, two/two), 0.7853982f);
	checkf("asinf(0.5)", asinf(in),      0.5235988f);
	checkf("acosf(0.5)", acosf(in),      1.0471976f);
	checkf("tanf(0.5)",  tanf(in),       0.5463025f);
	checkf("fmodf",      fmodf(sevenfive, two), 1.5000000f);
	checkf("powf(2,10)", powf(two, ten), 1024.000000f);

	/* Control: double functions use libnix libm881 (fp0) and are correct. */
	printf("\ncontrol (double)\n");
	checkd("sin(0.5)", sin(dhalf), 0.4794255);
	checkd("cos(0.5)", cos(dhalf), 0.8775826);
	checkd("sqrt(9)",  sqrt(9.0),  3.0000000);

	/* Real impact: rotation of the x axis by 90 deg. The axis is (2,0,0),
	 * so 1/sqrtf(4) must be 0.5; if sqrtf returns garbage the normalized
	 * axis and the whole matrix are wrong. */
	printf("\nMatrix::makeRotation(axis 2,0,0, 90 deg)\n");
	axis.x = 2.0f; axis.y = 0.0f; axis.z = 0.0f;
	makeRotation(&m, axis, 90.0f);
	checkf("right.x", m.right.x, 1.0f);
	checkf("at.x",    m.at.x,    0.0f);
	checkf("at.y",    m.at.y,   -1.0f);
	checkf("up.z",    m.up.z,    1.0f);
	if (matrixGarbage(&m))
		failures++;

	printf("\n%s: %d wrong\n", failures ? "FAILED" : "PASSED", failures);
	if (failures)
		printf("floatmath_test: %d failure(s)\n", failures);
	else
		printf("floatmath_test: all ok\n");
	fflush(stdout);
	return failures ? 1 : 0;
}
