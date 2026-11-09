#ifndef __GTA_REPLAY_ENDIAN_H__
#define __GTA_REPLAY_ENDIAN_H__

/*
 * Replay-file (replay.rep) endianness, M3.
 *
 * CReplay::SaveReplayToHD dumps the raw in-memory packet stream (whole
 * uint8[REPLAYBUFFERSIZE] record buffers) after an 8-byte "gta3_7f" magic;
 * PlayReplayFromHD reads them back raw.  Those buffers are native-endian, so a
 * PC (LE) file is garbled on 68k (BE) and vice-versa.
 *
 * To keep the on-disk format little-endian (matching the save-game approach in
 * src/save/FileSwap.h) the buffers are walked packet-by-packet and only
 * the multi-byte fields are byte-reversed by an involution, so the same
 * function serves save and load.  Byte fields, the CCompressedMatrixNotAligned
 * int8 rotation and the tMiscPacket bitfield byte are left alone.
 *
 * The helpers here are unconditional (they always reverse) so the host (LE)
 * unit test exercises the real code; Replay.cpp only calls them under
 * BIGENDIAN, so on LE the file stays the native LE dump it always was.
 *
 * This header is dependency-free C++ (only Endian.h) so the descriptor tables
 * can be unit-tested without the game headers.  The numeric offsets are pinned
 * against the real structs by static_assert in Replay.cpp.
 *
 * ABI note: the AmigaOS build uses the 68k default (2-byte aligned int/float),
 * so e.g. sizeof(tGeneralPacket) is 84, not the 88 documented by the disabled
 * VALIDATE_SIZE.  Do not add -malign-int.
 */

#include <stdint.h>
#include <stddef.h>

#include "Endian.h"

/* Packet ids, mirroring CReplay::REPLAYPACKET_* (Replay.h).  Distinct names so
 * they can coexist with the class' private enum. */
enum {
	REPLAY_PKT_END = 0,
	REPLAY_PKT_VEHICLE,
	REPLAY_PKT_BIKE,
	REPLAY_PKT_PED_HEADER,
	REPLAY_PKT_PED_UPDATE,
	REPLAY_PKT_GENERAL,
	REPLAY_PKT_CLOCK,
	REPLAY_PKT_WEATHER,
	REPLAY_PKT_ENDOFFRAME,
	REPLAY_PKT_TIMER,
	REPLAY_PKT_BULLET_TRACES,
	REPLAY_PKT_PARTICLE,
	REPLAY_PKT_MISC,
	REPLAY_PKT_COUNT
};

/* One run of `count` multi-byte scalars of `width` bytes at `offset` from the
 * start of the packet.  width is 2 or 4; the scalars must be contiguous. */
struct ReplayField {
	uint16_t offset;
	uint8_t width;
	uint8_t count;
};

/* Native packet sizes, indexed by packet id.  These are the 68k 2-byte-aligned
 * sizes; Replay.cpp static_asserts them against sizeof() on the target. */
static constexpr size_t kReplayPacketSize[REPLAY_PKT_COUNT] = {
	4,	/* END */
	48,	/* VEHICLE */
	42,	/* BIKE */
	8,	/* PED_HEADER */
	40,	/* PED_UPDATE */
	84,	/* GENERAL */
	4,	/* CLOCK */
	8,	/* WEATHER */
	4,	/* ENDOFFRAME */
	6,	/* TIMER */
	28,	/* BULLET_TRACES */
	20,	/* PARTICLE */
	12	/* MISC */
};

/* Per-type swapped fields.  Offsets are native (2-byte ABI) offsets from the
 * packet start; Replay.cpp static_asserts them against offsetof(). */
