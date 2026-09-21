// Copyright Epic Games, Inc. All Rights Reserved.

#include "ItemPickupTipsWidget.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "TimerManager.h"

namespace
{
    // 按名字找 WBP 里的动画。
    //
    // 注意 UE 5.7 的 UUserWidget 上没有 GetAnimationByName —— 动画实际挂在
    // UWidgetBlueprintGeneratedClass::Animations 上，要走这里查。
    // （工程里招架提示 UMonsterBase::FindParryPromptAnimation 用的是同一套写法。）
    UWidgetAnimation* FindWidgetAnimation(const UUserWidget* Widget, FName AnimationName)
    {
        if (!Widget || AnimationName.IsNone())
        {
            return nullptr;
        }

        const UWidgetBlueprintGeneratedClass* BGClass =
            Cast<UWidgetBlueprintGeneratedClass>(Widget->GetClass());
        if (!BGClass)
        {
            return nullptr;
        }

        for (const TObjectPtr<UWidgetAnimation>& Anim : BGClass->Animations)
        {
            if (Anim && Anim->GetFName() == AnimationName)
            {
                return Anim.Get();
            }
        }
        return nullptr;
    }

    // ---- 布局体检用的两句话 ----
    //
    // 一个控件的「位置不对」永远只有两种可能：外层容器给它的空间不对，
    // 或者它自己在容器里的槽位（锚点/偏移/对齐）不对。所以这两件事要一起打出来，
    // 光看画面是分不清的。

    FString DescribeWidgetFacts(const UWidget* Widget)
    {
        if (!Widget)
        {
            return TEXT("<不存在>");
        }

        const FVector2D Desired = Widget->GetDesiredSize();
        const FVector2D Actual = Widget->GetCachedGeometry().GetLocalSize();

        return FString::Printf(
            TEXT("%s  类=%s  可见性=%d  期望=(%.1f, %.1f)  实测=(%.1f, %.1f)"),
            *Widget->GetName(), *Widget->GetClass()->GetName(),
            static_cast<int32>(Widget->GetVisibility()),
            Desired.X, Desired.Y, Actual.X, Actual.Y);
    }

    FString DescribeSlotFacts(const UWidget* Widget)
    {
        if (!Widget)
        {
            return TEXT("<不存在>");
        }

        if (!Widget->Slot)
        {
            return TEXT("<根控件：没有槽位，尺寸完全由外层容器决定>");
        }

        FString Text = Widget->Slot->GetClass()->GetName();

        if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
        {
            const FAnchors Anchors = CanvasSlot->GetAnchors();
            const FMargin Offsets = CanvasSlot->GetOffsets();
            const FVector2D Alignment = CanvasSlot->GetAlignment();
            const FVector2D Position = CanvasSlot->GetPosition();
            const FVector2D Size = CanvasSlot->GetSize();

            Text += FString::Printf(
                TEXT("  Anchors=Min(%.3f, %.3f) Max(%.3f, %.3f)  Alignment=(%.2f, %.2f)\n"
                     "          Offsets=(L %.1f, T %.1f, R %.1f, B %.1f)  Position=(%.1f, %.1f)  Size=(%.1f, %.1f)"),
                Anchors.Minimum.X, Anchors.Minimum.Y, Anchors.Maximum.X, Anchors.Maximum.Y,
                Alignment.X, Alignment.Y,
                Offsets.Left, Offsets.Top, Offsets.Right, Offsets.Bottom,
                Position.X, Position.Y, Size.X, Size.Y);
        }
        else
        {
            Text += TEXT("（非画布槽位：位置由父容器的排列规则决定）");
        }

        return Text;
    }
}

// ======================================================================
// UItemTipWidget —— 单条提示
// ======================================================================

