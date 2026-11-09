#include "common.h"

#include <stdlib.h>

#include "Font.h"
#include "Pad.h"
#include "Text.h"
#include "main.h"
#include "Timer.h"
#include "DMAudio.h"
#include "FileMgr.h"
#include "Streaming.h"
#include "TxdStore.h"
#include "General.h"
#include "Camera.h"
#include "Vehicle.h"
#include "Bike.h"
#include "PlayerSkin.h"
#include "PlayerInfo.h"
#include "World.h"
#include "Renderer.h"
#include "RwHelper.h"
#include "AnimManager.h"
#include "AnimBlendAssocGroup.h"
#include "AnimViewer.h"
#include "PlayerPed.h"
#include "Pools.h"
#include "References.h"
#include "PathFind.h"
#include "HandlingMgr.h"
#include "TempColModels.h"
#include "Particle.h"
#include "CdStream.h"
#include "Messages.h"
#include "CarCtrl.h"
#include "FileLoader.h"
#include "ModelIndices.h"
#include "Clock.h"
#include "Timecycle.h"
#include "RpAnimBlend.h"
#include "AnimBlendAssociation.h"
#include "Shadows.h"
#include "VisibilityPlugins.h"
#include "Radar.h"
#include "Hud.h"
#include "debugmenu.h"

int CAnimViewer::animTxdSlot = 0;
CEntity *CAnimViewer::pTarget = nil;

void
CAnimViewer::Render(void)
{
	if(pTarget == nil)
		return;

#ifdef FIX_BUGS
	if(pTarget->IsPed())
		((CPed*)pTarget)->UpdateRpHAnim();
#endif

	if(CModelInfo::GetModelInfo(pTarget->GetModelIndex())->GetModelType() == MITYPE_VEHICLE){
		// The viewer target is a CObject, so CRenderer::RenderOneNonRoad does
		// not take the vehicle branch. Run the same setup here so the
		// rotor/tail-rotor alpha atomics are sorted and drawn like the game.
		bool resetLights = pTarget->SetupLighting();
		CVisibilityPlugins::SetupVehicleVariables(pTarget->GetClump());
		CVisibilityPlugins::InitAlphaAtomicList();
		SetCullMode(rwCULLMODECULLNONE);
		pTarget->Render();
		pTarget->bImBeingRendered = true;
		CVisibilityPlugins::RenderAlphaAtomics();
		pTarget->bImBeingRendered = false;
		SetCullMode(rwCULLMODECULLBACK);
		pTarget->RemoveLighting(resetLights);
	}else{
		pTarget->Render();
		CRenderer::RenderOneNonRoad(pTarget);
	}
}