static constexpr ReplayField kReplayFieldsGeneral[] = {
	{ 2, 4, 16 },	/* CMatrix camera_pos: 16 floats (value region only) */
	{ 72, 4, 3 }	/* CVector player_pos */
};
static constexpr ReplayField kReplayFieldsVehicle[] = {
	{ 4, 4, 3 },	/* CCompressedMatrixNotAligned matrix.m_vecPos */
	{ 24, 2, 1 },	/* uint16 mi */
	{ 26, 4, 1 }	/* uint32 panels */
};
static constexpr ReplayField kReplayFieldsBike[] = {
	{ 4, 4, 3 },	/* matrix.m_vecPos */
	{ 24, 2, 1 }	/* uint16 mi */
};
static constexpr ReplayField kReplayFieldsPedHeader[] = {
	{ 2, 2, 1 }	/* uint16 mi */
};
static constexpr ReplayField kReplayFieldsPedUpdate[] = {
	{ 18, 4, 3 },	/* matrix.m_vecPos */
	{ 36, 2, 1 }	/* uint16 weapon_model */
};
static constexpr ReplayField kReplayFieldsWeather[] = {
	{ 4, 4, 1 }	/* float interpolation */
};
static constexpr ReplayField kReplayFieldsTimer[] = {
	{ 2, 4, 1 }	/* uint32 timer */
};
static constexpr ReplayField kReplayFieldsBulletTraces[] = {
	{ 4, 4, 3 },	/* CVector inf */
	{ 16, 4, 3 }	/* CVector sup */
};
static constexpr ReplayField kReplayFieldsParticle[] = {
	{ 10, 2, 3 },	/* int16 pos_x/pos_y/pos_z */
	{ 16, 4, 1 }	/* float size */
};
static constexpr ReplayField kReplayFieldsMisc[] = {
	{ 2, 4, 1 },	/* uint32 cam_shake_start */
	{ 6, 4, 1 }	/* float cam_shake_strength */
};

static inline size_t replayPacketSize(uint8_t type)
{
	return type < REPLAY_PKT_COUNT ? kReplayPacketSize[type] : 0;
}

/* Returns the swapped-field table for a packet id and stores its length in *n.
 * Packets with no multi-byte fields return 0 with *n == 0. */
static inline const ReplayField *replayFields(uint8_t type, int *n)
{
	switch (type) {
	case REPLAY_PKT_GENERAL:		*n = 2; return kReplayFieldsGeneral;
	case REPLAY_PKT_VEHICLE:		*n = 3; return kReplayFieldsVehicle;
	case REPLAY_PKT_BIKE:			*n = 2; return kReplayFieldsBike;
	case REPLAY_PKT_PED_HEADER:		*n = 1; return kReplayFieldsPedHeader;
	case REPLAY_PKT_PED_UPDATE:		*n = 2; return kReplayFieldsPedUpdate;
	case REPLAY_PKT_WEATHER:		*n = 1; return kReplayFieldsWeather;
	case REPLAY_PKT_TIMER:			*n = 1; return kReplayFieldsTimer;
	case REPLAY_PKT_BULLET_TRACES:	*n = 2; return kReplayFieldsBulletTraces;
	case REPLAY_PKT_PARTICLE:		*n = 2; return kReplayFieldsParticle;
	case REPLAY_PKT_MISC:			*n = 2; return kReplayFieldsMisc;
	default:						*n = 0; return 0;
	}
}

/* Byte-wise, unaligned-safe in-place reversal of every field in the table.
 * Unconditional: it always reverses bytes, so calling it twice restores the
 * input (involution).  Callers on LE simply never call it. */
static inline void replaySwapFields(uint8_t *packet, const ReplayField *f, int n)
{
	for (int i = 0; i < n; i++) {
		uint8_t *p = packet + f[i].offset;
		if (f[i].width == 2)
			Swap16Array(p, f[i].count);
		else
			Swap32Array(p, f[i].count);
	}
}

/* Walk one replay buffer packet-by-packet until the END packet, reversing the
 * multi-byte fields of each packet.  `len` is the buffer size: the walk stops
 * at the end and refuses to touch a packet that would straddle it, so a
 * truncated or END-less replay file cannot read or write out of bounds.  The
 * packet sizes keep the walk aligned to real packet starts, so this is exact
 * for a well-formed buffer. */
static inline void replaySwapBuffer(uint8_t *buf, size_t len)
{
	size_t offset = 0;
	while (offset < len && buf[offset] != REPLAY_PKT_END) {
		size_t size = replayPacketSize(buf[offset]);
		if (size == 0 || size > len - offset)
			break;	/* unknown id or truncated packet: stop */
		int n = 0;
		const ReplayField *f = replayFields(buf[offset], &n);
		if (f && n)
			replaySwapFields(buf + offset, f, n);
		offset += size;
	}
}

#endif /* __GTA_REPLAY_ENDIAN_H__ */
