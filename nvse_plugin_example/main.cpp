
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "commands_animation.h"
#include "hooks.h"
#include "utility.h"
#include <filesystem>
#include <span>

#include "file_animations.h"
#include "GameData.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "game_types.h"
#include <set>

#include "NiObjects.h"
#include "SimpleINILibrary.h"

#define RegisterScriptCommand(name) 	nvse->RegisterCommand(&kCommandInfo_ ##name);

IDebugLog		gLog("kNVSE.log");

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface;
NVSEInterface* g_nvseInterface;
NVSECommandTableInterface* g_cmdTable;
const CommandInfo* g_TFC;
PlayerCharacter* g_player;


#if RUNTIME
NVSEScriptInterface* g_script;
#endif

bool isEditor = false;
extern std::list<BurstFireData> g_burstFireQueue;

void HandleAnimTimes()
{
	for (auto iter = g_timeTrackedAnims.begin(); iter != g_timeTrackedAnims.end(); ++iter)
	{
		auto& [anim, animTime] = *iter;
		auto& time = animTime.time;
		auto* animData = animTime.animData;
		auto* actor = animData->actor;
		time += GetTimePassed(animTime.animData, anim->animGroup->groupID);

		if (animTime.respectEndKey)
		{
			const auto revert3rdPersonAnimTimes = [&]()
			{
				animTime.anim3rdCounterpart->animGroup->numKeys = animTime.numThirdPersonKeys;
				animTime.anim3rdCounterpart->animGroup->keyTimes = animTime.thirdPersonKeys;
				animTime.povState = POVSwitchState::POV3rd;
			};
			if (time >= anim->endKeyTime && !animTime.finishedEndKey || actor->GetWeaponForm() != animTime.actorWeapon)
			{
				revert3rdPersonAnimTimes();
				animTime.finishedEndKey = true;
			}
			else
			{
				if (animTime.povState == POVSwitchState::NotSet)
				{
					const auto sequenceId = anim->animGroup->GetGroupInfo()->sequenceType;
					auto* anim3rd = g_thePlayer->baseProcess->GetAnimData()->animSequence[sequenceId];
					if (!anim3rd || (anim3rd->animGroup->groupID & 0xFF) != (anim->animGroup->groupID & 0xFF))
						anim3rd = anim->Get3rdPersonCounterpart();
					if (!anim3rd)
						continue;
					animTime.anim3rdCounterpart = anim3rd;
					if (!animTime.thirdPersonKeys)
					{
						animTime.numThirdPersonKeys = anim3rd->animGroup->numKeys;
						animTime.thirdPersonKeys = anim3rd->animGroup->keyTimes;
					}
				}
				if (g_thePlayer->IsThirdPerson() && animTime.povState != POVSwitchState::POV3rd)
				{
					revert3rdPersonAnimTimes();
				}
				else if (!g_thePlayer->IsThirdPerson() && animTime.povState != POVSwitchState::POV1st)
				{
					animTime.anim3rdCounterpart->animGroup->numKeys = anim->animGroup->numKeys;
					animTime.anim3rdCounterpart->animGroup->keyTimes = anim->animGroup->keyTimes;
					animTime.povState = POVSwitchState::POV1st;
				}
			}
		}
		if (animTime.callScript && animTime.scriptStage < animTime.scripts.size())
		{
			auto& p = animTime.scripts.at(animTime.scriptStage);
			if (time > p.second)
			{
				g_script->CallFunction(p.first, animTime.animData->actor, nullptr, nullptr, 0);
				++animTime.scriptStage;
			}
		}
	}
	for (auto& [savedAnim, animTime] : g_timeTrackedGroups)
	{
		auto& [time, animData, groupId, anim] = animTime;
		time += GetTimePassed(animTime.animData, groupId);
		auto* actor = animData->actor;
		if (savedAnim->conditionScript)
		{
			auto* animInfo = GetGroupInfo(groupId);
			auto* curAnim = animData->animSequence[animInfo->sequenceType];
			if (curAnim && curAnim->animGroup->groupID == groupId)
			{
				NVSEArrayVarInterface::Element arrResult;
				if (g_script->CallFunction(savedAnim->conditionScript, actor, nullptr, &arrResult, 0))
				{
					const auto result = static_cast<bool>(arrResult.Number());
					const auto customAnimState = anim ? anim->state : kAnimState_Inactive;
					if (customAnimState == kAnimState_Inactive && result && curAnim != anim
						|| customAnimState != kAnimState_Inactive && !result && curAnim == anim)
					{
						auto* resultAnim = GameFuncs::PlayAnimGroup(animData, groupId, 1, -1, -1);
#if _DEBUG
						if (!resultAnim)
							DebugBreak();
#endif
					}
				}
			}
		}
	}
}

