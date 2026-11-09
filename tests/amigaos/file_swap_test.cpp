/*
 * H3: save-file byte-swap unit test (src/save/FileSwap.h).
 *
 * saveFileSwap converts the little-endian save format to native order. It must
 * get each width right (u8 no-op, u16/u32/u64), swap aggregate structs
 * word-wise (CVector/CMatrix are arrays of 32-bit floats), recurse through
 * arrays, be involutional, and work on unaligned addresses (040/060 fault on
 * unaligned long access, so everything goes through memcpy/byte loops).
 *
 * The game's CRGBA overload (no-op, colours are byte-ordered) lives in
 * GenericGameStorage.cpp; here a stand-in byte struct proves overload
 * resolution prefers a user overload over the generic word-swap template.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes DH11:file_swap.log.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "FileSwap.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Independent reference: reverse the `n` bytes at p. */
static void ref_reverse(void *p, size_t n)
{
	uint8_t *b = (uint8_t*)p;
	for (size_t i = 0; i < n / 2; i++) {
		uint8_t t = b[i];
		b[i] = b[n - 1 - i];
		b[n - 1 - i] = t;
	}
}

/* A stand-in for the game's byte-ordered colour struct + its no-op overload. */
struct RGBA { uint8_t r, g, b, a; };
static inline void saveFileSwap(RGBA *p) { (void)p; }

/* Independent on-disk LE codec: the save format's bytes are little-endian no
 * matter the host, so build/parse the framing bytes by hand. */
