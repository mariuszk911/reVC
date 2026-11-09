/*
 * stream_async_test.c - target-only test for the AmigaOS async CdStream loader
 * (src/skel/amigaos/amiga_stream.c), scope 1 of the asynchronous
 * CdStream backend.
 *
 * #includes the production source (same pattern as libfix_math.c including
 * amiga_libfix.c) so the loader is exercised exactly as it ships. Writes its own
 * DH11: log and ends "stream_async_test: all ok".
 *
 * Covers, per the plan's implementation notes:
 *   - async reads of two channels interleaved with busy polls, content/order;
 *   - a forced error (short read at EOF) reported once by SC_GetStatus;
 *   - the path open + size query used by CdStreamAddImage/GetGTA3ImgSize;
 *   - shutdown with an in-flight read (no crash, loader stops cleanly).
 *
 * Build: cross-built by tests/amigaos/Makefile into build/amigaos/test (DH11:).
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "StreamChannel.h"

#include "../../src/skel/amigaos/amiga_stream.c"

#ifndef LOGFILE
#define LOGFILE "DH11:stream_async_test.log"
#endif
#ifndef DATA_FILE
#define DATA_FILE "DH11:stream_async_data.bin"
#endif

#define SECTOR 2048
#define SECTORS 8
#define TOTAL (SECTORS * SECTOR)

static unsigned char expected[TOTAL];
static unsigned char buf0[4 * SECTOR];
static unsigned char buf1[4 * SECTOR];
static unsigned char buf2[TOTAL];

static int failures;

#define CHECK(cond) do { \
	if (cond) { printf("ok   %s\n", #cond); } \
	else { printf("FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__); failures++; } \
} while (0)

static void
fill_expected(void)
{
	int i;
	for (i = 0; i < TOTAL; i++)
		expected[i] = (unsigned char)((i * 13 + 7) & 0xFF);
}

static int
make_data(void)
{
	FILE *f = fopen(DATA_FILE, "wb");
	if (!f)
		return 0;
	if (fwrite(expected, 1, TOTAL, f) != TOTAL) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

int main(void)
{
	struct StreamChannel c0, c1, c2;
	unsigned long reads = 0, errors = 0, bytes = 0;
	int img;

	freopen(LOGFILE, "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("stream_async_test: start\n");

	fill_expected();
	if (!make_data()) {
		printf("stream_async_test: cannot create data file\n");
		printf("stream_async_test: 1 failure(s)\n");
		return 1;
	}

	AmStreamInit();
	printf("loader running: %d\n", AmStreamIsRunning());
	CHECK(AmStreamIsRunning() == 1);

	img = AmStreamAddImage(DATA_FILE);
	printf("image index: %d size: %lu\n", img, AmStreamImageSize(img));
	CHECK(img == 0);
	CHECK(AmStreamImageSize(img) == (unsigned long)TOTAL);

	/* --- Two interleaved async reads, content and order. --- */
	SC_Reset(&c0);
	SC_Reset(&c1);
	buf0[0] = 0;
	memset(buf0, 0xAA, sizeof(buf0));
	memset(buf1, 0xAA, sizeof(buf1));

	SC_Submit(&c0, 4);
	AmStreamSubmit(0, &c0, img, buf0, 0, 4 * SECTOR);
	SC_Submit(&c1, 4);
	AmStreamSubmit(1, &c1, img, buf1, 4 * SECTOR, 4 * SECTOR);

	/* Interleave a bounded set of busy polls with the wait. */
	{
		int polls = 0;
		while ((SC_IsBusy(&c0) || SC_IsBusy(&c1)) && polls < 10000)
			polls++;
		printf("busy polls before wait: %d\n", polls);
	}
	AmStreamWait(0, &c0);
	AmStreamWait(1, &c1);

	CHECK(SC_IsBusy(&c0) == 0);
	CHECK(SC_IsBusy(&c1) == 0);
	CHECK(SC_PeekStatus(&c0) == STREAM_NONE);
	CHECK(SC_PeekStatus(&c1) == STREAM_NONE);
	CHECK(memcmp(buf0, expected, 4 * SECTOR) == 0);
	CHECK(memcmp(buf1, expected + 4 * SECTOR, 4 * SECTOR) == 0);

	/* --- Forced error: read past EOF must be reported once. --- */
	SC_Reset(&c2);
	SC_Submit(&c2, 1);
	AmStreamSubmit(2, &c2, img, buf2, (unsigned long)TOTAL, SECTOR);
	AmStreamWait(2, &c2);
	CHECK(SC_IsBusy(&c2) == 0);
	CHECK(SC_PeekStatus(&c2) == STREAM_ERROR);
	CHECK(SC_GetStatus(&c2) == STREAM_ERROR);
	CHECK(SC_GetStatus(&c2) == STREAM_NONE);

	/* --- Forced error: bad image index. --- */
	SC_Reset(&c2);
	SC_Submit(&c2, 1);
	AmStreamSubmit(2, &c2, 999, buf2, 0, SECTOR);
	AmStreamWait(2, &c2);
	CHECK(SC_PeekStatus(&c2) == STREAM_ERROR);
	CHECK(SC_GetStatus(&c2) == STREAM_ERROR);

	AmStreamGetStats(&reads, &errors, &bytes);
	printf("stats: reads=%lu errors=%lu bytes=%lu\n", reads, errors, bytes);
	CHECK(reads >= 2);
	CHECK(errors >= 2);
	CHECK(bytes == (unsigned long)(4 * SECTOR) * 2);

	/* --- Shutdown with an in-flight read. Two outcomes are both safe:
	 * the loader was mid-read (it completes before AmStreamShutdown returns,
	 * so buf2 is intact), or it had not started (the request is abandoned and
	 * the channel stays QUEUED). Either way it must not touch freed memory
	 * after Shutdown returns. --- */
	SC_Reset(&c2);
	memset(buf2, 0xAA, sizeof(buf2));
	SC_Submit(&c2, SECTORS);
	AmStreamSubmit(2, &c2, img, buf2, 0, TOTAL);
	{
		int spins = 0;
		while (c2.state == SC_QUEUED && spins < 1000000)
			spins++;
	}
	AmStreamShutdown();
	CHECK(AmStreamIsRunning() == 0);
	if (c2.state == SC_DONE)
		CHECK(memcmp(buf2, expected, TOTAL) == 0);
	else
		CHECK(c2.state == SC_QUEUED);

	/* Close the image explicitly (production does this in
	 * CdStreamRemoveImages before CdStreamShutdown); otherwise the data file
	 * handle stays open and the next run's fopen("wb") fails. */
	AmStreamRemoveImages();
	CHECK(AmStreamImageSize(img) == 0);

	if (failures) {
		printf("stream_async_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("stream_async_test: all ok\n");
	return 0;
}
