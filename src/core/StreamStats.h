#ifndef __GTA_STREAMSTATS_H__
#define __GTA_STREAMSTATS_H__

/*
 * REVC_STREAMSTATS: opt-in streaming diagnostics (AmigaOS only).
 *
 * The engine's streaming path is shared with upstream reVC and is only
 * instrumented here; every hook is a no-op unless the switch is set. The
 * implementation lives in src/core/StreamStats.cpp and logs through
 * amiga_boot_log() (not debug(), which is off unless REVC_DEBUGLOG is set).
 *
 * What it answers, per window and per slow frame:
 *   - request rate and maximum requested-list size
 *   - files/bytes parsed vs. bytes actually read from disk (waste ratio)
 *   - ConvertBufferToObject / FinishLoadingLargeFile CPU and slowest files
 *   - MakeSpaceFor evictions and collision/scene LoadAllRequestedModels cost
 *   - top loaded/removed resources (eviction thrash)
 *   - main-task milliseconds blocked in CdStreamSync (I/O wait)
 *
 * Switches (see FUN_WITH_FLAGS.md):
 *   REVC_STREAMSTATS=1            enable (default off)
 *   REVC_STREAMSTATS_FRAMES=N     dump a window every N CStreaming::Update
 *                                 calls (default 150)
 *   REVC_STREAMSTATS_SLOW_MS=N    log a slow frame when the streaming update
 *                                 takes at least N ms; at most 8 lines per
 *                                 window, the rest are counted and summarised
 *                                 (default 20, 0 = off)
 */

#ifdef AMIGAOS

int  ssEnabled(void);
void ssFrameBegin(void);
void ssFrameEnd(void);

void ssRequestAdded(int streamId);
void ssListSize(int n);
void ssBatchIssued(int files, unsigned int bytes);
void ssConvertDone(int streamId, unsigned int ms);
void ssFinishDone(int streamId, unsigned int ms);
void ssResourceLoaded(int streamId, unsigned int bytes);
void ssResourceRemoved(int streamId);

unsigned int ssMakeSpaceBegin(void);
void ssMakeSpaceDone(unsigned int t0, int evicted, int fallback);

unsigned int ssLoadAllBegin(void);
void ssLoadAllDone(unsigned int t0, int files, unsigned int bytes);

void ssShutdown(void);

#else /* !AMIGAOS */

inline int ssEnabled(void) { return 0; }
inline void ssFrameBegin(void) {}
inline void ssFrameEnd(void) {}

inline void ssRequestAdded(int) {}
inline void ssListSize(int) {}
inline void ssBatchIssued(int, unsigned int) {}
inline void ssConvertDone(int, unsigned int) {}
inline void ssFinishDone(int, unsigned int) {}
inline void ssResourceLoaded(int, unsigned int) {}
inline void ssResourceRemoved(int) {}

inline unsigned int ssMakeSpaceBegin(void) { return 0; }
inline void ssMakeSpaceDone(unsigned int, int, int) {}

inline unsigned int ssLoadAllBegin(void) { return 0; }
inline void ssLoadAllDone(unsigned int, int, unsigned int) {}

inline void ssShutdown(void) {}

#endif /* AMIGAOS */

#endif /* __GTA_STREAMSTATS_H__ */
