// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BagTypes.h"
#include "ItemPickupTipsWidget.generated.h"

class UImage;
class UTextBlock;
class UVerticalBox;
class UWidgetAnimation;

/**
 * 单条「获得物品」提示（C++ 基类）。
 *
 * ★ 用法：新建 WBP_ItemTip 时，右上角 Class Settings → Parent Class 选 ItemTipWidget（本类）。
 *   控件名必须与下面 BindWidget 的属性【一字不差】，否则蓝图编译会直接报错并写出期望的名字：
 *     Img_ItemIcon  —— Image，物品图标（BindWidgetOptional：没有图标时自动隐藏整块）
 *     Txt_ItemName  —— TextBlock，物品名
 *     Txt_Count     —— TextBlock，数量（默认显示成「×3」）
 *
 * 为什么做成 C++ 基类：提示条数是运行时才知道的（一次掉几种就有几条），
 * 若全在蓝图里连，需要手工 Create Widget / Add Child / 转类型 / Set Text 一长串节点，
 * 任何一环写错都不报错、只是不显示。现在创建、填数据、计时移除统统由 C++ 做，
 * 蓝图只负责外观（排版、字体、动画）。
 */
UCLASS(Abstract)
class WUTHERINGWAVES_API UItemTipWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    /** 填入一条提示的内容（由 UItemPickupTipsWidget::AddTip 调用，也可在蓝图里手动调用） */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    virtual void SetTipData(const FBagItemEntry& Item, int32 ObtainedCount);

    /** 播放入场动画（动画名见 InAnimName；找不到同名动画时静默跳过，不影响提示显示） */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    void PlayTipIn();

    /** 播放淡出动画（动画名见 OutAnimName；找不到同名动画时静默跳过） */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    void PlayTipOut();

protected:
    /**
     * 控件构建完成时先把自己收起来 —— 只有 SetTipData() 真的填过内容才显示。
     *
     * 为什么必须这么做：本控件的根通常是一张【不带尺寸约束】的画布，
     * 一旦被全屏画布渲染（或被人手摆进别的界面里预览），它内部用 Fill 锚点
     * 的背景图就会被拉伸到整个屏幕 —— 现象就是「一运行就出现一个大框占满屏幕」。
     * 收起来之后，没有数据的一条提示永远不会占屏幕。
     */
    virtual void NativeConstruct() override;

    /** 是否已经填过内容。NativeConstruct 与 SetTipData 的调用先后顺序不固定，用这个标志兜住两种情况 */
    bool bHasData = false;

    /** 物品图标。没有配图标的掉落物会自动隐藏，不会留一块空白 */
    UPROPERTY(meta = (BindWidgetOptional), BlueprintReadOnly, Category = "Item|Tip")
    TObjectPtr<UImage> Img_ItemIcon;

    /** 物品名 */
    UPROPERTY(meta = (BindWidget), BlueprintReadOnly, Category = "Item|Tip")
    TObjectPtr<UTextBlock> Txt_ItemName;

    /** 数量文本（默认显示成「×3」） */
    UPROPERTY(meta = (BindWidget), BlueprintReadOnly, Category = "Item|Tip")
    TObjectPtr<UTextBlock> Txt_Count;

    /** 数量前缀：最终文本 = 前缀 + 数字（默认「×3」）。
     *  想只显示纯数字就把这里清空；想显示「+3」就填「+」 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip")
    FString CountPrefix = TEXT("×");

    /** 入场动画名（要在 WBP_ItemTip 的 Animations 里有一个同名动画；没有就填 None） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip")
    FName InAnimName = TEXT("anim_in");

    /** 淡出动画名（同上；没有就填 None） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip")
    FName OutAnimName = TEXT("anim_out");

    /** 找不到绑定控件/动画只提示一次，避免连打时刷屏 */
    bool bBindWarningLogged = false;
    bool bAnimWarningLogged = false;
};

/**
 * 「获得物品」提示列表容器（C++ 基类）。
 *
 * ★ 用法：新建 WBP_ItemPickupTips 时，Parent Class 选 ItemPickupTipsWidget（本类）。
 *   里面放一个 VerticalBox，名字必须是 VBox_ItemTips（定位到屏幕左侧、垂直中偏下）。
 *
 * 整条链路全部由 C++ 驱动，蓝图侧【不需要加接口、不需要连任何事件】：
 *     怪物死亡 → GrantDropsToPlayer() → AddItemByRow() 入包
 *              → ABattleCharacter::NotifyItemObtained()
 *              → 本类 AddTip()  ← 到这里
 *              → CreateWidget(WBP_ItemTip) → 填数据 → AddChildToVerticalBox
 *              → 到时间自动播淡出 → 从列表移除
 *
 * 为什么不用「蓝图实现接口事件」：那种写法一改就要回蓝图里连五六个节点，
 * 且转类型、绑定、时序任一处写错都是静默失效。放进 C++ 后，
 * 掉几种物品就自动生成几条提示，蓝图只决定长什么样。
 */