void
CAnimViewer::Initialise(void) {

	// we need messages, messages needs hud, hud needs those
	int hudSlot = CTxdStore::AddTxdSlot("hud");
	CTxdStore::LoadTxd(hudSlot, "MODELS/HUD.TXD");
	CHud::m_Wants_To_Draw_Hud = false;

	animTxdSlot = CTxdStore::AddTxdSlot("generic");
	CTxdStore::Create(animTxdSlot);
	int particleSlot = CTxdStore::AddTxdSlot("particle");
	CTxdStore::LoadTxd(particleSlot, "MODELS/PARTICLE.TXD");
	CTxdStore::SetCurrentTxd(animTxdSlot);
	CPools::Initialise();
	CReferences::Init();
	TheCamera.Init();
	TheCamera.SetRwCamera(Scene.camera);
	TheCamera.Cams[TheCamera.ActiveCam].Front = CVector(0.0f, 1.0f, 0.0f);
	TheCamera.Cams[TheCamera.ActiveCam].Distance = 5.0f;
	ThePaths.Init();
	ThePaths.AllocatePathFindInfoMem(4500);
	CCollision::Init();
	CWorld::Initialise();
	mod_HandlingManager.Initialise();
	CTempColModels::Initialise();
	CAnimManager::Initialise();
	CModelInfo::Initialise();
	CParticle::Initialise();
	CCarCtrl::Init();
	CPedStats::Initialise();
	CMessages::Init();
	CdStreamAddImage("MODELS\\GTA3.IMG");
	CFileLoader::LoadLevel("DATA\\DEFAULT.DAT");
	CFileLoader::LoadLevel("DATA\\ANIMVIEWER.DAT");
	CStreaming::Init();
	for(int i = 0; i < MODELINFOSIZE; i++)
		if(CModelInfo::GetModelInfo(i))
			CModelInfo::GetModelInfo(i)->ConvertAnimFileIndex();
	CStreaming::LoadInitialPeds();
	CStreaming::RequestSpecialModel(MI_PLAYER, "player", STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);
	CRenderer::Init();
	CVehicleModelInfo::LoadVehicleColours();
#ifdef FIX_BUGS
	CVehicleModelInfo::LoadEnvironmentMaps();
#endif
	CAnimManager::LoadAnimFiles();
	CWorld::PlayerInFocus = 0;
	// Models only: weapons are not needed, and skipping this also avoids the
	// weapon.dat short-line parser edge case in the viewer.
	CPed::Initialise();
	CTimer::Initialise();
	CClock::Initialise(60000);
	CTimeCycle::Initialise();
	CCarCtrl::Init();
	CPlayerPed *player = new CPlayerPed();
	player->SetPosition(1000.0f, 1000.0f, 1000.0f);
	CWorld::Players[0].m_pPed = player;
	CDraw::SetFOV(120.0f);
	CDraw::ms_fLODDistance = 500.0f;

	// Special-character models (MI_SPECIAL01..MI_SPECIAL21) are skipped by
	// NextModelId; do not load them here. Requesting all of them with
	// STREAMFLAGS_DONT_REMOVE wastes streaming memory, and creating a CPed
	// from one of them hard-locks the Amiga viewer.

	// From LCS. idk if needed
	int vanBlock = CAnimManager::GetAnimationBlockIndex("van");
	int bikesBlock = CAnimManager::GetAnimationBlockIndex("bikes");
	int bikevBlock = CAnimManager::GetAnimationBlockIndex("bikev");
	int bikehBlock = CAnimManager::GetAnimationBlockIndex("bikeh");
	int bikedBlock = CAnimManager::GetAnimationBlockIndex("biked");
	CStreaming::FlushRequestList();
	CStreaming::RequestAnim(vanBlock, STREAMFLAGS_DEPENDENCY);
	CStreaming::RequestAnim(bikesBlock, STREAMFLAGS_DEPENDENCY);
	CStreaming::RequestAnim(bikevBlock, STREAMFLAGS_DEPENDENCY);
	CStreaming::RequestAnim(bikehBlock, STREAMFLAGS_DEPENDENCY);
	CStreaming::RequestAnim(bikedBlock, STREAMFLAGS_DEPENDENCY);
	CStreaming::LoadAllRequestedModels(false);
	CAnimManager::AddAnimBlockRef(vanBlock);
	CAnimManager::AddAnimBlockRef(bikesBlock);
	CAnimManager::AddAnimBlockRef(bikevBlock);
	CAnimManager::AddAnimBlockRef(bikehBlock);
	CAnimManager::AddAnimBlockRef(bikedBlock);
}

int
LastPedModelId(int modelId)
{
	CBaseModelInfo *model;
	for(;;){
		assert(modelId < MODELINFOSIZE);
		model = CModelInfo::GetModelInfo(modelId);
		if (model && model->GetModelType() == MITYPE_PED
#ifdef FIX_BUGS
		    && !(modelId >= MI_SPECIAL01 && modelId <= MI_SPECIAL21)
#endif
		    )
			break;
		modelId--;
	}
	return modelId;
}

int
FirstCarModelId(int modelId)
{
	CBaseModelInfo *model;
	for(;;){
		assert(modelId < MODELINFOSIZE);
		model = CModelInfo::GetModelInfo(modelId);
		if (model && model->GetModelType() == MITYPE_VEHICLE)
			break;
		modelId++;
	}
	return modelId;
}


static bool IsViewerModelUsable(int modelId);

int
NextModelId(int modelId, int wantedChange)
{
	// Max. 2 trials wasn't here, it's me that added it.

	int tryCount = 2;
	int ogModelId = modelId;

	while(tryCount != 0) {
		modelId += wantedChange;
		if (modelId < 0 || modelId >= MODELINFOSIZE) {
			tryCount--;
			wantedChange = -wantedChange;
		} else if (modelId != 5 && modelId != 6 && modelId != 405 &&
		           IsViewerModelUsable(modelId)) {
			return modelId;
		}
	}
	return ogModelId;
}

static bool
IsViewerModelUsable(int modelId)
{
	if(modelId < 0 || modelId >= MODELINFOSIZE)
		return false;
	if(modelId >= MI_SPECIAL01 && modelId <= MI_SPECIAL21)
		return false;
	CBaseModelInfo *model = CModelInfo::GetModelInfo(modelId);
	if(model == nil)
		return false;
	int type = model->GetModelType();
	return type == MITYPE_PED || type == MITYPE_VEHICLE ||
	       type == MITYPE_SIMPLE || type == MITYPE_CLUMP;
}

static int
ViewerStartModel(void)
{
	const char *startModel = getenv("REVC_ANV_MODEL");
	if(startModel){
		int id = atoi(startModel);
		if(IsViewerModelUsable(id))
			return id;
	}
	return 0;
}

