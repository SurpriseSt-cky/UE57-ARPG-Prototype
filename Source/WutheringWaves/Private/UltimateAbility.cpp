// Copyright WutheringWaves. All Rights Reserved.

#include "UltimateAbility.h"
#include "BattleCharacter.h"
#include "AbilitySystemComponent.h"
#include "TimerManager.h"

UUltimateAbility::UUltimateAbility()
{
    // 实例化策略：每次激活独立实例（默认）
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
}

bool UUltimateAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayTagContainer* SourceTags,
    const FGameplayTagContainer* TargetTags,
    FGameplayTagContainer* OptionalRelevantTags) const
{
    // 无冷却配置 → 只做基础判定
    if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
    {
        return false;
    }

    // 角色侧状态判定（复用现有 CanCastUltimate）
    if (const ABattleCharacter* Character = Cast<ABattleCharacter>(GetAvatarActorFromActorInfo()))
    {
        return Character->CanCastUltimate();
    }
    return false;
}

void UUltimateAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    // Commit 冷却/消耗（P2 外壳阶段暂未配置 CooldownGameplayEffect，此处预留）
    // CommitAbility(Handle, ActorInfo, ActivationInfo);

    ABattleCharacter* Character = Cast<ABattleCharacter>(GetAvatarActorFromActorInfo());
    if (!Character)
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    // 调用现有大招逻辑（核心逻辑保留在原函数，外壳式封装）
    Character->Ultimate();

    // 启动轮询：大招结束后 EndAbility
    if (UWorld* World = Character->GetWorld())
    {
        World->GetTimerManager().SetTimer(PollTimerHandle,
            this, &UUltimateAbility::PollUltimateEnd, PollInterval, true);
    }
}

void UUltimateAbility::PollUltimateEnd()
{
    const ABattleCharacter* Character = Cast<ABattleCharacter>(GetAvatarActorFromActorInfo());
    if (!Character || !Character->IsUltimateCasting())
    {
        // 大招结束 → 停止轮询并结束 Ability
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().ClearTimer(PollTimerHandle);
        }
        EndAbility(CurrentSpecHandle, GetCurrentActorInfo(), GetCurrentActivationInfo(), true, false);
    }
}

void UUltimateAbility::EndAbility(const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    bool bReplicateEndAbility,
    bool bWasCancelled)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(PollTimerHandle);
    }
    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
