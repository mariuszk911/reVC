/*
 * M3 endianness unit test.
 *
 * Builds on the host (little-endian) and for m68k (big-endian) with
 * -DBIGENDIAN. It only uses the unconditional primitives for the byte-level
 * assertions, so it must pass identically on both hosts.
 */
#include <stdio.h>
#include <string.h>

#include "Endian.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { printf("ok   %s\n", #cond); } \
	else { printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

int main(void)
{
#ifdef AMIGAOS
	freopen("DH11:endian_test.log", "w", stdout);
#endif
	/* Unaligned little-endian reads. */
	unsigned char le[8];
	le[0] = 0x78; le[1] = 0x56; le[2] = 0x34; le[3] = 0x12;
	le[4] = 0xEF; le[5] = 0xCD; le[6] = 0xAB; le[7] = 0x90;

	CHECK(ReadLE16(le) == 0x5678);
	CHECK(ReadLE32(le) == 0x12345678u);
	CHECK(ReadLE32(le + 4) == 0x90ABCDEFu);

	/* In-place byte reversal is an involution and works unaligned. */
	unsigned char buf[9];
	memset(buf, 0xAA, sizeof(buf));
	memcpy(buf + 1, le, 4);		/* unaligned group: 78 56 34 12 */
	Swap32Array(buf + 1, 1);
	CHECK(buf[1] == 0x12 && buf[2] == 0x34 && buf[3] == 0x56 && buf[4] == 0x78);

	uint16_t v16 = 0x1234;
	CHECK(Swap16(v16) == 0x3412);
	CHECK(Swap16(Swap16(v16)) == v16);

	uint32_t v32 = 0x12345678u;
	CHECK(Swap32(v32) == 0x78563412u);
	CHECK(Swap32(Swap32(v32)) == v32);

	uint64_t v64 = 0x0123456789ABCDEFull;
	CHECK(Swap64(Swap64(v64)) == v64);

	float f = -1234.5f;
	CHECK(SwapFloat(SwapFloat(f)) == f);
	CHECK(SwapFloat(0.0f) == 0.0f);	/* all-zero bytes are invariant */

	/* SwapFloat must reverse exactly the four bytes of the value. */
	uint32_t fb, rev;
	float expect;
	memcpy(&fb, &f, 4);
	rev = Swap32(fb);
	memcpy(&expect, &rev, 4);
	CHECK(SwapFloat(f) == expect);

	/* RW_SWAP* behaviour depends on host endianness. */
#ifdef BIGENDIAN
	CHECK(RW_SWAP32(v32) == 0x78563412u);
	CHECK(RW_SWAP16(v16) == 0x3412);
#else
	CHECK(RW_SWAP32(v32) == v32);
	CHECK(RW_SWAP16(v16) == v16);
#endif

	/* RW_SWAP*_ARR must be a semantic no-op on LE and a reversal on BE. */
	unsigned char arr[8];
	memcpy(arr, le, 8);
	RW_SWAP32_ARR(arr, 2);
#ifdef BIGENDIAN
	CHECK(arr[0] == 0x12 && arr[1] == 0x34 && arr[2] == 0x56 && arr[3] == 0x78);
#else
	CHECK(memcmp(arr, le, 8) == 0);
#endif

	if (failures) {
		printf("endian_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("endian_test: all ok\n");
	return 0;
}
