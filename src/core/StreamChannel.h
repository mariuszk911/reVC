#ifndef __GTA_STREAM_CHANNEL_H__
#define __GTA_STREAM_CHANNEL_H__

/*
 * Lock-free single-producer/single-consumer streaming channel state, shared by
 * the main task and the AmigaOS loader Process (src/skel/amigaos/amiga_stream.c).
 *
 * Scope 1 of the asynchronous CdStream backend. This header has
 * zero engine includes so the pure state machine can be unit-tested on the host
 * (tests/amigaos/stream_channel_test.c); the status constants must match
 * src/core/CdStream.h and that is enforced by a _Static_assert in
 * src/core/CdStream_amiga.cpp, which includes both.
 *
 * Concurrency contract: the loader is a separate Exec Process on the SAME CPU
 * (68040 / PiStorm3D is single-core), so there is no SMP: the only hazard is
 * compiler reordering, not CPU-level races. `volatile` on every shared field
 * stops the compiler caching a stale value, and the handoff is bracketed by
 * opaque calls (Signal/Wait) the compiler will not reorder memory across.
 * `std::atomic` is unavailable (C loader + C++03 engine), so the contract is the
 * single-core CPU plus those two rules; it is deliberately NOT SMP-safe.
 *
 * The write ORDER in SC_OnStart/SC_OnComplete is deliberate and keeps the main
 * task from sampling an intermediate state as "done":
 *   - SC_OnStart sets bReading first, then state; SC_GetStatus treats either
 *     bReading OR state == SC_READING as READING, so a preemption between the
 *     two reads cannot make a just-started read look done.
 *   - SC_OnComplete publishes nStatus/state BEFORE clearing bReading last, so
 *     once bReading reads 0 the completion is fully visible.
 * SC_IsBusy/SC_GetStatus snapshot the fields once so a preemption mid-read cannot
 * mix an old and a new value into one test; the result is still correct because
 * the reader's field order (bReading -> state -> sectorsToRead -> nStatus) is the
 * reverse of SC_OnComplete's release order, so a terminal state always implies the
 * status published with it is already visible.
 */

/* Status values, used when this header is built standalone (host test,
 * amiga_stream.c). When CdStream.h has already been included, its enum provides
 * the same names, so defining macros here would shadow/duplicate it. */
#if !defined(__GTA_CDSTREAM_H__) && !defined(STREAM_NONE)
#define STREAM_NONE	0
#define STREAM_SUCCESS	1
#define STREAM_READING	0xFF
#define STREAM_ERROR	0xFE
#define STREAM_WAITING	0xFA
#endif

enum StreamChannelState
{
	SC_FREE = 0,
	SC_QUEUED,
	SC_READING,
	SC_DONE,
	SC_ERROR
};

struct StreamChannel
{
	volatile int state;
	volatile int bReading;
	volatile int sectorsToRead;
	volatile int nStatus;
};

/* True while an issued read has not been completed by the loader. */
static inline int
SC_IsBusy(const struct StreamChannel *chan)
{
	int st = chan->state;	/* one snapshot: no QUEUED/READING mix */

	return st == SC_QUEUED || st == SC_READING;
}

/* Back to a clean idle slot (remove images / shutdown). */
static inline void
SC_Reset(struct StreamChannel *chan)
{
	chan->bReading = 0;
	chan->sectorsToRead = 0;
	chan->nStatus = STREAM_NONE;
	chan->state = SC_FREE;
}

/* Main task: publish a new request, then Signal the loader. */
static inline void
SC_Submit(struct StreamChannel *chan, int sectors)
{
	chan->bReading = 0;
	chan->sectorsToRead = sectors;
	chan->nStatus = STREAM_NONE;
	chan->state = SC_QUEUED;
}

/* Loader: about to perform the read. */
static inline void
SC_OnStart(struct StreamChannel *chan)
{
	chan->bReading = 1;	/* first: a poll must never see NONE here */
	chan->state = SC_READING;
}

/* Loader: read finished; publish before clearing bReading (the release). */
static inline void
SC_OnComplete(struct StreamChannel *chan, int err)
{
	chan->sectorsToRead = 0;
	chan->nStatus = err ? STREAM_ERROR : STREAM_NONE;
	chan->state = err ? SC_ERROR : SC_DONE;
	chan->bReading = 0;	/* release: last */
}

/* Main task: the one-shot status consumed by ProcessLoadingChannel. */
static inline int
SC_GetStatus(struct StreamChannel *chan)
{
	int reading = chan->bReading;
	int st = chan->state;
	int sectors = chan->sectorsToRead;
	int status = chan->nStatus;

	if (reading || st == SC_READING)
		return STREAM_READING;
	/* sectors != 0 matches CdStream_posix.cpp's "nSectorsToRead != 0"
	 * WAITING test; state covers the window before SC_Submit publishes it. */
	if (st == SC_QUEUED || sectors != 0)
		return STREAM_WAITING;
	if (status != STREAM_NONE) {
		chan->nStatus = STREAM_NONE;
		return status;
	}
	return STREAM_NONE;
}

/* Main task: CdStreamSync must observe the error without clearing it (D5). */
static inline int
SC_PeekStatus(const struct StreamChannel *chan)
{
	return chan->nStatus;
}

#endif /* __GTA_STREAM_CHANNEL_H__ */
