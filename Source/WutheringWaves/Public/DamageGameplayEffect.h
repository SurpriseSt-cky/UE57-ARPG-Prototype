// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "DamageGameplayEffect.generated.h"

/**
 * 伤害 GameplayEffect（P1）。
 *
 * 通过 Execution（UDamageExecutionCalculation）结算伤害：护盾先吸收、剩余扣血。
 * 伤害值由应用方通过 SetByCaller 标签 "Damage.SetByCaller" 传入。
 * 该 GE 为瞬时（Instant）效果，无持续时间、不叠加。
 */
UCLASS()
class WUTHERINGWAVES_API UDamageGameplayEffect : public UGameplayEffect
{
    GENERATED_BODY()

public:
    UDamageGameplayEffect();
};
