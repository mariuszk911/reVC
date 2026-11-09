/*
 * Single shared instance of the frame-profiler state.
 *
 * FrameProfile.h is included by several translation units (main.cpp, Game.cpp,
 * RwHelper.cpp, amigaos.cpp, FrameProfileOverlay.cpp). The m68k gcc 6.5
 * toolchain does not merge the function-local static of an inline function
 * across TUs, so the state cannot be a header-local static; it is defined here
 * exactly once and returned by fp::fpS(). The host/68k unit test defines
 * FP_HEADER_ONLY and needs no such TU.
 */

#include "FrameProfile.h"

namespace fp {

static FpState fpGlobalState;

FpState &
fpS(void)
{
	return fpGlobalState;
}

} // namespace fp