void
PlayAnimation(RpClump *clump, AssocGroupId animGroup, AnimationId anim)
{
	CAnimBlendAssociation *currentAssoc = RpAnimBlendClumpGetAssociation(clump, anim);

	if (currentAssoc && currentAssoc->IsPartial())
		delete currentAssoc;

	RpAnimBlendClumpSetBlendDeltas(clump, ASSOC_PARTIAL, -8.0f);

	CAnimBlendAssociation *animAssoc = CAnimManager::BlendAnimation(clump, animGroup, anim, 8.0f);
	animAssoc->flags |= ASSOC_DELETEFADEDOUT;
	animAssoc->SetCurrentTime(0.0f);
	animAssoc->SetRun();
}

void
CAnimViewer::Update(void)
{
	static int modelId = -1;
	static int animId = 0;
	static bool reloadIFP = false;

	if(modelId < 0)
		modelId = ViewerStartModel();

	AssocGroupId animGroup = ASSOCGRP_STD;
	int nextModelId = modelId;
	CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(modelId);

	if (modelInfo->GetModelType() == MITYPE_PED) {
		animGroup = (AssocGroupId)((CPedModelInfo*)modelInfo)->m_animGroup;

		if (animId > ANIM_STD_IDLE)
			animGroup = ASSOCGRP_STD;

		if (reloadIFP) {
			if (pTarget) {
				CWorld::Remove(pTarget);
				if (pTarget)
					delete pTarget;
			}
			pTarget = nil;
			
			// These calls were inside of LoadIFP function.
			CAnimManager::Shutdown();
			CAnimManager::Initialise();
			CAnimManager::LoadAnimFiles();

			reloadIFP = false;
		}
	} else {
		animGroup = ASSOCGRP_STD;
	}
	CPad::UpdatePads();
	CPad* pad = CPad::GetPad(0);
#ifdef DEBUGMENU
	DebugMenuProcess();
#endif

	CStreaming::UpdateForAnimViewer();
	CStreaming::RequestModel(modelId, 0);
	if (CStreaming::HasModelLoaded(modelId)) {

		bool targetJustCreated = false;
		if (!pTarget) {

			if (modelInfo->GetModelType() == MITYPE_VEHICLE) {
				// Display-only viewer: build the vehicle clump as a CObject.
				// A full CAutomobile requires handling, wheel pools and vehicle
				// simulation state; creating it here hard-locks the Amiga.
				pTarget = new CObject(modelId, true);
				if (!modelInfo->GetColModel())
					modelInfo->SetColModel(&CTempColModels::ms_colModelWheel1);
				pTarget->SetStatus(STATUS_ABANDONED);
			} else if (modelInfo->GetModelType() == MITYPE_PED) {
				pTarget = new CPed(PEDTYPE_CIVMALE);
				pTarget->SetModelIndex(modelId);
			} else {
				pTarget = new CObject(modelId, true);
				if (!modelInfo->GetColModel())
				{
					modelInfo->SetColModel(&CTempColModels::ms_colModelWheel1);
				}
				pTarget->SetStatus(STATUS_ABANDONED);
			}
			pTarget->SetPosition(0.0f, 0.0f, 0.0f);
			CWorld::Add(pTarget);
			TheCamera.TakeControl(pTarget, CCam::MODE_MODELVIEW, JUMP_CUT, CAMCONTROL_SCRIPT);
			targetJustCreated = true;
		}
		if (pTarget->IsVehicle() || pTarget->IsPed() || pTarget->IsObject()) {
			((CPhysical*)pTarget)->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
		}
#ifdef FIX_BUGS
		// so we don't end up in the water
		pTarget->GetMatrix().GetPosition().z = 10.0f;
#else
		pTarget->GetMatrix().GetPosition().z = 0.0f;
#endif

		if (modelInfo->GetModelType() == MITYPE_PED) {
			((CPed*)pTarget)->bKindaStayInSamePlace = true;

			if (targetJustCreated) {
				// Keep the animation selected before the model switch. If the
				// new model's group does not have that animation, use anim 0.
				if (CAnimManager::GetAnimAssocGroups()[animGroup].numAssociations <= animId)
					animId = 0;
				PlayAnimation(pTarget->GetClump(), animGroup, (AnimationId)animId);
			}

			// Triangle in mobile
			if (pad->GetSquareJustDown()) {
				reloadIFP = true;
				AsciiToUnicode("IFP reloaded", gUString);
				CMessages::AddMessage(gUString, 1000, 0);

			} else if (pad->GetCrossJustDown()) {
				PlayAnimation(pTarget->GetClump(), animGroup, (AnimationId)animId);
				AsciiToUnicode("Animation restarted", gUString);
				CMessages::AddMessage(gUString, 1000, 0);

			} else if (pad->GetCircleJustDown()) {
				PlayAnimation(pTarget->GetClump(), animGroup, ANIM_STD_IDLE);
				AsciiToUnicode("Idle animation playing", gUString);
				CMessages::AddMessage(gUString, 1000, 0);

			} else if (pad->GetDPadUpJustDown()) {
				animId--;
				if (animId < 0) {
					animId = ANIM_STD_NUM - 1;
				}
				PlayAnimation(pTarget->GetClump(), animGroup, (AnimationId)animId);

				sprintf(gString, "Current anim: %d", animId);
				AsciiToUnicode(gString, gUString);
				CMessages::AddMessage(gUString, 1000, 0);

			} else if (pad->GetDPadDownJustDown()) {
				animId = (animId == (ANIM_STD_NUM - 1) ? 0 : animId + 1);
				PlayAnimation(pTarget->GetClump(), animGroup, (AnimationId)animId);

				sprintf(gString, "Current anim: %d", animId);
				AsciiToUnicode(gString, gUString);
				CMessages::AddMessage(gUString, 1000, 0);

			} else if (pad->GetStartJustDown()) {

			} else if (pad->GetLeftShoulder1JustDown()) {
				nextModelId = FirstCarModelId(modelId);
				AsciiToUnicode("Switched to vehicles", gUString);
				CMessages::AddMessage(gUString, 1000, 0);
				// Originally it was GetPad(1)->LeftShoulder2
			} else if (pad->NewState.Triangle) {
				((CPedModelInfo *)CModelInfo::GetModelInfo(pTarget->GetModelIndex()))->AnimatePedColModelSkinned(pTarget->GetClump());
				AsciiToUnicode("Ped Col model will be animated as long as you hold the button", gUString);
				CMessages::AddMessage(gUString, 100, 0);
			}

			// From LCS
			if (CAnimManager::GetAnimAssocGroups()[animGroup].numAssociations <= animId)
				animId = 0;

		} else if (modelInfo->GetModelType() == MITYPE_VEHICLE) {

			if (pad->GetLeftShoulder1JustDown()) {
				nextModelId = LastPedModelId(modelId);
				AsciiToUnicode("Switched to peds", gUString);
				CMessages::AddMessage(gUString, 1000, 0);
				// Start in mobile
			} else if (pad->GetSquareJustDown()) {
				CVehicleModelInfo::LoadVehicleColours();
				AsciiToUnicode("Carcols.dat reloaded", gUString);
				CMessages::AddMessage(gUString, 1000, 0);
			}
		}
	}

	if (pad->GetDPadLeftJustDown()) {
		nextModelId = NextModelId(modelId, -1);

		sprintf(gString, "Current model ID: %d", nextModelId);
		AsciiToUnicode(gString, gUString);
		CMessages::AddMessage(gUString, 1000, 0);

	} else if (pad->GetDPadRightJustDown()) {
		nextModelId = NextModelId(modelId, 1);

		sprintf(gString, "Current model ID: %d", nextModelId);
		AsciiToUnicode(gString, gUString);
		CMessages::AddMessage(gUString, 1000, 0);
	}
	// There were extra codes here to let us change model id by 50, but xbox CPad struct is different, so I couldn't port.

	if (nextModelId != modelId) {
		modelId = nextModelId;
			if (pTarget) {
			CWorld::Remove(pTarget);
			if (pTarget)
				delete pTarget;
		}
		pTarget = nil;
		return;
	}

	if (pTarget) {
		// SetPosition/SetOrientation only touch the entity matrix. The clump
		// frame is normally synced by CWorld::Process(), which the viewer does
		// not run; update it explicitly or the model stays at (0,0,0).
		pTarget->GetMatrix().UpdateRW();
		pTarget->UpdateRwFrame();
	}

	CTimeCycle::Update();
	// The viewer is display-only: do not run world/AI/physics simulation.
	// CWorld::Process() depends on CSurfaceTable, DMAudio, CColStore, CEventList,
	// CRouteNode and CPopulation, none of which the viewer initialises. Advance
	// the target's animation association directly so the model still animates.
	if (pTarget) {
		if (pTarget->IsPed())
			RpAnimBlendClumpUpdateAnimations(pTarget->GetClump(), CTimer::GetTimeStepInSeconds());
		TheCamera.Process();
	}
}

void
CAnimViewer::Shutdown(void)
{
	if (CWorld::Players[0].m_pPed)
		delete CWorld::Players[0].m_pPed;

	CWorld::ShutDown();
	CModelInfo::ShutDown();
	CAnimManager::Shutdown();
	CTimer::Shutdown();
	CStreaming::Shutdown();
	CTxdStore::RemoveTxdSlot(animTxdSlot);
}
