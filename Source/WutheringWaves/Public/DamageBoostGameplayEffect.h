// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "DamageBoostGameplayEffect.generated.h"

/**
 * 增伤 GameplayEffect（P4）。
 *
 * 通过 Grant "Buff.DamageBoost" GameplayTag 实现增伤状态：
 *   - 应用该 GE（带 Duration）→ 目标拥有 Buff.DamageBoost Tag
 *   - GetOutgoingDamageMultiplier 改查该 Tag（替代旧 bUltimateBuffActive 布尔）
 *   - GE 到期自动移除 Tag → 增伤结束（无需手动 Timer）
 *
 * 时长通过 SetByCaller 标签 "Buff.DamageBoost.Duration" 传入；
 * 增伤幅度通过 SetByCaller 标签 "Buff.DamageBoost.Multiplier" 传入（由 GetOutgoingDamageMultiplier 读取）。
 */
UCLASS()
class WUTHERINGWAVES_API UDamageBoostGameplayEffect : public UGameplayEffect
{
    GENERATED_BODY()

public:
    UDamageBoostGameplayEffect();
};
