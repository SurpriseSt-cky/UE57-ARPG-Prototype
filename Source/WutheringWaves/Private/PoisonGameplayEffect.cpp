// Copyright WutheringWaves. All Rights Reserved.

#include "PoisonGameplayEffect.h"
#include "PoisonExecutionCalculation.h"
#include "WutheringWavesTags.h"
#include "GameplayTagContainer.h"
#include "GameplayModMagnitudeCalculation.h"

UPoisonGameplayEffect::UPoisonGameplayEffect()
{
    // 有持续时间：到期自动移除 Granted Tag（替代手动 Tick 递减）
    DurationPolicy = EGameplayEffectDurationType::HasDuration;

    // 时长通过 SetByCaller 标签 "State.Poisoned.Duration" 动态传入
    FSetByCallerFloat DurationByCaller;
    DurationByCaller.DataTag = WutheringWavesTags::State_Poisoned_Duration_Tag();
    DurationMagnitude = FGameplayEffectModifierMagnitude(DurationByCaller);

    // 周期结算：每 Period 秒触发一次 Execution（毒伤）。
    // Period 是 FScalableFloat，固定默认 1s（与 PoisonTickInterval 默认值一致）。
    // 若需自定义周期，可在蓝图里改该 GE 的 Period 值。
    Period.SetValue(1.0f);
    bExecutePeriodicEffectOnApplication = false; // 施加瞬间不立即结算毒伤（对齐原 UpdatePoison 首个 tick 延迟 1s）

    // 周期毒伤通过 Execution 结算（护盾先吸收、剩余扣血，由 UPoisonExecutionCalculation 完成）
    FGameplayEffectExecutionDefinition ExecDef;
    ExecDef.CalculationClass = UPoisonExecutionCalculation::StaticClass();
    Executions.Add(ExecDef);

    // Grant 中毒 Tag：应用期间目标拥有 State.Poisoned
    InheritableGameplayEffectTags.Added.AddTag(WutheringWavesTags::State_Poisoned_Tag());
}
