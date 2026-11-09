/*
 * Host + 68k unit test for the async CdStream channel state machine
 * (src/core/StreamChannel.h), scope 1 of the AmigaOS async streaming plan.
 *
 * The state machine is the lock-free handoff between the main task and the
 * loader Process, so every transition the plan's section 5 pins down is
 * checked here: submit -> start -> complete -> status, the one-shot error, the
 * QUEUED -> WAITING report, SC_PeekStatus's no-clear (D5) and SC_GetStatus's
 * take-and-clear, busy transitions, and reset.
 */
#include <stdio.h>
#include <string.h>

#include "StreamChannel.h"

#ifndef LOGFILE
#define LOGFILE "DH11:stream_channel.log"
#endif

static int failures;

#define CHECK(cond) do { \
	if (cond) { printf("ok   %s\n", #cond); } \
	else { printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

int main(void)
{
	struct StreamChannel c;

#ifdef AMIGAOS
	freopen(LOGFILE, "w", stdout);
#endif

	printf("stream_channel state machine test\n");

	/* Fresh slot. */
	SC_Reset(&c);
	CHECK(c.state == SC_FREE);
	CHECK(c.bReading == 0);
	CHECK(c.sectorsToRead == 0);
	CHECK(c.nStatus == STREAM_NONE);
	CHECK(SC_IsBusy(&c) == 0);
	CHECK(SC_GetStatus(&c) == STREAM_NONE);
	CHECK(SC_PeekStatus(&c) == STREAM_NONE);

	/* Submit: queued, not yet reading -> WAITING, sectors recorded. */
	SC_Submit(&c, 7);
	CHECK(c.state == SC_QUEUED);
	CHECK(c.sectorsToRead == 7);
	CHECK(c.nStatus == STREAM_NONE);
	CHECK(SC_IsBusy(&c) == 1);
	CHECK(SC_GetStatus(&c) == STREAM_WAITING);
	CHECK(SC_GetStatus(&c) == STREAM_WAITING);	/* still queued */

	/* Loader starts: READING until complete. */
	SC_OnStart(&c);
	CHECK(c.state == SC_READING);
	CHECK(c.bReading == 1);
	CHECK(SC_IsBusy(&c) == 1);
	CHECK(SC_GetStatus(&c) == STREAM_READING);

	/* Successful completion: not busy, one-shot status is NONE. */
	SC_OnComplete(&c, 0);
	CHECK(c.state == SC_DONE);
	CHECK(c.bReading == 0);
	CHECK(c.sectorsToRead == 0);
	CHECK(c.nStatus == STREAM_NONE);
	CHECK(SC_IsBusy(&c) == 0);
	CHECK(SC_GetStatus(&c) == STREAM_NONE);

	/* Error completion: one-shot STREAM_ERROR, then NONE. */
	SC_Submit(&c, 3);
	SC_OnStart(&c);
	SC_OnComplete(&c, 1);
	CHECK(c.state == SC_ERROR);
	CHECK(c.nStatus == STREAM_ERROR);
	CHECK(SC_IsBusy(&c) == 0);
	CHECK(SC_GetStatus(&c) == STREAM_ERROR);
	CHECK(SC_GetStatus(&c) == STREAM_NONE);	/* consumed once */

	/* SC_PeekStatus (D5) must not clear the error. */
	SC_Submit(&c, 1);
	SC_OnStart(&c);
	SC_OnComplete(&c, 1);
	CHECK(SC_PeekStatus(&c) == STREAM_ERROR);
	CHECK(SC_PeekStatus(&c) == STREAM_ERROR);	/* still there */
	CHECK(c.nStatus == STREAM_ERROR);
	CHECK(SC_GetStatus(&c) == STREAM_ERROR);	/* now consumed */
	CHECK(SC_PeekStatus(&c) == STREAM_NONE);

	/* A new submit must clear a stale status and a stale busy flag. */
	SC_Submit(&c, 0);
	CHECK(c.state == SC_QUEUED);
	CHECK(c.nStatus == STREAM_NONE);
	CHECK(SC_IsBusy(&c) == 1);

	/* Completion without an explicit OnStart (fast loader) still settles. */
	SC_OnComplete(&c, 0);
	CHECK(SC_IsBusy(&c) == 0);
	CHECK(SC_GetStatus(&c) == STREAM_NONE);
	CHECK(c.state == SC_DONE);

	/* Re-submit after done overwrites cleanly. */
	SC_Submit(&c, 9);
	CHECK(c.sectorsToRead == 9);
	SC_OnStart(&c);
	SC_OnComplete(&c, 0);
	CHECK(SC_IsBusy(&c) == 0);

	/* A just-started read must read as READING even if a poll samples between
	 * SC_OnStart's bReading and state writes (state is authoritative). */
	SC_Reset(&c);
	SC_Submit(&c, 2);
	c.state = SC_READING;
	c.bReading = 0;
	CHECK(SC_GetStatus(&c) == STREAM_READING);
	c.bReading = 1;
	CHECK(SC_GetStatus(&c) == STREAM_READING);

	/* Reset for remove/shutdown. */
	SC_Reset(&c);
	CHECK(c.state == SC_FREE);
	CHECK(SC_IsBusy(&c) == 0);
	CHECK(SC_GetStatus(&c) == STREAM_NONE);

	if (failures) {
		printf("stream_channel_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("stream_channel_test: all ok\n");
	return 0;
}
