/*
 * CPool<T,U> index/handle arithmetic.
 *
 * CPedPool is CPool<CPed, CPlayerPed>, so a slot is sizeof(CPlayerPed) bytes but
 * the object stored in it is a CPed. CPed::IsPointerValid() does
 *     pedIndex = GetPedPool()->GetIndex(this) >> 8;
 *     if (pedIndex < 0 || pedIndex >= NUMPEDS) return false;
 * so if New() and GetIndex()/GetSlot() ever disagree on the stride (ABI, sizeof,
 * 32-bit overflow, endianness), every live ped resolves to a bogus slot and the
 * near-ped list is pruned away -> melee stops connecting.
 *
 * This asserts the arithmetic is self-consistent for both instantiation shapes
 * (U == T and U > T) on host and 68k. templates.h expects int32/nil/debug/
 * printf/assert from common.h; we provide those shims so it compiles on the
 * host. Writes DH11:pool_index_test.log on the target.
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

typedef int32_t int32;
typedef uint8_t uint8;
typedef uint32_t uint32;
#define nil nullptr
static inline void debug(const char *, ...) {}

#include "templates.h"

static int failures;

#define CHECK(cond) do { \
	if (cond) { std::printf("ok   %s\n", #cond); } \
	else { std::printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

/* Mimic the real size relationship: sizeof(CPlayerPed) > sizeof(CPed). */
struct Slot1740 { uint8 pad[1740]; };
struct Obj1478  { uint8 pad[1478]; };

static const int NSLOTS = 140;	/* NUMPEDS */

/* U == T */
static void test_same_type()
{
	CPool<Obj1478> pool(NSLOTS, "same");
	Obj1478 *p[NSLOTS];
	for (int i = 0; i < NSLOTS; i++)
		p[i] = pool.New();

	CHECK(pool.GetSlot(1) != nil && pool.GetSlot(0) != nil);
	CHECK((uint8*)pool.GetSlot(1) - (uint8*)pool.GetSlot(0) == (long)sizeof(Obj1478));

	for (int i = 0; i < NSLOTS; i++) {
		CHECK((int)(pool.GetIndex(p[i]) >> 8) == i);
		CHECK((int)pool.GetJustIndex_NoFreeAssert(p[i]) == i);
		CHECK(pool.GetSlot(i) == p[i]);
		CHECK(pool.GetAt(pool.GetIndex(p[i])) == p[i]);
	}

	/* Recycle: delete slot 7, next New lands in 7 and still indexes correctly. */
	pool.Delete(p[7]);
	Obj1478 *n = pool.New();
	CHECK((int)(pool.GetIndex(n) >> 8) == 7);
	CHECK(pool.GetJustIndex_NoFreeAssert(n) == 7);
}

/* U > T: this is the CPedPool shape. */
static void test_bigger_slot()
{
	CPool<Obj1478, Slot1740> pool(NSLOTS, "pedlike");
	Obj1478 *p[NSLOTS];
	for (int i = 0; i < NSLOTS; i++)
		p[i] = pool.New();

	CHECK((uint8*)pool.GetSlot(1) - (uint8*)pool.GetSlot(0) == (long)sizeof(Slot1740));

	for (int i = 0; i < NSLOTS; i++) {
		CHECK((int)(pool.GetIndex(p[i]) >> 8) == i);
		CHECK((int)pool.GetJustIndex_NoFreeAssert(p[i]) == i);
		CHECK(pool.GetSlot(i) == p[i]);
	}

	/* New(handle) must recreate the same slot. */
	int32 h = pool.GetIndex(p[11]);
	Obj1478 *q = pool.New(h);
	CHECK(q == p[11]);
	CHECK((int)(pool.GetIndex(q) >> 8) == 11);

	/* The index form IsPointerValid relies on must land inside [0,NUMPEDS). */
	for (int i = 0; i < NSLOTS; i++) {
		int idx = (int)pool.GetJustIndex_NoFreeAssert(p[i]);
		CHECK(idx >= 0 && idx < NSLOTS);
	}
}

/* A pointer that does not belong to the pool yields an out-of-range index --
 * this is the failure mode the game tripped: live-looking peds resolving to
 * slots outside [0,NUMPEDS). */
static void test_foreign_pointer()
{
	CPool<Obj1478, Slot1740> pool(NSLOTS, "foreign");
	Obj1478 *p = pool.New();

	/* One slot below the pool base: the index must be -1 (i.e. rejected).
	 * Use GetJustIndex_NoFreeAssert: GetIndex() would read m_flags[-1]. */
	Obj1478 *below = (Obj1478*)((uint8*)pool.GetSlot(0) - sizeof(Slot1740));
	CHECK((int)pool.GetJustIndex_NoFreeAssert(below) == -1);

	/* And the real slot is still -1-free and inside the IsPointerValid bound. */
	CHECK((int)pool.GetJustIndex_NoFreeAssert(p) == 0);
}

int main()
{
#ifdef AMIGAOS
	std::freopen("DH11:pool_index_test.log", "w", stdout);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
#endif

	test_same_type();
	test_bigger_slot();
	test_foreign_pointer();

	if (failures) {
		std::printf("pool_index_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("pool_index_test: all ok\n");
	return 0;
}
