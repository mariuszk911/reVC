/*
 * H2: real-asset endian round-trip (GXT).
 *
 * Verifies that the byte-swap the game applies to a shipped PC asset produces
 * the same native values as the host-independent ReadLE* helpers. This is the
 * exact contract the AmigaOS big-endian loaders rely on: on the 68k build the
 * RW_SWAP/array swaps must turn little-endian file bytes into native values; on
 * the LE host they are no-ops. By comparing a *swapped* struct against a
 * ReadLE-derived reference we get a meaningful assertion on both hosts.
 *
 * The fixture is the shipped American GXT (TEXT/american.gxt). If it is not
 * found the test reports SKIP (assets are not in every checkout/CI).
 *
 * On AmigaOS it writes DH11:asset_endian.log and takes the asset path as argv[1].
 * The harness stages the fixture as DH11:american.gxt; the defaults also try
 * the staged game tree (DH11:reVc_pistorm3d/TEXT/american.gxt, the real location
 * -- there is no gamefiles/ subdir) and the source-tree copies. It never
 * touches reVC.log.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Endian.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { printf("ok   %s\n", #cond); } \
	else { printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* The game's CKeyEntry on a 32-bit target: uint32 valueOffset + char key[8]. */
typedef struct {
	uint32_t valueOffset;
	char key[8];
} KeyEntry;

static unsigned char *loadFile(const char *path, long *outLen)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long len;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) { fclose(f); return NULL; }
	buf = (unsigned char*)malloc((size_t)len);
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf); fclose(f); return NULL;
	}
	fclose(f);
	*outLen = len;
	return buf;
}

/* Find a chunk by magic; fills data offset (past the 8-byte header) and size. */
static int findChunk(const unsigned char *d, long len, const char *magic,
                     long *dataOff, int32_t *size)
{
	long off = 0;
	while (off + 8 <= len) {
		int32_t sz = (int32_t)ReadLE32(d + off + 4);
		long avail = len - (off + 8);	/* bytes the declared size may span */
		if (memcmp(d + off, magic, 4) == 0) {
			/* Reject a chunk whose declared size overflows or runs past EOF. */
			if (sz < 0 || (long)sz > avail)
				return 0;
			*dataOff = off + 8;
			*size = sz;
			return 1;
		}
		/* Stop before an unchecked size can wrap `off` past len. */
		if (sz <= 0 || (long)sz > avail)
			break;
		off += 8 + sz;
	}
	return 0;
}

int main(int argc, char **argv)
{
#ifdef AMIGAOS
	freopen("DH11:asset_endian.log", "w", stdout);
#endif

	const char *paths[6];
	int np = 0;
	if (argc > 1) paths[np++] = argv[1];
	paths[np++] = "DH11:american.gxt";
	paths[np++] = "DH11:reVc_pistorm3d/TEXT/american.gxt";
	paths[np++] = "gamefiles/TEXT/american.gxt";
	paths[np++] = "../../gamefiles/TEXT/american.gxt";

	unsigned char *d = NULL;
	long len = 0;
	for (int i = 0; i < np && !d; i++)
		d = loadFile(paths[i], &len);

	if (!d) {
		printf("asset_endian_test: SKIP (american.gxt not found)\n");
		return 0;
	}
	printf("asset: %ld bytes\n", len);

	long tkeyOff = 0, tdatOff = 0, tablOff = 0;
	int32_t tkeySz = 0, tdatSz = 0, tablSz = 0;
	int haveKey = findChunk(d, len, "TKEY", &tkeyOff, &tkeySz);
	int haveDat = findChunk(d, len, "TDAT", &tdatOff, &tdatSz);
	int haveTbl = findChunk(d, len, "TABL", &tablOff, &tablSz);
	CHECK(haveKey && haveDat && haveTbl);

	if (haveKey && haveDat && haveTbl) {
		int nkey = tkeySz / (int)sizeof(KeyEntry);
		int ntab = tablSz / 12;
		printf("TKEY entries=%d TDAT chars=%d TABL entries=%d\n",
		       nkey, tdatSz / 2, ntab);
		CHECK(nkey == 2499);
		CHECK(tdatSz / 2 == 78215);
		CHECK(ntab == 79);
		CHECK(tdatSz > 0 && (tdatSz % 2) == 0);

		/* TKEY: loader swaps the 32-bit valueOffset of every entry. The
		 * key bytes must be left untouched. */
		{
			int i, bad = 0;
			for (i = 0; i < nkey; i++) {
				const unsigned char *raw = d + tkeyOff + i * sizeof(KeyEntry);
				KeyEntry e;
				uint32_t ref = ReadLE32(raw);
				memcpy(&e, raw, sizeof(e));
				/* exactly what CKeyArray::Load does */
				RW_SWAP32_ARR(&e.valueOffset, 1);
				if (e.valueOffset != ref) bad++;
				if (memcmp(e.key, raw + 4, 8) != 0) bad++;
			}
			CHECK(bad == 0);
			printf("TKEY swap mismatches=%d\n", bad);
		}

		/* TDAT: loader swaps every UTF-16 code unit. */
		if (tdatSz > 0 && (tdatSz % 2) == 0) {
			int nchars = tdatSz / 2, i, bad = 0;
			unsigned char *copy = (unsigned char*)malloc((size_t)tdatSz);
			CHECK(copy != NULL);
			if (copy) {
				memcpy(copy, d + tdatOff, (size_t)tdatSz);
				/* exactly what CData::Load does */
				RW_SWAP16_ARR(copy, (size_t)nchars);
				for (i = 0; i < nchars; i++) {
					uint16_t got, ref = ReadLE16(d + tdatOff + i * 2);
					memcpy(&got, copy + i * 2, 2);
					if (got != ref) bad++;
				}
				CHECK(bad == 0);
				printf("TDAT swap mismatches=%d\n", bad);
				/* First code unit of the American GXT is '~'. */
				CHECK(ReadLE16(d + tdatOff) == '~');
				free(copy);
			}
		}

		/* Concrete key -> string lookup on the swapped data. Mirrors
		 * CKeyArray::Update/Search: value = data + valueOffset. */
		{
			int i, foundAccura = 0, foundAnswer = 0;
			char accura[32] = {0};
			for (i = 0; i < nkey; i++) {
				const unsigned char *raw = d + tkeyOff + i * sizeof(KeyEntry);
				KeyEntry e;
				memcpy(&e, raw, sizeof(e));
				RW_SWAP32_ARR(&e.valueOffset, 1);
				if (memcmp(e.key, "ACCURA\0\0", 8) == 0) {
					foundAccura = 1;
					CHECK(e.valueOffset == 40996);
					/* decode UTF-16LE at TDAT+offset, bounded by TDAT */
					int j = 0;
					if (e.valueOffset < (uint32_t)tdatSz) {
						int maxj = (int)((tdatSz - (int32_t)e.valueOffset) / 2);
						if (maxj > 31) maxj = 31;
						while (j < maxj) {
							uint16_t c = ReadLE16(d + tdatOff + e.valueOffset + j * 2);
							if (c == 0) break;
							accura[j++] = (char)c;
						}
					}
					accura[j] = 0;
				}
				if (memcmp(e.key, "ANSWER\0\0", 8) == 0)
					foundAnswer = 1;
			}
			CHECK(foundAccura);
			CHECK(foundAnswer);
			CHECK(strcmp(accura, "Accuracy") == 0);
			printf("ACCURA -> '%s'\n", accura);
		}
	}

	free(d);

	if (failures) {
		printf("asset_endian_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("asset_endian_test: all ok\n");
	return 0;
}
