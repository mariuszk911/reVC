#ifndef REVC_PARSE_FLOAT_H
#define REVC_PARSE_FLOAT_H

/*
 * Header-only tokenizer + numeric field parsers/writers for the AmigaOS 68k
 * port.
 *
 * libnix's `sscanf("%f", ...)` conversion and `strtof` are locale-dependent on
 * this toolchain (they stop at '.' when AmigaOS Language is not English; see
 * tests/amigaos/locale_float_test.c) and `printf`'s float formatting is
 * unreliable, so neither text/scene parsing nor settings round-trips may use
 * libc float conversion. These helpers use only integer accumulation and double
 * `*` / `/` (hardware FPU ops with -mhard-float), never strtod/atof/sscanf or
 * printf %f.
 *
 * They mirror the sscanf semantics the parser relied on:
 *   rfReadWord    ~ "%s"  (skip leading whitespace, copy non-whitespace token)
 *   rfParseFloat  ~ "%f"  (optional sign, digits, '.', [eE][+-]exp)
 *   rfParseInt    ~ "%d"  (optional sign, leading decimal integer; "50.0"->50)
 *   rfFormatFloat is the writer inverse: "-?d.dddddd", always with '.', so an
 *                 rfFormatFloat -> rfParseFloat round-trip is locale-safe.
 */

