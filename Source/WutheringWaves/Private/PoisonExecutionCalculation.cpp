// Copyright WutheringWaves. All Rights Reserved.

#include "PoisonExecutionCalculation.h"
#include "BattleAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "WutheringWavesTags.h"

UPoisonExecutionCalculation::UPoisonExecutionCalculation()
{
    // 声明需要捕获的属性：护盾、生命、最大生命（目标侧）
    ShieldCaptureDef = FGameplayEffectAttributeCaptureDefinition(
        UBattleAttributeSet::GetShieldAttribute(), EGameplayEffectAttributeCaptureSource::Target, false);
    HealthCaptureDef = FGameplayEffectAttributeCaptureDefinition(
        UBattleAttributeSet::GetHealthAttribute(), EGameplayEffectAttributeCaptureSource::Target, false);
    MaxHealthCaptureDef = FGameplayEffectAttributeCaptureDefinition(
        UBattleAttributeSet::GetMaxHealthAttribute(), EGameplayEffectAttributeCaptureSource::Target, false);

    RelevantAttributesToCapture.Add(ShieldCaptureDef);
    RelevantAttributesToCapture.Add(HealthCaptureDef);
    RelevantAttributesToCapture.Add(MaxHealthCaptureDef);
}

void UPoisonExecutionCalculation::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
    FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
    const FGameplayTagContainer* SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
    const FGameplayTagContainer* TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

    FAggregatorEvaluateParameters EvalParams;
    EvalParams.SourceTags = SourceTags;
    EvalParams.TargetTags = TargetTags;

    // ---- 读取每秒毒伤比例（SetByCaller，由 ApplyPoison 传入）----
    float Ratio = 0.0f;
    Spec.GetSetByCallerMagnitude(WutheringWavesTags::State_Poisoned_DamagePerSecondRatio_Tag(), false, Ratio);
    if (Ratio <= 0.0f)
    {
        return; // 无毒伤
    }

    // ---- 读取本 GE 的实际周期秒数（Period）----
    const float PeriodSeconds = FMath::Max(0.01f, Spec.GetPeriod());

    // ---- 捕获目标最大生命、护盾、生命 ----
    float MaxHealth = 0.0f;
    float Shield = 0.0f;
    float Health = 0.0f;
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(MaxHealthCaptureDef, EvalParams, MaxHealth);
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(ShieldCaptureDef, EvalParams, Shield);
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(HealthCaptureDef, EvalParams, Health);

    // 本周期毒伤 = 最大血量 × 每秒比例 × 周期秒数
    const float RawDamage = MaxHealth * Ratio * PeriodSeconds;
    if (RawDamage <= 0.0f)
    {
        return;
    }

    // ---- 护盾优先吸收，剩余扣血 ----
    const float Absorbed = FMath::Min(Shield, RawDamage);
    const float OverflowToHealth = RawDamage - Absorbed;

    if (Absorbed > 0.0f)
    {
        OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
            UBattleAttributeSet::GetShieldAttribute(), EGameplayModOp::Additive, -Absorbed));
    }
    if (OverflowToHealth > 0.0f)
    {
        OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
            UBattleAttributeSet::GetHealthAttribute(), EGameplayModOp::Additive, -OverflowToHealth));
    }
}