void UItemTipWidget::NativeConstruct()
{
    Super::NativeConstruct();

    // 没填过内容就收起来。
    // ★ 为什么放在这里而不是「靠 CreateWidget 之后才显示」：本控件也可能被人手摆到
    //   别的控件树里（设计器预览、误当成列表容器用……），那样永远没人给它 SetTipData，
    //   它就会拿自己那套「铺满父级」的锚点一直显示着 —— 也就是「一运行就占满屏幕」。
    if (!bHasData)
    {
        SetVisibility(ESlateVisibility::Collapsed);
    }
}

void UItemTipWidget::SetTipData(const FBagItemEntry& Item, int32 ObtainedCount)
{
    // 物品名兜底：数据表里 tool_name 没填时，退回显示行名 ——
    // 提示上出现一行字总比出现一块空白好排查
    const FString DisplayName = Item.ToolName.IsEmpty()
        ? Item.RowName.ToString()
        : Item.ToolName;

    if (!Txt_ItemName || !Txt_Count)
    {
        // 正常情况下走不到这里：BindWidget 会让「控件名对不上」在蓝图编译期就报错。
        // 留着是为了万一有人把属性改成 BindWidgetOptional 后好定位。
        if (!bBindWarningLogged)
        {
            bBindWarningLogged = true;
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] %s 缺少绑定控件：需要名为 Txt_ItemName 和 Txt_Count 的 TextBlock。"
                     "修法：检查 WBP_ItemTip 里这两个控件的名字是否一字不差。"),
                *GetClass()->GetName());
        }
        return;
    }

    Txt_ItemName->SetText(FText::FromString(DisplayName));

    // 数量文本：默认「×3」。CountPrefix 在 WBP_ItemTip 的 Class Defaults → Item|Tip 里可改
    Txt_Count->SetText(FText::FromString(CountPrefix + FString::FromInt(ObtainedCount)));

    if (Img_ItemIcon)
    {
        if (Item.ToolImage)
        {
            Img_ItemIcon->SetBrushFromTexture(Item.ToolImage, true);
            Img_ItemIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            // 掉落物没配图标：收起整块，避免左侧留一块空白把文字挤走
            Img_ItemIcon->SetVisibility(ESlateVisibility::Collapsed);
        }
    }

    // 内容填进来了 —— 从这一刻起这条提示才算有效，可以显示（与 NativeConstruct 的收起配对）
    bHasData = true;
    SetVisibility(ESlateVisibility::SelfHitTestInvisible);

    PlayTipIn();

    UE_LOG(LogTemp, Log, TEXT("[Drop] 提示条目已生成：%s | 本次 ×%d"),
        *DisplayName, ObtainedCount);
}

void UItemTipWidget::PlayTipIn()
{
    if (InAnimName.IsNone())
    {
        return;
    }

    if (UWidgetAnimation* Anim = FindWidgetAnimation(this, InAnimName))
    {
        PlayAnimation(Anim);
        return;
    }

    // 没做动画不算错误 —— 提示本身已经显示出来了，只是没有入场过渡
    if (!bAnimWarningLogged)
    {
        bAnimWarningLogged = true;
        UE_LOG(LogTemp, Log,
            TEXT("[Drop] %s 里没有名为 '%s' 的动画，跳过入场动画（提示仍会正常显示）。"
                 "想要淡入效果就在 Animations 面板新建一个同名动画。"),
            *GetClass()->GetName(), *InAnimName.ToString());
    }
}

void UItemTipWidget::PlayTipOut()
{
    if (OutAnimName.IsNone())
    {
        return;
    }

    if (UWidgetAnimation* Anim = FindWidgetAnimation(this, OutAnimName))
    {
        PlayAnimation(Anim);
        return;
    }

    if (!bAnimWarningLogged)
    {
        bAnimWarningLogged = true;
        UE_LOG(LogTemp, Log,
            TEXT("[Drop] %s 里没有名为 '%s' 的动画，跳过淡出动画。"),
            *GetClass()->GetName(), *OutAnimName.ToString());
    }
}

