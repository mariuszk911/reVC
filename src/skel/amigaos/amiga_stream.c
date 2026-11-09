/*
 * amiga_stream.c - AmigaOS asynchronous CdStream backend (scope 1).
 *
 * A background Exec Process (created with CreateNewProcTags, verified by
 * tests/amigaos/spawn_spike.c) performs the blocking dos.library reads while the
 * main task keeps rendering. The handshake is signal-only: the loader owns its
 * own request signal, and a MsgPort is deliberately NOT used (a port created on
 * the main task would PutMsg-signal main, not the loader).
 *
 * DELIBERATELY libnix/stdio-free: this file must not call malloc/printf/debug()
 * from the loader, because re3_debug() shares a global buffer and amigaRawWrite()
 * shares fds *and* the serial IOExtSer (amigaos.cpp). Only exec/dos and the plain
 * shared counters in gCtx are touched from the loader task.
 *
 * Compiled automatically by build/amigaos/{Makefile,build.py} (every .c under src).
 */
#if defined(AMIGAOS)

#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "AmigaStreamAsync.h"
#include "StreamChannel.h"
#include "PathNormalize.h"

#define AM_MAX_CHANNELS	8
#define AM_MAX_IMAGES	8
#define AM_LOADER_STACK	16384
#define AM_PATH_MAX	512

/* Compiler barrier for the main<->loader handoff: order the shared-field
 * publishes before the Signal that releases them (and the reads after the Wait
 * that acquires them), so correctness does not rely on Signal/Wait never being
 * inlined. The CPU is single-core, so no CPU-level fence is needed. */
#define AM_BARRIER() __asm__ __volatile__("" ::: "memory")

struct AmStreamImage
{
	BPTR fh;		/* 0 = unused */
	unsigned long size;
};

struct AmStreamRequest
{
	/* pending is written LAST by the main task (after all payload fields) and
	 * cleared by the loader. The loader selects work by `pending`, not by the
	 * channel's SC_QUEUED state, so a scan can never pick up a channel whose
	 * request fields are still being written. */
	volatile int pending;
	struct StreamChannel *volatile chan;
	volatile int img;
	void *volatile buf;
	volatile unsigned long byteOff;
	volatile unsigned long byteLen;
};

struct AmStreamCtx
{
	struct Task *mainTask;
	struct Task *loaderTask;
	BYTE mainReadySig;
	BYTE reqSig;
	BYTE doneSig;
	BYTE exitSig;		/* main-owned; loader's last action before it dies */
	volatile LONG quit;
	volatile LONG loaderExited;
	volatile ULONG readCount;
	volatile ULONG errorCount;
	volatile ULONG byteCount;
	volatile LONG lastIoErr;
};

static struct AmStreamImage gImages[AM_MAX_IMAGES];
static int gNumImages;
static struct AmStreamCtx gCtx;
static struct AmStreamRequest gReq[AM_MAX_CHANNELS];
static int gStarted;
static int gLoaderUp;

static void amStreamLoader(void);

/* Perform one queued read on `ch`. Runs on the loader, or inline on the main
 * task when the loader could not be spawned (synchronous fallback). */
static void
amStreamReadOne(int ch)
{
	struct AmStreamRequest *r = &gReq[ch];
	struct StreamChannel *chan = r->chan;
	int ok = 0;

	if (chan == NULL)
		return;

	r->pending = 0;

	SC_OnStart(chan);

	if (r->img >= 0 && r->img < gNumImages && gImages[r->img].fh != 0 &&
	    r->buf != NULL && r->byteLen > 0) {
		struct AmStreamImage *img = &gImages[r->img];

		if (Seek(img->fh, (LONG)r->byteOff, OFFSET_BEGINNING) != -1) {
			LONG n = Read(img->fh, r->buf, (LONG)r->byteLen);

			if (n == (LONG)r->byteLen)
				ok = 1;
			else
				gCtx.lastIoErr = IoErr();
		} else {
			gCtx.lastIoErr = IoErr();
		}
	} else {
		gCtx.lastIoErr = ERROR_OBJECT_NOT_FOUND;
	}

	if (ok) {
		gCtx.byteCount += r->byteLen;
		gCtx.readCount++;
	} else {
		gCtx.errorCount++;
	}

	SC_OnComplete(chan, ok ? 0 : 1);
}

