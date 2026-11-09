/*
 * locale_float_test: does libnix `sscanf("%f")` depend on the process locale
 * (AmigaOS Language / locale.library decimal point) on this toolchain?
 *
 * Repro of the tester's IPL parse failure: with Language=polski the loader got
 *   "-1029.901001" -> x=-1029, y=0, z=901001   (decimal point not recognised)
 * while Language=english gave x=-1029, y=-526, z=10.
 *
 * Compares raw sscanf("%f") against the port's locale-independent rfParseFloat.
 * Writes DH11:locale_float.log; terminal line "<name>: all ok"/"N failure(s)".
 */

#include <stdio.h>
#include <string.h>

#ifdef AMIGAOS
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>
#endif

#include "ParseFloat.h"

static int failures;

static void checki(const char *what, int got, int want)
{
	printf("%s %s (got %d want %d)\n", got == want ? "ok  " : "FAIL", what, got, want);
	if (got != want)
		failures++;
}

/* Report both parsers on one input, scaled so any decimal-point split shows. */
static void probe(const char *tag, const char *s)
{
	volatile float sv = -123456.0f;
	volatile float rv = -123456.0f;

	sscanf(s, "%f", (float *)&sv);
	rfParseFloat(s, (float *)&rv);
	printf("PROBE %s s='%s' sscanf=(%d) sscanf_x1e6=%d rf=(%d) rf_x1e6=%d\n",
	       tag, s, (int)sv, (int)(sv * 1000000.0f), (int)rv, (int)(rv * 1000000.0f));
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:locale_float.log", "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);
#endif

#ifdef AMIGAOS
	{
		char lang[64];
		lang[0] = 0;
		if (GetVar((CONST_STRPTR)"ENV:Language", (STRPTR)lang, sizeof(lang), 0) <= 0)
			strcpy(lang, "(unset)");
		printf("LANG ENV:Language='%s'\n", lang);
	}
#endif

	probe("default1", "-1029.901001");
	probe("default2", "-526.8988647");
	probe("default3", "10.60756874");

#ifdef AMIGAOS
	/* Force Polish and re-probe: if libnix reads the locale per call this flips. */
	SetVar((CONST_STRPTR)"ENV:Language", (CONST_STRPTR)"polski", -1, 0);
	{
		char lang[64];
		lang[0] = 0;
		GetVar((CONST_STRPTR)"ENV:Language", (STRPTR)lang, sizeof(lang), 0);
		printf("LANG after set='%s'\n", lang);
	}
	probe("polski", "-1029.901001");
#endif

	/* The locale-independent parser must always be right. */
	{
		volatile float v = 0.0f;
		rfParseFloat("-1029.901001", (float *)&v);
		checki("rf-1029-int", (int)v, -1029);
		rfParseFloat("-526.8988647", (float *)&v);
		checki("rf-526-int", (int)v, -526);
		rfParseFloat("10.60756874", (float *)&v);
		checki("rf-10-int", (int)v, 10);
	}

	if (failures) {
		printf("locale_float_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("locale_float_test: all ok\n");
	return 0;
}