static inline int
rfIsSpace(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static inline const char *
rfSkipSpace(const char *p)
{
	while (rfIsSpace((unsigned char)*p))
		p++;
	return p;
}

/* Copy the next whitespace-delimited token into dst (NUL-terminated, at most
 * cap-1 chars). Returns the pointer just past the token, or 0 if there is no
 * token (p already at end of string). */
static inline const char *
rfReadWord(const char *p, char *dst, int cap)
{
	int n = 0;

	p = rfSkipSpace(p);
	if (*p == '\0')
		return 0;
	while (*p != '\0' && !rfIsSpace((unsigned char)*p)) {
		if (n < cap - 1)
			dst[n++] = *p;
		p++;
	}
	dst[n] = '\0';
	return p;
}

/* Signed decimal float. Returns the input pointer unchanged (and leaves *out
 * untouched) when no digit was seen, matching an sscanf field that did not
 * convert. Handles "+1", ".5", "5.", "1e3" and "1.5e-2". */
static inline const char *
rfParseFloat(const char *p, float *out)
{
	const char *start = p;
	const char *q;
	int neg = 0;
	long long ipart = 0, fpart = 0;
	int fdig = 0, exp = 0, eneg = 0, anydigit = 0;
	int i;

	p = rfSkipSpace(p);
	q = p;
	if (*q == '+' || *q == '-') {
		neg = (*q == '-');
		q++;
	}
	while (*q >= '0' && *q <= '9') {
		if (ipart < 100000000000000000LL)
			ipart = ipart * 10 + (*q - '0');
		anydigit = 1;
		q++;
	}
	if (*q == '.') {
		const char *r = q + 1;
		while (*r >= '0' && *r <= '9') {
			if (fdig < 18) {
				fpart = fpart * 10 + (*r - '0');
				fdig++;
			}
			anydigit = 1;
			r++;
		}
		q = r;
	}
	if (!anydigit)
		return start;
	if (*q == 'e' || *q == 'E') {
		const char *r = q + 1;
		if (*r == '+' || *r == '-') {
			eneg = (*r == '-');
			r++;
		}
		if (*r >= '0' && *r <= '9') {
			while (*r >= '0' && *r <= '9') {
				if (exp < 100000)
					exp = exp * 10 + (*r - '0');
				r++;
			}
			q = r;
		}
	}
	{
		double fdiv = 1.0, v, ev = 1.0;
		int e = exp;

		for (i = 0; i < fdig; i++)
			fdiv *= 10.0;
		v = (double)ipart + (double)fpart / fdiv;

		if (e > 38)
			e = 38;
		for (i = 0; i < e; i++)
			ev *= 10.0;
		if (eneg)
			v /= ev;
		else
			v *= ev;
		*out = (float)(neg ? -v : v);
	}
	return q;
}

/* Locale-independent atof(): parse a leading float, 0.0 when no digit was seen
 * (matching atof's failure value). Never consults the C locale. */
static inline float
rfAtof(const char *p)
{
	float v = 0.0f;
	rfParseFloat(p, &v);
	return v;
}

/* Signed leading decimal integer. `%d` semantics: it stops at '.', so "50.0"
 * yields 50. Returns the input pointer unchanged (and leaves *out untouched)
 * when no digit was seen. */
static inline const char *
rfParseInt(const char *p, int *out)
{
	const char *start = p;
	const char *q;
	int neg = 0, anydigit = 0, overflow = 0;
	long v = 0;

	p = rfSkipSpace(p);
	q = p;
	if (*q == '+' || *q == '-') {
		neg = (*q == '-');
		q++;
	}
	while (*q >= '0' && *q <= '9') {
		int d = *q - '0';
		if (v > (2147483647L - d) / 10)
			overflow = 1;
		else
			v = v * 10 + d;
		anydigit = 1;
		q++;
	}
	if (!anydigit)
		return start;
	if (overflow)
		*out = neg ? (-2147483647 - 1) : 2147483647;
	else
		*out = (int)(neg ? -v : v);
	return q;
}

/* Unsigned hex integer, `%x` semantics (optional 0x/0X prefix). Returns the
 * input pointer unchanged (and leaves *out untouched) when no hex digit was
 * seen. */
static inline const char *
rfParseHex(const char *p, int *out)
{
	const char *start = p;
	unsigned int v = 0;
	int anydigit = 0;

	p = rfSkipSpace(p);
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	for (;;) {
		int c = (unsigned char)*p, d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		v = v * 16u + (unsigned int)d;
		anydigit = 1;
		p++;
	}
	if (!anydigit)
		return start;
	*out = (int)v;
	return p;
}

/* Locale-independent fixed-point float writer (6 decimals). This is the inverse
 * of the value grammar rfParseFloat accepts ("-?d.dddddd") and always emits '.'
 * regardless of the process locale, so StoreIni()/ReadIniIfExists() round-trip
 * without libc printf %f or strtof. Always NUL-terminates within cap bytes; NaN,
 * +/-Inf and magnitudes too large for the integer path fall back to "0.000000"
 * (or are clamped) rather than emitting garbage. */
static inline void
rfFormatFloat(char *out, int cap, float val)
{
	double v = (double)val;
	int neg = 0, ip = 0, n = 0, i, pos = 0;
	unsigned int fp, div;
	char intbuf[12];
	char frac[6];

	if (cap <= 0)
		return;
	if (!(v == v) || v > 1.0e30 || v < -1.0e30)	/* NaN / +/-Inf */
		v = 0.0;
	if (v < 0.0) {
		neg = 1;
		v = -v;
	}
	if (v > 2000000000.0)			/* keep the integer part in 32-bit range */
		v = 2000000000.0;

	ip = (int)v;
	fp = (unsigned int)((v - (double)ip) * 1000000.0 + 0.5);
	if (fp >= 1000000u) {
		fp -= 1000000u;
		ip += 1;
	}

	if (ip == 0) {
		intbuf[n++] = '0';
	} else {
		char rev[12];
		int r = 0;
		while (ip > 0 && r < (int)sizeof(rev)) {
			rev[r++] = (char)('0' + ip % 10);
			ip /= 10;
		}
		while (r > 0)
			intbuf[n++] = rev[--r];
	}

	div = 100000u;
	for (i = 0; i < 6; i++) {
		frac[i] = (char)('0' + (fp / div) % 10u);
		div /= 10u;
	}

	if (neg && pos < cap - 1)
		out[pos++] = '-';
	for (i = 0; i < n && pos < cap - 1; i++)
		out[pos++] = intbuf[i];
	if (pos < cap - 1)
		out[pos++] = '.';
	for (i = 0; i < 6 && pos < cap - 1; i++)
		out[pos++] = frac[i];
	out[pos] = '\0';
}

#endif /* REVC_PARSE_FLOAT_H */