static void
amStreamLoader(void)
{
	gCtx.loaderTask = FindTask(NULL);
	gCtx.reqSig = AllocSignal(-1);
	if (gCtx.reqSig < 0) {
		/* Cannot receive requests. Tell main and let the Process exit rather
		 * than spin on an invalid signal bit (which would also orphan us). */
		gCtx.loaderExited = 1;
		AM_BARRIER();
		Signal(gCtx.mainTask, 1L << gCtx.mainReadySig);
		return;
	}
	gCtx.loaderExited = 0;

	AM_BARRIER();
	Signal(gCtx.mainTask, 1L << gCtx.mainReadySig);

	for (;;) {
		Wait(1L << gCtx.reqSig);
		AM_BARRIER();

		if (gCtx.quit)
			break;

		/* Drain every channel with a published request (single per channel).
		 * `pending` is set after the payload, so a stale request is never
		 * selected. */
		{
			int ch;
			for (ch = 0; ch < AM_MAX_CHANNELS; ch++) {
				struct StreamChannel *c = gReq[ch].chan;
				if (gReq[ch].pending && c != NULL && SC_IsBusy(c))
					amStreamReadOne(ch);
			}
		}

		AM_BARRIER();
		Signal(gCtx.mainTask, 1L << gCtx.doneSig);
	}

	/* Signal bits are per-task: free our own before the Process exits. The
	 * final Signal(exitSig) is our last action, so main can free its bits as
	 * soon as it observes exitSig (loaderExited is diagnostic only). */
	FreeSignal(gCtx.reqSig);
	gCtx.reqSig = -1;
	gCtx.loaderExited = 1;
	Signal(gCtx.mainTask, 1L << gCtx.exitSig);
}

void
AmStreamInit(void)
{
	struct Process *p;
	int i;

	if (gStarted)
		return;

	memset(&gCtx, 0, sizeof(gCtx));
	gCtx.mainReadySig = -1;
	gCtx.reqSig = -1;
	gCtx.doneSig = -1;
	gCtx.exitSig = -1;
	gCtx.mainTask = FindTask(NULL);
	gCtx.mainReadySig = AllocSignal(-1);
	gCtx.doneSig = AllocSignal(-1);
	gCtx.exitSig = AllocSignal(-1);

	gStarted = 1;
	gLoaderUp = 0;
	gNumImages = 0;

	for (i = 0; i < AM_MAX_CHANNELS; i++)
		gReq[i].chan = NULL;
	for (i = 0; i < AM_MAX_IMAGES; i++) {
		gImages[i].fh = 0;
		gImages[i].size = 0;
	}

	if (gCtx.mainReadySig < 0 || gCtx.doneSig < 0 || gCtx.exitSig < 0)
		return;	/* Submit will run inline */

	p = CreateNewProcTags(
		NP_Entry, (ULONG)amStreamLoader,
		NP_Name, (ULONG)"reVC_cdstream",
		NP_StackSize, AM_LOADER_STACK,
		NP_Priority, 0,
		TAG_DONE);
	if (p == NULL)
		return;	/* Submit will run inline */

	/* Bounded wait for the loader to come up (~5 s at 50 Hz); on failure Submit
	 * runs inline. SetSignal clears the bit as it is read. A loader that
	 * appears only after the timeout stays blocked on reqSig until process
	 * exit (harmless: async silently degrades to sync). */
	{
		int t;
		for (t = 0; t < 250; t++) {
			if (SetSignal(0L, 1L << gCtx.mainReadySig) & (1L << gCtx.mainReadySig))
				break;
			Delay(1);
		}
	}
	AM_BARRIER();
	gLoaderUp = (gCtx.reqSig >= 0) ? 1 : 0;
}

