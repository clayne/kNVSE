#pragma once
#include <functional>
#include <deque>
#include <PluginAPI.h>

#include "commands_animation.h"

extern std::deque<std::function<void()>> g_executionQueue;
extern NVSEArrayVarInterface* g_arrayVarInterface;
extern NVSEStringVarInterface* g_stringVarInterface;

#define IS_TRANSITION_FIX 0

void Revert3rdPersonAnimTimes(AnimTime& animTime, BSAnimGroupSequence* anim);

bool IsGodMode();

float GetAnimTime(const AnimData* animData, const BSAnimGroupSequence* anim);