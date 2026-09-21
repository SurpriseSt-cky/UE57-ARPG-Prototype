// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "DodgeAbility.generated.h"

class ABattleCharacter;

/**
 * 闪避 GameplayAbility（P3：外壳式封装）。
 *
 * 该 Ability 是闪避的 GAS 外壳：ActivateAbility 时直接调用角色现有的 Dodge()
 * 函数（打断普攻/技能、完美闪避判定、蒙太奇、位移、残影等核心逻辑保留在原函数）。
 * 闪避无敌帧已由无敌 GameplayEffect 独立管理时长，故 Dodge() 返回后即可 EndAbility。
 */
UCLASS()
class WUTHERINGWAVES_API UDodgeAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    UDodgeAbility();

    virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags = nullptr,
        const FGameplayTagContainer* TargetTags = nullptr,
        FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        const FGameplayEventData* TriggerEventData) override;
};
