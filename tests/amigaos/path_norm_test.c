/*
 * H6: path/name normalisation unit test (src/core/PathNormalize.h).
 *
 * AmigaOS must accept the Windows-style names used by the PC assets: convert
 * '\' separators to '/' and strip a trailing space (Win32 does this, AmigaDOS
 * does not). These helpers are used by _fcaseopen (crossplatform.cpp); getting
 * them wrong is the "file-open failures when starting a new game" class of bug.
 *
 * Host + 68k; writes DH11:path_norm.log on the target.
 */

#include <stdio.h>
#include <string.h>

#include "PathNormalize.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { printf("ok   %s\n", #cond); } \
	else { printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Copy `in` into a local buffer, normalise, and compare with `want`. */
static void expect(const char *in, const char *want, const char *name)
{
	char buf[256];
	size_t n;
	strncpy(buf, in, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	n = AmigaNormalizePath(buf);
	if (strcmp(buf, want) == 0 && n == strlen(want)) {
		printf("ok   norm %s\n", name);
	} else {
		printf("FAIL norm %s ('%s' != '%s', len=%d)\n",
		       name, buf, want, (int)n);
		failures++;
	}
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:path_norm.log", "w", stdout);
#endif

	expect("models\\generic.txd", "models/generic.txd", "backslash");
	expect("a\\b\\c", "a/b/c", "nested backslash");
	expect("data\\maps\\foo.dat ", "data/maps/foo.dat", "backslash+space");
	expect("trailing   ", "trailing", "trailing spaces");
	expect("abc\t\r\n", "abc", "trailing tab/cr/lf");
	expect("a b ", "a b", "interior space kept");
	expect("plain/file", "plain/file", "unchanged");
	expect("", "", "empty");
	expect(" ", "", "only space");
	expect("\\", "/", "single backslash");

	/* Buffer must be NUL-terminated at the returned length. */
	{
		char buf[64];
		size_t n;
		strcpy(buf, "x\\y   ");
		n = AmigaNormalizePath(buf);
		CHECK(n == 3);
		CHECK(buf[n] == '\0');
	}

	/* Trim-only and slash-only behaviour is independently testable. */
	{
		char t[64];
		strcpy(t, "a\\b ");   /* backslash + trailing space */
		CHECK(TrimTrailingWhitespace(t) == 3);
		CHECK(strcmp(t, "a\\b") == 0);
		CHECK(BackslashToSlash(t) == 3);
		CHECK(strcmp(t, "a/b") == 0);
	}

	/* AmigaPathVariant: the shared open-candidate helper used by _fcaseopen,
	 * the sync CdStream backend and the async loader. */
	{
		char v[64];
		size_t n;

		n = AmigaPathVariant(v, sizeof(v), "models\\GeNeRiC.TXD ", 0);
		CHECK(n == strlen("models/GeNeRiC.TXD"));
		CHECK(strcmp(v, "models/GeNeRiC.TXD") == 0);

		n = AmigaPathVariant(v, sizeof(v), "models\\GeNeRiC.TXD ", 1);
		CHECK(n == strlen("models/generic.txd"));
		CHECK(strcmp(v, "models/generic.txd") == 0);

		n = AmigaPathVariant(v, sizeof(v), "models\\GeNeRiC.TXD ", 2);
		CHECK(n == strlen("MODELS/GENERIC.TXD"));
		CHECK(strcmp(v, "MODELS/GENERIC.TXD") == 0);

		CHECK(AmigaPathVariant(v, sizeof(v), "x", 3) == 0);
		CHECK(AmigaPathVariant(v, 0, "x", 0) == 0);
	}

	if (failures) {
		printf("path_norm_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("path_norm_test: all ok\n");
	return 0;
}
