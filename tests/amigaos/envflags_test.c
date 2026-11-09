/*
 * Canonical REVC_* token parser unit test (src/core/EnvFlags.h).
 *
 * Every environment switch is resolved through rfEnvParseBool()/rfEnvParseInt(),
 * so this pins the exact accepted token set and the failure cases. A value that
 * is present but not recognised must return 0 (the platform layer turns that
 * into a fail-fast); an absent/empty value is the caller's default and is not
 * parsed here.
 *
 * Host + 68k; writes DH11:envflags_test.log on the target.
 */

#include <stdio.h>

#include "EnvFlags.h"

static int failures;

static void expect_bool(const char *in, int want_valid, int want_val,
                        const char *name)
{
	int val = -1;
	int got = rfEnvParseBool(in, &val);

	if (got != want_valid || (want_valid && val != want_val)) {
		printf("FAIL bool %s ('%s' -> valid=%d val=%d, want valid=%d val=%d)\n",
		       name, in ? in : "(null)", got, val, want_valid, want_val);
		failures++;
	} else {
		printf("ok   bool %s\n", name);
	}
}

static void expect_int(const char *in, int want_valid, int want_val,
                       const char *name)
{
	int val = -12345;
	int got = rfEnvParseInt(in, &val);

	if (got != want_valid || (want_valid && val != want_val)) {
		printf("FAIL int %s ('%s' -> valid=%d val=%d, want valid=%d val=%d)\n",
		       name, in ? in : "(null)", got, val, want_valid, want_val);
		failures++;
	} else {
		printf("ok   int %s\n", name);
	}
}

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:envflags_test.log", "w", stdout);
#endif
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Truthy tokens, case-insensitive. */
	expect_bool("1", 1, 1, "one");
	expect_bool("y", 1, 1, "y");
	expect_bool("Y", 1, 1, "Y");
	expect_bool("yes", 1, 1, "yes");
	expect_bool("YES", 1, 1, "YES");
	expect_bool("true", 1, 1, "true");
	expect_bool("True", 1, 1, "True");
	expect_bool("on", 1, 1, "on");
	expect_bool("On", 1, 1, "On");

	/* Falsey tokens: recognised, value 0. */
	expect_bool("0", 1, 0, "zero");
	expect_bool("n", 1, 0, "n");
	expect_bool("N", 1, 0, "N");
	expect_bool("no", 1, 0, "no");
	expect_bool("NO", 1, 0, "NO");
	expect_bool("off", 1, 0, "off");
	expect_bool("false", 1, 0, "false");
	expect_bool("FALSE", 1, 0, "FALSE");

	/* Unrecognised / empty / NULL: invalid (caller decides). */
	expect_bool("2", 0, 0, "two");
	expect_bool("noton", 0, 0, "noton");
	expect_bool("yesplease", 0, 0, "yesplease");
	expect_bool("tru", 0, 0, "tru");
	expect_bool("", 0, 0, "empty");
	expect_bool(0, 0, 0, "null");

	/* Integers: strict base-10, no trailing garbage, in range. */
	expect_int("0", 1, 0, "zero");
	expect_int("5", 1, 5, "five");
	expect_int("+7", 1, 7, "plus7");
	expect_int("-3", 1, -3, "minus3");
	expect_int("2147483647", 1, 2147483647, "intmax");
	expect_int("-2147483647", 1, -2147483647, "intmin1");

	expect_int("2147483648", 0, 0, "overflow");
	expect_int("99999999999999999999", 0, 0, "huge");
	expect_int("abc", 0, 0, "abc");
	expect_int("1x", 0, 0, "trailing");
	expect_int("x1", 0, 0, "leading");
	expect_int("1 ", 0, 0, "trailing-space");
	expect_int("+", 0, 0, "lone-plus");
	expect_int("-", 0, 0, "lone-minus");
	expect_int("", 0, 0, "empty");
	expect_int(0, 0, 0, "null");

	if (failures) {
		printf("envflags_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("envflags_test: all ok\n");
	return 0;
}
