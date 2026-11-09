#ifndef __GTA_ENDIAN_H__
#define __GTA_ENDIAN_H__

/*
 * Byte-wise, unaligned-safe endianness helpers for the AmigaOS 68k port (M3).
 *
 * The shipped PC assets (DFF/TXD/IFP/IMG/COL/saves) are little-endian, but the
 * game reads them with RwStreamRead()/fread() into raw buffers and then casts
 * those buffers to int/float. 68000/010 fault on unaligned word/long access and
 * 040/060 take alignment exceptions, so every helper here touches bytes only.
 *
 * The unconditional primitives (Swap16/32/64, Swap*Array, ReadLE*) are host
 * endianness independent and can be used in unit tests on any machine. The
 * RW_SWAP* macros compile to the matching reverse on BIGENDIAN and to a no-op
 * on little-endian hosts, so call sites stay unchanged on x86.
 *
 * This header is includable from C and C++.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Unconditional byte reversal ---------------------------------------- */

static inline uint16_t Swap16(uint16_t v)
{
	return (uint16_t)((uint16_t)(v << 8) | (uint16_t)(v >> 8));
}

static inline uint32_t Swap32(uint32_t v)
{
	return ((v & 0x000000FFu) << 24) |
	       ((v & 0x0000FF00u) << 8)  |
	       ((v & 0x00FF0000u) >> 8)  |
	       ((v & 0xFF000000u) >> 24);
}

static inline uint64_t Swap64(uint64_t v)
{
	return ((uint64_t)Swap32((uint32_t)v) << 32) | (uint64_t)Swap32((uint32_t)(v >> 32));
}

static inline float SwapFloat(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	u = Swap32(u);
	memcpy(&f, &u, sizeof(f));
	return f;
}

static inline double SwapDouble(double d)
{
	uint64_t u;
	memcpy(&u, &d, sizeof(u));
	u = Swap64(u);
	memcpy(&d, &u, sizeof(d));
	return d;
}

/* In-place reversal of an array of scalars; unaligned-safe, no-op is done by
 * the RW_SWAP*_ARR macros below, these always really reverse. */
static inline void Swap16Array(void *data, size_t count)
{
	uint8_t *b = (uint8_t*)data;
	while (count--) {
		uint8_t t = b[0];
		b[0] = b[1];
		b[1] = t;
		b += 2;
	}
}

static inline void Swap32Array(void *data, size_t count)
{
	uint8_t *b = (uint8_t*)data;
	while (count--) {
		uint8_t t0 = b[0], t1 = b[1];
		b[0] = b[3];
		b[1] = b[2];
		b[2] = t1;
		b[3] = t0;
		b += 4;
	}
}

static inline void Swap64Array(void *data, size_t count)
{
	uint8_t *b = (uint8_t*)data;
	while (count--) {
		uint8_t t0 = b[0], t1 = b[1], t2 = b[2], t3 = b[3];
		b[0] = b[7];
		b[1] = b[6];
		b[2] = b[5];
		b[3] = b[4];
		b[4] = t3;
		b[5] = t2;
		b[6] = t1;
		b[7] = t0;
		b += 8;
	}
}

/* ---- Read little-endian values from unaligned bytes --------------------- */
/* These return the numeric value regardless of host endianness. */

static inline uint16_t ReadLE16(const void *p)
{
	const uint8_t *b = (const uint8_t*)p;
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t ReadLE32(const void *p)
{
	const uint8_t *b = (const uint8_t*)p;
	return ((uint32_t)b[0])        |
	       ((uint32_t)b[1] << 8)   |
	       ((uint32_t)b[2] << 16)  |
	       ((uint32_t)b[3] << 24);
}

static inline float ReadLEFloat(const void *p)
{
	uint32_t u = ReadLE32(p);
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

/* ---- Host-aware conversion macros ---------------------------------------- */
/* On BIGENDIAN these byte-reverse; on little-endian they vanish entirely. */

#if defined(BIGENDIAN)

#define RW_SWAP16(x)       Swap16((uint16_t)(x))
#define RW_SWAP32(x)       Swap32((uint32_t)(x))
#define RW_SWAP64(x)       Swap64((uint64_t)(x))
#define RW_SWAPF(x)        SwapFloat(x)
#define RW_SWAPD(x)        SwapDouble(x)
#define RW_SWAP16_ARR(p,n) Swap16Array((p), (n))
#define RW_SWAP32_ARR(p,n) Swap32Array((p), (n))
#define RW_SWAP64_ARR(p,n) Swap64Array((p), (n))

#else

#define RW_SWAP16(x)       (x)
#define RW_SWAP32(x)       (x)
#define RW_SWAP64(x)       (x)
#define RW_SWAPF(x)        (x)
#define RW_SWAPD(x)        (x)
#define RW_SWAP16_ARR(p,n) ((void)(p), (void)(n))
#define RW_SWAP32_ARR(p,n) ((void)(p), (void)(n))
#define RW_SWAP64_ARR(p,n) ((void)(p), (void)(n))

#endif

#ifdef __cplusplus
}
#endif

#endif /* __GTA_ENDIAN_H__ */
