// Copyright WutheringWaves. All Rights Reserved.

#include "BattleAttributeSet.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

UBattleAttributeSet::UBattleAttributeSet()
{
    // 默认值与角色现有手写字段对齐（后续由初始化 GameplayEffect 覆盖）
    InitHealth(100.0f);
    InitMaxHealth(100.0f);
    InitShield(0.0f);
    InitStamina(120.0f);
    InitMaxStamina(120.0f);
    InitConcertoEnergy(0.0f);
    InitMaxConcertoEnergy(100.0f);
    InitAttack(100.0f);
    InitDefense(50.0f);
    InitPoise(100.0f);
    InitMaxPoise(100.0f);
    InitCritRate(5.0f);    // 整数百分比：5 = 5% 暴击率
    InitCritDamage(150.0f); // 整数百分比：150 = 150% 暴击伤害（1.5 倍）
}

void UBattleAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
    Super::PreAttributeChange(Attribute, NewValue);
    ClampAttribute(Attribute, NewValue);
}

void UBattleAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
    Super::PostGameplayEffectExecute(Data);

    // 伤害/治疗结算后统一 Clamp（保证 Health 在 [0, MaxHealth]）。
    // 注意：这里直接用 SetCurrentValue（而非 setter），避免在 GE 结算回调里再触发
    // SetNumericAttributeBase 造成递归/时序问题。
    if (Data.EvaluatedData.Attribute == GetHealthAttribute())
    {
        Health.SetCurrentValue(FMath::Clamp(Health.GetCurrentValue(), 0.0f, MaxHealth.GetCurrentValue()));
    }
    else if (Data.EvaluatedData.Attribute == GetShieldAttribute())
    {
        Shield.SetCurrentValue(FMath::Max(0.0f, Shield.GetCurrentValue()));
    }
    else if (Data.EvaluatedData.Attribute == GetStaminaAttribute())
    {
        Stamina.SetCurrentValue(FMath::Clamp(Stamina.GetCurrentValue(), 0.0f, MaxStamina.GetCurrentValue()));
    }
    else if (Data.EvaluatedData.Attribute == GetConcertoEnergyAttribute())
    {
        ConcertoEnergy.SetCurrentValue(FMath::Clamp(ConcertoEnergy.GetCurrentValue(), 0.0f, MaxConcertoEnergy.GetCurrentValue()));
    }
    else if (Data.EvaluatedData.Attribute == GetPoiseAttribute())
    {
        Poise.SetCurrentValue(FMath::Clamp(Poise.GetCurrentValue(), 0.0f, MaxPoise.GetCurrentValue()));
    }
}

void UBattleAttributeSet::ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue)
{
    if (Attribute == GetHealthAttribute())
    {
        NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
    }
    else if (Attribute == GetShieldAttribute())
    {
        NewValue = FMath::Max(0.0f, NewValue);
    }
    else if (Attribute == GetStaminaAttribute())
    {
        NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxStamina());
    }
    else if (Attribute == GetConcertoEnergyAttribute())
    {
        NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxConcertoEnergy());
    }
    else if (Attribute == GetPoiseAttribute())
    {
        NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxPoise());
    }
}

void UBattleAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, Health, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, Shield, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, Stamina, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, MaxStamina, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, ConcertoEnergy, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, MaxConcertoEnergy, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, Attack, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, Defense, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, Poise, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, MaxPoise, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, CritRate, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UBattleAttributeSet, CritDamage, COND_None, REPNOTIFY_Always);
}

// ---- 复制回调（P0 占位，后续接 UI 刷新）----
void UBattleAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, Health, OldValue); }
void UBattleAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, MaxHealth, OldValue); }
void UBattleAttributeSet::OnRep_Shield(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, Shield, OldValue); }
void UBattleAttributeSet::OnRep_Stamina(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, Stamina, OldValue); }
void UBattleAttributeSet::OnRep_MaxStamina(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, MaxStamina, OldValue); }
void UBattleAttributeSet::OnRep_ConcertoEnergy(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, ConcertoEnergy, OldValue); }
void UBattleAttributeSet::OnRep_MaxConcertoEnergy(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, MaxConcertoEnergy, OldValue); }
void UBattleAttributeSet::OnRep_Attack(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, Attack, OldValue); }
void UBattleAttributeSet::OnRep_Defense(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, Defense, OldValue); }
void UBattleAttributeSet::OnRep_Poise(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, Poise, OldValue); }
void UBattleAttributeSet::OnRep_MaxPoise(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, MaxPoise, OldValue); }
void UBattleAttributeSet::OnRep_CritRate(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, CritRate, OldValue); }
void UBattleAttributeSet::OnRep_CritDamage(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UBattleAttributeSet, CritDamage, OldValue); }
