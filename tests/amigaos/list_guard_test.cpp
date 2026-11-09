/*
 * H8: CLinkList<T>::InsertSorted runaway-walk guard.
 *
 * A corrupt/cyclic list made InsertSorted spin forever on AmigaOS (an FCMP.S
 * loop). The guard bounds the walk and returns nil. This test builds a valid
 * sorted list, then a deliberately cyclic one, and asserts the walk terminates.
 *
 * templates.h expects int32/nil/debug/printf/assert from common.h; we provide
 * those shims instead of pulling in the renderware headers so the header can be
 * compiled on the host. Host + 68k; writes DH11:list_guard.log on the target.
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

struct Item {
	int sort;
};

int main()
{
#ifdef AMIGAOS
	std::freopen("DH11:list_guard.log", "w", stdout);
#endif

	/* Valid sorted insertion preserves ascending order. */
	{
		CLinkList<Item> list;
		list.Init(4);
		Item a = {5}, b = {1}, c = {3}, d = {2};
		CHECK(list.InsertSorted(a) != nil);
		CHECK(list.InsertSorted(b) != nil);
		CHECK(list.InsertSorted(c) != nil);
		CHECK(list.InsertSorted(d) != nil);
		int got[4];
		int i = 0;
		for (CLink<Item> *p = list.head.next; p != &list.tail; p = p->next)
			got[i++] = p->item.sort;
		CHECK(i == 4);
		/* inserted 5,1,3,2 -> ascending 1,2,3,5 */
		CHECK(got[0] == 1 && got[1] == 2 && got[2] == 3 && got[3] == 5);
		list.Shutdown();
	}

	/* Exhausting the free list returns nil (no crash). */
	{
		CLinkList<Item> list;
		list.Init(1);
		Item a = {1}, b = {2};
		CHECK(list.InsertSorted(a) != nil);
		CHECK(list.InsertSorted(b) == nil);
		list.Shutdown();
	}

	/* Corrupt cyclic walk must terminate via the guard, not hang. */
	{
		CLinkList<Item> list;
		list.Init(4);		/* one free node, so the free-list check is not the cause */
		Item a = {1}, b = {2}, c = {3}, d = {9};
		CHECK(list.InsertSorted(a) != nil);
		CHECK(list.InsertSorted(b) != nil);
		CHECK(list.InsertSorted(c) != nil);
		/* Splice the last node back to the first: no longer reaches &tail. */
		list.tail.prev->next = list.head.next;
		/* d.sort is larger than every node, so the comparison never breaks and
		 * the walk must hit the guard; without it this call would hang. */
		CLink<Item> *r = list.InsertSorted(d);
		CHECK(r == nil);
		/* Restore a sane tail so Shutdown/delete is clean (links only). */
		list.tail.prev->next = &list.tail;
		list.Shutdown();
	}

	if (failures) {
		std::printf("list_guard_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("list_guard_test: all ok\n");
	return 0;
}
