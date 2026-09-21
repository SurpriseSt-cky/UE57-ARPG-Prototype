// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "PoisonGameplayEffect.generated.h"

/**
 * 中毒 GameplayEffect（P4）。
 *
 * 通过 Grant "State.Poisoned" GameplayTag 实现中毒状态，周期（Periodic）结算毒伤：
 *   - HasDuration + Period：持续期间每 Period 秒触发一次 UPoisonExecutionCalculation 结算毒伤
 *   - 应用该 GE → 目标拥有 State.Poisoned Tag（IsPoisoned/RefreshStatusIcons 据此判定）
 *   - GE 到期自动移除 Tag → 中毒结束（替代旧 UpdatePoison 的 Tick 手动递减）
 *
 * 时长、周期秒数、每秒毒伤比例均通过 SetByCaller 标签动态传入。
 */
UCLASS()
class WUTHERINGWAVES_API UPoisonGameplayEffect : public UGameplayEffect
{
    GENERATED_BODY()

public:
    UPoisonGameplayEffect();
};
