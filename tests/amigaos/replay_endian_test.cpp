/*
 * M3: replay-file byte-swap unit test (src/control/ReplayEndian.h).
 *
 * replaySwapBuffer() walks a native replay packet stream and byte-reverses the
 * multi-byte fields of each packet, so the same function turns a 68k (BE)
 * buffer into the little-endian on-disk form and back (it is an involution).
 * Byte fields, the CCompressedMatrixNotAligned int8 rotation and the
 * tMiscPacket bitfield byte must be left untouched.
 *
 * The packet sizes/offsets live in the header's tables; Replay.cpp pins them
 * against the real structs with static_assert on the target.  This test pins
 * the tables' *values* and the swap/walk mechanics independently.
 *
 * Runs on the host and cross-linked for 68k; on AmigaOS writes
 * DH11:replay_endian.log.  The descriptor offsets are the 68k 2-byte-aligned
 * ABI (see ReplayEndian.h).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ReplayEndian.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

static void reverse_scalar(uint8_t *p, int width)
{
	for (int i = 0; i < width / 2; i++) {
		uint8_t t = p[i];
		p[i] = p[width - 1 - i];
		p[width - 1 - i] = t;
	}
}

/* Reverse each descriptor run in an already-copied destination buffer. */
static void ref_apply(uint8_t *dst, size_t packet, const ReplayField *f, int n)
{
	for (int i = 0; i < n; i++)
		for (int s = 0; s < f[i].count; s++)
			reverse_scalar(dst + packet + f[i].offset + s * f[i].width, f[i].width);
}