// ======================================================================
// UItemPickupTipsWidget —— 提示列表容器
// ======================================================================

void UItemPickupTipsWidget::AddTip(const FBagItemEntry& Item, int32 ObtainedCount)
{
    // ---- 前置检查：配置缺失时给出「具体怎么修」，只提示一次避免刷屏 ----
    if (!VBox_ItemTips)
    {
        if (!bSetupWarningLogged)
        {
            bSetupWarningLogged = true;
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] %s 里找不到名为 VBox_ItemTips 的 VerticalBox，"
                     "提示无处安放（物品已正常入包）。\n"
                     "      修法：在 WBP_ItemPickupTips 里放一个 VerticalBox，"
                     "把它的名字改成 VBox_ItemTips，锚点设到屏幕左侧。"),
                *GetClass()->GetName());
        }
        return;
    }

    if (!TipWidgetClass)
    {
        if (!bSetupWarningLogged)
        {
            bSetupWarningLogged = true;
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] %s 的 Tip Widget Class 还没指定，无法生成提示条"
                     "（物品已正常入包）。\n"
                     "      修法：打开 WBP_ItemPickupTips → 左侧选中它自己（或右上角 Class Defaults）"
                     "→ Details → Item|Tip → Tip Widget Class 选 WBP_ItemTip。"),
                *GetClass()->GetName());
        }
        return;
    }

    // ---- 创建一条提示 ----
    UItemTipWidget* Tip = CreateWidget<UItemTipWidget>(this, TipWidgetClass);
    if (!Tip)
    {
        if (!bSetupWarningLogged)
        {
            bSetupWarningLogged = true;
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] %s 无法创建提示条目（物品已正常入包）。\n"
                     "      最常见原因：WBP_ItemTip 的 Parent Class 不是 ItemTipWidget。\n"
                     "      修法：打开 WBP_ItemTip → 右上角 Class Settings → Parent Class "
                     "→ 选 ItemTipWidget → Compile。"),
                *GetClass()->GetName());
        }
        return;
    }

    // ---- 先套一层固定尺寸的 SizeBox，再挂进列表 ----
    // 为什么非套不可：WBP_ItemTip 的根往往是不带尺寸约束的画布（CanvasPanel），
    // 它单独放进 VerticalBox 时拿不到确定的宽高 —— 内部控件若用 Fill 锚点，
    // 就会被撑到父级给多大就多大（在 1920x1080 的全屏画布上 = 占满屏幕）。
    // 套上 SizeBox 之后，「一条提示多大」由本类的 TipWidth / TipHeight 决定，
    // 与 WBP 内部的锚点怎么摆无关。想用 WBP 自己的尺寸就把两者设成 0。
    UWidget* Row = Tip;
    if (TipWidth > 0.0f || TipHeight > 0.0f)
    {
        USizeBox* RowBox = NewObject<USizeBox>(this);

        // ★ 注意：SetWidthOverride 传 0 也会把 bOverride_WidthOverride 置 true（覆盖成 0 宽），
        //   而不是「取消覆盖」，所以必须先判值再设
        if (TipWidth > 0.0f)
        {
            RowBox->SetWidthOverride(TipWidth);
        }
        if (TipHeight > 0.0f)
        {
            RowBox->SetHeightOverride(TipHeight);
        }

        RowBox->AddChild(Tip);
        Row = RowBox;
    }

    VBox_ItemTips->AddChildToVerticalBox(Row);

    // 先 Add 再填数据：控件已在树里，SetText/SetBrush 当帧就能生效，
    // 顺序反过来（先填后 Add）也能用，但某些情况下首帧会闪一下空文本。
    // 顺带一提，SetTipData 会把提示从 Collapsed 打开（见那里的注释）
    Tip->SetTipData(Item, ObtainedCount);

    // ---- 停留计时：到点 → 播淡出 → 再延时摘除 ----
    if (UWorld* World = GetWorld())
    {
        FTimerHandle Handle;
        FTimerDelegate Delegate = FTimerDelegate::CreateUObject(this, &UItemPickupTipsWidget::BeginTipFade, Tip);
        World->GetTimerManager().SetTimer(Handle, Delegate, FMath::Max(0.1f, TipLifetime), false);
        ActiveTips.Add(Tip, Handle);
    }

    TrimExcessTips();
}