UCLASS(Abstract)
class WUTHERINGWAVES_API UItemPickupTipsWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    /**
     * 追加一条提示（C++ 在物品入包后自动调用）。
     *
     * @param Item           物品条目（ToolName / ToolImage 等直接可用）
     * @param ObtainedCount  本次获得的数量 —— 提示上「×N」的 N
     */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    virtual void AddTip(const FBagItemEntry& Item, int32 ObtainedCount);

    /** 立刻清空所有提示（例如切场景/死亡时调用） */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    void ClearAllTips();

    /**
     * 当前列表里正在显示的提示条数。
     *
     * ★ 为什么要有这个访问器：体检日志必须在【列表非空】时也打一次 ——
     *   「提示位置错位 / 显示不全」只在有条目时才看得出来。调用方（BattleCharacter）
     *   靠它判断「刚刚多了一条」，从而把体检时机从「仅首帧」扩到「首帧 + 每次出内容」。
     */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    int32 GetTipCount() const;

    /**
     * 把布局事实摊开打进日志：控件树、槽位类型、锚点 / 偏移 / 对齐、期望尺寸、实测尺寸。
     *
     * 为什么需要它：提示「跑出屏幕 / 显示不全」时，只有两层的数字都看得见，
     * 才能分清是**外层那块全屏画布**偏了、还是**VBox 自己的锚点**设错了 ——
     * 光看画面是分不出来的。
     *
     * 注意：必须在控件**已经被绘制过**之后调用（未绘制时 GetCachedGeometry 恒为 0×0，
     * 那不能当成「尺寸坏了」）。
     * C++ 只报事实，不改布局 —— UMG 的排版归设计器管。
     */
    UFUNCTION(BlueprintCallable, Category = "Item|Tip")
    void LogLayoutDiagnostics(const FString& Reason);

    /** 单条提示的控件类 —— 必须指定为 WBP_ItemTip（父类是本文件里的 ItemTipWidget） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip")
    TSubclassOf<UItemTipWidget> TipWidgetClass;

    /** 最多同时显示几条（超出的从最旧一条开始移除） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip", meta = (ClampMin = "1"))
    int32 MaxVisibleTips = 6;

    /** 每条提示停留多久后开始淡出（秒） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip", meta = (ClampMin = "0.1"))
    float TipLifetime = 2.5f;

    /** 淡出动画播放多久后真正把条目从列表里摘掉（秒） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip", meta = (ClampMin = "0.0"))
    float FadeOutTail = 0.3f;

    /** 每条提示的固定宽度（像素）。C++ 会自动给每条提示套一层这个尺寸的 SizeBox，
     *  这样「一条提示多大」不取决于 WBP_ItemTip 内部的锚点是怎么摆的。
     *  设成 0 = 该方向不干预，用 WBP 自己的期望尺寸。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip", meta = (ClampMin = "0.0"))
    float TipWidth = 240.0f;

    /** 每条提示的固定高度（像素）。0 = 不干预。默认 36 够放一行 16 号字 + 图标 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip", meta = (ClampMin = "0.0"))
    float TipHeight = 36.0f;

    // ==================== 槽位基准（设计意图） ====================
    // ★★ 为什么必须把「设计意图」变成一组可比对的常量：
    //    体检光把【实测槽位】和【手册基准】两串数字分别打出来是【没有用的】——
    //    2026-09-14 那次实测：Anchors.Y / Alignment.Y / 尺寸策略 三项全偏，两串数字
    //    在日志里隔了 20 行，没有任何一处把它们对上，于是「提示跑到屏幕外」白查一轮。
    //    现在体检直接拿这几个值算偏差、算容量、给结论。
    //
    // ★ 默认值 = 手册 §3.2.2 的基准。想让提示停在别处（例如就想要左下角），
    //   改这四个值即可 —— 体检会以【你声明的】为基准，C++ 不替你觉得「应该在哪」。
    // ★ 改了这几个值要重启编辑器（新增 UPROPERTY 不支持 Live Coding 热补丁）。

    /** 期望的锚点（Min 与 Max 相同：把 VBox 钉在这个比例位置）。默认 (0, 0.34) = 左边缘、自上而下 34% 高度 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip|Baseline")
    FVector2D TipSlotBaselineAnchors = FVector2D(0.0, 0.34);

    /** 期望的对齐（(0,0) = 让 VBox 的左上角对齐锚点，而不是中心/左下角） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip|Baseline")
    FVector2D TipSlotBaselineAlignment = FVector2D(0.0, 0.0);

    /** 期望的 Position（锚点到控件左上角的像素偏移）。默认 (60, 0) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip|Baseline")
    FVector2D TipSlotBaselinePosition = FVector2D(60.0, 0.0);

    /** 期望勾上「Size To Content」。
     *  ★ 漏了它是最常见的「提示显示不全」：VBox 高度被固定成槽位高度，
     *    条目比它多就被裁；而这个失败【不会】表现成 Size=(0,0)，所以
     *    「只在 Size 为 0 时才报警」的判据是漏的（2026-09-14 实测就是这一种）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item|Tip|Baseline")
    bool bTipSlotBaselineSizeToContent = true;

protected:
    /** 提示列表容器（名字必须叫 VBox_ItemTips） */
    UPROPERTY(meta = (BindWidget), BlueprintReadOnly, Category = "Item|Tip")
    TObjectPtr<UVerticalBox> VBox_ItemTips;

    /** 时间到 → 播淡出 */
    void BeginTipFade(UItemTipWidget* Tip);

    /** 淡出播完 → 真正移除 */
    void RemoveTip(UItemTipWidget* Tip);

    /** 超出 MaxVisibleTips 时从最旧一条开始摘 */
    void TrimExcessTips();

    /** 正在显示的提示 → 它的停留计时器 */
    TMap<TWeakObjectPtr<UItemTipWidget>, FTimerHandle> ActiveTips;

    bool bSetupWarningLogged = false;
};
