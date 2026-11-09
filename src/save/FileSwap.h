#ifndef __GTA_FILE_SWAP_H__
#define __GTA_FILE_SWAP_H__

/*
 * Little-endian save-file -> native byte swap, split out of
 * GenericGameStorage.cpp so the per-type swap rules can be unit-tested on the
 * host (tests/amigaos/file_swap_test.cpp) without pulling in the game/window
 * headers. The swap primitives live in src/core/Endian.h.
 *
 * Scalars are swapped by width. Structs/vectors/arrays of 32-bit scalars are
 * swapped word-wise, which covers CVector/CMatrix/etc. Byte-ordered colour
 * structs (CRGBA/CRGB) must NOT be swapped; GenericGameStorage.cpp provides the
 * CRGBA overload next to its use so overload resolution in the read/write
 * macros picks it up.
 *
 * C++ only (template). Kept unconditional so the same code is exercised on the
 * LE host; on a little-endian target the callers simply never invoke it.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "Endian.h"

// The save file is always little-endian (COMPATIBLE_SAVES). These convert a
// scalar read from / written to it to native order (identity on little-endian).
#define SAVE_LE16(x) RW_SWAP16(x)
#define SAVE_LE32(x) RW_SWAP32(x)

// Byte-swap one POD field between the little-endian save-file format and native
// order. Scalars use their width; structs/vectors/arrays of 32-bit scalars are
// swapped word-wise (covers CVector, CMatrix, ...). Byte structs (CRGBA, CRGB)
// must NOT be swapped - handled by the overload in GenericGameStorage.cpp.
template<typename T>
static inline void saveFileSwap(T *p)
{
	uint8_t *b = (uint8_t*)p;
	size_t i, sz = sizeof(T);
	switch (sz) {
	case 1:
		return;
	case 2: { uint16_t v; memcpy(&v, b, 2); v = Swap16(v); memcpy(b, &v, 2); return; }
	case 4: { uint32_t v; memcpy(&v, b, 4); v = Swap32(v); memcpy(b, &v, 4); return; }
	case 8: { uint64_t v; memcpy(&v, b, 8); v = Swap64(v); memcpy(b, &v, 8); return; }
	}
	if ((sz % 4) == 0) {
		for (i = 0; i < sz; i += 4) {
			uint8_t t = b[i]; b[i] = b[i+3]; b[i+3] = t;
			t = b[i+1]; b[i+1] = b[i+2]; b[i+2] = t;
		}
	}
}

// Arrays: swap element by element (covers int32[N], wchar[N], CVector[N], ...).
template<typename T, size_t N>
static inline void saveFileSwap(T (*p)[N])
{
	for (size_t i = 0; i < N; i++)
		saveFileSwap(&(*p)[i]);
}

#endif /* __GTA_FILE_SWAP_H__ */