void
AmStreamShutdown(void)
{
	if (!gStarted)
		return;

	if (gLoaderUp) {
		gCtx.quit = 1;
		Signal(gCtx.loaderTask, 1L << gCtx.reqSig);
		/* exitSig is signalled by the loader as its very last action, so it
		 * is safe to free our bits once it arrives (no trailing Signal on a
		 * freed bit, unlike keying off loaderExited). */
		Wait(1L << gCtx.exitSig);
		gLoaderUp = 0;
	}

	/* reqSig was allocated by the loader task and is freed there on exit
	 * (signal bits are per-task); main owns mainReadySig/doneSig/exitSig. */
	if (gCtx.doneSig >= 0)
		FreeSignal(gCtx.doneSig);
	if (gCtx.mainReadySig >= 0)
		FreeSignal(gCtx.mainReadySig);
	if (gCtx.exitSig >= 0)
		FreeSignal(gCtx.exitSig);
	gCtx.reqSig = gCtx.doneSig = gCtx.mainReadySig = gCtx.exitSig = -1;

	gStarted = 0;
}

int
AmStreamIsRunning(void)
{
	return gLoaderUp;
}

int
AmStreamAddImage(const char *path)
{
	char buf[AM_PATH_MAX];
	BPTR fh = 0;
	int variant;

	if (path == NULL || gNumImages >= AM_MAX_IMAGES)
		return -1;

	/* Exact, all-lower, then all-upper, via the shared normalisation helper
	 * (identical to _fcaseopen and the synchronous backend). */
	for (variant = 0; variant < 3; variant++) {
		if (AmigaPathVariant(buf, sizeof(buf), path, variant) == 0 && variant > 0)
			continue;
		fh = Open((STRPTR)buf, MODE_OLDFILE);
		if (fh != 0)
			break;
	}
	if (fh == 0)
		return -1;

	{
		LONG size = -1;

		if (Seek(fh, 0, OFFSET_END) != -1)
			size = Seek(fh, 0, OFFSET_BEGINNING);
		gImages[gNumImages].size = (size > 0) ? (unsigned long)size : 0;
	}

	gImages[gNumImages].fh = fh;

	gNumImages++;
	return gNumImages - 1;
}

void
AmStreamRemoveImages(void)
{
	int i;

	for (i = 0; i < gNumImages; i++) {
		if (gImages[i].fh != 0) {
			Close(gImages[i].fh);
			gImages[i].fh = 0;
		}
		gImages[i].size = 0;
	}
	gNumImages = 0;
}

unsigned long
AmStreamImageSize(int img)
{
	if (img < 0 || img >= gNumImages)
		return 0;
	return gImages[img].size;
}

void
AmStreamSubmit(int ch, struct StreamChannel *chan, int img,
               void *buf, unsigned long byteOff, unsigned long byteLen)
{
	if (ch < 0 || ch >= AM_MAX_CHANNELS)
		return;

	gReq[ch].chan = chan;
	gReq[ch].img = img;
	gReq[ch].buf = buf;
	gReq[ch].byteOff = byteOff;
	gReq[ch].byteLen = byteLen;
	gReq[ch].pending = 1;	/* publish last: the loader selects on this */

	if (!gLoaderUp) {
		amStreamReadOne(ch);
		return;
	}

	AM_BARRIER();
	Signal(gCtx.loaderTask, 1L << gCtx.reqSig);
}

void
AmStreamWait(int ch, struct StreamChannel *chan)
{
	(void)ch;

	if (!gLoaderUp)
		return;

	while (SC_IsBusy(chan))
		Wait(1L << gCtx.doneSig);
	AM_BARRIER();
}

void
AmStreamGetStats(unsigned long *reads, unsigned long *errors,
                 unsigned long *bytes)
{
	if (reads)
		*reads = gCtx.readCount;
	if (errors)
		*errors = gCtx.errorCount;
	if (bytes)
		*bytes = gCtx.byteCount;
}

long
AmStreamLastError(void)
{
	return gCtx.lastIoErr;
}

#endif /* AMIGAOS */
