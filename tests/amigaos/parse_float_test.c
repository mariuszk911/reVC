/*
 * Unit test for src/core/ParseFloat.h (rfSkipSpace / rfReadWord /
 * rfParseFloat / rfParseInt).
 *
 * These helpers replace libnix `sscanf("%f")` in CObjectData::Initialise,
 * because the float conversion is unreliable on this toolchain (same ABI
 * class as the broken sinf/sqrtf). The test is pure integer +
 * double logic, so it runs both on the host and cross-built for 68k.
 *
 * Assertions compare numeric values, never printed text (%f is broken in this
 * libnix). Terminal line: "<name>: all ok" / "<name>: N failure(s)".
 */

#include <stdio.h>
#include <string.h>

#include "ParseFloat.h"

static int failures;

static void report(const char *what, int ok)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void checkf(const char *what, float got, float want)
{
	float d = got - want;
	float scale = want < 0.0f ? -want : want;

	if (d < 0.0f)
		d = -d;
	if (scale < 1.0f)
		scale = 1.0f;
	report(what, d <= 1e-4f * scale);
}

static void checki(const char *what, int got, int want)
{
	report(what, got == want);
}

static void checks(const char *what, const char *got, const char *want)
{
	report(what, strcmp(got, want) == 0);
}

static void checkptr(const char *what, const char *got, const char *want)
{
	report(what, got == want);
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:parse_float.log", "w", stdout);
#endif

	/* --- whitespace / word reading ------------------------------------- */
	{
		char w[32];
		const char *p = rfSkipSpace(" \t\r\n  hello world");
		checks("skipspace-reaches", p, "hello world");

		p = rfReadWord("   ml_coffwin02\t\t9999.0\n", w, sizeof(w));
		checks("readword-value", w, "ml_coffwin02");
		checks("readword-stops-at-tab", p, "\t\t9999.0\n");

		p = rfReadWord("   \t\n", w, sizeof(w));
		checkptr("readword-empty", p, (const char *)0);

		/* Token longer than the buffer must truncate, not overflow. */
		memset(w, 0x7f, sizeof(w));
		rfReadWord("abcdefghijklmnopqrstuvwxyz0123456789", w, 8);
		checks("readword-truncates", w, "abcdefg");
	}

	/* --- rfParseFloat -------------------------------------------------- */
	{
		static const struct { const char *s; float want; } cases[] = {
			{ "99999.0", 99999.0f },
			{ "0.99", 0.99f },
			{ "0.1", 0.1f },
			{ "1.0", 1.0f },
			{ "125.0", 125.0f },
			{ "50.0", 50.0f },
			{ "-3.5", -3.5f },
			{ "+2.25", 2.25f },
			{ ".5", 0.5f },
			{ "5.", 5.0f },
			{ "1e3", 1000.0f },
			{ "1.5e-2", 0.015f },
			{ "2E+2", 200.0f },
			{ "0", 0.0f },
		};
		unsigned i;

		for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
			float v = -12345.0f;
			const char *p = rfParseFloat(cases[i].s, &v);
			checkf(cases[i].s, v, cases[i].want);
			/* Fully numeric input must be consumed. */
			if (*p != '\0') {
				report("parsefloat-consumed", 0);
			}
		}

		/* Non-numeric: helper must not write and must return the seed. */
		{
			const char *in = "  ;comment";
			float v = 7.5f;
			const char *p = rfParseFloat(in, &v);
			checkptr("parsefloat-nodigit-pointer", p, in);
			checkf("parsefloat-nodigit-value", v, 7.5f);
		}
		/* No digit after sign/point. */
		{
			const char *in = "-e5";
			float v = 1.25f;
			const char *p = rfParseFloat(in, &v);
			checkptr("parsefloat-sign-nodigit-pointer", p, in);
			checkf("parsefloat-sign-nodigit-value", v, 1.25f);
		}
	}

	/* --- rfParseInt (incl. %d truncation of "50.0") -------------------- */
	{
		int v;
		const char *p;

		v = -1; p = rfParseInt("0", &v);       checki("parseint-0", v, 0);
		report("parseint-0-end", *p == '\0');

		v = -1; p = rfParseInt("4", &v);       checki("parseint-4", v, 4);
		(void)p;

		v = -1; p = rfParseInt("125 ", &v);    checki("parseint-125", v, 125);
		checks("parseint-125-stop", p, " ");

		v = 0;  p = rfParseInt("-7", &v);      checki("parseint-neg", v, -7);
		(void)p;

		/* "50.0" must truncate at '.', matching "%d". */
		v = -1; p = rfParseInt(" 50.0", &v);   checki("parseint-trunc", v, 50);
		checks("parseint-trunc-stop", p, ".0");

		/* No digit: untouched, pointer unchanged. */
		v = 9; p = rfParseInt("  x", &v);      checki("parseint-nodigit-value", v, 9);
		checkptr("parseint-nodigit-pointer", p, "  x");

		/* Overflow saturates instead of wrapping (signed-overflow UB). */
		v = 0; p = rfParseInt("2147483647", &v);  checki("parseint-intmax", v, 2147483647);
		v = 0; p = rfParseInt("2147483648", &v);  checki("parseint-overflow", v, 2147483647);
		v = 0; p = rfParseInt("-2147483648", &v); checki("parseint-intmin", v, -2147483647 - 1);
		v = 0; p = rfParseInt("-2147483649", &v); checki("parseint-underflow", v, -2147483647 - 1);
		v = 0; p = rfParseInt("99999999999999999999", &v); checki("parseint-huge", v, 2147483647);
		(void)p;
	}

	/* --- rfParseHex: %x semantics (optional 0x prefix) ------------------ */
	{
		int v;
		const char *p;

		v = -1; p = rfParseHex("0", &v);      checki("parsehex-0", v, 0);
		v = -1; p = rfParseHex("1a", &v);     checki("parsehex-1a", v, 26);
		v = -1; p = rfParseHex("0x1F", &v);   checki("parsehex-0x1f", v, 31);
		v = -1; p = rfParseHex("ff ", &v);    checki("parsehex-ff", v, 255);
		v = 7;  p = rfParseHex(" z", &v);     checki("parsehex-nodigit", v, 7);
		(void)p;
	}

	/* --- rfFormatFloat: locale-safe writer, round-trips through rfParseFloat */
	{
		static const float vals[] = {
			0.0f, 0.0025f, 1.0f, -3.5f, 0.0003125f, 12.5f, 99999.0f, 0.99f,
			-0.005f, 1.0f/200.0f
		};
		unsigned i;

		for (i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
			char s[50];
			float back = -12345.0f;
			const char *p;
			rfFormatFloat(s, sizeof(s), vals[i]);
			p = rfParseFloat(s, &back);
			checkf("format-roundtrip", back, vals[i]);
			report("format-roundtrip-consumed", *p == '\0');
		}

		/* Exact text: always '.', fixed 6 decimals, sign preserved. */
		{
			char s[50];
			rfFormatFloat(s, sizeof(s), 0.0025f);
			checks("format-0.0025", s, "0.002500");
		}
		{
			char s[50];
			rfFormatFloat(s, sizeof(s), -3.5f);
			checks("format-neg", s, "-3.500000");
		}
		{
			char s[50];
			rfFormatFloat(s, sizeof(s), 0.0f);
			checks("format-zero", s, "0.000000");
		}

		/* Truncation and degenerate buffers must stay NUL-terminated. */
		{
			char s[4];
			rfFormatFloat(s, sizeof(s), 1.234567f);
			report("format-truncation-terminated", s[3] == '\0');
		}
		{
			char s[1];
			s[0] = 'X';
			rfFormatFloat(s, sizeof(s), 1.0f);
			report("format-cap1-terminated", s[0] == '\0');
		}
	}

	/* --- full object.dat row (comma-free after the loader's translation) */
	{
		const char *row =
			"ml_coffwin02\t\t9999.0\t\t99999.0\t0.99\t\t0.1\t\t50.0\t\t125.0\t1.0\t0\t4\t0";
		char name[256];
		float mass, turnMass, airResistance, elasticity, percentSubmerged,
		      uprootLimit, collisionDamageMultiplier;
		int damageEffect, responseCase, camAvoid;
		const char *q = row;

		mass = turnMass = airResistance = elasticity = percentSubmerged =
			uprootLimit = collisionDamageMultiplier = -1.0f;
		damageEffect = responseCase = camAvoid = -1;

		q = rfReadWord(q, name, sizeof(name));
		q = rfParseFloat(q, &mass);
		q = rfParseFloat(q, &turnMass);
		q = rfParseFloat(q, &airResistance);
		q = rfParseFloat(q, &elasticity);
		q = rfParseFloat(q, &percentSubmerged);
		q = rfParseFloat(q, &uprootLimit);
		q = rfParseFloat(q, &collisionDamageMultiplier);
		q = rfParseInt(q, &damageEffect);
		q = rfParseInt(q, &responseCase);
		q = rfParseInt(q, &camAvoid);

		checks("row-name", name, "ml_coffwin02");
		checkf("row-mass", mass, 9999.0f);
		checkf("row-turnmass", turnMass, 99999.0f);
		checkf("row-airres", airResistance, 0.99f);
		checkf("row-elasticity", elasticity, 0.1f);
		checkf("row-pctsub", percentSubmerged, 50.0f);
		checkf("row-uproot", uprootLimit, 125.0f);
		checkf("row-coldmgmul", collisionDamageMultiplier, 1.0f);
		checki("row-damageeffect", damageEffect, 0);
		checki("row-responsecase", responseCase, 4);
		checki("row-camavoid", camAvoid, 0);
	}

	/* --- a real nbspraydoor row (the line printed before the crash) ----- */
	{
		const char *row =
			"nbspraydoor\t\t\t99999.0\t99999.0\t0.99\t\t0.1\t\t50.0\t\t0.0\t1.0\t0\t4\t1";
		float mass, turnMass, airres;
		char name[256];
		const char *q = row;

		mass = turnMass = airres = -1.0f;
		q = rfReadWord(q, name, sizeof(name));
		q = rfParseFloat(q, &mass);
		q = rfParseFloat(q, &turnMass);
		(void)rfParseFloat(q, &airres);

		checks("nbspray-name", name, "nbspraydoor");
		checkf("nbspray-mass", mass, 99999.0f);
		checkf("nbspray-turnmass", turnMass, 99999.0f);
	}

	if (failures) {
		printf("parse_float_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("parse_float_test: all ok\n");
	return 0;
}
