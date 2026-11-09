/*
 * H7/T1: CPU-skinning math unit test (vendor/librw/src/mini/miniskin.h).
 *
 * The MINI backend has no vertex shader, so skinned vertices are blended on the
 * CPU: p' = sum_i w_i * (inverseBind_i * hier_i) * p, using the same derivation
 * as the GL3 backend's uploadSkinMatrices(). miniskin.h is the extracted,
 * dependency-free copy of that math, included by both minipipe.cpp and here so
 * the test exercises the exact production code.
 *
 * Covers: the 4-weight blend (identity/translation/midpoint/4-matrix, guard
 * cases, non-normalised weights), LOCAL and WORLD matrix composition vs an
 * independent double-precision reference, and mult order sensitivity.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes DH11:skinning.log.
 * No libm float calls (%f / %g formatting is broken in this libnix, and CXX
 * tests are not linked with libfix_math.o), so nothing is printed as a float and
 * the reference math uses double.
 */

#include <cstdio>

#include "miniskin.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Force a genuine runtime read so gcc cannot constant-fold the math. */
static float vf(float v) { volatile float t = v; return t; }

static double dabs(double x) { return x < 0.0 ? -x : x; }

static void
setWeights(float *w, float a, float b, float c, float d)
{
	w[0] = vf(a); w[1] = vf(b); w[2] = vf(c); w[3] = vf(d);
}

static void
mkTranslation(MiniMat *m, float x, float y, float z)
{
	miniMatIdentity(m);
	m->p[0] = vf(x); m->p[1] = vf(y); m->p[2] = vf(z);
}

/* +90 degrees about Z: (x,y,z) -> (-y, x, z), with translation pos. */
static void
mkRotZ90(MiniMat *m, float px, float py, float pz)
{
	m->r[0] = vf(0.0f); m->r[1] = vf(1.0f); m->r[2] = vf(0.0f);
	m->u[0] = vf(-1.0f); m->u[1] = vf(0.0f); m->u[2] = vf(0.0f);
	m->a[0] = vf(0.0f); m->a[1] = vf(0.0f); m->a[2] = vf(1.0f);
	m->p[0] = vf(px); m->p[1] = vf(py); m->p[2] = vf(pz);
}

static void
check3(const char *name, const float got[3], double wx, double wy, double wz,
       double eps)
{
	if (dabs(got[0] - wx) <= eps && dabs(got[1] - wy) <= eps &&
	    dabs(got[2] - wz) <= eps)
		std::printf("ok   %s\n", name);
	else {
		std::printf("FAIL %s\n", name);
		failures++;
	}
}

/* Independent double reference: flat layout r[3] u[3] a[3] p[3]. */
static void
toD(const MiniMat *m, double d[12])
{
	for (int i = 0; i < 3; i++) {
		d[i]     = m->r[i];
		d[3 + i] = m->u[i];
		d[6 + i] = m->a[i];
		d[9 + i] = m->p[i];
	}
}

static void
refMul(double o[12], const double a[12], const double b[12])
{
	/* o = a * b, same row-major convention as Matrix::mult_ (base.cpp:562). */
	for (int row = 0; row < 3; row++) {
		o[row * 3 + 0] = a[row * 3 + 0] * b[0] + a[row * 3 + 1] * b[3] + a[row * 3 + 2] * b[6];
		o[row * 3 + 1] = a[row * 3 + 0] * b[1] + a[row * 3 + 1] * b[4] + a[row * 3 + 2] * b[7];
		o[row * 3 + 2] = a[row * 3 + 0] * b[2] + a[row * 3 + 1] * b[5] + a[row * 3 + 2] * b[8];
	}
	o[9]  = a[9] * b[0] + a[10] * b[3] + a[11] * b[6] + b[9];
	o[10] = a[9] * b[1] + a[10] * b[4] + a[11] * b[7] + b[10];
	o[11] = a[9] * b[2] + a[10] * b[5] + a[11] * b[8] + b[11];
}

static void
checkMat(const char *name, const MiniMat *got, const double want[12], double eps)
{
	int ok = 1;
	double g[12];
	toD(got, g);
	for (int i = 0; i < 12; i++)
		if (dabs(g[i] - want[i]) > eps) ok = 0;
	if (ok)
		std::printf("ok   %s\n", name);
	else {
		std::printf("FAIL %s\n", name);
		failures++;
	}
}

static int
matNear(const MiniMat *a, const MiniMat *b, double eps)
{
	const float *x = (const float *)a;
	const float *y = (const float *)b;
	for (int i = 0; i < 12; i++)
		if (dabs((double)x[i] - (double)y[i]) > eps) return 0;
	return 1;
}

