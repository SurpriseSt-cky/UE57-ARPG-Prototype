// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectAttributeCaptureDefinition.h"
#include "DamageExecutionCalculation.generated.h"

/**
 * 伤害结算 Execution（P1：正统 GAS 伤害计算）。
 *
 * 由伤害 GameplayEffect 调用，负责「护盾先吸收 → 剩余扣血」的结算：
 *   1. 从 SetByCaller 读取原始伤害值（Damage）。
 *   2. 护盾（Shield 属性）优先吸收，超出部分扣 Health 属性。
 *   3. 写入 OutExecutionOutput 让 ASC 应用到目标的 AttributeSet。
 *
 * 注意：无敌帧判定不在此处理——由 ABattleCharacter::TakeDamage 在应用 GE 之前
 * 拦截（P3 阶段再把无敌帧迁移为 GameplayEffect）。
 */
UCLASS()
class WUTHERINGWAVES_API UDamageExecutionCalculation : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UDamageExecutionCalculation();

    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;

    // 捕获定义（护盾、生命——目标侧）
    FGameplayEffectAttributeCaptureDefinition ShieldCaptureDef;
    FGameplayEffectAttributeCaptureDefinition HealthCaptureDef;
};
