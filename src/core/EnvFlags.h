#ifndef __GTA_ENV_FLAGS_H__
#define __GTA_ENV_FLAGS_H__

/*
 * Canonical token parsing for the REVC_* AmigaDOS environment switches, split
 * out of the platform layer so it is host-testable (tests/amigaos/envflags_test.c)
 * without pulling in exec/dos/game headers.
 *
 * Every switch must go through these helpers, so a value means the same thing
 * for every flag. The accepted tokens are deliberately narrow:
 *
 *   bool true:  1, y, yes, true, on
 *   bool false: 0, n, no, off, false
 *
 * Anything else is UNRECOGNISED: the parser returns 0 (so the caller can decide
 * between "absent -> default" and "present but invalid -> fail fast").
 */

static inline char rfEnvLower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline int rfEnvTokenIs(const char *v, const char *tok)
{
	while (*tok) {
		if (rfEnvLower(*v) != *tok)
			return 0;
		v++;
		tok++;
	}
	return *v == '\0';
}

/* Parse a boolean token. Returns 1 and stores 0/1 in *out when recognised;
 * returns 0 for NULL, the empty string, or any unrecognised text. */
static inline int rfEnvParseBool(const char *v, int *out)
{
	if (v == 0 || v[0] == '\0')
		return 0;
	if (rfEnvTokenIs(v, "1") || rfEnvTokenIs(v, "y") ||
	    rfEnvTokenIs(v, "yes") || rfEnvTokenIs(v, "true") ||
	    rfEnvTokenIs(v, "on")) {
		*out = 1;
		return 1;
	}
	if (rfEnvTokenIs(v, "0") || rfEnvTokenIs(v, "n") ||
	    rfEnvTokenIs(v, "no") || rfEnvTokenIs(v, "off") ||
	    rfEnvTokenIs(v, "false")) {
		*out = 0;
		return 1;
	}
	return 0;
}

/* Strict base-10 integer: an optional sign followed by at least one digit and
 * nothing else. Returns 0 for NULL, the empty string, trailing garbage, or a
 * value that does not fit in a signed int. */
static inline int rfEnvParseInt(const char *v, int *out)
{
	const char *p = v;
	int neg = 0;
	long acc = 0;

	if (p == 0 || *p == '\0')
		return 0;
	if (*p == '+' || *p == '-') {
		neg = (*p == '-');
		p++;
	}
	if (*p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9') {
		int d = *p - '0';
		/* Bound before multiplying: `long` is 32-bit on the m68k target, so
		 * computing acc*10 first would overflow (UB) and evade the check. */
		if (acc > (2147483647L - d) / 10)
			return 0;
		acc = acc * 10 + d;
		p++;
	}
	if (*p != '\0')
		return 0;
	*out = (int)(neg ? -acc : acc);
	return 1;
}

#endif /* __GTA_ENV_FLAGS_H__ */
