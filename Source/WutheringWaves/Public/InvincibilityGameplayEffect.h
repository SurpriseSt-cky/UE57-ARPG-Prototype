// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "InvincibilityGameplayEffect.generated.h"

/**
 * 无敌 GameplayEffect（P3）。
 *
 * 通过 Grant "State.Invincible" GameplayTag 实现无敌帧：
 *   - 应用该 GE（带 Duration）→ 目标拥有 State.Invincible Tag
 *   - TakeDamage / IsInvincibleNow 改为查该 Tag（替代旧 bIsInvincible 布尔 + Timer）
 *   - GE 到期自动移除 Tag → 无敌结束（无需手动 Timer）
 *
 * 时长通过 SetByCaller 标签 "State.Invincible.Duration" 传入。
 */
UCLASS()
class WUTHERINGWAVES_API UInvincibilityGameplayEffect : public UGameplayEffect
{
    GENERATED_BODY()

public:
    UInvincibilityGameplayEffect();
};