void HandleBurstFire()
{
	auto iter = g_burstFireQueue.begin();
	while (iter != g_burstFireQueue.end())
	{
		auto& [animData, anim, index, hitKeys, timePassed, shouldEject, lastNiTime] = *iter;
		auto* currentAnim = animData->animSequence[kSequence_Weapon];
		auto* weap = animData->actor->baseProcess->GetWeaponInfo();
		auto* weapon = weap ? weap->weapon : nullptr;
		auto* actor = animData->actor;
		const auto erase = [&]()
		{
			iter = g_burstFireQueue.erase(iter);
		};
		timePassed += GetTimePassed(animData, anim->animGroup->groupID);
		if (currentAnim != anim)
		{
			erase();
			continue;
		}
		if (anim->lastScaledTime < lastNiTime)
		{
			erase();
			continue;
		}
		lastNiTime = anim->lastScaledTime;
		if (timePassed <= anim->animGroup->keyTimes[kSeqState_HitOrDetach])
		{
			// first hit handled by engine
			// don't want duplicated shootings
			++iter;
			continue;
		}
		if (timePassed > hitKeys.at(index)->m_fTime)
		{
			if (auto* ammoInfo = actor->baseProcess->GetAmmoInfo()) // static_cast<Decoding::MiddleHighProcess*>(animData->actor->baseProcess)->ammoInfo
			{
				const bool* godMode = reinterpret_cast<bool*>(0x11E07BA);
				if (!*godMode)
				{
					//const auto clipSize = GetWeaponInfoClipSize(actor);
					if (DidActorReload(actor, ReloadSubscriber::BurstFire) || ammoInfo->count == 0)
					{
						// reloaded
						erase();
						continue;
					}
#if 0
					const auto ammoCount = ammoInfo->countDelta;
					if (weapon && (ammoCount == 0 || ammoCount == clipSize))
					{
						erase();
						continue;
					}
#endif
				}
			}
			++index;
			if (!IsPlayersOtherAnimData(animData))
			{
				animData->actor->FireWeapon();
				if (weapon && GameFuncs::IsDoingAttackAnimation(animData->actor) && !weapon->IsMeleeWeapon() && !weapon->IsAutomatic())
				{
					// eject
					animData->actor->baseProcess->SetQueuedIdleFlag(kIdleFlag_AttackEjectEaseInFollowThrough);
					GameFuncs::HandleQueuedAnimFlags(animData->actor);
				}
			}
		}
		
		if (index < hitKeys.size())
			++iter;
		else
			erase();
	}
}
const auto kNVSEVersion = 8;

bool __fastcall ActivateToBlendHook(NiControllerManager* mgr, void* _EDX, BSAnimGroupSequence* from, BSAnimGroupSequence* to, float fEaseIn, int priority, bool startOver, float weight, NiControllerSequence* sync)
{
	return GameFuncs::BlendFromPose(mgr, to, fEaseIn, to->destFrame * 0.1f, 0, sync);
}