void UItemPickupTipsWidget::BeginTipFade(UItemTipWidget* Tip)
{
    if (!Tip)
    {
        return;
    }

    Tip->PlayTipOut();

    UWorld* World = GetWorld();
    if (!World || FadeOutTail <= 0.0f)
    {
        RemoveTip(Tip);
        return;
    }

    FTimerHandle Handle;
    FTimerDelegate Delegate = FTimerDelegate::CreateUObject(this, &UItemPickupTipsWidget::RemoveTip, Tip);
    World->GetTimerManager().SetTimer(Handle, Delegate, FadeOutTail, false);
}

void UItemPickupTipsWidget::RemoveTip(UItemTipWidget* Tip)
{
    if (!Tip)
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        if (FTimerHandle* Handle = ActiveTips.Find(Tip))
        {
            World->GetTimerManager().ClearTimer(*Handle);
        }
    }
    ActiveTips.Remove(Tip);

    if (!VBox_ItemTips)
    {
        return;
    }

    // 摘「整行」。条目可能被固定尺寸的 SizeBox 包着（见 AddTip），这时
    // 对 VBox 直接调 RemoveChild(Tip) 是摘不掉的（Tip 不是它的直接孩子），
    // 只摘里层还会剩一个空包装占着高度 —— 所以按行找到它再整行删掉。
    for (int32 Index = 0; Index < VBox_ItemTips->GetChildrenCount(); ++Index)
    {
        UWidget* Child = VBox_ItemTips->GetChildAt(Index);
        if (!Child)
        {
            continue;
        }

        const bool bIsThisRow = (Child == Tip)
            || (Cast<UContentWidget>(Child) && Cast<UContentWidget>(Child)->GetContent() == Tip);

        if (bIsThisRow)
        {
            VBox_ItemTips->RemoveChildAt(Index);
            return;
        }
    }
}

void UItemPickupTipsWidget::TrimExcessTips()
{
    if (!VBox_ItemTips)
    {
        return;
    }

    // 连打多只怪时可能一瞬间涌进很多条：从最旧一条开始摘，保证列表不会溢出屏幕
    while (VBox_ItemTips->GetChildrenCount() > FMath::Max(1, MaxVisibleTips))
    {
        UWidget* Oldest = VBox_ItemTips->GetChildAt(0);
        if (!Oldest)
        {
            break;
        }

        // 条目外面可能还包着一层固定尺寸的 SizeBox（见 AddTip），所以要往里再找一层
        UItemTipWidget* OldestTip = Cast<UItemTipWidget>(Oldest);
        if (!OldestTip)
        {
            if (const UContentWidget* RowWrapper = Cast<UContentWidget>(Oldest))
            {
                OldestTip = Cast<UItemTipWidget>(RowWrapper->GetContent());
            }
        }

        if (OldestTip)
        {
            RemoveTip(OldestTip);   // 它会连外面的包装一起整行摘掉
        }
        else
        {
            // 列表里混进了非提示控件（比如占位用的 Spacer）：直接摘掉，别让它卡住循环
            VBox_ItemTips->RemoveChildAt(0);
        }
    }
}

void UItemPickupTipsWidget::ClearAllTips()
{
    if (UWorld* World = GetWorld())
    {
        // 这里必须用非 const 迭代：FTimerManager::ClearTimer 的参数是 FTimerHandle&（要写回）
        for (TPair<TWeakObjectPtr<UItemTipWidget>, FTimerHandle>& Pair : ActiveTips)
        {
            World->GetTimerManager().ClearTimer(Pair.Value);
        }
    }
    ActiveTips.Reset();

    if (VBox_ItemTips)
    {
        VBox_ItemTips->ClearChildren();
    }
}

