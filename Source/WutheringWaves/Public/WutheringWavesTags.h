// Copyright WutheringWaves. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"

/**
 * GAS 相关 GameplayTag 统一定义（native tag 注册）。
 *
 * 用 UE_DECLARE_GAMEPLAY_TAG_EXTERN 在头文件声明、UE_DEFINE_GAMEPLAY_TAG 在
 * WutheringWavesTags.cpp 定义，由引擎在模块加载时正式注册，避免
 * FGameplayTag::RequestGameplayTag 对未注册 tag 触发 ensure（导致编辑器启动卡死）。
 *
 * 命名空间内提供带 _Tag 后缀的函数式接口（返回 FGameplayTag 值），供多个 cpp 复用。
 * 函数名与 native tag 变量名区分（变量名不带后缀，函数名带 _Tag 后缀），避免重定义。
 *
 * 注意：返回值为 FGameplayTag（值），因为 FNativeGameplayTag::operator FGameplayTag() 返回临时值，
 * 若返回引用会悬垂。
 */
namespace WutheringWavesTags
{
    // ---- native tag 变量声明（定义见 WutheringWavesTags.cpp）----
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_SetByCaller);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Invincible);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Invincible_Duration);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Poisoned);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Poisoned_Duration);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Poisoned_DamagePerSecondRatio);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Buff_DamageBoost);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Buff_DamageBoost_Multiplier);
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Buff_DamageBoost_Duration);

    // ---- 函数式接口（返回 FGameplayTag 值，函数名带 _Tag 后缀）----
    // 伤害值 SetByCaller 标签（伤害 GE 用）
    inline FGameplayTag Damage_SetByCaller_Tag()
    {
        return Damage_SetByCaller;
    }

    // 无敌状态 Tag（无敌 GE Grant，TakeDamage 据此免疫）
    inline FGameplayTag State_Invincible_Tag()
    {
        return State_Invincible;
    }

    // 无敌时长 SetByCaller 标签（无敌 GE 用）
    inline FGameplayTag State_Invincible_Duration_Tag()
    {
        return State_Invincible_Duration;
    }

    // 中毒状态 Tag（中毒 GE Grant，IsPoisoned/RefreshStatusIcons 据此判定）
    inline FGameplayTag State_Poisoned_Tag()
    {
        return State_Poisoned;
    }

    // 中毒时长 SetByCaller 标签（中毒 GE 用）
    inline FGameplayTag State_Poisoned_Duration_Tag()
    {
        return State_Poisoned_Duration;
    }

    // 中毒每秒伤害比例 SetByCaller 标签（中毒 GE 用）
    inline FGameplayTag State_Poisoned_DamagePerSecondRatio_Tag()
    {
        return State_Poisoned_DamagePerSecondRatio;
    }

    // 增伤状态 Tag（增伤 GE Grant，GetOutgoingDamageMultiplier 据此判定）
    inline FGameplayTag Buff_DamageBoost_Tag()
    {
        return Buff_DamageBoost;
    }

    // 增伤幅度 SetByCaller 标签（增伤 GE 用，系数如 0.1 = +10%）
    inline FGameplayTag Buff_DamageBoost_Multiplier_Tag()
    {
        return Buff_DamageBoost_Multiplier;
    }

    // 增伤时长 SetByCaller 标签（增伤 GE 用）
    inline FGameplayTag Buff_DamageBoost_Duration_Tag()
    {
        return Buff_DamageBoost_Duration;
    }
}
