// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "UltimateAbility.generated.h"

class ABattleCharacter;

/**
 * 大招 GameplayAbility（P2：外壳式封装样板）。
 *
 * 该 Ability 是大招（R 键）的 GAS 外壳：ActivateAbility 时直接调用角色现有的
 * Ultimate() 函数（多段蒙太奇/伤害/镜头/时停等核心逻辑仍保留在原函数），
 * 再通过 Timer 轮询 IsUltimateCasting() 检测大招结束，随后 EndAbility。
 *
 * 目的：验证 GAS 能力框架（TryActivateAbility → ActivateAbility → EndAbility）
 * 能跑通，为后续把技能/能量技也迁入 Ability 打样板。核心逻辑暂不重写，风险可控。
 */
UCLASS()
class WUTHERINGWAVES_API UUltimateAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    UUltimateAbility();

    virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags = nullptr,
        const FGameplayTagContainer* TargetTags = nullptr,
        FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        const FGameplayEventData* TriggerEventData) override;

    virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        bool bReplicateEndAbility,
        bool bWasCancelled) override;

protected:
    // 轮询大招是否结束（IsUltimateCasting 变 false）
    void PollUltimateEnd();

    // 轮询间隔（秒）
    float PollInterval = 0.05f;

    FTimerHandle PollTimerHandle;
};