int main()
{
#ifdef AMIGAOS
	freopen("DH11:skinning.log", "w", stdout);
#endif

	float base[3];
	base[0] = vf(1.0f); base[1] = vf(2.0f); base[2] = vf(3.0f);
	unsigned char idx0[4] = { 0, 0, 0, 0 };

	/* Blend: identity + single weight 1 -> base position. */
	{
		MiniMat mats[1];
		float w[4], out[3];
		miniMatIdentity(&mats[0]);
		setWeights(w, 1.0f, 0.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx0, 1, mats);
		check3("blend identity w=1", out, 1.0, 2.0, 3.0, 1e-5);
	}

	/* Blend: translation + weight 1 -> base + t. */
	{
		MiniMat mats[1];
		float w[4], out[3];
		mkTranslation(&mats[0], 5.0f, -3.0f, 2.0f);
		setWeights(w, 1.0f, 0.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx0, 1, mats);
		check3("blend translation w=1", out, 6.0, -1.0, 5.0, 1e-4);
	}

	/* Blend: two translations at 0.5/0.5 -> midpoint. */
	{
		MiniMat mats[2];
		unsigned char idx[4] = { 0, 1, 0, 0 };
		float w[4], out[3];
		mkTranslation(&mats[0], 2.0f, 0.0f, 0.0f);
		mkTranslation(&mats[1], 0.0f, 4.0f, 0.0f);
		setWeights(w, 0.5f, 0.5f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx, 2, mats);
		check3("blend two translations 0.5/0.5", out, 2.0, 4.0, 3.0, 1e-4);
	}

	/* Blend: four distinct matrices, weights summing to 1. Expected values
	 * computed by hand in double (base (1,2,3)): (2.7, 4.0, 5.5). */
	{
		MiniMat mats[4];
		unsigned char idx[4] = { 0, 1, 2, 3 };
		float w[4], out[3];
		mkTranslation(&mats[0], 1.0f, 0.0f, 0.0f);
		mkTranslation(&mats[1], 0.0f, 2.0f, 0.0f);
		mkTranslation(&mats[2], 0.0f, 0.0f, 3.0f);
		mkTranslation(&mats[3], 4.0f, 4.0f, 4.0f);
		setWeights(w, 0.1f, 0.2f, 0.3f, 0.4f);
		miniSkinBlend(out, base, w, idx, 4, mats);
		check3("blend four matrices sum=1", out, 2.7, 4.0, 5.5, 1e-4);
	}

	/* Blend: a non-diagonal (rotation) matrix so every one of the nine 3x3
	 * terms matters. Translation-only cases leave the off-diagonals multiplied
	 * by zero, so a transposed/scrambled basis would slip through them.
	 * rotZ90 maps (x,y,z)->(-y,x,z); p=(0,0,5), base (1,2,3) -> (-2,1,8). */
	{
		MiniMat mats[1];
		float w[4], out[3];
		mkRotZ90(&mats[0], 0.0f, 0.0f, 5.0f);
		setWeights(w, 1.0f, 0.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx0, 1, mats);
		check3("blend rotation w=1", out, -2.0, 1.0, 8.0, 1e-4);
	}

	/* Blend: rotation + translation at 0.5/0.5 -> average of (-2,1,3) and
	 * (11,2,3) = (4.5, 1.5, 3.0). */
	{
		MiniMat mats[2];
		unsigned char idx[4] = { 0, 1, 0, 0 };
		float w[4], out[3];
		mkRotZ90(&mats[0], 0.0f, 0.0f, 0.0f);
		mkTranslation(&mats[1], 10.0f, 0.0f, 0.0f);
		setWeights(w, 0.5f, 0.5f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx, 2, mats);
		check3("blend rotation+translation 0.5/0.5", out, 4.5, 1.5, 3.0, 1e-4);
	}

	/* Guard: w == 0.0f is skipped (a huge translation must not contribute). */
	{
		MiniMat mats[2];
		unsigned char idx[4] = { 0, 1, 0, 0 };
		float w[4], out[3];
		mkTranslation(&mats[0], 1000.0f, 1000.0f, 1000.0f);
		miniMatIdentity(&mats[1]);
		setWeights(w, 0.0f, 1.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx, 2, mats);
		check3("guard zero weight skipped", out, 1.0, 2.0, 3.0, 1e-5);
	}

	/* Guard: index >= nBones is skipped (bone 3 ignored, nBones == 1). */
	{
		MiniMat mats[2];
		unsigned char idx[4] = { 0, 3, 0, 0 };
		float w[4], out[3];
		miniMatIdentity(&mats[0]);
		mkTranslation(&mats[1], 100.0f, 0.0f, 0.0f);
		setWeights(w, 1.0f, 1.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx, 1, mats);
		check3("guard index >= nBones skipped", out, 1.0, 2.0, 3.0, 1e-5);
	}

	/* Guard: nBones == 0 and all-invalid -> (0,0,0), not the base point. */
	{
		MiniMat mats[2];
		unsigned char idx[4] = { 0, 1, 0, 0 };
		float w[4], out[3];
		miniMatIdentity(&mats[0]);
		miniMatIdentity(&mats[1]);
		setWeights(w, 1.0f, 1.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx, 0, mats);
		check3("guard nBones==0 -> zero", out, 0.0, 0.0, 0.0, 1e-6);

		setWeights(w, 0.0f, 0.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, idx, 2, mats);
		check3("guard all weights zero -> zero", out, 0.0, 0.0, 0.0, 1e-6);

		unsigned char bad[4] = { 5, 6, 7, 8 };
		setWeights(w, 1.0f, 1.0f, 0.0f, 0.0f);
		miniSkinBlend(out, base, w, bad, 2, mats);
		check3("guard all indices out of range -> zero", out, 0.0, 0.0, 0.0, 1e-6);
	}

	/* Non-normalised weights: only 0.5 -> 0.5x the transformed point. */
	{
		MiniMat mats[1];
		float w[4], out[3], b[3];
		mkTranslation(&mats[0], 2.0f, 4.0f, 6.0f);
		b[0] = vf(2.0f); b[1] = vf(2.0f); b[2] = vf(2.0f);
		setWeights(w, 0.5f, 0.0f, 0.0f, 0.0f);
		miniSkinBlend(out, b, w, idx0, 1, mats);
		check3("blend non-normalised 0.5", out, 2.0, 3.0, 4.0, 1e-4);
	}

	/* Compose LOCAL: out = inv * hier, vs independent double reference. */
	{
		MiniMat inv, hier, out;
		double di[12], dh[12], want[12];
		mkTranslation(&inv, 1.0f, 2.0f, 3.0f);
		mkRotZ90(&hier, 0.0f, 0.0f, 5.0f);
		miniSkinComposeLocal(&out, &inv, &hier, 1);
		toD(&inv, di);
		toD(&hier, dh);
		refMul(want, di, dh);
		checkMat("compose LOCAL inv*hier", &out, want, 1e-5);
	}

	/* Compose LOCAL with n=2 exercises the loop/indexing beyond element 0. */
	{
		MiniMat inv[2], hier[2], out[2];
		double di[12], dh[12], want[12];
		mkTranslation(&inv[0], 1.0f, 2.0f, 3.0f);
		mkRotZ90(&hier[0], 0.0f, 0.0f, 5.0f);
		mkTranslation(&inv[1], -2.0f, 0.0f, 4.0f);
		mkTranslation(&hier[1], 3.0f, 3.0f, 3.0f);
		miniSkinComposeLocal(out, inv, hier, 2);
		toD(&inv[0], di); toD(&hier[0], dh); refMul(want, di, dh);
		checkMat("compose LOCAL n=2 [0]", &out[0], want, 1e-5);
		toD(&inv[1], di); toD(&hier[1], dh); refMul(want, di, dh);
		checkMat("compose LOCAL n=2 [1]", &out[1], want, 1e-5);
	}

	/* Compose WORLD: out = inv * (hier * invLTM), vs reference. */
	{
		MiniMat inv, hier, invLTM, out;
		double di[12], dh[12], dl[12], dtmp[12], want[12];
		mkTranslation(&inv, -1.0f, 0.5f, 2.0f);
		mkRotZ90(&hier, 3.0f, -2.0f, 1.0f);
		mkTranslation(&invLTM, 0.25f, 0.5f, -0.75f);
		miniSkinComposeWorld(&out, &inv, &hier, &invLTM, 1);
		toD(&inv, di);
		toD(&hier, dh);
		toD(&invLTM, dl);
		refMul(dtmp, dh, dl);       /* hier * invLTM */
		refMul(want, di, dtmp);     /* inv * (...) */
		checkMat("compose WORLD inv*(hier*invLTM)", &out, want, 1e-5);
	}

	/* Compose WORLD with n=2: shared invLTM, per-bone inv/hier. */
	{
		MiniMat inv[2], hier[2], invLTM, out[2];
		double di[12], dh[12], dl[12], dtmp[12], want[12];
		mkTranslation(&inv[0], -1.0f, 0.5f, 2.0f);
		mkRotZ90(&hier[0], 3.0f, -2.0f, 1.0f);
		mkTranslation(&inv[1], 2.0f, -1.0f, 0.5f);
		mkRotZ90(&hier[1], -1.0f, 1.0f, 2.0f);
		mkTranslation(&invLTM, 0.25f, 0.5f, -0.75f);
		miniSkinComposeWorld(out, inv, hier, &invLTM, 2);
		toD(&invLTM, dl);
		toD(&inv[0], di); toD(&hier[0], dh);
		refMul(dtmp, dh, dl); refMul(want, di, dtmp);
		checkMat("compose WORLD n=2 [0]", &out[0], want, 1e-5);
		toD(&inv[1], di); toD(&hier[1], dh);
		refMul(dtmp, dh, dl); refMul(want, di, dtmp);
		checkMat("compose WORLD n=2 [1]", &out[1], want, 1e-5);
	}

	/* Order sensitivity: mult(A,B) != mult(B,A) for translate/rotate. */
	{
		MiniMat a, b, ab, ba;
		mkTranslation(&a, 10.0f, 0.0f, 0.0f);
		mkRotZ90(&b, 0.0f, 0.0f, 0.0f);
		miniMatMult(&ab, &a, &b);
		miniMatMult(&ba, &b, &a);
		CHECK(!matNear(&ab, &ba, 1e-3));
	}

	if (failures) {
		std::printf("skinning_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("skinning_test: all ok\n");
	return 0;
}