void HandleProlongedAim()
{
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* animData1st = g_thePlayer->firstPersonAnimData;
	auto* highProcess = g_thePlayer->GetHighProcess();
	auto* curWeaponAnim = animData3rd->animSequence[kSequence_Weapon];
	if (!curWeaponAnim || !curWeaponAnim->animGroup->IsAttackIS() || highProcess->isAiming)
		return;
	auto* weapon = g_thePlayer->GetWeaponForm();
	if (!weapon)
		return;
	const auto curGroupId = curWeaponAnim->animGroup->groupID;
	const UInt16 hipfireId = curGroupId - 3;

	//SafeWrite8(0x495244 + 1, 0);
	//SafeWriteBuf(0xA35008, "\xEB\x9", 2);
	//WriteRelCall(0x4952F8, ActivateToBlendHook);
	for (auto* animData : {animData3rd, animData1st})
	{
		auto* hipfireAnim = GetGameAnimation(animData, hipfireId);
		auto* sourceAnim = animData->animSequence[kSequence_Weapon];
		//animData->animSequence[kSequence_Weapon] = hipfireAnim;
		//animData->groupIDs[kSequence_Weapon] = hipfireId;
		
		//hipfireAnim->offset = -sourceAnim->startTime;
		//hipfireAnim->startTime = sourceAnim->startTime;
		hipfireAnim->destFrame = sourceAnim->startTime / hipfireAnim->frequency;
		//animData->noBlend120 = true;
		//const auto oldBlend = hipfireAnim->animGroup->blend;
		std::span blocks{ sourceAnim->controlledBlocks, sourceAnim->numControlledBlocks };
		
		GameFuncs::PlayAnimGroup(animData, hipfireId, 1, -1, -1);
		
		//auto* niBlock = GetNifBlock(g_thePlayer, 2, "Bip01 L Thumb12");
		//hipfireAnim->animGroup->blend = oldBlend;
		//const auto duration = sourceAnim->endKeyTime - (sourceAnim->startTime + sourceAnim->offset);
		//const auto result = GameFuncs::BlendFromPose(animData->controllerManager, hipfireAnim, sourceAnim->startTime, 0.0f, 0, nullptr);
		int i = 0;
	}
	//WriteRelCall(0x4952F8, 0xA2E280);
	//SafeWriteBuf(0xA35008, "\xD9\xEE", 2);
	//SafeWrite8(0x495244 + 1, 1);
	highProcess->SetCurrentActionAndSequence(hipfireId, GetGameAnimation(animData3rd, hipfireId));
}

void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		g_thePlayer = *(PlayerCharacter **)0x011DEA3C;
		LoadFileAnimPaths();
		Console_Print("kNVSE version %d", kNVSEVersion);
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		const auto isMenuMode = CdeclCall<bool>(0x702360);
		if (!isMenuMode)
		{
			HandleBurstFire();
			HandleAnimTimes();
			//HandleProlongedAim();
			HandleOnActorReload();
			//auto* niBlock = GetNifBlock(g_thePlayer, 2, "Bip01 L Thumb12");
			//static auto lastZ = niBlock->m_localTranslate.z;
			//if (lastZ != niBlock->m_localTranslate.z)
				//Console_Print("%g", niBlock->m_localTranslate.z);
		}
	}

}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = kNVSEVersion;

	// version checks
	if (!nvse->isEditor && nvse->nvseVersion < PACKED_NVSE_VERSION)
	{
		const auto str = FormatString("kNVSE: NVSE version too old (got %X expected at least %X). Plugin will NOT load! Install the latest version here: https://github.com/xNVSE/NVSE/releases/", nvse->nvseVersion, PACKED_NVSE_VERSION);
#if !_DEBUG
		ShowErrorMessageBox(str.c_str());
#endif
		_ERROR(str.c_str());
		return false;
	}

	if (!nvse->isEditor)
	{
		if (nvse->runtimeVersion < RUNTIME_VERSION_1_4_0_525)
		{
			_ERROR("incorrect runtime version (got %08X need at least %08X)", nvse->runtimeVersion, RUNTIME_VERSION_1_4_0_525);
			return false;
		}

		if (nvse->isNogore)
		{
			_ERROR("NoGore is not supported");
			return false;
		}
	}

	else
	{
		isEditor = true;
		if (nvse->editorVersion < CS_VERSION_1_4_0_518)
		{
			_ERROR("incorrect editor version (got %08X need at least %08X)", nvse->editorVersion, CS_VERSION_1_4_0_518);
			return false;
		}
	}
	return true;
}


int g_logLevel = 0;

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
#if _DEBUG
	if (false)
	{
		PatchPause(0);
		g_thePlayer->baseProcess->GetCurrentSequence();
	}
#endif
	g_pluginHandle = nvse->GetPluginHandle();
	g_nvseInterface = (NVSEInterface*)nvse;
	g_messagingInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);
	g_script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	
	nvse->SetOpcodeBase(0x3920);
	
	RegisterScriptCommand(ForcePlayIdle)
	RegisterScriptCommand(SetWeaponAnimationPath)
	RegisterScriptCommand(SetActorAnimationPath)
	RegisterScriptCommand(PlayAnimationPath)
	RegisterScriptCommand(ForceStopIdle)
	
	if (isEditor)
	{
		return true;
	}

	ApplyHooks();
	return true;
}
