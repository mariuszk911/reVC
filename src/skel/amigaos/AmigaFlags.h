#ifndef __GTA_AMIGA_FLAGS_H__
#define __GTA_AMIGA_FLAGS_H__

/*
 * AmigaOS REVC_* feature switches, resolved once from the AmigaDOS environment.
 * One central lazy cache so the ~20 gates do not each carry their own static and
 * accessor (amigaos.cpp). Values are read on the first amigaFlags() call and
 * never change afterwards.
 */
struct AmigaFlags {
	int lockArrays;		// REVC_LOCKARRAYS   (default on)
	int matFx;		// REVC_MATFX        (default on)
	int lockWin;		// REVC_LOCKWIN      (default on)
	int texUniq;		// REVC_TEXUNIQ      (default off)
	int asyncStream;	// REVC_ASYNC_STREAM (default on)
	int texMemLog;		// REVC_TEXMEMLOG    (default off)
	int profile;		// REVC_PROFILE      (default off)
	int profileMs;		// REVC_PROFILE_MS   (default 0 = auto)
	int randomSplash;	// REVC_RANDOMSPLASH (default off; random LOADSC splash)
	int clearColor;		// REVC_CLEAR        (default off; colour-clear the frame)
	int directFetch;	// REVC_DIRECT       (default off; minigl.library 30.x LE path)
	int streamStats;	// REVC_STREAMSTATS  (default off)
	int streamStatsFrames;	// REVC_STREAMSTATS_FRAMES  (default 150)
	int streamStatsSlowMs;	// REVC_STREAMSTATS_SLOW_MS (default 20)
};

const AmigaFlags &amigaFlags(void);

#endif /* __GTA_AMIGA_FLAGS_H__ */
