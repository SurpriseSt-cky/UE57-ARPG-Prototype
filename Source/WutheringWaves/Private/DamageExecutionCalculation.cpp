// Copyright WutheringWaves. All Rights Reserved.

#include "DamageExecutionCalculation.h"
#include "BattleAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "WutheringWavesTags.h"

UDamageExecutionCalculation::UDamageExecutionCalculation()
{
    // 声明需要捕获的属性：护盾、生命（目标侧）
    ShieldCaptureDef = FGameplayEffectAttributeCaptureDefinition(
        UBattleAttributeSet::GetShieldAttribute(), EGameplayEffectAttributeCaptureSource::Target, false);
    HealthCaptureDef = FGameplayEffectAttributeCaptureDefinition(
        UBattleAttributeSet::GetHealthAttribute(), EGameplayEffectAttributeCaptureSource::Target, false);

    RelevantAttributesToCapture.Add(ShieldCaptureDef);
    RelevantAttributesToCapture.Add(HealthCaptureDef);
}

void UDamageExecutionCalculation::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
    FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
    const FGameplayTagContainer* SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
    const FGameplayTagContainer* TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

    FAggregatorEvaluateParameters EvalParams;
    EvalParams.SourceTags = SourceTags;
    EvalParams.TargetTags = TargetTags;

    // ---- 读取原始伤害值（SetByCaller，由 TakeDamage/ApplySilentDamage 传入）----
    float RawDamage = 0.0f;
    Spec.GetSetByCallerMagnitude(WutheringWavesTags::Damage_SetByCaller_Tag(), false, RawDamage);

    if (RawDamage <= 0.0f)
    {
        return; // 无伤害
    }

    // ---- 捕获目标当前护盾与生命 ----
    float Shield = 0.0f;
    float Health = 0.0f;
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(ShieldCaptureDef, EvalParams, Shield);
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(HealthCaptureDef, EvalParams, Health);

    // ---- 护盾优先吸收，剩余扣血 ----
    const float Absorbed = FMath::Min(Shield, RawDamage);
    const float OverflowToHealth = RawDamage - Absorbed;

    // 输出：护盾 - Absorbed，生命 - OverflowToHealth
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
