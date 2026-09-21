// Copyright WutheringWaves. All Rights Reserved.

#include "DamageBoostGameplayEffect.h"
#include "WutheringWavesTags.h"
#include "GameplayTagContainer.h"
#include "GameplayModMagnitudeCalculation.h"

UDamageBoostGameplayEffect::UDamageBoostGameplayEffect()
{
    // 有持续时间：到期自动移除 Granted Tag（替代手动 Timer）
    DurationPolicy = EGameplayEffectDurationType::HasDuration;

    // 时长通过 SetByCaller 标签 "Buff.DamageBoost.Duration" 动态传入
    FSetByCallerFloat DurationByCaller;
    DurationByCaller.DataTag = WutheringWavesTags::Buff_DamageBoost_Duration_Tag();
    DurationMagnitude = FGameplayEffectModifierMagnitude(DurationByCaller);

    // Grant 增伤 Tag：应用期间目标拥有 Buff.DamageBoost（GetOutgoingDamageMultiplier 据此判定）
    InheritableGameplayEffectTags.Added.AddTag(WutheringWavesTags::Buff_DamageBoost_Tag());
}
