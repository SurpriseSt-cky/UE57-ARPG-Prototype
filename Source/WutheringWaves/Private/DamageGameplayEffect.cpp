// Copyright WutheringWaves. All Rights Reserved.

#include "DamageGameplayEffect.h"
#include "DamageExecutionCalculation.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayModMagnitudeCalculation.h"

UDamageGameplayEffect::UDamageGameplayEffect()
{
    // 瞬时伤害：立即结算，无持续时间
    DurationPolicy = EGameplayEffectDurationType::Instant;

    // 通过 Execution 结算伤害（护盾吸收 + 扣血在 Execution 内完成）
    FGameplayEffectExecutionDefinition ExecDef;
    ExecDef.CalculationClass = UDamageExecutionCalculation::StaticClass();
    Executions.Add(ExecDef);
}
