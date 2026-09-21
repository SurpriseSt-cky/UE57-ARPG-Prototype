// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CharaTeamWidget.generated.h"

class ABattleCharacter;
class UButton;
class UImage;
class UTextBlock;
class UCanvasPanel;
class UUniformGridPanel;
class UOverlay;
class UWrapBox;

/**
 * 动态按钮回调载体（编队 UI 专用）。
 *
 * 为什么需要它：UButton::OnClicked 是无参 DYNAMIC 多播委托，AddDynamic 只能接收
 * 「成员函数名字面量」，无法为运行时动态生成的 N 个按钮（8 个队伍号 + N 个角色条目）
 * 各写一个回调。与背包的 UBagWidgetAction 同一个模式（铁律 6）。
 */
UCLASS()
class WUTHERINGWAVES_API UCharaTeamAction : public UObject
{
    GENERATED_BODY()

public:
    /** 动作类型：0=切换队伍（Arg=队伍下标） 1=点选角色（Arg=拥有列表下标） 2=打开快速编队 3=完成编队 4=关闭整个编队界面 5=关闭快速编队覆盖层 */
    UPROPERTY()
    int32 ActionType = 0;

    UPROPERTY()
    int32 Arg = INDEX_NONE;

    UPROPERTY()
    TWeakObjectPtr<class UCharaTeamWidget> Owner;

    UFUNCTION()
    void OnClicked();
};

/**
 * 角色编队界面（L 键唤起）—— 纯 C++ 构建整棵控件树，无需任何 WBP 资产。
 *
 * 【主视图】（参考《鸣潮》编队页）
 *   · 左上标题「队伍 N」
 *   · 左侧队伍数字列（1~8，点击切换查看；当前队高亮）
 *   · 中间 3 个立绘槽：立绘图 + Lv + 名字（成员从 Data_chara_imfor 同步）
 *   · 右下「快速编队」按钮
 * 【快速编队视图】
 *   · 拥有角色的头像网格（自动从数据表生成）
 *   · 按点击顺序标记 1/2/3（再点一次取消，编号前移）
 *   · 底部「完成」按钮：本次选择【逐位】写回当前队伍 ——
 *     第 i 位选了角色 → 覆盖队伍第 i 位；该位没选 → 队伍第 i 位保持原样（为空就填充）
 *
 * ★ 数据与交互逻辑都在 ABattleCharacter 上（Teams / CurrentTeamIndex / SetTeamMembers），
 *   本类只负责「画出来 + 把点击转译成对 Owner 的调用」。
 */
UCLASS()
class WUTHERINGWAVES_API UCharaTeamWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    // 数据持有者（角色）。创建后立刻设置。
    UPROPERTY()
    TWeakObjectPtr<ABattleCharacter> OwnerCharacter;

    virtual void NativeConstruct() override;

    // ---- 载体回调统一入口 ----
    void HandleAction(int32 ActionType, int32 Arg);

    // 切换查看的队伍（刷新主视图）
    void SelectTeam(int32 TeamIndex);

    // 进入快速编队视图（清空本次选择）
    void EnterQuickEdit();

    // 完成：把本次选择逐位写回当前队伍，回主视图
    void FinishQuickEdit();

    // 关闭整个编队界面（close_line_butt）：回调 Owner 的 CloseCharaTeamUI（移除本控件 + 恢复游戏）
    void CloseEntireUI();

    // 关闭快速编队覆盖层（close_edit_butt）：隐藏覆盖层 + 清空本次选择，回主视图（不关闭整个界面）
    void CloseQuickEditView();

    // 点选 / 取消选择一个角色（OwnedIndex = 拥有列表下标，最多 3 个）
    void TogglePick(int32 OwnedIndex);

    // 刷新全部显示（打开时 / 切队伍后 / 完成后调用）
    void RefreshAll();

    // 刷新快速编队网格（进入视图 / 点选后调用）
    void RefreshQuickEdit();

private:
    // ---- 控件构建（NativeConstruct 一次性建好两套视图）----
    void BuildMainView();
    void BuildQuickEditView();

    // WBP 模式：设计器里已搭好控件树（WidgetTree->RootWidget 非空）时走这里，
    // 按约定名把成员绑到 WBP 控件上；返回 false 表示核心控件没绑全（调用方回退纯 C++ 构建）。
    bool BindDesignedWidgets();

    // 立绘槽：3 个槽的控件句柄（刷新时填内容）
    struct FSlotWidgets
    {
        UImage* PortraitImage = nullptr;
        UTextBlock* LevelText = nullptr;
        UTextBlock* NameText = nullptr;
        UOverlay* Root = nullptr;
    };

    // ---- 主视图控件 ----
    UPROPERTY()
    TObjectPtr<UCanvasPanel> MainRoot;

    UPROPERTY()
    TObjectPtr<UTextBlock> TeamTitleText;

    UPROPERTY()
    TArray<TObjectPtr<UButton>> TeamButtons;   // 左侧队伍号按钮（1~8）

    UPROPERTY()
    TArray<TObjectPtr<UTextBlock>> TeamButtonTexts;

    // 3 个立绘槽（★ 不加 UPROPERTY：FSlotWidgets 是普通嵌套结构，无需反射；
    // 控件的存活由 WidgetTree 保证，这里只是便捷句柄）
    TArray<FSlotWidgets> Slots;

    UPROPERTY()
    TObjectPtr<UButton> QuickEditButton;

    // ---- 快速编队视图控件 ----
    UPROPERTY()
    TObjectPtr<UCanvasPanel> QuickEditRoot;     // 覆盖层（Hidden <-> Visible）

    UPROPERTY()
    TObjectPtr<UUniformGridPanel> PickGrid;

    UPROPERTY()
    TArray<TObjectPtr<UWidget>> PickEntries;    // 生成的角色条目（刷新时先清）

    UPROPERTY()
    TObjectPtr<UButton> FinishButton;

    // ---- 状态 ----
    // 本次快速编队的选择（存「拥有列表」的下标；按点击顺序，最多 3 个）
    UPROPERTY()
    TArray<int32> PendingPickIndexes;

    // 动态按钮回调载体（防 GC）
    UPROPERTY()
    TArray<TObjectPtr<UCharaTeamAction>> Actions;

    // ---- 小工具 ----
    // 运行时构造一个「金色描边」选中框 Image（Border 画法，不需要贴图资产）
    UImage* MakeSelectionFrame(const FLinearColor& Color, float BorderWidth);

    // 构建一个动态按钮并绑载体回调；返回按钮（调用方决定怎么放进布局）
    UButton* MakeActionButton(const FText& Label, int32 ActionType, int32 Arg,
        const FVector2D& Size, const FLinearColor& BgColor);
};
