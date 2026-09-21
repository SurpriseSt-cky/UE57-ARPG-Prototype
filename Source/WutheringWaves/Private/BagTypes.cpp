// Copyright Epic Games, Inc. All Rights Reserved.

#include "BagTypes.h"
#include "BattleCharacter.h"

void UBagWidgetAction::OnClicked()
{
    // Owner 用 TWeakObjectPtr：背包关闭/角色销毁后按钮即使残留也不会访问野指针
    if (ABattleCharacter* Character = Owner.Get())
    {
        Character->HandleBagAction(ActionType, Index);
    }
}

// 注意：UINTERFACE 里的 BlueprintNativeEvent【不要】自己写 _Implementation 默认实现。
// UHT 会在生成代码里自动生成一个（定义就在 GENERATED_BODY() 展开处），
// 自己再定义一个会直接撞 error C2084「函数已有主体」。
//
// 由此带出一个必须防的坑：某个控件如果**只加了接口、却没在事件图表里实现对应事件**，
// Execute_ 就会落到 UHT 生成的那个默认实现上。所以 ABattleCharacter::NotifyItemObtained
// 在调用前会先用 FindFunction 确认该控件真的实现了事件函数（见那里的注释）。
