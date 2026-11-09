/*
 * mpega_lib.h -- binding glue for mpega.library (Stephane Tavenard, V2.4).
 *
 * The vendored mpega.h (developer/include/libraries/mpega.h, GCC-corrected via
 * dev/c/MPEGAPPCdev) declares the types; this header adds the library base and
 * the 68k LVO dispatch (inline macros), mirroring how <proto/ahi.h> is used.
 *
 * mpega.library is freeware; the user installs it as LIBS:mpega.library, like
 * ahi.device. It must be opened with OpenLibrary("mpega.library", 2) before any
 * of the macros below are called.
 */
#ifndef REVC_MPEGA_LIB_H
#define REVC_MPEGA_LIB_H

#include <exec/types.h>
#include <utility/hooks.h>
#include <inline/macros.h>

#include "mpega.h"

#ifndef MPEGA_BASE_NAME
#define MPEGA_BASE_NAME MPEGABase
#endif

/* LVOs (##bias 30 => first function at -30). */
#define MPEGA_open(name, ctrl) \
	LP2(0x1e, MPEGA_STREAM *, MPEGA_open, char *, name, a0, MPEGA_CTRL *, ctrl, a1,\
	, MPEGA_BASE_NAME)

#define MPEGA_close(mpds) \
	LP1NR(0x24, MPEGA_close, MPEGA_STREAM *, mpds, a0,\
	, MPEGA_BASE_NAME)

#define MPEGA_decode_frame(mpds, pcm) \
	LP2(0x2a, LONG, MPEGA_decode_frame, MPEGA_STREAM *, mpds, a0, WORD **, pcm, a1,\
	, MPEGA_BASE_NAME)

#define MPEGA_seek(mpds, ms) \
	LP2(0x30, LONG, MPEGA_seek, MPEGA_STREAM *, mpds, a0, ULONG, ms, d0,\
	, MPEGA_BASE_NAME)

#define MPEGA_time(mpds, ms) \
	LP2(0x36, LONG, MPEGA_time, MPEGA_STREAM *, mpds, a0, ULONG *, ms, a1,\
	, MPEGA_BASE_NAME)

#define MPEGA_find_sync(buffer, size) \
	LP2(0x3c, LONG, MPEGA_find_sync, BYTE *, buffer, a0, LONG, size, d0,\
	, MPEGA_BASE_NAME)

#define MPEGA_scale(mpds, scale) \
	LP2(0x42, LONG, MPEGA_scale, MPEGA_STREAM *, mpds, a0, LONG, scale, d0,\
	, MPEGA_BASE_NAME)

#endif /* REVC_MPEGA_LIB_H */
