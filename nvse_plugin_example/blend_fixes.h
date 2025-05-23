﻿#pragma once
#include "commands_animation.h"
#include "GameProcess.h"

void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest, NiControllerSequence* pkIdle);

namespace BlendFixes
{
	enum Result
	{
		RESUME, SKIP
	};

	Result ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim);
	void ApplyAttackISToAttackFix();
	void ApplyAttackToAttackISFix();
	void ApplyAimBlendHooks();
	void FixConflictingPriorities(BSAnimGroupSequence* pkSource, BSAnimGroupSequence* pkDest, BSAnimGroupSequence* pkIdle);
	void ApplyHooks();
	void FixPrematureFirstPersonEnd(AnimData* animData, BSAnimGroupSequence* anim);
	void ApplyMissingUpDownAnims(AnimData* animData);


	
}

template <typename F>
BSAnimGroupSequence* GetActiveSequenceWhere(AnimData* animData, F&& predicate)
{
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (IS_TYPE(bsSequence, BSAnimGroupSequence) && bsSequence && predicate(bsSequence))
			return bsSequence;
	}
	return nullptr;
}

inline BSAnimGroupSequence* GetActiveSequenceByGroupID(AnimData* animData, AnimGroupID groupId)
{
	return GetActiveSequenceWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return sequence->animGroup->GetBaseGroupID() == groupId;
	});
}