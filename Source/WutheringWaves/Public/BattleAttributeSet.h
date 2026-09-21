// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "BattleAttributeSet.generated.h"

// GAS 属性访问宏（生成 Get/Set/Init 便捷方法）
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * 战斗角色的 GAS 属性集（P0 地基）。
 *
 * 将角色现有手写 float 字段（生命/耐力/协奏能量/攻击/防御/Poise）迁移为
 * UAttributeSet 的 FGameplayAttributeData，供 GameplayEffect / GameplayAbility
 * 通过属性系统统一读写。当前 P0 仅搭建属性集与复制、Clamp 逻辑；
 * 尚未切断与旧手写字段（MaxHealth/CurrentHealth 等）的桥接，逐阶段替换。
 */
UCLASS()
class WUTHERINGWAVES_API UBattleAttributeSet : public UAttributeSet
{
    GENERATED_BODY()

public:
    UBattleAttributeSet();

    // 属性被修改后统一做 Clamp（保证不越界），并复制到网络
    virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
    virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // ---- 生命 ----
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Health", ReplicatedUsing = OnRep_Health)
    FGameplayAttributeData Health;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, Health)

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Health", ReplicatedUsing = OnRep_MaxHealth)
    FGameplayAttributeData MaxHealth;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, MaxHealth)

    // ---- 护盾 ----
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Health", ReplicatedUsing = OnRep_Shield)
    FGameplayAttributeData Shield;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, Shield)

    // ---- 耐力 ----
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Stamina", ReplicatedUsing = OnRep_Stamina)
    FGameplayAttributeData Stamina;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, Stamina)

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Stamina", ReplicatedUsing = OnRep_MaxStamina)
    FGameplayAttributeData MaxStamina;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, MaxStamina)

    // ---- 协奏能量 ----
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Concerto", ReplicatedUsing = OnRep_ConcertoEnergy)
    FGameplayAttributeData ConcertoEnergy;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, ConcertoEnergy)

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Concerto", ReplicatedUsing = OnRep_MaxConcertoEnergy)
    FGameplayAttributeData MaxConcertoEnergy;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, MaxConcertoEnergy)

    // ---- 攻击 / 防御 ----
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Combat", ReplicatedUsing = OnRep_Attack)
    FGameplayAttributeData Attack;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, Attack)

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Combat", ReplicatedUsing = OnRep_Defense)
    FGameplayAttributeData Defense;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, Defense)

    // ---- Poise（弹刀/韧性）----
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Combat", ReplicatedUsing = OnRep_Poise)
    FGameplayAttributeData Poise;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, Poise)

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Combat", ReplicatedUsing = OnRep_MaxPoise)
    FGameplayAttributeData MaxPoise;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, MaxPoise)

    // ---- 暴击（整数百分比语义）----
    // CritRate：暴击触发概率（整数百分比，5 = 5% 概率触发暴击；不设上限）。
    // CritDamage：暴击伤害倍率（整数百分比，150 = 150% = 1.5 倍伤害）。
    // 两者均以整数百分比存储，伤害结算时 ÷100 换算为小数参与计算。
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Combat", ReplicatedUsing = OnRep_CritRate)
    FGameplayAttributeData CritRate;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, CritRate)

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Combat", ReplicatedUsing = OnRep_CritDamage)
    FGameplayAttributeData CritDamage;
    ATTRIBUTE_ACCESSORS(UBattleAttributeSet, CritDamage)

protected:
    // 复制回调（客户端收到后刷新 UI 等，P0 先留空占位，后续接 UI）
    UFUNCTION()
    void OnRep_Health(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_Shield(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_Stamina(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_MaxStamina(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_ConcertoEnergy(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_MaxConcertoEnergy(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_Attack(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_Defense(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_Poise(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_MaxPoise(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_CritRate(const FGameplayAttributeData& OldValue);
    UFUNCTION()
    void OnRep_CritDamage(const FGameplayAttributeData& OldValue);

    // 临时 Clamp 辅助
    void ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue);
};
