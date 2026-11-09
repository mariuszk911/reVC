/*
 * Standalone 68k validation for the object.dat parser fix.
 *
 * On this toolchain libnix `sscanf("%f", ...)` is unreliable (float values are
 * not stored), which makes CObjectData::Initialise treat every object as
 * "different from default" and overflow ms_aObjectInfo[] (assert at
 * ObjectData.cpp:97). The new rfParseFloat/rfParseInt helpers in
 * src/core/ParseFloat.h avoid libc float conversion entirely.
 *
 * This test parses one real object.dat row twice -- once with sscanf("%s %f
 * %f %f %f %f %f %f %d %d %d") and once with the helpers -- and checks the
 * helper results against the known values. The sscanf result is printed
 * informationally (BROKEN/ok) but not counted, because the point of the fix is
 * to work regardless of libc.
 *
 * Writes DH11:objdata_scan.log (harness convention); inputs come from a
 * volatile source so gcc cannot constant-fold the parsing. Never prints float
 * values -- %f is broken in this libnix.
 */

#include <stdio.h>
#include <string.h>

#include "ParseFloat.h"

#ifndef LOGFILE
#define LOGFILE "DH11:objdata_scan.log"
#endif

static int failures;

static float fdiff(float a, float b)
{
	float d = a - b;
	return d < 0.0f ? -d : d;
}

static int feq(float a, float b)
{
	float scale = b < 0.0f ? -b : b;
	if (scale < 1.0f)
		scale = 1.0f;
	return fdiff(a, b) <= 1e-4f * scale;
}

static void report(const char *what, int ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void checkf(const char *what, float got, float want)
{
	report(what, feq(got, want));
}

static void checki(const char *what, int got, int want)
{
	report(what, got == want);
}

int main(void)
{
	/* Runtime-built input; the volatile source prevents folding. */
	static volatile char src[] =
		"ml_coffwin02 9999.0 99999.0 0.99 0.1 50.0 125.0 1.0 0 4 0";
	char buf[128];
	int i, sscanfOk;
	char sname[256], rname[256];
	float smass, sturn, sair, sela, spct, supr, scol;
	int sde, src2, scam;
	float rmass, rturn, rair, rela, rpct, rupr, rcol;
	int rde, rrsp, rcam;
	const char *q;

#ifdef AMIGAOS
	freopen(LOGFILE, "w", stdout);
#endif

	for (i = 0; src[i] && i < (int)sizeof(buf)-1; i++)
		buf[i] = src[i];
	buf[i] = '\0';

	printf("objdata parser scan (%d bytes)\n", i);

	/* libc path (informational: expected to be broken on libnix). */
	smass = sturn = sair = sela = spct = supr = scol = -1.0f;
	sde = src2 = scam = -1;
	sscanf(buf, "%s %f %f %f %f %f %f %f %d %d %d", sname,
		&smass, &sturn, &sair, &sela, &spct, &supr, &scol,
		&sde, &src2, &scam);
	sscanfOk = feq(smass, 9999.0f) && feq(sturn, 99999.0f) && feq(sair, 0.99f)
		&& feq(sela, 0.1f) && feq(spct, 50.0f) && feq(supr, 125.0f)
		&& feq(scol, 1.0f) && sde == 0 && src2 == 4 && scam == 0;
	printf("libnix sscanf(\"%%f\"): %s\n", sscanfOk ? "ok" : "BROKEN");

	/* New path (must yield the correct values). */
	rmass = rturn = rair = rela = rpct = rupr = rcol = -1.0f;
	rde = rrsp = rcam = -1;
	q = buf;
	q = rfReadWord(q, rname, sizeof(rname));
	q = rfParseFloat(q, &rmass);
	q = rfParseFloat(q, &rturn);
	q = rfParseFloat(q, &rair);
	q = rfParseFloat(q, &rela);
	q = rfParseFloat(q, &rpct);
	q = rfParseFloat(q, &rupr);
	q = rfParseFloat(q, &rcol);
	q = rfParseInt(q, &rde);
	q = rfParseInt(q, &rrsp);
	q = rfParseInt(q, &rcam);

	report("rfParse-name", strcmp(rname, "ml_coffwin02") == 0);
	checkf("rfParse-mass", rmass, 9999.0f);
	checkf("rfParse-turnmass", rturn, 99999.0f);
	checkf("rfParse-airres", rair, 0.99f);
	checkf("rfParse-elasticity", rela, 0.1f);
	checkf("rfParse-pctsub", rpct, 50.0f);
	checkf("rfParse-uproot", rupr, 125.0f);
	checkf("rfParse-coldmgmul", rcol, 1.0f);
	checki("rfParse-damageeffect", rde, 0);
	checki("rfParse-responsecase", rrsp, 4);
	checki("rfParse-camavoid", rcam, 0);

	if (failures) {
		printf("objdata_scan_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("objdata_scan_test: all ok\n");
	return 0;
}
