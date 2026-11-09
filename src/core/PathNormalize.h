#ifndef __GTA_PATH_NORMALIZE_H__
#define __GTA_PATH_NORMALIZE_H__

/*
 * Small pure path-normalisation helpers used by _fcaseopen
 * (src/skel/crossplatform.cpp) and split out so they can be unit-tested on the
 * host (tests/amigaos/path_norm_test.c).
 *
 * AmigaOS data files come from Windows: separators are '\' and some component
 * names carry a trailing space (Win32 strips those; AmigaDOS does not). Both
 * transformations are in-place on a NUL-terminated buffer and return the new
 * length.
 */

#include <string.h>

/* Strip trailing spaces/tabs/CR/LF. */
static inline size_t TrimTrailingWhitespace(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
	                 s[n-1] == '\r' || s[n-1] == '\n'))
		s[--n] = '\0';
	return n;
}

/* Windows-style separators -> AmigaDOS '/'. */
static inline size_t BackslashToSlash(char *s)
{
	size_t i, n = strlen(s);
	for (i = 0; i < n; i++)
		if (s[i] == '\\')
			s[i] = '/';
	return n;
}

/* Full AmigaOS normalisation: trim then fix separators. */
static inline size_t AmigaNormalizePath(char *s)
{
	TrimTrailingWhitespace(s);
	BackslashToSlash(s);
	return strlen(s);
}

/*
 * Fill `buf` with the `variant`-th case candidate used by the open-with-case-
 * fallback paths:
 *   0 = normalized as-is, 1 = all lower, 2 = all upper.
 * Returns the resulting length, or 0 when `variant` is out of range or there is
 * no room (the caller may still try variant 0). Shared by _fcaseopen, the
 * synchronous CdStream backend and the async loader so the three cannot drift.
 */
static inline size_t AmigaPathVariant(char *buf, size_t cap, const char *in,
                                      int variant)
{
	size_t n, i;

	if (cap == 0 || variant < 0 || variant > 2)
		return 0;
	n = strlen(in);
	if (n >= cap)
		n = cap - 1;
	memcpy(buf, in, n);
	buf[n] = '\0';
	n = AmigaNormalizePath(buf);
	if (variant == 1) {
		for (i = 0; i < n; i++)
			if (buf[i] >= 'A' && buf[i] <= 'Z')
				buf[i] += 32;
	} else if (variant == 2) {
		for (i = 0; i < n; i++)
			if (buf[i] >= 'a' && buf[i] <= 'z')
				buf[i] -= 32;
	}
	return n;
}

#endif /* __GTA_PATH_NORMALIZE_H__ */
