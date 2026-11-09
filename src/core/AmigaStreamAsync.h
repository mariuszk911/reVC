#ifndef __GTA_AMIGA_STREAM_ASYNC_H__
#define __GTA_AMIGA_STREAM_ASYNC_H__

/*
 * C API for the AmigaOS asynchronous CdStream backend (scope 1).
 *
 * Implemented in src/skel/amigaos/amiga_stream.c with a background Exec
 * Process. src/core/CdStream_amiga.cpp is the only engine caller; the header is
 * engine-type-free (it only forward-declares struct StreamChannel) so it does
 * not drag exec/dos headers into the host build.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct StreamChannel;

/* Start/stop the loader Process and its signals. Init is idempotent. */
void AmStreamInit(void);
void AmStreamShutdown(void);

/* 1 while the loader Process is running; 0 when it could not be spawned (in
 * which case Submit performs the read inline, i.e. the old synchronous path). */
int AmStreamIsRunning(void);

/* Image table. AmStreamAddImage returns the image index or -1 on error; it
 * applies the AmigaOS path normalisation and the exact/lower/upper case
 * fallback used by _fcaseopen (see src/skel/crossplatform.cpp). */
int AmStreamAddImage(const char *path);
void AmStreamRemoveImages(void);
unsigned long AmStreamImageSize(int img);	/* 0 for a bad index */

/* Queue a read on channel `ch` and wake the loader. `chan` is the same scalar
 * the main task passed to SC_Submit, so the two sides share one state word. If
 * the loader is not running the read completes inline before returning. */
void AmStreamSubmit(int ch, struct StreamChannel *chan, int img,
                    void *buf, unsigned long byteOff, unsigned long byteLen);

/* Block the main task until `chan` is no longer busy. No-op if not running. */
void AmStreamWait(int ch, struct StreamChannel *chan);

/* Plain counters for main-thread logging; any pointer may be NULL. */
void AmStreamGetStats(unsigned long *reads, unsigned long *errors,
                      unsigned long *bytes);

/* Last dos.library IoErr() seen by the loader (0 if none), for main-thread
 * logging. */
long AmStreamLastError(void);

#ifdef __cplusplus
}
#endif

/* Main-task milliseconds spent blocked in CdStreamSync (async wait or the
 * inline synchronous read). C++ linkage; implemented in
 * src/core/CdStream_amiga.cpp. Used by REVC_STREAMSTATS. The C loader TU never
 * calls it. Never reset: CdStreamShutdown() reports the cumulative value. */
unsigned int AmStreamBlockMs(void);

/* Backend-neutral read statistics: async loader counters or the synchronous
 * CdStreamReadSync counters. C++ linkage; implemented in CdStream_amiga.cpp. */
void CdStreamGetStats(unsigned long *reads, unsigned long *errors,
                      unsigned long *bytes);

#endif /* __GTA_AMIGA_STREAM_ASYNC_H__ */