int32 UItemPickupTipsWidget::GetTipCount() const
{
    // 以「列表里实际挂着几个孩子」为准，而不是 ActiveTips.Num()：
    // 后者在淡出期间还留着条目（视觉上仍占高度），而体检要看的是【看得见的高度占了多少】。
    return VBox_ItemTips ? VBox_ItemTips->GetChildrenCount() : 0;
}

void UItemPickupTipsWidget::LogLayoutDiagnostics(const FString& Reason)
{
    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] ============ 提示列表布局体检（%s）============"), *Reason);
    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] 控件类 = %s"), *GetClass()->GetName());

    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] [根] %s"), *DescribeWidgetFacts(this));
    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout]      %s"), *DescribeSlotFacts(this));

    if (!VBox_ItemTips)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop][Layout] VBox_ItemTips 不存在 —— 这个控件不是提示列表容器。\n"
                 "      修法：在 WBP_ItemPickupTips 里放一个 VerticalBox，名字改成 VBox_ItemTips。"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] [VBox_ItemTips] %s"), *DescribeWidgetFacts(VBox_ItemTips));
    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout]      %s"), *DescribeSlotFacts(VBox_ItemTips));

    // 父链：看清 VBox 到底被谁撑着（顺序是 VBox → 父 → … → 根）
    FString Chain;
    for (const UWidget* Parent = VBox_ItemTips->GetParent(); Parent; Parent = Parent->GetParent())
    {
        Chain += FString::Printf(TEXT("%s(%s) → "), *Parent->GetName(), *Parent->GetClass()->GetName());
    }
    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout]      父链 = %s(根)"), Chain.IsEmpty() ? TEXT("<空>") : *Chain);

    // 逐条实测：判断「是不是宽度/高度塌成 0」
    const int32 ChildCount = VBox_ItemTips->GetChildrenCount();
    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] 当前 %d 条提示，逐条实测（C++ 包装尺寸 = %.0f × %.0f）："),
        ChildCount, TipWidth, TipHeight);

    for (int32 Index = 0; Index < ChildCount; ++Index)
    {
        const UWidget* Child = VBox_ItemTips->GetChildAt(Index);
        UE_LOG(LogTemp, Log, TEXT("[Drop][Layout]   [%d] %s"), Index, *DescribeWidgetFacts(Child));

        if (const UPanelWidget* RowPanel = Cast<UPanelWidget>(Child))
        {
            if (RowPanel->GetChildrenCount() > 0)
            {
                UE_LOG(LogTemp, Log, TEXT("[Drop][Layout]       内容 = %s"),
                    *DescribeWidgetFacts(RowPanel->GetChildAt(0)));
            }
        }
    }

    // ==================== 槽位判据：把「实测」和「设计意图」对上 ====================
    // ★★ 这一段才是体检真正的产出。上面那几行只是原材料 —— 2026-09-14 那次就是只有
    //    原材料：三项偏差（Anchors.Y / Alignment.Y / 尺寸策略）全在日志里，基准也在
    //    日志里，但隔了 20 行、没有任何一处把它们对上，于是「提示跑到屏幕外」白查一轮。
    //    教训：报事实 ≠ 给结论。体检必须自己算出「偏了多少、会导致什么」。
    const FVector2D CanvasSize = GetCachedGeometry().GetLocalSize();
    const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(VBox_ItemTips->Slot);

    if (!CanvasSlot || CanvasSize.X <= 1.0f || CanvasSize.Y <= 1.0f)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Drop][Layout] 槽位判据：跳过。原因：%s"),
            !CanvasSlot
                ? TEXT("VBox 的槽位不是 Canvas Panel Slot —— 位置由父容器的排列规则决定，没有锚点可判")
                : TEXT("画布尺寸还是 0×0（这一帧控件还没被绘制过，GetCachedGeometry 恒为 0×0，"
                       "不能当成「尺寸坏了」；等下一条提示时还会再体检一次）"));
        UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] =================================================="));
        return;
    }

    const FAnchors Anchors = CanvasSlot->GetAnchors();
    const FVector2D Align = CanvasSlot->GetAlignment();
    const FVector2D Pos = CanvasSlot->GetPosition();
    const FVector2D SlotSize = CanvasSlot->GetSize();
    const bool bAutoSize = CanvasSlot->GetAutoSize();

    // 槽位矩形（画布坐标）。画布 = 视口 ⇒ 画布坐标就是屏幕坐标。
    // 公式与引擎一致：左上角 = 锚点比例位置 + Position − Alignment × 自身尺寸
    const auto RectTopLeft = [](const FVector2D& Anchor, const FVector2D& InAlign,
                                const FVector2D& InPos, const FVector2D& InSize,
                                const FVector2D& Canvas) -> FVector2D
    {
        return FVector2D(Anchor.X * Canvas.X, Anchor.Y * Canvas.Y) + InPos - InAlign * InSize;
    };

    // 基准那一侧的尺寸：勾了 Size To Content 就用「内容自然尺寸」，否则用当前固定尺寸当参照
    const int32 RowsForHeight = FMath::Max(1, GetTipCount());
    const FVector2D ContentNaturalSize(
        FMath::Max(1.0f, TipWidth),
        FMath::Max(1.0f, TipHeight) * static_cast<float>(RowsForHeight));
    const FVector2D BaselineSize = bTipSlotBaselineSizeToContent ? ContentNaturalSize : SlotSize;

    const FVector2D ActualTL = RectTopLeft(Anchors.Minimum, Align, Pos, SlotSize, CanvasSize);
    const FVector2D BaselineTL = RectTopLeft(
        TipSlotBaselineAnchors, TipSlotBaselineAlignment, TipSlotBaselinePosition, BaselineSize, CanvasSize);

    UE_LOG(LogTemp, Log,
        TEXT("[Drop][Layout] ---- 槽位判据（vs 设计意图）----\n"
             "      画布 = %.0f × %.0f（= 视口，所以画布坐标就是屏幕坐标）\n"
             "      期望：Anchors=(%.3f, %.3f)  Alignment=(%.2f, %.2f)  Position=(%.1f, %.1f)  尺寸=%s\n"
             "      实测：Anchors=(%.3f, %.3f)  Alignment=(%.2f, %.2f)  Position=(%.1f, %.1f)  尺寸=%s\n"
             "      落点：期望左上角=(%.1f, %.1f)  实测左上角=(%.1f, %.1f)"),
        CanvasSize.X, CanvasSize.Y,
        TipSlotBaselineAnchors.X, TipSlotBaselineAnchors.Y,
        TipSlotBaselineAlignment.X, TipSlotBaselineAlignment.Y,
        TipSlotBaselinePosition.X, TipSlotBaselinePosition.Y,
        bTipSlotBaselineSizeToContent ? TEXT("Size To Content") : TEXT("固定尺寸"),
        Anchors.Minimum.X, Anchors.Minimum.Y, Align.X, Align.Y, Pos.X, Pos.Y,
        bAutoSize
            ? TEXT("Size To Content")
            : *FString::Printf(TEXT("固定 (%.1f, %.1f)"), SlotSize.X, SlotSize.Y),
        BaselineTL.X, BaselineTL.Y, ActualTL.X, ActualTL.Y);

    // ---- 逐项偏差 ----
    TArray<FString> Deviations;

    if (!FMath::IsNearlyEqual(Anchors.Minimum.X, TipSlotBaselineAnchors.X, 0.001f)
        || !FMath::IsNearlyEqual(Anchors.Maximum.X, TipSlotBaselineAnchors.X, 0.001f))
    {
        Deviations.Add(FString::Printf(
            TEXT("Anchors.X = %.3f，期望 %.3f（0 = 左边缘、1 = 右边缘）"),
            Anchors.Minimum.X, TipSlotBaselineAnchors.X));
    }

    if (!FMath::IsNearlyEqual(Anchors.Minimum.Y, TipSlotBaselineAnchors.Y, 0.001f)
        || !FMath::IsNearlyEqual(Anchors.Maximum.Y, TipSlotBaselineAnchors.Y, 0.001f))
    {
        const TCHAR* Where =
            FMath::IsNearlyEqual(Anchors.Minimum.Y, 1.0f, 0.001f) ? TEXT("钉在【屏幕底边】") :
            FMath::IsNearlyEqual(Anchors.Minimum.Y, 0.0f, 0.001f) ? TEXT("钉在【屏幕顶边】") :
                                                                    TEXT("钉在屏幕中间某处");
        Deviations.Add(FString::Printf(
            TEXT("Anchors.Y = %.3f，期望 %.3f（%s）"), Anchors.Minimum.Y, TipSlotBaselineAnchors.Y, Where));
    }

    if (!FMath::IsNearlyEqual(Align.X, TipSlotBaselineAlignment.X, 0.001f)
        || !FMath::IsNearlyEqual(Align.Y, TipSlotBaselineAlignment.Y, 0.001f))
    {
        // Alignment 是「控件的哪个点落在锚点上」。非 (0,0) 时 Position 的含义会跟着变，
        // 这正是「锚点看着对、位置却偏」的常见来源。
        const TCHAR* Meaning =
            FMath::IsNearlyEqual(Align.Y, 1.0f, 0.001f) ? TEXT("锚点被当成【下边】参考，整块上移一个自身高度") :
            FMath::IsNearlyEqual(Align.Y, 0.5f, 0.001f) ? TEXT("锚点被当成【垂直中心】，整块上移半个高度") :
                                                          TEXT("锚点参考点不是左上角");
        Deviations.Add(FString::Printf(
            TEXT("Alignment = (%.2f, %.2f)，期望 (%.2f, %.2f) → %s"),
            Align.X, Align.Y,
            TipSlotBaselineAlignment.X, TipSlotBaselineAlignment.Y, Meaning));
    }

    if (!FMath::IsNearlyEqual(Pos.X, TipSlotBaselinePosition.X, 0.5f)
        || !FMath::IsNearlyEqual(Pos.Y, TipSlotBaselinePosition.Y, 0.5f))
    {
        Deviations.Add(FString::Printf(
            TEXT("Position = (%.1f, %.1f)，期望 (%.1f, %.1f)"),
            Pos.X, Pos.Y, TipSlotBaselinePosition.X, TipSlotBaselinePosition.Y));
    }

    if (bAutoSize != bTipSlotBaselineSizeToContent)
    {
        Deviations.Add(bAutoSize
            ? TEXT("尺寸策略：勾了 Size To Content（期望不勾）")
            : FString::Printf(TEXT("尺寸策略：用固定尺寸 (%.1f, %.1f)，期望勾上 Size To Content"), SlotSize.X, SlotSize.Y));
    }

    if (Deviations.Num() == 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Drop][Layout] ✅ 槽位与设计意图一致：锚点 / 对齐 / 位置 / 尺寸策略四项全部匹配。\n"
                 "      提示会落在这里：左上角 (%.1f, %.1f)，尺寸 %s\n"
                 "      若此时画面里提示仍然错位，那就不是槽位的问题 —— 看上面「组件事实」那一行：\n"
                 "      整个画布跟着投影点走，投影点偏了整块都会偏。"),
            ActualTL.X, ActualTL.Y,
            bAutoSize ? TEXT("随内容（Size To Content）")
                      : *FString::Printf(TEXT("固定 (%.1f, %.1f)"), SlotSize.X, SlotSize.Y));
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop][Layout] ★ 槽位与设计意图有 %d 处不一致 —— 这就是「提示位置错位」的来源："),
            Deviations.Num());
        for (int32 Index = 0; Index < Deviations.Num(); ++Index)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Drop][Layout]     [%d] %s"), Index + 1, *Deviations[Index]);
        }

        UE_LOG(LogTemp, Warning,
            TEXT("[Drop][Layout]     合计偏差：横向 %+.1f px、纵向 %+.1f px（正数=偏右/偏下）"),
            ActualTL.X - BaselineTL.X, ActualTL.Y - BaselineTL.Y);
    }

    // ---- 容量：这是「显示不完全」最直接的那个数 ----
    const float RowHeight = FMath::Max(1.0f, TipHeight);
    const int32 WorstRows = FMath::Max(1, MaxVisibleTips);
    const float NeedHeight = RowHeight * static_cast<float>(WorstRows);
    const int32 FitRows = FMath::FloorToInt(SlotSize.Y / RowHeight);

    if (!bAutoSize && SlotSize.Y + 0.5f < NeedHeight)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop][Layout] ★★ 容量不够 —— 这就是「提示显示不完全」：\n"
                 "      槽位高度固定 %.1f px，每条 %.1f px → 最多完整显示 %d 条；\n"
                 "      而列表上限 MaxVisibleTips = %d，满列表需要 %.1f px。\n"
                 "      ★ 注意这个失败【不会】表现成 Size=(0,0)，所以「只在 Size 为 0 时才报警」的判据是漏的。\n"
                 "      修法：勾上 Size To Content（并清掉 Height Override），让 VBox 按内容长高。"),
            SlotSize.Y, RowHeight, FitRows, WorstRows, NeedHeight);
    }

    // ---- 生长方向：VerticalBox 永远从【框顶】往下排，这跟「把底边钉住」的直觉相反 ----
    if (!bAutoSize && Align.Y > 0.5f)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop][Layout] ★ 生长方向与直觉相反：Alignment.Y = %.2f 把框的【底边】钉在锚点上，\n"
                 "      但 VerticalBox 永远是「从框顶往下排」—— 所以条目是从框顶往下长，\n"
                 "      多出来的那条朝【屏幕底部】冲出去，而不是向上长。\n"
                 "      想让它在某个位置往下排：Alignment 设 (0, 0)、锚点设到那个位置；\n"
                 "      想让它从底部往上堆：只能靠 Size To Content + 锚点设在下方（高度由内容长出来）。"),
            Align.Y);
    }

    UE_LOG(LogTemp, Log,
        TEXT("[Drop][Layout] 修法（WBP_ItemPickupTips → 选中 VBox_ItemTips → Details 的 Slot 段）：\n"
             "        Anchors          Min 和 Max 两处都设成 (%.3f, %.3f)\n"
             "        Alignment        设成 (%.2f, %.2f)\n"
             "        Position         设成 (%.1f, %.1f)\n"
             "        Size To Content  %s\n"
             "      注：C++ 只报事实、不会去改这里的锚点 —— UMG 排版归设计器管。\n"
             "      想让它停在别处也行：把本控件的 Item|Tip|Baseline 那四个值改成你想要的，\n"
             "      体检就会以【你声明的】为基准，不再报偏差。"),
        TipSlotBaselineAnchors.X, TipSlotBaselineAnchors.Y,
        TipSlotBaselineAlignment.X, TipSlotBaselineAlignment.Y,
        TipSlotBaselinePosition.X, TipSlotBaselinePosition.Y,
        bTipSlotBaselineSizeToContent ? TEXT("☑ 勾上（并清掉 Height Override）") : TEXT("☐ 不勾"));

    UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] =================================================="));
}
