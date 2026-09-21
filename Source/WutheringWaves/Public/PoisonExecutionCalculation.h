// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectAttributeCaptureDefinition.h"
#include "PoisonExecutionCalculation.generated.h"

/**
 * 中毒周期伤害结算 Execution（P4）。
 *
 * 由中毒 GameplayEffect（Periodic）每个周期调用，负责「护盾先吸收 → 剩余扣血」的毒伤结算：
 *   1. 从 SetByCaller 读取每秒毒伤比例（PoisonDamagePerSecondRatio）。
 *   2. 捕获目标 MaxHealth，计算本周期毒伤 = MaxHealth * 比例 * 周期秒数。
 *   3. 护盾（Shield 属性）优先吸收，超出部分扣 Health 属性。
 *   4. 写入 OutExecutionOutput 让 ASC 应用到目标的 AttributeSet。
 *
 * 与原 UpdatePoison 的 Tick 扣血行为完全对齐（毒伤同样优先扣护盾）。
 */
UCLASS()
class WUTHERINGWAVES_API UPoisonExecutionCalculation : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UPoisonExecutionCalculation();

    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;

    // 捕获定义（护盾、生命、最大生命——目标侧）
    FGameplayEffectAttributeCaptureDefinition ShieldCaptureDef;
    FGameplayEffectAttributeCaptureDefinition HealthCaptureDef;
    FGameplayEffectAttributeCaptureDefinition MaxHealthCaptureDef;
};
