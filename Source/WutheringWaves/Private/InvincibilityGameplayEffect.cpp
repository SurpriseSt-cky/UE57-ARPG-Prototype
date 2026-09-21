// Copyright WutheringWaves. All Rights Reserved.

#include "InvincibilityGameplayEffect.h"
#include "WutheringWavesTags.h"
#include "GameplayTagContainer.h"
#include "GameplayModMagnitudeCalculation.h"

UInvincibilityGameplayEffect::UInvincibilityGameplayEffect()
{
    // 有持续时间：到期自动移除 Granted Tag（无需手动 Timer）
    DurationPolicy = EGameplayEffectDurationType::HasDuration;

    // 时长通过 SetByCaller 标签 "State.Invincible.Duration" 动态传入
    // （普通闪避 0.3s，完美闪避为动态计算的 PerfectInvincibilityTime）
    FSetByCallerFloat DurationByCaller;
    DurationByCaller.DataTag = WutheringWavesTags::State_Invincible_Duration_Tag();
    DurationMagnitude = FGameplayEffectModifierMagnitude(DurationByCaller);

    // Grant 无敌 Tag：应用期间目标拥有 State.Invincible
    InheritableGameplayEffectTags.Added.AddTag(WutheringWavesTags::State_Invincible_Tag());
}
