#ifndef __GTA_PLATFORM_H__
#define __GTA_PLATFORM_H__

/*
 * Platform facade (D9 / W-9). Shared engine code calls these neutral hooks
 * instead of raw `#ifdef AMIGAOS` blocks. Each build links exactly one
 * implementation:
 *
 *   - src/skel/generic/platform_generic.cpp   (PC / other platforms)
 *   - src/skel/amigaos/platform_amiga.cpp     (AmigaOS 68k)
 *
 * Keep the signatures platform-neutral (no exec/NDK types) and the
 * implementations small; anything non-trivial lives in the platform TU.
 */

namespace rw { struct Object; }

struct RwMiniStats;	// vendor/librw/src/mini/rwminihost.h

namespace plat {

// Write one log line to the platform sink. AmigaOS: reVC.log + serial (via
// amiga_log_raw). Generic: stderr. The caller owns any trailing newline.
void LogWrite(const char *s);

// White-model attribution diagnostic. AmigaOS (REVC_TEXMEMLOG): count materials
// with no texture on obj and log the model name once at load time. No-op
// elsewhere. `obj` is the engine `RwObject` (== rw::Object).
void LogWhiteTexModel(rw::Object *obj, const char *modelName);

// End-of-frame platform diagnostic hook. AmigaOS: distinct-texture census
// (REVC_TEXUNIQ). No-op elsewhere.
void FrameEndDiag(void);

// Whether the render backend can draw the transparent-water pass. Generic (PC/
// GL3) can; AmigaOS minigl.library 29.x can as well.
bool CanRenderTransparentWater(void);

// Copy the last MINI render-stats snapshot (`RwMiniStats`, see
// vendor/librw/src/mini/rwminihost.h): per-pass timings and submission counts
// from the frame that just ended, plus the static device caps. The per-frame
// fields are zero while the profiler is off. PC/reference builds zero it.
void MiniStats(RwMiniStats &out);

}

#endif /* __GTA_PLATFORM_H__ */