int main()
{
#ifdef AMIGAOS
	freopen("DH11:replay_endian.log", "w", stdout);
#endif

	/* replaySwapFields: exact byte KAT + involution, both widths, count > 1,
	 * and a deliberately odd (unaligned) base pointer. */
	{
		uint8_t a[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
		const ReplayField f1[] = { { 1, 4, 1 } };	/* unaligned u32 at +1 */
		const ReplayField f2[] = { { 0, 2, 3 } };	/* three u16 */
		uint8_t b[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
		uint8_t a0[6], b0[6];
		std::memcpy(a0, a, 6);
		std::memcpy(b0, b, 6);

		replaySwapFields(a, f1, 1);
		CHECK(a[0] == 0x11 && a[1] == 0x55 && a[2] == 0x44 && a[3] == 0x33 && a[4] == 0x22 && a[5] == 0x66);
		replaySwapFields(a, f1, 1);
		CHECK(std::memcmp(a, a0, 6) == 0);

		replaySwapFields(b, f2, 1);
		CHECK(b[0] == 0x22 && b[1] == 0x11 && b[2] == 0x44 && b[3] == 0x33 && b[4] == 0x66 && b[5] == 0x55);
		replaySwapFields(b, f2, 1);
		CHECK(std::memcmp(b, b0, 6) == 0);
	}

	/* replayPacketSize: exact native 68k sizes for all 13 ids (+ unknown). */
	{
		const size_t want[REPLAY_PKT_COUNT] = {
			4, 48, 42, 8, 40, 84, 4, 8, 4, 6, 28, 20, 12
		};
		int ok = 1;
		for (int t = 0; t < REPLAY_PKT_COUNT; t++)
			if (replayPacketSize((uint8_t)t) != want[t])
				ok = 0;
		CHECK(ok);
		CHECK(replayPacketSize(REPLAY_PKT_END) == 4);
		CHECK(replayPacketSize(REPLAY_PKT_VEHICLE) == 48);
		CHECK(replayPacketSize(REPLAY_PKT_BIKE) == 42);
		CHECK(replayPacketSize(REPLAY_PKT_PED_UPDATE) == 40);
		CHECK(replayPacketSize(REPLAY_PKT_GENERAL) == 84);
		CHECK(replayPacketSize(REPLAY_PKT_TIMER) == 6);
		CHECK(replayPacketSize(REPLAY_PKT_MISC) == 12);
		CHECK(replayPacketSize(REPLAY_PKT_COUNT) == 0);
		CHECK(replayPacketSize(99) == 0);
	}

	/* replayFields: per-type counts. */
	{
		int n = -1;
		CHECK(replayFields(REPLAY_PKT_GENERAL, &n) == kReplayFieldsGeneral && n == 2);
		CHECK(replayFields(REPLAY_PKT_VEHICLE, &n) == kReplayFieldsVehicle && n == 3);
		CHECK(replayFields(REPLAY_PKT_PED_UPDATE, &n) == kReplayFieldsPedUpdate && n == 2);
		CHECK(replayFields(REPLAY_PKT_PARTICLE, &n) == kReplayFieldsParticle && n == 2);
		CHECK(replayFields(REPLAY_PKT_CLOCK, &n) == 0 && n == 0);
		CHECK(replayFields(REPLAY_PKT_END, &n) == 0 && n == 0);
	}

	/* Single-packet KAT with hand-written offsets: TIMER (type 9) = 1-byte
	 * type + u32 timer at +2, size 6. */
	{
		uint8_t buf[10];
		std::memset(buf, 0, sizeof(buf));
		buf[0] = REPLAY_PKT_TIMER;
		buf[2] = 0x44; buf[3] = 0x33; buf[4] = 0x22; buf[5] = 0x11;
		buf[6] = REPLAY_PKT_END;
		replaySwapBuffer(buf, sizeof(buf));
		CHECK(buf[0] == REPLAY_PKT_TIMER && buf[1] == 0x00);
		CHECK(buf[2] == 0x11 && buf[3] == 0x22 && buf[4] == 0x33 && buf[5] == 0x44);
		CHECK(buf[6] == REPLAY_PKT_END && buf[7] == 0x00);
		replaySwapBuffer(buf, sizeof(buf));
		CHECK(buf[2] == 0x44 && buf[3] == 0x33 && buf[4] == 0x22 && buf[5] == 0x11);
	}

	/* Multi-packet walk: GENERAL, VEHICLE, PED_UPDATE, TIMER, END at their
	 * native sizes.  Every non-swapped byte must be untouched and the swap
	 * must be an involution. */
	{
		const size_t genOff = 0, vehOff = 84, pedOff = 132, timOff = 172, endOff = 178;
		const size_t total = endOff + 4;
		uint8_t buf[256], orig[256], expect[256], again[256];
		for (size_t i = 0; i < total; i++)
			buf[i] = (uint8_t)(i * 37 + 11);
		buf[genOff] = REPLAY_PKT_GENERAL;
		buf[vehOff] = REPLAY_PKT_VEHICLE;
		buf[pedOff] = REPLAY_PKT_PED_UPDATE;
		buf[timOff] = REPLAY_PKT_TIMER;
		buf[endOff] = REPLAY_PKT_END;
		std::memcpy(orig, buf, sizeof(buf));

		std::memcpy(expect, buf, sizeof(buf));
		ref_apply(expect, genOff, kReplayFieldsGeneral, 2);
		ref_apply(expect, vehOff, kReplayFieldsVehicle, 3);
		ref_apply(expect, pedOff, kReplayFieldsPedUpdate, 2);
		ref_apply(expect, timOff, kReplayFieldsTimer, 1);

		replaySwapBuffer(buf, sizeof(buf));
		CHECK(std::memcmp(buf, expect, total) == 0);
		/* Non-swapped bytes really are unchanged: type bytes and the trailing
		 * bytes after END were never in a descriptor range. */
		CHECK(buf[genOff] == REPLAY_PKT_GENERAL && buf[genOff + 1] == orig[genOff + 1]);
		CHECK(buf[vehOff] == REPLAY_PKT_VEHICLE && buf[pedOff] == REPLAY_PKT_PED_UPDATE);
		CHECK(buf[endOff] == REPLAY_PKT_END && buf[endOff + 1] == orig[endOff + 1]);
		/* Make sure at least one swapped value actually changed. */
		CHECK(std::memcmp(buf, orig, total) != 0);

		std::memcpy(again, buf, sizeof(buf));
		replaySwapBuffer(again, sizeof(again));
		CHECK(std::memcmp(again, orig, total) == 0);
	}

	/* replaySwapBuffer: a buffer that starts with END is a no-op, and an
	 * unknown packet id cannot loop forever. */
	{
		uint8_t buf[8] = { REPLAY_PKT_END, 0xAA, 0xBB, 0xCC, 0, 0, 0, 0 };
		uint8_t orig[8];
		std::memcpy(orig, buf, sizeof(buf));
		replaySwapBuffer(buf, sizeof(buf));
		CHECK(std::memcmp(buf, orig, sizeof(buf)) == 0);

		uint8_t bad[8] = { 99, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
		uint8_t bad0[8];
		std::memcpy(bad0, bad, sizeof(bad));
		replaySwapBuffer(bad, sizeof(bad));
		CHECK(std::memcmp(bad, bad0, sizeof(bad)) == 0);
	}

	/* Bound regression: a packet that would straddle `len` must be neither
	 * swapped nor read past the end.  VEHICLE (48) fits at 0; GENERAL (84)
	 * starts at 48 but 48+84 > 100, so only the VEHICLE swap may happen. */
	{
		uint8_t b[100], expect[100];
		for (size_t i = 0; i < sizeof(b); i++)
			b[i] = (uint8_t)(i * 5 + 1);
		b[0] = REPLAY_PKT_VEHICLE;
		b[48] = REPLAY_PKT_GENERAL;
		std::memcpy(expect, b, sizeof(b));
		ref_apply(expect, 0, kReplayFieldsVehicle, 3);
		replaySwapBuffer(b, sizeof(b));
		CHECK(std::memcmp(b, expect, sizeof(b)) == 0);
	}

	if (failures) {
		std::printf("replay_endian_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("replay_endian_test: all ok\n");
	return 0;
}
