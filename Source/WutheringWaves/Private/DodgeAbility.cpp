// Copyright WutheringWaves. All Rights Reserved.

#include "DodgeAbility.h"
#include "BattleCharacter.h"
#include "AbilitySystemComponent.h"

UDodgeAbility::UDodgeAbility()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
}

bool UDodgeAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayTagContainer* SourceTags,
    const FGameplayTagContainer* TargetTags,
    FGameplayTagContainer* OptionalRelevantTags) const
{
    if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
    {
        return false;
    }

    // 角色存在即可激活（Dodge() 内部会做受击硬直/大招/技能/锁定等完整判定）
    return Cast<ABattleCharacter>(GetAvatarActorFromActorInfo()) != nullptr;
}

void UDodgeAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    ABattleCharacter* Character = Cast<ABattleCharacter>(GetAvatarActorFromActorInfo());
    if (!Character)
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    // 调用现有闪避逻辑（核心逻辑保留在原函数；无敌帧由无敌 GE 独立管理时长）
    Character->Dodge();

    // 闪避即时完成（无敌帧走 GE 独立计时），立即结束 Ability
    EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
