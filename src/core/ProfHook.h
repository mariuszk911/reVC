#ifndef __GTA_PROFHOOK_H__
#define __GTA_PROFHOOK_H__

/*
 * Profiling / trace seam (W-6). Shared engine files include this instead of
 * FrameProfile.h and call the REVC_FP_* macros.
 *
 * On AmigaOS they forward to the real frame profiler (src/core/FrameProfile.*).
 * On every other build they compile to no-ops and that TU is not compiled at
 * all (see src/CMakeLists.txt and build/amigaos/build.py), so the per-entity
 * span calls and the shared-file divergence disappear off-AmigaOS.
 */

// The fp::FP_* slot constants are macro arguments on both platforms, so the
// declarations header is always included; only the implementation TU is
// AmigaOS-only.
#include "FrameProfile.h"

#ifdef AMIGAOS

#define REVC_FP_BEGINFRAME()      fp::fpBeginFrame()
#define REVC_FP_ENDFRAME()        fp::fpEndFrame()
#define REVC_FP_RESET()           fp::fpReset()
#define REVC_FP_ENABLED()         (fp::fpEnabled() != 0)
#define REVC_FP_DRAWOVERLAY()     fpDrawOverlay()

#ifdef REVC_FP_LEAN

// Lean build: the per-entity and per-phase spans are compiled out. The slot and
// group arguments must not be evaluated at all, so these are ((void)0), not
// ((void)(slot)): the latter still evaluates the expression, which is exactly
// the cost a lean build exists to remove.
#define REVC_FP_BEGIN(slot)       ((void)0)
#define REVC_FP_END(slot)         ((void)0)
#define REVC_FP_PUSHGROUP(name)   ((void)0)
#define REVC_FP_POPGROUP()        ((void)0)

#else

#define REVC_FP_BEGIN(slot)       fp::fpBegin(slot)
#define REVC_FP_END(slot)         fp::fpEnd(slot)
#define REVC_FP_PUSHGROUP(name)   fp::fpPushGroup(name)
#define REVC_FP_POPGROUP()        fp::fpPopGroup()

#endif /* REVC_FP_LEAN */

#else

#define REVC_FP_BEGINFRAME()      ((void)0)
#define REVC_FP_ENDFRAME()        ((void)0)
#define REVC_FP_BEGIN(slot)       ((void)(slot))
#define REVC_FP_END(slot)         ((void)(slot))
#define REVC_FP_PUSHGROUP(name)   ((void)(name))
#define REVC_FP_POPGROUP()        ((void)0)
#define REVC_FP_RESET()           ((void)0)
#define REVC_FP_ENABLED()         0
#define REVC_FP_DRAWOVERLAY()     ((void)0)

#endif

#endif /* __GTA_PROFHOOK_H__ */
