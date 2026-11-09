/*
 * librw Matrix::makeRotation edge-case test (68k FPU / libnix float ABI).
 *
 * Migrated from the ad-hoc build/amigaos/matrix_test.c (the stray copy was
 * deleted from build/); it exercises the *fixed* makeRotation semantics:
 * a degenerate/near-zero axis returns identity instead of normalising a tiny
 * axis (sqrtf of denormals is unreliable on this libm). The rotation body is a
 * verbatim copy of the current vendor/librw/src/base.cpp implementation, since
 * librw cannot be linked standalone here.
 *
 * This is a platform test, not a logic test: it catches float garbage from the
 * broken soft-float libm (see floatmath_test.c and src/skel/amigaos/
 * amiga_libfix.c). Inputs are volatile so gcc cannot constant-fold the calls.
 *
 * Cross-compiled into build/amigaos/test (DH11:) and run on the target; writes
 * DH11:matrix_test.log. %g is broken in this libnix, so only %f is used.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#include "AngleWrap.h"

#define LOGFILE "DH11:matrix_test.log"

typedef struct { float x, y, z; } V3d;

typedef struct {
	V3d right;
	uint32_t flags;
	V3d up;
	uint32_t pad1;
	V3d at;
	uint32_t pad2;
	V3d pos;
	uint32_t pad3;
} Matrix;

static float vf(float v) { volatile float t = v; return t; }

static float dotv(V3d a, V3d b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

static V3d scalev(V3d a, float r)
{
	V3d v;
	v.x = a.x*r; v.y = a.y*r; v.z = a.z*r;
	return v;
}

/* Verbatim port of vendor/librw/src/base.cpp Matrix::makeRotation (fixed). */
static void makeRotation(Matrix *dst, V3d axis, float angle)
{
	float len = dotv(axis, axis);
	V3d v;
	float s, c, t;

	if (len < 1e-12f) {
		/* Degenerate/near-zero axis: identity, not a scale-by-cos. */
		dst->right.x = 1.0f; dst->right.y = 0.0f; dst->right.z = 0.0f; dst->flags = 3;
		dst->up.x    = 0.0f; dst->up.y    = 1.0f; dst->up.z    = 0.0f;
		dst->at.x    = 0.0f; dst->at.y    = 0.0f; dst->at.z    = 1.0f;
		return;
	}
	len = 1.0f/sqrtf(len);
	v = scalev(axis, len);
	angle = angle*(float)3.14159265358979323846/180.0f;
	s = sinf(angle);
	c = cosf(angle);
	t = 1.0f - c;
	dst->right.x = c + v.x*v.x*t; dst->right.y = v.x*v.y*t + v.z*s; dst->right.z = v.z*v.x*t - v.y*s;
	dst->up.x    = v.x*v.y*t - v.z*s; dst->up.y = c + v.y*v.y*t;    dst->up.z = v.y*v.z*t + v.x*s;
	dst->at.x    = v.z*v.x*t + v.y*s; dst->at.y = v.y*v.z*t - v.x*s; dst->at.z = c + v.z*v.z*t;
	dst->flags = 3;
}

static int isIdentity(const Matrix *m)
{
	static const float e = 1e-4f;
	return fabsf(m->right.x-1.0f) < e && fabsf(m->right.y) < e && fabsf(m->right.z) < e &&
	       fabsf(m->up.y-1.0f) < e    && fabsf(m->up.x) < e    && fabsf(m->up.z) < e &&
	       fabsf(m->at.z-1.0f) < e    && fabsf(m->at.x) < e    && fabsf(m->at.y) < e;
}

static int isGarbage(const Matrix *m)
{
	static const float thresh = 1000.0f;
	return !(fabsf(m->right.x) < thresh && fabsf(m->right.y) < thresh && fabsf(m->right.z) < thresh &&
	         fabsf(m->up.x)    < thresh && fabsf(m->up.y)    < thresh && fabsf(m->up.z)    < thresh &&
	         fabsf(m->at.x)    < thresh && fabsf(m->at.y)    < thresh && fabsf(m->at.z)    < thresh);
}

