// Generic (PC/other) implementation of the platform facade; see Platform.h.
// The AmigaOS build links src/skel/amigaos/platform_amiga.cpp instead.

#include "Platform.h"

#include <stdio.h>
#include <string.h>

// librw's root is this build's include root (`vendor/librw`), so the neutral
// host seam is reached as `src/mini/...` here; the AmigaOS skeleton roots it at
// `vendor/librw/src` and uses `mini/...`.
#include "src/mini/rwminihost.h"

namespace plat {

void LogWrite(const char *s)
{
	fputs(s, stdout);
}

void LogWhiteTexModel(rw::Object *, const char *)
{
}

void FrameEndDiag(void)
{
}

bool CanRenderTransparentWater(void)
{
	return true;
}

void MiniStats(RwMiniStats &out)
{
	memset(&out, 0, sizeof(out));
}

}
