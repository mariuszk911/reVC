#ifndef __GTA_MATRIX_GUARD_H__
#define __GTA_MATRIX_GUARD_H__

/*
 * Orthonormality guard for affine rotation bases that are written field by
 * field (e.g. `GetRight()/GetForward()/GetUp() = CrossProduct(...)`) instead of
 * being built by the CMatrix ops. Header-only and <math.h>-only, so the pure
 * logic can be unit-tested on the host (see tests/amigaos/matrix_guard_test.cpp).
 *
 * Why: a direct write of an un-normalised cross product produced a scaled,
 * non-orthonormal basis on a live entity (`yt_doors14`: r=(2010,-146,0),
 * f=(146,2010,0), u=(0,0,4061427) = |r|^2). `GetBoundCentre()` then multiplied
 * the collision sphere by it, `CPhysical::RemoveAndAdd`'s sector indices blew
 * past the array and libnix raised an exec memory alert (Guru #80000004). This
 * mirrors the librw `Matrix::rotate` zero/near-zero-axis fix: a degenerate or
 * absurdly scaled basis must be normalised before it can reach bounds/world
 * code, never divided by a zero-length axis.
 *
 * r, f, u each point at three contiguous floats (x, y, z). None of the helpers
 * allocates or calls float-returning `*f` libm: the length is taken with the
 * double `sqrt` (libnix libm881 on 68k), so they behave on the target ABI too.
 */

#include <math.h>

/* A rotation axis must have a plausible unit-ish length. Live reVC entity bases
 * are orthonormal and the few scaled ones sit near 0.4-1.0, so a magnitude far
 * outside this (deliberately wide) band is a corrupt direct write rather than a
 * legitimate model transform. Kept wide to avoid touching real geometry. */
#define MATRIXGUARD_AXIS_MIN 0.05f
#define MATRIXGUARD_AXIS_MAX 100.0f

/* Below this squared length an axis counts as degenerate (zero/parallel). */
#define MATRIXGUARD_DEGENERATE_SQR 1e-12f

static inline float
MatrixGuard_MagSqr3(const float *v)
{
	return v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
}

/* Finite and of plausible length for a rotation axis. NaN and +-Inf fail. */
static inline int
MatrixGuard_AxisIsSane(const float *v)
{
	float m2 = MatrixGuard_MagSqr3(v);
	return m2 >= MATRIXGUARD_AXIS_MIN*MATRIXGUARD_AXIS_MIN &&
	       m2 <= MATRIXGUARD_AXIS_MAX*MATRIXGUARD_AXIS_MAX;
}

/* True when the basis needs repair: any axis non-finite, zero/degenerate, or
 * grossly scaled (the `yt_doors14` signature). Cheap enough for a per-move
 * guard (`CPhysical::RemoveAndAdd`). */
static inline int
MatrixGuard_BasisNeedsRepair(const float *r, const float *f, const float *u)
{
	return !MatrixGuard_AxisIsSane(r) ||
	       !MatrixGuard_AxisIsSane(f) ||
	       !MatrixGuard_AxisIsSane(u);
}

static inline void
MatrixGuard_Cross3(float *out, const float *a, const float *b)
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

/* Normalise in place; returns 0 if the input was degenerate/non-finite and the
 * output was left at the given fallback, 1 if it produced a unit vector. */
static inline int
MatrixGuard_Normalise3(float *v, const float *fallback)
{
	float m2 = MatrixGuard_MagSqr3(v);
	if (m2 > MATRIXGUARD_DEGENERATE_SQR && m2 < (1.0e30f*1.0e30f)) {
		float inv = (float)(1.0 / sqrt((double)m2));
		v[0] *= inv; v[1] *= inv; v[2] *= inv;
		return 1;
	}
	v[0] = fallback[0]; v[1] = fallback[1]; v[2] = fallback[2];
	return 0;
}

/* Rebuild an orthonormal, right-handed basis from (r, f). Mirrors
 * CMatrix::Reorthogonalise but survives every degenerate input: zero/parallel
 * r and f, NaN/Inf, and gross scaling. `p` (position) is not touched. */
static inline void
MatrixGuard_Orthonormalise(float *r, float *f, float *u)
{
	static const float ex[3] = { 1.0f, 0.0f, 0.0f };
	static const float ey[3] = { 0.0f, 1.0f, 0.0f };
	static const float ez[3] = { 0.0f, 0.0f, 1.0f };
	const float *best;

	/* r -> unit (fallback +X). */
	MatrixGuard_Normalise3(r, ex);

	/* If f is degenerate or (near-)parallel to r, replace it with a vector
	 * guaranteed to be well off r: cross(r, world-axis least aligned to r). */
	MatrixGuard_Cross3(u, r, f);
	if (MatrixGuard_MagSqr3(u) <= MATRIXGUARD_DEGENERATE_SQR) {
		if (r[0]*r[0] <= r[1]*r[1] && r[0]*r[0] <= r[2]*r[2])
			best = ex;
		else if (r[1]*r[1] <= r[2]*r[2])
			best = ey;
		else
			best = ez;
		MatrixGuard_Cross3(f, r, best);
		MatrixGuard_Cross3(u, r, f);
	}

	/* u = r x f, unit (fallback +Z). */
	MatrixGuard_Normalise3(u, ez);
	/* r = f x u, unit (fallback +X). */
	MatrixGuard_Cross3(r, f, u);
	MatrixGuard_Normalise3(r, ex);
	/* f = u x r is unit by construction. */
	MatrixGuard_Cross3(f, u, r);
}

#endif /* __GTA_MATRIX_GUARD_H__ */
