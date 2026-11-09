// AmigaOS implementation of the platform facade; see Platform.h.
// The generic build links src/skel/generic/platform_generic.cpp instead.

#include "common.h"
#include "Platform.h"

#include <stdio.h>

extern "C" void amiga_log_raw(const char *s);
#include "amiga_boot.h"

// librw MINI host seam (RwMiniStats / rwminiGetFrameStats). The only TU that
// pulls the librw header; the shared overlay reaches the stats through the
// plat::MiniStats facade.
#include "mini/rwminihost.h"

// librw MINI backend per-frame distinct-texture census (REVC_TEXUNIQ, no-op off).
extern "C" void miniTexUniqFrameEnd(void);

namespace plat {

void LogWrite(const char *s)
{
	amiga_log_raw(s);
}

// REVC_TEXMEMLOG: white-model attribution. The MINI backend draws a mesh with
// no usable texture as flat white (GL_TEXTURE_2D off), and cannot name the
// model. Log the model name once, at load time, with the nil/total material
// ratio -- "nil=k/n" tells a fully untextured (white) model (k==n) apart from
// one that merely has an auxiliary untextured material (k<n), which renders
// fine. A missing lookup would instead produce a named dummy reported as
// deadtex by the backend, so nil means "no texture reference at all".
static int gWhiteTexNil, gWhiteTexTotal;

static RpMaterial *
WhiteTexMaterialCB(RpMaterial *mat, void *)
{
	gWhiteTexTotal++;
	if(RpMaterialGetTexture(mat) == nil)
		gWhiteTexNil++;
	return mat;
}

static RpAtomic *
WhiteTexAtomicCB(RpAtomic *atomic, void *)
{
	RpGeometryForAllMaterials(RpAtomicGetGeometry(atomic), WhiteTexMaterialCB, nil);
	return atomic;
}

void LogWhiteTexModel(RwObject *obj, const char *modelName)
{
	char buf[176];

	if(!amiga_boot_texmemlog() || obj == nil)
		return;
	gWhiteTexNil = 0;
	gWhiteTexTotal = 0;
	if(RwObjectGetType(obj) == rpCLUMP)
		RpClumpForAllAtomics((RpClump*)obj, WhiteTexAtomicCB, nil);
	else
		WhiteTexAtomicCB((RpAtomic*)obj, nil);
	if(gWhiteTexNil){
		sprintf(buf, "mini: white model '%s' nil_mats=%d/%d",
		        modelName ? modelName : "?", gWhiteTexNil, gWhiteTexTotal);
		amiga_boot_log(buf);
	}
}

void FrameEndDiag(void)
{
	miniTexUniqFrameEnd();
}

bool CanRenderTransparentWater(void)
{
	return true;	// minigl.library 29.x handles the pass
}

void MiniStats(RwMiniStats &out)
{
	rwminiGetFrameStats(&out);
}

}