int main(void)
{
	int fails = 0;

#ifdef AMIGAOS
	freopen(LOGFILE, "w", stdout);
#endif
	printf("librw Matrix::makeRotation edge-case test\n\n");

	/* Sanity: the fixed libm must report correct values. Use a non-trivial
	 * argument (0.5) so a missing/broken libfix override cannot pass by luck. */
	{
		int ok = 1;
		if (fabsf(sinf(vf(0.0f))) > 0.001f) ok = 0;
		if (fabsf(cosf(vf(0.0f)) - 1.0f) > 0.001f) ok = 0;
		if (fabsf(sinf(vf(0.5f)) - 0.4794255f) > 0.001f) ok = 0;
		if (fabsf(cosf(vf(0.5f)) - 0.8775826f) > 0.001f) ok = 0;
		if (fabsf(sqrtf(vf(4.0f)) - 2.0f) > 0.001f) ok = 0;
		printf("float sanity: %s\n", ok ? "ok" : "FAIL");
		if (!ok) fails++;
	}

	/* Degenerate axes must yield identity (the librw zero-axis fix). */
	{
		V3d degenerate[] = {
			{ 0.0f, 0.0f, 0.0f },
			{ 1e-30f, 1e-30f, 1e-30f },
			{ 0.0f, 0.0f, 0.0f },
		};
		float angles[] = { 0.0f, 90.0f, 1e9f };
		int i;
		printf("\ndegenerate axis -> identity\n");
		for (i = 0; i < 3; i++) {
			Matrix m;
			V3d a;
			a.x = vf(degenerate[i].x); a.y = vf(degenerate[i].y); a.z = vf(degenerate[i].z);
			makeRotation(&m, a, vf(angles[i]));
			if (!isIdentity(&m)) {
				fails++;
				printf("FAIL axis#%d angle=%f not identity r=(%f,%f,%f)\n",
				       i, (double)angles[i],
				       (double)m.right.x, (double)m.right.y, (double)m.right.z);
			} else {
				printf("ok   axis#%d angle=%f\n", i, (double)angles[i]);
			}
		}
	}

	/* Non-degenerate axes/angles must produce a finite rotation, including
	 * huge axes (normalised). */
	{
		V3d axes[] = {
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f },
			{ 0.57735f, 0.57735f, 0.57735f },
			{ 1e30f, 1e30f, 1e30f },
		};
		float angles[] = { 0.0f, 45.0f, 90.0f, 180.0f, 270.0f, 360.0f, 4.15f };
		int a, g;
		printf("\nnon-degenerate -> finite rotation\n");
		for (a = 0; a < 5; a++) {
			for (g = 0; g < 7; g++) {
				Matrix m;
				V3d ax;
				int bad;
				ax.x = vf(axes[a].x); ax.y = vf(axes[a].y); ax.z = vf(axes[a].z);
				makeRotation(&m, ax, vf(angles[g]));
				bad = isGarbage(&m);
				if (bad) {
					fails++;
					printf("FAIL axis#%d angle=%f r=(%f,%f,%f) u=(%f,%f,%f) a=(%f,%f,%f)\n",
					       a, (double)angles[g],
					       (double)m.right.x, (double)m.right.y, (double)m.right.z,
					       (double)m.up.x, (double)m.up.y, (double)m.up.z,
					       (double)m.at.x, (double)m.at.y, (double)m.at.z);
				}
			}
		}
		printf("checked 5 axes x 7 angles, %s\n", fails ? "garbage found" : "all finite");
	}

	/* Large unwrapped angles: this libm's sinf/cosf lose argument reduction
	 * and return garbage (observed ~1e5 on target for 1e6+ degrees), which is
	 * why the engine wraps every angle with WrapRadianAngle before it reaches
	 * sin/cos. Only the wrapped result is asserted finite. The raw value is
	 * printed for evidence and is not a failure (it is correct on host). */
	{
		float big[] = { 1e6f, 1e9f, 1e12f };
		int i;
		printf("\nlarge unwrapped angle -> wrap first\n");
		for (i = 0; i < 3; i++) {
			float deg = vf(big[i]);
			float rad = vf(deg * PI / 180.0f);
			float raw = sinf(rad);
			volatile double drad = (double)rad;
			double dsin = sin(drad);
			float wrapped = sinf(WrapRadianAngle(rad));
			printf("angle=%f raw_sinf=%f raw_sin_double=%f wrapped_sinf=%f\n",
			       (double)deg, (double)raw, (double)dsin, (double)wrapped);
			if (!(wrapped > -1.001f && wrapped < 1.001f)) {
				fails++;
				printf("FAIL wrapped sinf out of range for angle=%f\n", (double)deg);
			}
		}
	}

	if (fails) {
		printf("\nmatrix_test: %d failure(s)\n", fails);
		return 1;
	}
	printf("\nmatrix_test: all ok\n");
	return 0;
}
