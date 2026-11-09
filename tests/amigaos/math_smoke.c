/*
 * M0 math smoke test (RTCW's mathtest analogue).
 *
 * libnix on m68k historically returned float results in d0 while callers read
 * fp0, and had broken printf rounding. A broken math library shows up as a
 * rotated 3D view or wrong movement long before a crash, so assert the basics.
 * Every passing line prints "ok", matching the RTCW pattern.
 */
#include <math.h>
#include <stdio.h>

static int failures;

#define CLOSE(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

static void check(const char *name, int ok)
{
	if (ok) {
		printf("ok   %s\n", name);
	} else {
		printf("FAIL %s\n", name);
		failures++;
	}
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:math_smoke.log", "w", stdout);
#endif
	check("sin(0)==0", CLOSE(sin(0.0), 0.0, 1e-9));
	check("cos(0)==1", CLOSE(cos(0.0), 1.0, 1e-9));
	check("sin(pi/2)==1", CLOSE(sin(M_PI / 2.0), 1.0, 1e-9));
	check("cos(pi)==-1", CLOSE(cos(M_PI), -1.0, 1e-9));
	check("sqrt(2)^2==2", CLOSE(sqrt(2.0) * sqrt(2.0), 2.0, 1e-9));
	check("atan2(0,-1)==pi", CLOSE(atan2(0.0, -1.0), M_PI, 1e-9));
	check("acos(-1)==pi", CLOSE(acos(-1.0), M_PI, 1e-9));
	check("exp(0)==1", CLOSE(exp(0.0), 1.0, 1e-9));
	check("log(1)==0", CLOSE(log(1.0), 0.0, 1e-9));
	check("fmod(5.5,2)==1.5", CLOSE(fmod(5.5, 2.0), 1.5, 1e-9));

	/* float (not double) results must survive the libnix call convention. */
	float fs = sinf(0.5f);
	check("sinf(0.5)", CLOSE(fs, sin(0.5), 1e-6));

	/* printf %f must round and be non-empty. */
	char out[64];
	snprintf(out, sizeof(out), "%.3f", 1.23456);
	check("printf %f rounds", out[0] == '1' && out[1] == '.' && out[2] == '2' &&
	                          out[3] == '3' && out[4] == '5' && out[5] == 0);

	if (failures) {
		printf("math_smoke: %d failure(s)\n", failures);
		return 1;
	}
	printf("math_smoke: all ok\n");
	return 0;
}
