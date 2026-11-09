/*
 * Item 3: orthonormality guard for field-wise-written rotation bases
 * (src/core/MatrixGuard.h), used by CMatrix::Reorthogonalise/RepairIfBroken
 * and by CPhysical::RemoveAndAdd to stop the yt_doors14 signature (an
 * un-normalised CrossProduct basis) from blowing the sector indices.
 *
 * The header is dependency-free and shared by production and this test, so the
 * numeric behaviour checked here is exactly what ships. Uses double reference
 * math only; no float-returning libm calls (CXX target tests are not linked
 * with libfix_math.o, and %g/%f formatting is unreliable in this libnix).
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:matrix_guard.log.
 */

#include <cstdio>

#include "MatrixGuard.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Defeat constant folding so the numbers really flow through the header. */
static float vf(float v) { volatile float t = v; return t; }

static double dmag2(const float *v)
{
	double x = v[0], y = v[1], z = v[2];
	return x*x + y*y + z*z;
}

static double ddot(const float *a, const float *b)
{
	return (double)a[0]*b[0] + (double)a[1]*b[1] + (double)a[2]*b[2];
}

static double ddet(const float *r, const float *f, const float *u)
{
	double rx = r[0], ry = r[1], rz = r[2];
	double fx = f[0], fy = f[1], fz = f[2];
	double ux = u[0], uy = u[1], uz = u[2];
	return rx*(fy*uz - fz*uy) - ry*(fx*uz - fz*ux) + rz*(fx*uy - fy*ux);
}

/* Unit length and mutually perpendicular, within tol. */
static int isOrthonormal(const float *r, const float *f, const float *u, double tol)
{
	double mr = dmag2(r), mf = dmag2(f), mu = dmag2(u);
	if (mr < 1.0 - tol || mr > 1.0 + tol) return 0;
	if (mf < 1.0 - tol || mf > 1.0 + tol) return 0;
	if (mu < 1.0 - tol || mu > 1.0 + tol) return 0;
	if (ddot(r, f) > tol || ddot(r, f) < -tol) return 0;
	if (ddot(r, u) > tol || ddot(r, u) < -tol) return 0;
	if (ddot(f, u) > tol || ddot(f, u) < -tol) return 0;
	return 1;
}

static void set3(float *v, float x, float y, float z)
{
	v[0] = vf(x); v[1] = vf(y); v[2] = vf(z);
}

/* Reproduce the exact corrupt basis captured for yt_doors14:
 * r/f are rotZ(-4.15 deg) scaled by ~2015, u = r x f is un-normalised (|r|^2). */
static void makeBrokenYachtBasis(float *r, float *f, float *u)
{
	set3(r,  2010.0f, -146.0f, 0.0f);
	set3(f,   146.0f, 2010.0f, 0.0f);
	set3(u,     0.0f,    0.0f, 4061427.0f);
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:matrix_guard.log", "w", stdout);
#endif

	std::printf("matrix-basis guard test\n\n");

	/* The real corruption: repairs to a bounded orthonormal basis. */
	{
		float r[3], f[3], u[3];
		makeBrokenYachtBasis(r, f, u);
		CHECK(MatrixGuard_BasisNeedsRepair(r, f, u));
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));
		CHECK(ddet(r, f, u) > 0.99 && ddet(r, f, u) < 1.01);
	}

	/* A normal rotation basis (the repaired yacht basis) must be left alone. */
	{
		float r[3], f[3], u[3];
		set3(r, 0.9975f, -0.0725f, 0.0f);
		set3(f, 0.0725f,  0.9975f, 0.0f);
		set3(u, 0.0f,      0.0f,   1.0f);
		CHECK(!MatrixGuard_BasisNeedsRepair(r, f, u));
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));
	}

	/* A legitimately scaled frame (~0.416, seen in DFF data) is in-band and
	 * must not be touched -- widening the guard must not rescale real models. */
	{
		float r[3], f[3], u[3];
		set3(r, 0.416f, 0.0f, 0.0f);
		set3(f, 0.0f, 0.416f, 0.0f);
		set3(u, 0.0f, 0.0f, 0.416f);
		CHECK(!MatrixGuard_BasisNeedsRepair(r, f, u));
	}

	/* Parallel axes (cross product is exactly zero): the algorithm itself must
	 * still emit an orthonormal basis, not divide by a zero axis. */
	{
		float r[3], f[3], u[3];
		set3(r, 1.0f, 0.0f, 0.0f);
		set3(f, 1.0f, 0.0f, 0.0f);
		set3(u, 0.0f, 0.0f, 1.0f);
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));
	}

	/* Zero axis: needs repair (degenerate) and repairs to a valid basis. */
	{
		float r[3], f[3], u[3];
		set3(r, 0.0f, 0.0f, 0.0f);
		set3(f, 0.0f, 1.0f, 0.0f);
		set3(u, 0.0f, 0.0f, 1.0f);
		CHECK(MatrixGuard_BasisNeedsRepair(r, f, u));
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));
	}

	/* NaN and Inf axes are flagged and recovered. */
	{
		float r[3], f[3], u[3];
		set3(r, 0.0f/0.0f, 0.0f, 0.0f);
		set3(f, 0.0f, 1.0f, 0.0f);
		set3(u, 0.0f, 0.0f, 1.0f);
		CHECK(MatrixGuard_BasisNeedsRepair(r, f, u));
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));

		set3(r, 1.0f, 0.0f, 0.0f);
		set3(f, 0.0f, 1.0f, 0.0f);
		set3(u, 1.0f/0.0f, 0.0f, 1.0f);
		CHECK(MatrixGuard_BasisNeedsRepair(r, f, u));
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));
	}

	/* A grossly scaled identity basis is flagged and normalised back. */
	{
		float r[3], f[3], u[3];
		set3(r, 2015.0f, 0.0f, 0.0f);
		set3(f, 0.0f, 2015.0f, 0.0f);
		set3(u, 0.0f, 0.0f, 2015.0f);
		CHECK(MatrixGuard_BasisNeedsRepair(r, f, u));
		MatrixGuard_Orthonormalise(r, f, u);
		CHECK(isOrthonormal(r, f, u, 1e-5));
	}

	/* Repair is idempotent. */
	{
		float r[3], f[3], u[3], r2[3], f2[3], u2[3];
		makeBrokenYachtBasis(r, f, u);
		MatrixGuard_Orthonormalise(r, f, u);
		r2[0]=r[0]; r2[1]=r[1]; r2[2]=r[2];
		f2[0]=f[0]; f2[1]=f[1]; f2[2]=f[2];
		u2[0]=u[0]; u2[1]=u[1]; u2[2]=u[2];
		MatrixGuard_Orthonormalise(r2, f2, u2);
		CHECK(isOrthonormal(r2, f2, u2, 1e-5));
		CHECK(ddot(r, r2) > 0.9999 && ddot(f, f2) > 0.9999 && ddot(u, u2) > 0.9999);
	}

	if (failures) {
		std::printf("matrix_guard_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("matrix_guard_test: all ok\n");
	return 0;
}