static void store_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
static uint32_t load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main()
{
#ifdef AMIGAOS
	freopen("DH11:file_swap.log", "w", stdout);
#endif

	/* Scalars: exact swapped values. */
	{
		uint8_t u8 = 0x12; saveFileSwap(&u8); CHECK(u8 == 0x12);            /* width 1: no-op */
		uint16_t u16 = 0x1234; saveFileSwap(&u16); CHECK(u16 == 0x3412);
		uint32_t u32 = 0x12345678u; saveFileSwap(&u32); CHECK(u32 == 0x78563412u);
		uint64_t u64 = 0x0123456789ABCDEFull;
		saveFileSwap(&u64); CHECK(u64 == 0xEFCDAB8967452301ull);
	}

	/* Involution on every width. */
	{
		uint16_t a = 0xBEEF; uint16_t a0 = a; saveFileSwap(&a); saveFileSwap(&a); CHECK(a == a0);
		uint32_t b = 0xDEADBEEFu; uint32_t b0 = b; saveFileSwap(&b); saveFileSwap(&b); CHECK(b == b0);
		uint64_t c = 0x1122334455667788ull; uint64_t c0 = c;
		saveFileSwap(&c); saveFileSwap(&c); CHECK(c == c0);
	}

	/* CVector-like: 3 floats (12 bytes) -> each 4-byte word reversed. */
	{
		struct Vec3 { float x, y, z; };
		Vec3 v = { 1.0f, -2.5f, 3.25f };
		Vec3 ref = v;
		ref_reverse(&ref.x, 4); ref_reverse(&ref.y, 4); ref_reverse(&ref.z, 4);
		saveFileSwap(&v);
		CHECK(std::memcmp(&v, &ref, sizeof(v)) == 0);
	}

	/* CMatrix-like: 16 floats (64 bytes) -> every word reversed. */
	{
		struct Mat4 { float m[16]; };
		Mat4 m, ref;
		for (int i = 0; i < 16; i++) { m.m[i] = (float)(i + 1) * 0.5f; ref.m[i] = m.m[i]; }
		for (int i = 0; i < 16; i++) ref_reverse(&ref.m[i], 4);
		saveFileSwap(&m);
		CHECK(std::memcmp(&m, &ref, sizeof(m)) == 0);
	}

	/* Array overload swaps element-wise. */
	{
		uint32_t arr[4] = { 0x01020304u, 0xAABBCCDDu, 0u, 0xFFFFFFFFu };
		uint32_t ref[4];
		for (int i = 0; i < 4; i++) { ref[i] = arr[i]; saveFileSwap(&ref[i]); }
		saveFileSwap(&arr);
		CHECK(std::memcmp(arr, ref, sizeof(arr)) == 0);
	}

	/* Array of 12-byte structs (CVector[N]) - exercises the recursive path. */
	{
		struct Vec3 { float x, y, z; };
		Vec3 arr[3] = { { 1,2,3 }, { 4,5,6 }, { 7,8,9 } };
		Vec3 ref[3];
		for (int i = 0; i < 3; i++) {
			ref[i] = arr[i];
			ref_reverse(&ref[i].x, 4); ref_reverse(&ref[i].y, 4); ref_reverse(&ref[i].z, 4);
		}
		saveFileSwap(&arr);
		CHECK(std::memcmp(arr, ref, sizeof(arr)) == 0);
	}

	/* Unaligned address must be handled (040/060 alignment faults). */
	{
		uint8_t buf[8];
		std::memset(buf, 0, sizeof(buf));
		uint32_t v = 0x11223344u;
		std::memcpy(buf + 1, &v, 4);
		saveFileSwap((uint32_t*)(void*)(buf + 1));
		uint32_t got;
		std::memcpy(&got, buf + 1, 4);
		CHECK(got == 0x44332211u);
	}

	/* User overload beats the generic template (CRGBA must not be swapped). */
	{
		RGBA c = { 0x10, 0x20, 0x30, 0x40 };
		saveFileSwap(&c);
		CHECK(c.r == 0x10 && c.g == 0x20 && c.b == 0x30 && c.a == 0x40);
	}

	/* Swap16Array: exact values, involution, n == 0 no-op. */
	{
		uint16_t a[4] = { 0x1234, 0xABCD, 0x00FF, 0xFFFF };
		const uint16_t ref[4] = { 0x3412, 0xCDAB, 0xFF00, 0xFFFF };
		Swap16Array(a, 4);
		CHECK(std::memcmp(a, ref, sizeof(a)) == 0);
		Swap16Array(a, 4);
		CHECK(a[0] == 0x1234 && a[1] == 0xABCD && a[2] == 0x00FF && a[3] == 0xFFFF);
		Swap16Array(a, 0);
		CHECK(a[0] == 0x1234);
	}

	/* Swap16Array must tolerate an unaligned header address. */
	{
		uint8_t buf[6];
		uint16_t v = 0x1234, got;
		std::memcpy(buf + 1, &v, 2);
		Swap16Array((uint16_t*)(void*)(buf + 1), 1);
		std::memcpy(&got, buf + 1, 2);
		CHECK(got == 0x3412);
	}

	/* Save-header round-trip: native -> stored LE -> native, element by element.
	 * Layout mirrors { wchar/wchar FileName[24]; SYSTEMTIME SaveDateTime; }. */
	{
		struct SaveHeaderFields { uint16_t name[24]; uint16_t time[8]; } h, stored;
		for (int i = 0; i < 24; i++) h.name[i] = (uint16_t)(0x0100 + i * 0x0101);
		for (int i = 0; i < 8; i++)  h.time[i] = (uint16_t)(2000 + i * 3);
		std::memcpy(&stored, &h, sizeof(h));
		Swap16Array(stored.name, 24);
		Swap16Array(stored.time, 8);
		int ok = 1;
		for (int i = 0; i < 24; i++) {
			uint16_t want = (uint16_t)((h.name[i] >> 8) | (h.name[i] << 8));
			if (stored.name[i] != want) ok = 0;
		}
		for (int i = 0; i < 8; i++) {
			uint16_t want = (uint16_t)((h.time[i] >> 8) | (h.time[i] << 8));
			if (stored.time[i] != want) ok = 0;
		}
		CHECK(ok);
		Swap16Array(stored.name, 24);
		Swap16Array(stored.time, 8);
		CHECK(std::memcmp(&stored, &h, sizeof(h)) == 0);
	}

	/* Framing: the 4-byte block size lands on disk little-endian on every host
	 * and SAVE_LE32 converts it back to native (identity on LE). */
	{
		uint32_t size = 201729u; /* 0x00031401 */
		uint32_t on_disk = SAVE_LE32(size);
		uint8_t bytes[4], expect[4];
		std::memcpy(bytes, &on_disk, 4);
		store_le32(expect, size);
		CHECK(std::memcmp(bytes, expect, 4) == 0);
		CHECK(bytes[0] == 0x01 && bytes[1] == 0x14 && bytes[2] == 0x03 && bytes[3] == 0x00);
		CHECK(load_le32(bytes) == size);
		/* Reader side: CFileMgr::Read drops the 4 raw bytes into a uint32, so on
		 * BE the in-memory value is byte-reversed; SAVE_LE32 converts it back. */
		uint32_t native_read;
		std::memcpy(&native_read, bytes, 4);
		CHECK(SAVE_LE32(native_read) == size);
	}

	/* Byte-sum checksum over the raw LE size bytes is host-independent, so the
	 * writer's shifted sum (PcClassSaveRoutine) matches the reader's raw-byte sum
	 * (CheckDataNotCorrupt) unchanged. */
	{
		uint32_t size = 201729u;
		uint32_t on_disk = SAVE_LE32(size);
		uint8_t bytes[4];
		std::memcpy(bytes, &on_disk, 4);
		int writer = (int)(size & 0xFF) + (int)((size >> 8) & 0xFF) +
		             (int)((size >> 16) & 0xFF) + (int)((size >> 24) & 0xFF);
		int reader = bytes[0] + bytes[1] + bytes[2] + bytes[3];
		CHECK(writer == reader);
	}

	/* Nested per-block framing: CopySizeAndPreparePointer must store the inner
	 * size little-endian so ReadDataFromBlock (which byte-swaps on BE) reads it
	 * back correctly; the outer PcClassSaveRoutine prefix is LE too. */
	{
		uint32_t data = 291u;          /* 0x00000123 */
		uint32_t outer = 296u;         /* 4 + align4(291) */
		uint32_t nestedLE = SAVE_LE32(data);
		uint32_t outerLE = SAVE_LE32(outer);
		uint8_t inner[4], ob[4];
		std::memcpy(inner, &nestedLE, 4);
		std::memcpy(ob, &outerLE, 4);
		/* On disk the framing bytes are LE regardless of host. */
		CHECK(inner[0] == 0x23 && inner[1] == 0x01 && inner[2] == 0x00 && inner[3] == 0x00);
		CHECK(ob[0] == 0x28 && ob[1] == 0x01 && ob[2] == 0x00 && ob[3] == 0x00);
		/* Reader: raw read then ReadDataFromBufferPointer (swap only on BE). */
		uint32_t got_inner;
		std::memcpy(&got_inner, inner, 4);
#ifdef BIGENDIAN
		saveFileSwap(&got_inner);
#endif
		CHECK(got_inner == data);
	}

	if (failures) {
		std::printf("file_swap_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("file_swap_test: all ok\n");
	return 0;
}
