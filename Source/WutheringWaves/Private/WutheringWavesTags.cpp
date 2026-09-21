// Copyright WutheringWaves. All Rights Reserved.

#include "WutheringWavesTags.h"

// native GameplayTag 正式注册：由引擎在模块加载时注册，避免 RequestGameplayTag 触发 ensure。
// 定义必须放在 .cpp（UE_DEFINE_GAMEPLAY_TAG 的 static_assert 强制要求 .cpp 扩展名）。
namespace WutheringWavesTags
{
    UE_DEFINE_GAMEPLAY_TAG(Damage_SetByCaller, "Damage.SetByCaller");
    UE_DEFINE_GAMEPLAY_TAG(State_Invincible, "State.Invincible");
    UE_DEFINE_GAMEPLAY_TAG(State_Invincible_Duration, "State.Invincible.Duration");
    UE_DEFINE_GAMEPLAY_TAG(State_Poisoned, "State.Poisoned");
    UE_DEFINE_GAMEPLAY_TAG(State_Poisoned_Duration, "State.Poisoned.Duration");
    UE_DEFINE_GAMEPLAY_TAG(State_Poisoned_DamagePerSecondRatio, "State.Poisoned.DamagePerSecondRatio");
    UE_DEFINE_GAMEPLAY_TAG(Buff_DamageBoost, "Buff.DamageBoost");
    UE_DEFINE_GAMEPLAY_TAG(Buff_DamageBoost_Multiplier, "Buff.DamageBoost.Multiplier");
    UE_DEFINE_GAMEPLAY_TAG(Buff_DamageBoost_Duration, "Buff.DamageBoost.Duration");
}
