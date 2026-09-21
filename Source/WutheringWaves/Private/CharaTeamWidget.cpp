// Copyright Epic Games, Inc. All Rights Reserved.

#include "CharaTeamWidget.h"
#include "CharaInfo.h"
#include "BattleCharacter.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Engine/Texture2D.h"
#include "UObject/UObjectIterator.h"

// ---- 内层控件自愈工具（2026-09-16）----
// 深度收集子树：比 GetWidgetFromName 多覆盖一类盲区 ——
// 嵌套 UserWidget 的子树（FindWidget 不下钻，嵌套控件对外是黑盒）。
// 注意：悬在树外的「游离控件」这里也看不见 —— 它们在实例化时就被丢弃，游离诊断走模板树子对象全集。
static void CollectSubTreeWidgets(UWidget* W, TArray<UWidget*>& Out)
{
    if (!W)
    {
        return;
    }
    Out.Add(W);
    if (UUserWidget* Nested = Cast<UUserWidget>(W))
    {
        CollectSubTreeWidgets(Nested->GetRootWidget(), Out);
        return;
    }
    if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
    {
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
        {
            CollectSubTreeWidgets(Panel->GetChildAt(i), Out);
        }
    }
}

// 运行时补建文本的统一样式（白字 + 描边阴影，不挡点击）
static void StyleBuiltSlotText(UTextBlock* Txt)
{
    if (!Txt)
    {
        return;
    }
    Txt->SetColorAndOpacity(FSlateColor(FLinearColor::White));
    FSlateFontInfo F = Txt->GetFont();
    F.Size = 16;
    Txt->SetFont(F);
    Txt->SetShadowOffset(FVector2D(1.f, 1.f));
    Txt->SetShadowColorAndOpacity(FLinearColor::Black);
    Txt->SetVisibility(ESlateVisibility::HitTestInvisible);
}

// ★ 强制控件在其父槽里铺满（2026-09-16「选中框比头像小」的根因修复）。
//   WBP_CharaHead_Slot 的根是 CanvasPanel，子控件全是 CanvasPanelSlot 绝对定位 ——
//   尺寸是 WBP 设计时的固定值（按旧 90×90 设计）。格子外套的 USizeBox 改成 120×120 后，
//   CanvasPanel 根会跟着拉伸，但子控件【不会自动缩放】：锚点铺满的跟着变 120，
//   固定尺寸的（img_select_frame = 90）原地不动 → 框 90 < 头像 120 →「框比头像小」。
//   框住整个格子 / 覆盖整个可点区域是功能语义不是审美，由 C++ 保证；等级文本位置不动。
static void ForceFillWithinParent(UWidget* W)
{
    if (!W || !W->Slot)
    {
        return;
    }
    if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(W->Slot))
    {
        Canvas->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
        Canvas->SetOffsets(FMargin(0.f));
    }
    else if (UOverlaySlot* Overlay = Cast<UOverlaySlot>(W->Slot))
    {
        Overlay->SetHorizontalAlignment(HAlign_Fill);
        Overlay->SetVerticalAlignment(VAlign_Fill);
    }
}

// ---- 动态按钮回调载体 ----
void UCharaTeamAction::OnClicked()
{
    if (Owner.IsValid())
    {
        Owner->HandleAction(ActionType, Arg);
    }
}

// ---- 构建 ----
void UCharaTeamWidget::NativeConstruct()
{
    Super::NativeConstruct();

    // ★ WBP 模式 vs 纯 C++ 模式：
    //   - 建了 WBP_CharaTeam 且在设计器里搭好控件（WidgetTree->RootWidget 非空）→ 走绑定模式；
    //   - 纯 C++ 类实例 / 空 WBP（没拖控件）→ 走代码构建（RootWidget 为 null 时）。
    if (BindDesignedWidgets())
    {
        // ★ 打开编队时，快速编队覆盖层必须隐藏 —— WBP 里 quick_edit_root 可能被
        //   设计成 Visible（搭控件时忘了设 Hidden），若不强制收起来，一按 L 就
        //   叠在主视图上（用户报的「没按快速编队就打开 quick_edit_root」）。
        if (QuickEditRoot)
        {
            QuickEditRoot->SetVisibility(ESlateVisibility::Hidden);
        }
        // 绑定成功：WBP 设计树就位，交给 RefreshAll 填内容，不再代码构建。
        RefreshAll();
        return;
    }

    // ---- 纯 C++ 构建（回退）----
    // WidgetTree 根：全屏画布。主视图与快速编队视图都挂在它下面。
    UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    WidgetTree->RootWidget = RootCanvas;

    // 全屏半透明黑底（编队界面压暗游戏画面）
    UImage* Bg = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("BgDim"));
    Bg->SetColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.75f));
    UCanvasPanelSlot* BgSlot = RootCanvas->AddChildToCanvas(Bg);
    BgSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
    BgSlot->SetOffsets(FMargin(0.f));

    BuildMainView();
    BuildQuickEditView();

    RefreshAll();
}

// WBP 模式绑定：按约定名把成员绑到设计树里的控件。
// 约定名（建 WBP_CharaTeam 时用）：
//   txt_team_title      TextBlock        队伍标题
//   btn_team_0 .. 7     Button           左侧队伍号（8 个）
//   slot_0 / 1 / 2      Overlay          中间 3 个立绘槽，每个内含：
//      img_portrait     Image            立绘
//      txt_level        TextBlock        Lv
//      txt_name         TextBlock        名字
//   btn_quick_edit      Button           快速编队
//   quick_edit_root     CanvasPanel      快速编队覆盖层（默认 Hidden）
//   grid_pick           UniformGridPanel 快速编队角色网格
//   btn_finish          Button           完成
// 核心控件（标题 + 3 槽 + 网格 + 完成 + 快编按钮 + 快编根）绑全 → 返回 true；
// 缺任何一个 → 返回 false，调用方回退纯 C++ 构建，绝不让「控件名写错」表现为白屏。
bool UCharaTeamWidget::BindDesignedWidgets()
{
    if (!WidgetTree || !WidgetTree->RootWidget)
    {
        return false; // 没有设计树 → 纯 C++ 构建
    }

    // ★ 分级绑定：
    //   主视图核心（标题 + 3 立绘槽）缺 → 整体回退纯 C++（否则 RefreshAll 白屏/崩）；
    //   快编视图（btn_quick_edit / grid_pick / btn_finish / quick_edit_root）缺 →
    //       只降级快编功能（EnterQuickEdit 里判空），主视图 + 关闭按钮照常工作。
    TeamTitleText = Cast<UTextBlock>(GetWidgetFromName(TEXT("txt_team_title")));

    // 3 个立绘槽（主视图核心，缺则回退）
    Slots.Reset();
    bool bSlotsOk = true;
    bool bAnyInnerMissing = false;

    // ★ 全树控件清单（引擎 ForEachWidget 同款遍历；RootWidget 不可达的控件不会出现在这里）。
    //   用途：① 内层控件精确名没命中时做模糊匹配，自愈「尾部空格/大小写/下划线差异」；
    //        ② 全部模糊失败时 dump 实测清单，让「WBP 里到底有什么」不再靠猜。
    TArray<UWidget*> AllTreeWidgets;
    if (WidgetTree)
    {
        WidgetTree->ForEachWidget([&AllTreeWidgets](UWidget* W) { if (W) AllTreeWidgets.Add(W); });
    }
    auto NormalizeName = [](const FString& In) -> FString
    {
        FString Out;
        for (TCHAR C : In)
        {
            if (C == ' ' || C == '\t' || C == '_') continue;
            Out.AppendChar((C >= 'A' && C <= 'Z') ? static_cast<TCHAR>(C + 32) : C);
        }
        return Out;
    };
    // 精确名优先，模糊名兜底；模糊命中时告警点名实际名字（用户顺手改规范，下次走精确名）
    auto FindInnerWidget = [&](const FString& Target) -> UWidget*
    {
        if (UWidget* Exact = GetWidgetFromName(*Target))
        {
            return Exact;
        }
        const FString NormTarget = NormalizeName(Target);
        for (UWidget* W : AllTreeWidgets)
        {
            if (W && NormalizeName(W->GetName()) == NormTarget)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Team] 控件 '%s' 精确名未命中，模糊匹配到 '%s'（%s）并已绑定。"
                         "→ 建议在 WBP_CharaTeam 里把它改名为精确的 '%s'（当前名字多半含多余空格或大小写差异）。"),
                    *Target, *W->GetName(), *W->GetClass()->GetName(), *Target);
                return W;
            }
        }
        return nullptr;
    };

    for (int32 i = 0; i < 3; ++i)
    {
        UOverlay* SlotRoot = Cast<UOverlay>(GetWidgetFromName(*FString::Printf(TEXT("slot_%d"), i)));
        if (!SlotRoot)
        {
            bSlotsOk = false;
            break;
        }
        FSlotWidgets W;
        W.Root = SlotRoot;
        // ① 精确名 ② 模糊名（去空格/下划线/忽略大小写）
        W.PortraitImage = Cast<UImage>(FindInnerWidget(FString::Printf(TEXT("img_portrait_%d"), i)));
        W.LevelText = Cast<UTextBlock>(FindInnerWidget(FString::Printf(TEXT("txt_level_%d"), i)));
        W.NameText = Cast<UTextBlock>(FindInnerWidget(FString::Printf(TEXT("txt_name_%d"), i)));

        // ③ 槽内「角色匹配」（对仍缺失的项）：在 slot_i 子树（含嵌套 UserWidget）里找
        //    Image/TextBlock 借用 —— 自愈「控件在但名字完全不同」。2026-09-16 截图实测：
        //    六个内层控件在「RootWidget 可达」的树里根本不存在（数据链路全绿、写入目标全缺失）。
        //    ★ 依据项目铁律 1 的例外条款：连续多轮资产实测证实 WBP 侧修不回来，这里 C++ 自愈，
        //      且只动 slot 内部、每一步都有日志点名，用户随时可以按日志恢复 WBP 正式控件。
        const bool bMissingBeforeRepair = (!W.PortraitImage || !W.LevelText || !W.NameText);
        if (bMissingBeforeRepair)
        {
            TArray<UWidget*> SubTree;
            CollectSubTreeWidgets(SlotRoot, SubTree);

            // 立绘：名字含 portrait/head/icon/img 的首个 Image，否则槽内首个 Image
            if (!W.PortraitImage)
            {
                UImage* Borrowed = nullptr;
                for (UWidget* Cand : SubTree)
                {
                    if (UImage* Img = Cast<UImage>(Cand))
                    {
                        if (!Borrowed)
                        {
                            Borrowed = Img;
                        }
                        const FString Low = NormalizeName(Img->GetName());
                        if (Low.Contains(TEXT("portrait")) || Low.Contains(TEXT("head")) ||
                            Low.Contains(TEXT("icon")) || Low.Contains(TEXT("img")))
                        {
                            Borrowed = Img;
                            break;
                        }
                    }
                }
                if (Borrowed)
                {
                    W.PortraitImage = Borrowed;
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Team] slot_%d 'img_portrait_%d' 未找到 → 按角色匹配借用槽内 Image '%s' 当立绘。"
                             "如与设计意图不符，请在 WBP_CharaTeam 的 slot_%d 里建规范名控件 img_portrait_%d。"),
                        i, i, *Borrowed->GetName(), i, i);
                }
            }
            // 等级/名字：按名字提示分派（level/lv → 等级、name → 名字）；无提示按顺序（第 1 个 → 名字、第 2 个 → 等级）
            if (!W.LevelText || !W.NameText)
            {
                TArray<UTextBlock*> Texts;
                for (UWidget* Cand : SubTree)
                {
                    if (UTextBlock* Txt = Cast<UTextBlock>(Cand))
                    {
                        Texts.Add(Txt);
                    }
                }
                for (UTextBlock* Txt : Texts)
                {
                    const FString Low = NormalizeName(Txt->GetName());
                    if (!W.LevelText && Low.Contains(TEXT("level")))
                    {
                        W.LevelText = Txt;
                    }
                    else if (!W.NameText && Low.Contains(TEXT("name")))
                    {
                        W.NameText = Txt;
                    }
                }
                if (!W.NameText && Texts.Num() > 0)
                {
                    W.NameText = Texts[0];
                }
                if (!W.LevelText && Texts.Num() > 1)
                {
                    W.LevelText = Texts[1];
                }
                if (W.LevelText || W.NameText)
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Team] slot_%d 等级/名字文本未找到 → 按角色/顺序借用槽内 TextBlock：等级='%s'、名字='%s'。"
                             "如与设计意图不符，请在 WBP_CharaTeam 的 slot_%d 里建规范名控件 txt_level_%d / txt_name_%d。"),
                        i,
                        W.LevelText ? *W.LevelText->GetName() : TEXT("无"),
                        W.NameText ? *W.NameText->GetName() : TEXT("无"),
                        i, i, i);
                }
            }
        }

        // ④ 运行时补建（最后兜底）：槽内连可借用的控件都没有 → C++ 造，保证「数据已解析」
        //    的信息一定有地方显示。补建立绘插在最底层（不盖住槽内已有文本）、全部不挡点击。
        if (!W.PortraitImage)
        {
            UImage* NewImg = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
            UPanelSlot* InsertedSlot = SlotRoot->InsertChildAt(0, NewImg);
            if (UOverlaySlot* S = Cast<UOverlaySlot>(InsertedSlot))
            {
                S->SetHorizontalAlignment(HAlign_Fill);
                S->SetVerticalAlignment(VAlign_Fill);
                S->SetPadding(FMargin(4.f));
            }
            NewImg->SetVisibility(ESlateVisibility::HitTestInvisible);
            NewImg->SetColorAndOpacity(FLinearColor(0.10f, 0.13f, 0.20f, 0.35f));
            W.PortraitImage = NewImg;
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] slot_%d 立绘彻底缺失（槽内无 Image 可借用）→ 已运行时补建 Image（铺满、不挡点击）。"
                     "在 WBP_CharaTeam 的 slot_%d 里建好正式的 img_portrait_%d 后，会自动优先用正式控件。"), i, i, i);
        }
        if (!W.LevelText)
        {
            UTextBlock* NewTxt = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
            if (UOverlaySlot* S = Cast<UOverlaySlot>(SlotRoot->AddChild(NewTxt)))
            {
                S->SetHorizontalAlignment(HAlign_Left);
                S->SetVerticalAlignment(VAlign_Top);
                S->SetPadding(FMargin(8.f, 6.f));
            }
            StyleBuiltSlotText(NewTxt);
            W.LevelText = NewTxt;
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] slot_%d 等级文本彻底缺失 → 已运行时补建 TextBlock（左上角）。"
                     "建好正式的 txt_level_%d 后会自动优先用正式控件。"), i, i);
        }
        if (!W.NameText)
        {
            UTextBlock* NewTxt = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
            if (UOverlaySlot* S = Cast<UOverlaySlot>(SlotRoot->AddChild(NewTxt)))
            {
                S->SetHorizontalAlignment(HAlign_Center);
                S->SetVerticalAlignment(VAlign_Bottom);
                S->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
            }
            StyleBuiltSlotText(NewTxt);
            W.NameText = NewTxt;
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] slot_%d 名字文本彻底缺失 → 已运行时补建 TextBlock（底部居中）。"
                     "建好正式的 txt_name_%d 后会自动优先用正式控件。"), i, i);
        }

        Slots.Add(W);
        if (bMissingBeforeRepair)
        {
            // 触发游离诊断/树 dump —— 补建成功也打：WBP 侧值得对照修复
            bAnyInnerMissing = true;
        }
        if (!W.PortraitImage || !W.LevelText || !W.NameText)
        {
            // 四层兜底后仍缺失 = slot 根不是可挂子级的容器（UOverlay 必然可挂）——防御性保留
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] slot_%d 内层控件在四层兜底（精确/模糊/借用/补建）后仍缺失 → 对应信息不会显示。"
                     "请检查 slot_%d 是否为可挂子级的容器（Overlay/Canvas 等）。"), i, i);
        }
    }

    // ★ 模糊匹配也救不回来 → dump 实测控件树（名字(类)），下一步修什么直接照着清单点。
    if (bAnyInnerMissing)
    {
        FString TreeDump;
        for (UWidget* W : AllTreeWidgets)
        {
            TreeDump += FString::Printf(TEXT("%s(%s)  "), *W->GetName(), *W->GetClass()->GetName());
        }
        UE_LOG(LogTemp, Warning,
            TEXT("[Team] 模糊匹配后仍有内层控件缺失 → 实测控件树清单（名字(类)）：\n%s\n"
                 "→ 清单里没有 img_portrait_0/1/2、txt_level_0/1/2、txt_name_0/1/2 的话，说明这些控件不在控件树里"
                 "（被删了/悬在树外/名字不同），请在 WBP_CharaTeam 里按这些名字重建或改名；清单里有的话照实际名字改代码约定名。"),
            *TreeDump);
    }

    if (!TeamTitleText || !bSlotsOk)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Team] WBP_CharaTeam 缺主视图核心控件（txt_team_title / slot_0~2）→ 回退纯 C++ 构建。"));
        return false;
    }

    // ---- 快编视图（可选；缺失只降级快编功能）----
    QuickEditButton = Cast<UButton>(GetWidgetFromName(TEXT("btn_quick_edit")));
    PickGrid = Cast<UUniformGridPanel>(GetWidgetFromName(TEXT("grid_pick")));
    QuickEditRoot = Cast<UCanvasPanel>(GetWidgetFromName(TEXT("quick_edit_root")));
    // 完成按钮（快编覆盖层内，写回队伍）：只绑 finish_pick_butt / pick_finish_butt。
    //   ★ btn_finish 是主视图上「关闭整个界面」的独立按钮（ActionType 4），
    //   不再作完成按钮的兜底名 —— 否则两者功能混淆（2026-09-17 用户要求拆开）。
    FinishButton = Cast<UButton>(GetWidgetFromName(TEXT("finish_pick_butt")));
    if (!FinishButton)
    {
        FinishButton = Cast<UButton>(GetWidgetFromName(TEXT("pick_finish_butt")));
    }

    // ★ grid_pick 缺失但 quick_edit_root 在 → 运行时自动补一个 UniformGridPanel 挂在
    //   quick_edit_root 下（铺满）。否则快速编队网格一个格子都不生成，用户只看到
    //   「快编里没有角色可选」。这是「降级兜底」：能自动补好的就别让功能整体失效。
    if (!PickGrid && QuickEditRoot)
    {
        PickGrid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("grid_pick"));
        PickGrid->SetMinDesiredSlotWidth(90.f);
        PickGrid->SetMinDesiredSlotHeight(90.f);
        PickGrid->SetSlotPadding(FMargin(5.f)); // 四周各 5px → 相邻格子间 10px 间隔
        if (UCanvasPanelSlot* GridSlot = QuickEditRoot->AddChildToCanvas(PickGrid))
        {
            GridSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
            GridSlot->SetOffsets(FMargin(20.f, 70.f, 20.f, 80.f));
        }
        UE_LOG(LogTemp, Log, TEXT("[Team] WBP_CharaTeam 缺 grid_pick，已在 quick_edit_root 内自动创建（8 列网格）。"));
    }

    // 8 个队伍号按钮（可选：没配也能跑，只是不能点选队伍）
    TeamButtons.Reset();
    TeamButtonTexts.Reset();
    for (int32 i = 0; i < 8; ++i)
    {
        UButton* Btn = Cast<UButton>(GetWidgetFromName(*FString::Printf(TEXT("btn_team_%d"), i)));
        TeamButtons.Add(Btn);
        // 队伍号按钮的文字：直接取按钮第一个子 TextBlock（WBP 里按钮下挂 TextBlock 即可）
        UTextBlock* Label = Btn ? Cast<UTextBlock>(Btn->GetChildAt(0)) : nullptr;
        TeamButtonTexts.Add(Label);
    }

    // 绑点击回调：队伍号（ActionType 0）
    for (int32 i = 0; i < TeamButtons.Num(); ++i)
    {
        if (TeamButtons[i])
        {
            UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
            Action->ActionType = 0;
            Action->Arg = i;
            Action->Owner = this;
            Actions.Add(Action);
            TeamButtons[i]->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
        }
    }
    // 快编按钮（ActionType 2）—— 只在控件存在时绑
    if (QuickEditButton)
    {
        UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
        Action->ActionType = 2;
        Action->Owner = this;
        Actions.Add(Action);
        QuickEditButton->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
    }
    // 完成按钮（ActionType 3）
    if (FinishButton)
    {
        UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
        Action->ActionType = 3;
        Action->Owner = this;
        Actions.Add(Action);
        FinishButton->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
    }

    // ★ 关闭按钮（可选，但配了就绑）：
    //   close_line_butt —— 关闭整个编队界面（回游戏）
    //   close_edit_butt —— 关闭快速编队覆盖层（回主视图）
    //   btn_finish       —— 关闭整个编队界面（2026-09-17 用户要求：独立于完成编队的 finish_pick_butt）
    if (UButton* CloseLine = Cast<UButton>(GetWidgetFromName(TEXT("close_line_butt"))))
    {
        UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
        Action->ActionType = 4;
        Action->Owner = this;
        Actions.Add(Action);
        CloseLine->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
    }
    if (UButton* BtnFinish = Cast<UButton>(GetWidgetFromName(TEXT("btn_finish"))))
    {
        UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
        Action->ActionType = 4; // 关闭整个界面
        Action->Owner = this;
        Actions.Add(Action);
        BtnFinish->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
    }
    if (UButton* CloseEdit = Cast<UButton>(GetWidgetFromName(TEXT("close_edit_butt"))))
    {
        UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
        Action->ActionType = 5;
        Action->Owner = this;
        Actions.Add(Action);
        CloseEdit->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
    }

    // ★ 队伍号按钮打「实际非 null 数 / 8」：TeamButtons 是 8 长度的对齐数组（WBP 没配的按钮
    //   null 占位），打 Num() 会把「只配了 2 个」谎报成「8 个」——2026-09-15 实测 WBP 里只有
    //   btn_team_0 / btn_team_1，之前日志显示「8 队伍号」纯属误导（日志只能打实数）。
    int32 NumTeamBtns = 0;
    for (const TObjectPtr<UButton>& B : TeamButtons)
    {
        if (B != nullptr)
        {
            ++NumTeamBtns;
        }
    }
    UE_LOG(LogTemp, Log, TEXT("[Team] WBP_CharaTeam 设计树已绑定（标题/3 槽/队伍号按钮 %d/8，缺的队伍无法点击查看/快编%s/关闭按钮）。"),
        NumTeamBtns,
        (PickGrid && QuickEditRoot && FinishButton && QuickEditButton) ? TEXT("完整") : TEXT("降级"));
    return true;
}

void UCharaTeamWidget::BuildMainView()
{
    UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetTree->RootWidget);
    if (!RootCanvas)
        return;

    // ---- 标题「队伍 N」左上 ----
    TeamTitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TeamTitle"));
    TeamTitleText->SetText(FText::FromString(TEXT("队伍 1")));
    TeamTitleText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
    { FSlateFontInfo F = TeamTitleText->GetFont(); F.Size = 26; TeamTitleText->SetFont(F); }
    UCanvasPanelSlot* TitleSlot = RootCanvas->AddChildToCanvas(TeamTitleText);
    TitleSlot->SetPosition(FVector2D(60.f, 24.f));
    TitleSlot->SetSize(FVector2D(240.f, 40.f));

    // ---- 左侧队伍数字列（1~8）----
    for (int32 i = 0; i < 8; ++i)
    {
        UButton* Btn = MakeActionButton(FText::AsNumber(i + 1), /*ActionType*/0, /*Arg*/i,
            FVector2D(48.f, 46.f), FLinearColor(0.08f, 0.10f, 0.16f, 0.9f));

        UTextBlock* Label = Cast<UTextBlock>(Btn->GetChildAt(0));
        TeamButtonTexts.Add(Label);

        UCanvasPanelSlot* BtnSlot = RootCanvas->AddChildToCanvas(Btn);
        BtnSlot->SetPosition(FVector2D(48.f, 90.f + i * 56.f));
        BtnSlot->SetSize(FVector2D(48.f, 46.f));
        TeamButtons.Add(Btn);
    }

    // ---- 中间 3 个立绘槽 ----
    for (int32 i = 0; i < 3; ++i)
    {
        UOverlay* SlotRoot = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), *FString::Printf(TEXT("CharaSlot%d"), i));
        SlotRoot->SetClipping(EWidgetClipping::ClipToBounds);

        // 槽背景（深蓝灰半透明，描边感用两层色差模拟）
        UImage* SlotBg = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), *FString::Printf(TEXT("SlotBg%d"), i));
        SlotBg->SetColorAndOpacity(FLinearColor(0.10f, 0.13f, 0.20f, 0.85f));
        SlotRoot->AddChildToOverlay(SlotBg);

        // 立绘（铺满；保持比例交给图片本身的尺寸策略——先简单拉伸，贴图配好了自然好看）
        UImage* Portrait = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), *FString::Printf(TEXT("SlotPortrait%d"), i));
        UOverlaySlot* PortraitSlot = SlotRoot->AddChildToOverlay(Portrait);
        PortraitSlot->SetHorizontalAlignment(HAlign_Fill);
        PortraitSlot->SetVerticalAlignment(VAlign_Fill);
        PortraitSlot->SetPadding(FMargin(6.f));

        // Lv（左下）
        UTextBlock* Lv = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("SlotLv%d"), i));
        Lv->SetColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.85f, 0.45f)));
        { FSlateFontInfo F = Lv->GetFont(); F.Size = 18; Lv->SetFont(F); }
        UOverlaySlot* LvSlot = SlotRoot->AddChildToOverlay(Lv);
        LvSlot->SetHorizontalAlignment(HAlign_Left);
        LvSlot->SetVerticalAlignment(VAlign_Bottom);
        LvSlot->SetPadding(FMargin(12.f, 0.f, 0.f, 34.f));

        // 名字（底部中间）
        UTextBlock* Name = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("SlotName%d"), i));
        Name->SetColorAndOpacity(FSlateColor(FLinearColor::White));
        { FSlateFontInfo F = Name->GetFont(); F.Size = 18; Name->SetFont(F); }
        UOverlaySlot* NameSlot = SlotRoot->AddChildToOverlay(Name);
        NameSlot->SetHorizontalAlignment(HAlign_Center);
        NameSlot->SetVerticalAlignment(VAlign_Bottom);
        NameSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));

        // 选中描边（当前查看队伍的槽不再单独高亮；描边留给快速编队用，这里恒隐藏）
        UImage* Frame = MakeSelectionFrame(FLinearColor(0.90f, 0.76f, 0.25f, 0.9f), 3.f);
        Frame->SetVisibility(ESlateVisibility::Hidden);
        UOverlaySlot* FrameSlot = SlotRoot->AddChildToOverlay(Frame);
        FrameSlot->SetHorizontalAlignment(HAlign_Fill);
        FrameSlot->SetVerticalAlignment(VAlign_Fill);

        FSlotWidgets W;
        W.Root = SlotRoot;
        W.PortraitImage = Portrait;
        W.LevelText = Lv;
        W.NameText = Name;
        Slots.Add(W);

        UCanvasPanelSlot* SlotCanvas = RootCanvas->AddChildToCanvas(SlotRoot);
        SlotCanvas->SetPosition(FVector2D(360.f + i * 300.f, 170.f));
        SlotCanvas->SetSize(FVector2D(250.f, 440.f));
    }

    // ---- 右下「快速编队」按钮 ----
    QuickEditButton = MakeActionButton(FText::FromString(TEXT("快速编队")), /*ActionType*/2, /*Arg*/INDEX_NONE,
        FVector2D(150.f, 50.f), FLinearColor(0.85f, 0.85f, 0.88f, 1.f));
    {
        // 深色字（浅色按钮底）
        if (UTextBlock* Label = Cast<UTextBlock>(QuickEditButton->GetChildAt(0)))
        {
            Label->SetColorAndOpacity(FSlateColor(FLinearColor(0.05f, 0.05f, 0.08f)));
        }
        UCanvasPanelSlot* BtnSlot = RootCanvas->AddChildToCanvas(QuickEditButton);
        BtnSlot->SetPosition(FVector2D(830.f, 540.f));
        BtnSlot->SetSize(FVector2D(150.f, 50.f));
    }

    // ---- 右下「已出战」状态标签（截图同位置的灰色状态，装饰性）----
    UTextBlock* DeployedText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DeployedState"));
    DeployedText->SetText(FText::FromString(TEXT("已出战")));
    DeployedText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.58f, 0.8f)));
    UCanvasPanelSlot* DeployedSlot = RootCanvas->AddChildToCanvas(DeployedText);
    DeployedSlot->SetPosition(FVector2D(875.f, 555.f));
    DeployedSlot->SetSize(FVector2D(110.f, 30.f));
}

void UCharaTeamWidget::BuildQuickEditView()
{
    UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetTree->RootWidget);
    if (!RootCanvas)
        return;

    // 覆盖层（第二层画布），默认隐藏
    QuickEditRoot = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("QuickEditRoot"));
    QuickEditRoot->SetVisibility(ESlateVisibility::Hidden);
    RootCanvas->AddChildToCanvas(QuickEditRoot);

    // 更深的背景
    UImage* Bg = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("QuickBg"));
    Bg->SetColorAndOpacity(FLinearColor(0.03f, 0.04f, 0.07f, 0.92f));
    UCanvasPanelSlot* BgSlot = QuickEditRoot->AddChildToCanvas(Bg);
    BgSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
    BgSlot->SetOffsets(FMargin(0.f));

    // 标题
    UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("QuickTitle"));
    Title->SetText(FText::FromString(TEXT("快速编队 — 按顺序点选 1~3 号位（再点一次取消），完成后逐位写回队伍")));
    Title->SetColorAndOpacity(FSlateColor(FLinearColor::White));
    { FSlateFontInfo F = Title->GetFont(); F.Size = 18; Title->SetFont(F); }
    UCanvasPanelSlot* TitleSlot = QuickEditRoot->AddChildToCanvas(Title);
    TitleSlot->SetPosition(FVector2D(60.f, 24.f));
    TitleSlot->SetSize(FVector2D(960.f, 34.f));

    // 角色网格（均匀网格）：UE5.7 无列数 API —— 面板宽 920 ÷ 槽最小宽 112 ≈ 每行 8 个
    PickGrid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("PickGrid"));
    PickGrid->SetMinDesiredSlotWidth(112.f);
    PickGrid->SetMinDesiredSlotHeight(132.f);
    UCanvasPanelSlot* GridSlot = QuickEditRoot->AddChildToCanvas(PickGrid);
    GridSlot->SetPosition(FVector2D(60.f, 80.f));
    GridSlot->SetSize(FVector2D(920.f, 420.f));

    // 底部「完成」按钮
    FinishButton = MakeActionButton(FText::FromString(TEXT("完成")), /*ActionType*/3, /*Arg*/INDEX_NONE,
        FVector2D(150.f, 50.f), FLinearColor(0.92f, 0.90f, 0.82f, 1.f));
    {
        if (UTextBlock* Label = Cast<UTextBlock>(FinishButton->GetChildAt(0)))
        {
            Label->SetColorAndOpacity(FSlateColor(FLinearColor(0.05f, 0.05f, 0.08f)));
        }
        UCanvasPanelSlot* BtnSlot = QuickEditRoot->AddChildToCanvas(FinishButton);
        BtnSlot->SetPosition(FVector2D(830.f, 540.f));
        BtnSlot->SetSize(FVector2D(150.f, 50.f));
    }
}

// ---- 小工具 ----
UImage* UCharaTeamWidget::MakeSelectionFrame(const FLinearColor& Color, float BorderWidth)
{
    // Border 画法：不画中间、只画描边 —— 不需要任何贴图资产
    UImage* Frame = NewObject<UImage>(this);
    FSlateBrush Brush;
    Brush.DrawAs = ESlateBrushDrawType::Border;
    Brush.OutlineSettings.Color = Color;
    Brush.OutlineSettings.Width = BorderWidth;
    Brush.TintColor = FSlateColor(FLinearColor::Transparent); // 中间透明
    Frame->SetBrush(Brush);
    return Frame;
}

UButton* UCharaTeamWidget::MakeActionButton(const FText& Label, int32 ActionType, int32 Arg,
    const FVector2D& Size, const FLinearColor& BgColor)
{
    UButton* Btn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
    Btn->SetBackgroundColor(BgColor);

    UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    Text->SetText(Label);
    Text->SetColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.94f, 1.f)));
    { FSlateFontInfo F = Text->GetFont(); F.Size = 18; Text->SetFont(F); }
    Btn->AddChild(Text);

    UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
    Action->ActionType = ActionType;
    Action->Arg = Arg;
    Action->Owner = this;
    Actions.Add(Action);
    Btn->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);

    return Btn;
}

// ---- 动作分发 ----
void UCharaTeamWidget::HandleAction(int32 ActionType, int32 Arg)
{
    switch (ActionType)
    {
    case 0: SelectTeam(Arg); break;
    case 1: TogglePick(Arg); break;
    case 2: EnterQuickEdit(); break;
    case 3: FinishQuickEdit(); break;
    case 4: CloseEntireUI(); break;
    case 5: CloseQuickEditView(); break;
    default: break;
    }
}

void UCharaTeamWidget::SelectTeam(int32 TeamIndex)
{
    if (!OwnerCharacter.IsValid())
        return;
    OwnerCharacter->SetCurrentTeamIndex(TeamIndex);
    RefreshAll();
}

void UCharaTeamWidget::EnterQuickEdit()
{
    // ★ 固定 3 槽：初始化为全空（INDEX_NONE）。每次进快编都是干净的一轮。
    PendingPickIndexes = { INDEX_NONE, INDEX_NONE, INDEX_NONE };

    // ★ 完成按钮兜底：WBP 若没在 quick_edit_root 里放 btn_finish（或放错位置），
    //   这里运行时在快编覆盖层里自动补一个「完成」按钮，保证快编视图一定能点完成。
    if (!FinishButton && QuickEditRoot)
    {
        FinishButton = MakeActionButton(FText::FromString(TEXT("完成")), /*ActionType*/3, /*Arg*/INDEX_NONE,
            FVector2D(150.f, 50.f), FLinearColor(0.92f, 0.90f, 0.82f, 1.f));
        if (UTextBlock* Label = Cast<UTextBlock>(FinishButton->GetChildAt(0)))
        {
            Label->SetColorAndOpacity(FSlateColor(FLinearColor(0.05f, 0.05f, 0.08f)));
        }
        if (UCanvasPanelSlot* BtnSlot = QuickEditRoot->AddChildToCanvas(FinishButton))
        {
            BtnSlot->SetAnchors(FAnchors(1.f, 1.f));
            BtnSlot->SetAlignment(FVector2D(1.f, 1.f));
            BtnSlot->SetOffsets(FMargin(-180.f, -70.f, 30.f, 20.f));
            BtnSlot->SetSize(FVector2D(150.f, 50.f));
        }
    }

    if (QuickEditRoot)
    {
        QuickEditRoot->SetVisibility(ESlateVisibility::Visible);
    }
    // ★ 入口诊断：拥有角色数（0 = 表没加载/没填行 → 网格必然一格不出）。
    if (OwnerCharacter.IsValid())
    {
        OwnerCharacter->EnsureCharaInfoTable();
        UE_LOG(LogTemp, Log, TEXT("[Team] 进入快速编队：拥有角色 %d 个，按顺序点击头像格子选 1~3 号位。"),
            OwnerCharacter->GetOwnedCharaRows().Num());
    }
    RefreshQuickEdit();
}

void UCharaTeamWidget::TogglePick(int32 OwnedIndex)
{
    // ★ 固定 3 槽语义（slot_0/slot_1/slot_2 三个固定位置，INDEX_NONE = 空槽）：
    //   - 已选（在某个槽里）→ 清空该槽（序号消失）；其余槽保持原位不变。
    //   - 未选 → 找第一个空槽填入；满 3 个（无空槽）→ 不操作（点其余角色不显示序号）。
    const int32 Existing = PendingPickIndexes.Find(OwnedIndex);
    if (Existing != INDEX_NONE)
    {
        // 取消：只清空该槽，不前移其它槽（B 仍是 2、C 仍是 3，等新点击继承这个空位）
        PendingPickIndexes[Existing] = INDEX_NONE;
    }
    else
    {
        const int32 Empty = PendingPickIndexes.IndexOfByKey(INDEX_NONE);
        if (Empty != INDEX_NONE)
        {
            PendingPickIndexes[Empty] = OwnedIndex;
        }
        // 无空槽 = 已满 3 个 → 忽略本次点击
    }
    RefreshQuickEdit();
}

void UCharaTeamWidget::FinishQuickEdit()
{
    if (!OwnerCharacter.IsValid())
        return;

    // 本次选择 → 行名列表：★ 按点击顺序「压实」写入（2026-09-15 用户明确要求，替代旧的
    // 「选位 i → 队伍位 i」位置对齐写法）—— 收集所有非空选择，依次写进队伍 slot_0..N-1；
    // 空掉的选位直接跳过、不制造中间空位。之前的位置对齐写法在「第 0 位取消/没选」时会把
    // 后面选的角色留在错位的位置（[None|B|None]），表现就是「有一个空位，其余信息显示不对」。
    // ★ 先预热表：GetOwnedCharaRows 是 const 函数、依赖表已加载，否则返回空 → 写回全空。
    OwnerCharacter->EnsureCharaInfoTable();
    const TArray<FName> OwnedRows = OwnerCharacter->GetOwnedCharaRows();
    TArray<FName> PickedRows;
    for (int32 i = 0; i < PendingPickIndexes.Num(); ++i)
    {
        const int32 Idx = PendingPickIndexes[i];
        if (Idx != INDEX_NONE && OwnedRows.IsValidIndex(Idx))
        {
            PickedRows.Add(OwnedRows[Idx]);
        }
    }
    TArray<FName> NewMembers;
    NewMembers.SetNum(3); // 固定 3 位，压实后多出的位保持 NAME_None
    for (int32 i = 0; i < PickedRows.Num() && i < 3; ++i)
    {
        NewMembers[i] = PickedRows[i];
    }

    // ★ 诊断：完成按钮点下去后，把「选了啥 → 解析成啥行名」整条链路打出来，
    //   让「信息没同步到 slot」能一眼定位是「没选到」「行名空」「写回没生效」哪一环。
    UE_LOG(LogTemp, Log,
        TEXT("[Team] FinishQuickEdit：本次选择 PendingPickIndexes=[%d,%d,%d]，OwnedRows 共 %d 行 → NewMembers=[%s|%s|%s]"),
        PendingPickIndexes.IsValidIndex(0) ? PendingPickIndexes[0] : INDEX_NONE,
        PendingPickIndexes.IsValidIndex(1) ? PendingPickIndexes[1] : INDEX_NONE,
        PendingPickIndexes.IsValidIndex(2) ? PendingPickIndexes[2] : INDEX_NONE,
        OwnedRows.Num(),
        *NewMembers[0].ToString(), *NewMembers[1].ToString(), *NewMembers[2].ToString());

    // ★ slot_0 不能为空：本轮一个都没选（或选的都没解析出）→ slot_0 兜底玩家自己，
    //   保证队伍永远至少有一个上场角色（BP_CharacterPlayer 自己）。
    if (NewMembers[0].IsNone())
    {
        const FName DefaultRow = OwnerCharacter->ResolveDefaultMemberRow();
        if (!DefaultRow.IsNone())
        {
            NewMembers[0] = DefaultRow;
        }
    }

    // 整体覆盖：本轮选了几个写几个，其余位清空（重新编队 = 新一轮整体替换，不是补空缺）
    OwnerCharacter->OverwriteTeamMembers(OwnerCharacter->GetCurrentTeamIndex(), NewMembers);

    // ★ 固定 3 槽：完成后清空（下次进快编 EnterQuickEdit 会重新初始化为全空）
    PendingPickIndexes.Reset();
    if (QuickEditRoot)
    {
        QuickEditRoot->SetVisibility(ESlateVisibility::Hidden);
    }
    RefreshAll();
}

void UCharaTeamWidget::CloseEntireUI()
{
    if (OwnerCharacter.IsValid())
    {
        // 复用 Owner 的关闭逻辑（移除本控件 + 恢复游戏 + 收回鼠标）
        OwnerCharacter->CloseCharaTeamUI();
    }
}

void UCharaTeamWidget::CloseQuickEditView()
{
    // 只关快速编队覆盖层，回主视图；本次选择作废
    PendingPickIndexes.Reset();
    if (QuickEditRoot)
    {
        QuickEditRoot->SetVisibility(ESlateVisibility::Hidden);
    }
    RefreshAll();
}

// ---- 刷新 ----
void UCharaTeamWidget::RefreshAll()
{
    if (!OwnerCharacter.IsValid())
        return;

    ABattleCharacter* Ch = OwnerCharacter.Get();

    // 标题
    if (TeamTitleText)
    {
        TeamTitleText->SetText(FText::FromString(FString::Printf(TEXT("队伍 %d"), Ch->GetCurrentTeamIndex() + 1)));
    }

    // 队伍号按钮：当前队高亮（金色字 + 亮底）
    // ★ TeamButtons 是「下标=队伍号」的对齐数组，WBP 没配的按钮以 null 占位
    //   （如只配了 btn_team_0/1，则 [2]~[7] 全是 null）。
    //   判据必须是「指针非空」—— IsValidIndex(i) 在 i<Num() 的循环里恒为 true，
    //   只验证下标范围，验证不了指针（上一版正是这里对 null 调 SetBackgroundColor 崩溃）。
    for (int32 i = 0; i < TeamButtons.Num(); ++i)
    {
        const bool bCurrent = (i == Ch->GetCurrentTeamIndex());
        if (TeamButtons[i])
        {
            TeamButtons[i]->SetBackgroundColor(bCurrent
                ? FLinearColor(0.55f, 0.45f, 0.12f, 1.f)
                : FLinearColor(0.08f, 0.10f, 0.16f, 0.9f));
        }
        if (TeamButtonTexts.IsValidIndex(i) && TeamButtonTexts[i])
        {
            TeamButtonTexts[i]->SetColorAndOpacity(FSlateColor(bCurrent
                ? FLinearColor(1.f, 0.85f, 0.35f)
                : FLinearColor(0.70f, 0.72f, 0.80f)));
        }
    }

    // 立绘槽：按当前队伍成员填
    // ★ 先预热表：否则 GetTeamMembers 依赖的 FindCharaInfo 会因表未加载而全部返回空 → slot 全显示「空」。
    Ch->EnsureCharaInfoTable();
    const TArray<FName>& Members = Ch->GetTeamMembers(Ch->GetCurrentTeamIndex());
    // slot_0 默认角色：未编过队（slot_0 为空）时兜底显示玩家自己，保证 slot_0 永不空。
    const FName DefaultRow = Ch->ResolveDefaultMemberRow();
    // ★ 兜底失效必须出声：表里没有任何一行的 CharaClass 等于玩家当前类 → DefaultRow 为 None，
    //   打开界面时 slot_0 就是空的，而表面症状只是「slot_0 没同步」。点名玩家类名 + 修法。
    if (DefaultRow.IsNone())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Team] slot_0 兜底失效：Data_chara_imfor 里没有一行的 CharaClass 等于玩家当前类（%s）→ 没编队时 slot_0 显示空。"
                 "修法：表里加一行，Chara Class 列选玩家蓝图类（%s），行名自定（如 10003）。"),
            *Ch->GetClass()->GetName(), *Ch->GetClass()->GetName());
    }

    // ★ 按顺序压实显示（2026-09-15 用户明确要求）：跳过数据里的空位（None），把有效成员
    //   依次填进 slot_0..N-1，剩余槽位显示「空」。数据中间的空位不再让后面的成员信息
    //   「不显示/错位」。例：数据 [A|None|C] → 显示 slot_0=A、slot_1=C、slot_2=空。
    TArray<FName> DisplayRows;
    for (const FName& MemberRow : Members)
    {
        if (!MemberRow.IsNone())
        {
            DisplayRows.Add(MemberRow);
        }
    }

    // ★ 一行汇总：队伍数据 + 压实后的实际显示 + slot_0 兜底行（None = 空位，属正常状态）。
    //   队伍数据是运行时状态：PIE/游戏每次重启都回到空，编队结果不跨回合保存。
    //   兜底行单独打出来 —— 「打开时 slot_0 显示的到底是谁」一眼可查（表里若有多行
    //   CharaClass 相同/行名怪异，这里直接暴露）。
    UE_LOG(LogTemp, Log,
        TEXT("[Team] 队伍 %d 成员数据：[%s | %s | %s] → 界面按顺序显示：[%s | %s | %s]（slot_0 兜底行='%s'；空位已跳过，没编满的队伍打开就是空位）"),
        Ch->GetCurrentTeamIndex() + 1,
        *Members[0].ToString(), *Members[1].ToString(), *Members[2].ToString(),
        DisplayRows.IsValidIndex(0) ? *DisplayRows[0].ToString() : TEXT("-"),
        DisplayRows.IsValidIndex(1) ? *DisplayRows[1].ToString() : TEXT("-"),
        DisplayRows.IsValidIndex(2) ? *DisplayRows[2].ToString() : TEXT("-"),
        *DefaultRow.ToString());

    for (int32 i = 0; i < Slots.Num(); ++i)
    {
        const FSlotWidgets& W = Slots[i];
        if (!W.Root)
            continue;

        FName Row = DisplayRows.IsValidIndex(i) ? DisplayRows[i] : NAME_None;
        // ★ slot_0 不能为空：空则回退玩家自己（只在显示层兜底，不改写 Teams 数据）。
        if (i == 0 && Row.IsNone() && !DefaultRow.IsNone())
        {
            Row = DefaultRow;
        }
        const FCharaInfoEntry* Info = Row.IsNone() ? nullptr : Ch->FindCharaInfo(Row);

        // ★ 诊断分级（按 2026-09-15 截图日志修正）：「行名 None = 空位」是正常状态（队伍没编满），
        //   不该告警 —— 之前把空位也打成 Warning，打开界面就出两条红字，把正常当故障、误导排查。
        //   只对两种真正的数据问题告警：① 填了行名却查不到表 ② CharaClass 空。
        if (!Row.IsNone() && !Info)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] slot_%d 行名 '%s' 在 Data_chara_imfor 里查不到（FindCharaInfo 返回空）→ 显示为空。"
                     "请检查行名是否与表一致、表是否已填该行。"),
                i, *Row.ToString());
        }
        else if (Info && !Info->CharaClass)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] slot_%d 行名 '%s' 的 CharaClass 为空 → 无法取等级/CDO，按空槽显示。"
                     "请给 Data_chara_imfor 该行填 Chara Class。"),
                i, *Row.ToString());
        }

        if (!Info || !Info->CharaClass)
        {
            // 空槽（清空显示；slot_0 兜底失效的场景由上面的 DefaultRow 告警点名）
            if (W.PortraitImage)
            {
                W.PortraitImage->SetBrushFromTexture(nullptr);
                W.PortraitImage->SetColorAndOpacity(FLinearColor::White);
            }
            if (W.LevelText) W.LevelText->SetText(FText::GetEmpty());
            if (W.NameText) W.NameText->SetText(FText::FromString(TEXT("空")));
            continue;
        }

        const ABattleCharacter* CDO = Info->CharaClass->GetDefaultObject<ABattleCharacter>();
        UTexture2D* ShowTex = Info->Portrait ? Info->Portrait.Get() : Info->HeadIcon.Get();
        if (W.PortraitImage)
        {
            // 立绘优先；没配立绘用头像；两者都空 → 占位色块（至少能看出「这个槽有角色」）
            W.PortraitImage->SetBrushFromTexture(ShowTex);
            W.PortraitImage->SetColorAndOpacity(ShowTex ? FLinearColor::White : FLinearColor(0.30f, 0.50f, 0.80f, 0.60f));
        }
        if (W.LevelText && CDO)
        {
            W.LevelText->SetText(FText::FromString(FString::Printf(TEXT("Lv %d"), CDO->GetCharacterLevelValue())));
        }
        // 名字三级兜底：表 CharaName → 蓝图 CDO 的 CharacterName → 行名。
        // ★ 表 CharaName 列与角色蓝图 CharacterName 都空时，名字文本一直是空白 ——
        //   视觉上就是「信息没同步」但日志全绿（数据链路没问题）；用行名兜底后
        //   槽里至少显示 '10001' 这类可辨识标识，同时日志注明名字来源。
        const bool bNameFromTable = !Info->CharaName.IsEmpty();
        FString UsedName = bNameFromTable ? Info->CharaName.ToString()
                                          : (CDO ? CDO->GetCharacterDisplayName() : FString());
        if (UsedName.IsEmpty())
        {
            UsedName = Row.ToString();
        }
        if (W.NameText)
        {
            W.NameText->SetText(FText::FromString(UsedName));
        }

        // ★ 每槽结论（铁律：诊断必须自己给结论，不能只报事实）分两段：
        //   ① 数据解析结果（行名/名字/等级/图）② 写入目标状态（三个内层控件引用在不在）。
        //   数据解析成功 ≠ 写入成功 —— 引用缺失时 SetText/SetBrush 被静默跳过，屏幕不会有
        //   任何变化；结论行必须如实区分这两种情况，别再打「已填充」误导（2026-09-16 教训）。
        const bool bImgOk = (W.PortraitImage != nullptr);
        const bool bLvOk = (W.LevelText != nullptr);
        const bool bNameOk = (W.NameText != nullptr);
        UE_LOG(LogTemp, Log,
            TEXT("[Team] slot_%d ← 行 '%s' 数据已解析：名字='%s'（%s）、Lv=%d、图=%s。写入目标：图%s、Lv%s、名%s。%s"),
            i, *Row.ToString(), *UsedName,
            bNameFromTable ? TEXT("来自表 CharaName") : TEXT("表未填名，用蓝图名/行名兜底"),
            CDO ? CDO->GetCharacterLevelValue() : 0,
            ShowTex ? *ShowTex->GetName() : TEXT("无——表里 Portrait 和 Head Icon 都没配，显示占位色块"),
            bImgOk ? TEXT("OK") : TEXT("缺失"),
            bLvOk ? TEXT("OK") : TEXT("缺失"),
            bNameOk ? TEXT("OK") : TEXT("缺失"),
            (bImgOk && bLvOk && bNameOk)
                ? TEXT("已写入；屏幕仍看不到 → 查 WBP_CharaTeam 层级/尺寸（被盖住/Size 0），不是数据问题。")
                : TEXT("→ 有「缺失」= 这项不会更新，屏幕不变的原因就在这（按绑定期的控件树清单修 WBP）。"));
    }
}

void UCharaTeamWidget::RefreshQuickEdit()
{
    if (!OwnerCharacter.IsValid() || !PickGrid)
        return;

    ABattleCharacter* Ch = OwnerCharacter.Get();

    // ★ 关键：GetOwnedCharaRows() 依赖 Data_chara_imfor 已加载。表懒加载未触发时它返回空，
    //   网格就会「一格都不生成」。必须先预热表（EnsureCharaInfoTable 幂等）。
    Ch->EnsureCharaInfoTable();
    const TArray<FName> OwnedRows = Ch->GetOwnedCharaRows();

    // ★ 诊断日志：网格空时能一眼看出是「表没数据」还是「拥有列表为空」。
    //   （之前「点了快速编队没反应」的根因正是表未加载导致 OwnedRows 为 0，一条日志就能定位。）
    if (OwnedRows.Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Team] 快速编队网格为空：GetOwnedCharaRows 返回 0 行。"
                 "请检查 Data_chara_imfor 是否已填角色行（CharaClass 列），或 OwnedCharaRows 过滤是否误空。"));
    }

    // 重建网格
    PickGrid->ClearChildren();
    PickEntries.Reset();
    Actions = Actions.FilterByPredicate([](const UCharaTeamAction* A) { return IsValid(A); });

    // ★ 统一格子尺寸与间隔（无论 grid_pick 是用户在 WBP 里建的还是代码自动补的，
    //   都强制 120×120 正方形 + 四周各 15px padding = 相邻格子间 30px 间隔）。
    //   这样「不改变正方形 + 30px 间隔」不依赖用户在 WBP 里的设置，行为可预期。
    constexpr float SlotSize = 120.f;
    constexpr float SlotGap = 30.f;
    PickGrid->SetMinDesiredSlotWidth(SlotSize);
    PickGrid->SetMinDesiredSlotHeight(SlotSize);
    PickGrid->SetSlotPadding(FMargin(SlotGap * 0.5f));

    // ★★★ 尊重用户在 WBP 里设置的 grid_pick 尺寸（1800×800），不再用 SetSize 覆盖。
    //   行列数 = 根据「容器实际尺寸 ÷ (格子 + 间隔)」动态反推，保持正方形 + 固定间隔：
    //   列数 = floor((容器宽 + 间隔) / (120 + 30))、行数同理。
    //   1800×800 → 列 = floor(1830/150) = 12、行 = floor(830/150) = 5 → 12×5=60 槽。
    //   ★★★ 固定铺满所有槽（「2 个角色占满整屏」bug 的根因与修法）：
    //   SUniformGridPanel 的行列数 = 「子槽位坐标最大值 + 1」，只建 2 格 → 按 2 列 1 行均分占屏。
    //   必须把行列数算出的槽全部铺上：有角色的放条目、空槽放 Hidden 占位
    //   （Hidden 参与行列划分但不绘制、不可点击），行列数稳定，格子尺寸稳定。
    int32 GridColumns = 8;
    int32 GridRows = 7;
    if (const UCanvasPanelSlot* GridCanvas = Cast<UCanvasPanelSlot>(PickGrid->Slot))
    {
        const FVector2D GridSize = GridCanvas->GetSize();
        if (GridSize.X > 1.f && GridSize.Y > 1.f)
        {
            GridColumns = FMath::Max(1, FMath::FloorToInt((GridSize.X + SlotGap) / (SlotSize + SlotGap)));
            GridRows = FMath::Max(1, FMath::FloorToInt((GridSize.Y + SlotGap) / (SlotSize + SlotGap)));
            UE_LOG(LogTemp, Log,
                TEXT("[Team] grid_pick 尺寸 %.0f×%.0f → 按 %.0f 格子 + %.0f 间隔算出 %d 列 × %d 行（%d 槽）。"),
                GridSize.X, GridSize.Y, SlotSize, SlotGap, GridColumns, GridRows, GridColumns * GridRows);
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Team] grid_pick 的尺寸读到 0（%s）→ 回退默认 8 列 × 7 行。"
                     "若布局异常，请检查 WBP 里 grid_pick 的 Size X/Y 是否已设为 1800×800。"),
                GridSize.X <= 1.f ? TEXT("宽度异常") : TEXT("高度异常"));
        }
    }
    const int32 TotalSlots = FMath::Max(GridColumns * GridRows, OwnedRows.Num());
    for (int32 i = 0; i < TotalSlots; ++i)
    {
        const FCharaInfoEntry* Info = OwnedRows.IsValidIndex(i) ? Ch->FindCharaInfo(OwnedRows[i]) : nullptr;
        if (!Info)
        {
            // 空槽：Hidden 占位（不可视、不可点，但参与 UniformGrid 的行列计算）
            UImage* Spacer = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
            Spacer->SetVisibility(ESlateVisibility::Hidden);
            PickGrid->AddChildToUniformGrid(Spacer, i / GridColumns, i % GridColumns);
            continue;
        }

        // ★ 单个角色条目：改用 WBP_CharaHead_Slot（用户设计的头像格），
        //   里面已含 img_head_icon（头像）/ img_select_frame（选中框）/ tx_level_chara（等级）。
        //   外面套 USizeBox 钉死正方形（SlotSize）；再叠一个序号 TextBlock（右上 1~3）。
        const ABattleCharacter* CDO = Info->CharaClass ? Info->CharaClass->GetDefaultObject<ABattleCharacter>() : nullptr;
        UUserWidget* SlotWidget = nullptr;
        const TSubclassOf<UUserWidget> HeadSlotClass = Ch->GetCharaHeadSlotClass();
        if (HeadSlotClass)
        {
            SlotWidget = CreateWidget<UUserWidget>(GetWorld(), HeadSlotClass);
        }

        // 根节点：优先 WBP 格子；加载失败/未建 WBP → 回退旧的纯 C++ 条目，保证功能不断。
        UWidget* CellRoot = nullptr;
        UButton* ClickTarget = nullptr;
        UImage* FrameImg = nullptr;
        UTextBlock* Order = nullptr;
        if (SlotWidget)
        {
            // 用 WBP 格子：填充头像 + 等级
            if (UImage* HeadImg = Cast<UImage>(SlotWidget->GetWidgetFromName(TEXT("img_head_icon"))))
            {
                // ★ 头像必须 HitTestInvisible：WBP_CharaHead_Slot 里 img_head_icon 叠在 head_butt
                //   按钮上方，若保持 Visible 会拦截鼠标点击 → 按钮 OnClicked 收不到事件，
                //   表现为「格子无法点击选择」。与角色面板 ApplyCharaHeadSlotVisual 同一处理。
                HeadImg->SetVisibility(ESlateVisibility::HitTestInvisible);
                // ★ 头像铺满格子：CanvasPanel 子控件不会跟随 SizeBox 缩放，锚点/尺寸不是铺满时
                //   头像会按 WBP 设计时的固定值显示（格子改 120 后头像/框尺寸脱节）。
                ForceFillWithinParent(HeadImg);
                if (Info->HeadIcon.Get())
                {
                    HeadImg->SetBrushFromTexture(Info->HeadIcon.Get(), false);
                    HeadImg->SetColorAndOpacity(FLinearColor::White);
                }
                else
                {
                    HeadImg->SetColorAndOpacity(FLinearColor(0.25f, 0.30f, 0.40f, 1.f));
                }
            }
            if (UTextBlock* LvText = Cast<UTextBlock>(SlotWidget->GetWidgetFromName(TEXT("tx_level_chara"))))
            {
                LvText->SetText(CDO ? FText::FromString(FString::Printf(TEXT("Lv.%d"), CDO->GetCharacterLevelValue())) : FText::GetEmpty());
                // 等级文本也不应拦截点击
                LvText->SetVisibility(ESlateVisibility::HitTestInvisible);
            }
            FrameImg = Cast<UImage>(SlotWidget->GetWidgetFromName(TEXT("img_select_frame")));
            // 选中框默认隐藏（未选中不显示、也不挡点击）；选中时 Visible 但仍要 HitTestInvisible
            // 让「再点一次取消」能穿透选中框命中按钮。
            if (FrameImg)
            {
                // ★ 选中框铺满格子（「选中框比头像小」的根因修复）：img_select_frame 在 WBP 里
                //   是按旧 90×90 设计的固定尺寸（CanvasPanelSlot 绝对定位），格子锁 120 后它不跟随，
                //   框 90 < 头像 120 → 截图里框明显小于头像。框住整个格子是功能语义，C++ 保证。
                ForceFillWithinParent(FrameImg);
                FrameImg->SetVisibility(ESlateVisibility::Hidden);
            }
            // 点击目标：head_butt（实际名）→ btn_head（旧约定）→ 根节点（若本身是 Button）
            ClickTarget = Cast<UButton>(SlotWidget->GetWidgetFromName(TEXT("head_butt")));
            if (!ClickTarget)
            {
                ClickTarget = Cast<UButton>(SlotWidget->GetWidgetFromName(TEXT("btn_head")));
            }
            if (ClickTarget)
            {
                // ★ 按钮铺满格子：head_butt 同样是 WBP 固定尺寸（旧 90），格子 120 后若不铺满，
                //   四周 30px 是点不到的死角（视觉看不出、点击时才暴露）。
                ForceFillWithinParent(ClickTarget);
            }
            if (!ClickTarget)
            {
                ClickTarget = Cast<UButton>(SlotWidget->GetRootWidget());
            }

            // 套 USizeBox 钉死正方形（尺寸 = SlotSize，与网格 MinDesiredSlot 一致）。
            // ★ 根因修复（「头像框大小并非格子大小」）：WBP_CharaHead_Slot 的根是 CanvasPanel
            //   （root_head_slot），直接 AddChild 后它不会自动撑满 SizeBox —— 必须把返回的
            //   USizeBoxSlot 显式设成 HAlign_Fill/VAlign_Fill，头像格才会填满，
            //   否则 CanvasPanel 保持其设计时尺寸（可能比格子小或塌缩），头像「比格子小」。
            USizeBox* CellBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
            CellBox->SetWidthOverride(SlotSize);
            CellBox->SetHeightOverride(SlotSize);
            if (USizeBoxSlot* CellSlot = Cast<USizeBoxSlot>(CellBox->AddChild(SlotWidget)))
            {
                CellSlot->SetHorizontalAlignment(HAlign_Fill);
                CellSlot->SetVerticalAlignment(VAlign_Fill);
            }
            CellRoot = CellBox;
        }
        else
        {
            // 回退：纯 C++ 按钮条目（头像 + 金框 + 序号 + Lv），保证没建 WBP 时也能跑
            UButton* Btn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
            Btn->SetBackgroundColor(FLinearColor(0.10f, 0.13f, 0.20f, 0.95f));
            UOverlay* EntryRoot = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
            UImage* Head = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
            Head->SetBrushFromTexture(Info->HeadIcon.Get());
            UOverlaySlot* HeadSlot = EntryRoot->AddChildToOverlay(Head);
            HeadSlot->SetHorizontalAlignment(HAlign_Fill);
            HeadSlot->SetVerticalAlignment(VAlign_Fill);
            FrameImg = MakeSelectionFrame(FLinearColor(0.92f, 0.78f, 0.25f, 1.f), 3.f);
            UOverlaySlot* FrameSlot = EntryRoot->AddChildToOverlay(FrameImg);
            FrameSlot->SetHorizontalAlignment(HAlign_Fill);
            FrameSlot->SetVerticalAlignment(VAlign_Fill);
            Btn->AddChild(EntryRoot);
            ClickTarget = Btn;
            CellRoot = Btn;
        }

        // 序号 TextBlock（右上角，叠在格子上）—— 两种路径都叠
        Order = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
        Order->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.3f)));
        { FSlateFontInfo F = Order->GetFont(); F.Size = 16; Order->SetFont(F); }
        Order->SetVisibility(ESlateVisibility::HitTestInvisible);
        if (UOverlay* OrderHost = Cast<UOverlay>(CellRoot))
        {
            UOverlaySlot* OrderSlot = OrderHost->AddChildToOverlay(Order);
            OrderSlot->SetHorizontalAlignment(HAlign_Right);
            OrderSlot->SetVerticalAlignment(VAlign_Top);
            OrderSlot->SetPadding(FMargin(0.f, 2.f, 6.f, 0.f));
        }
        else
        {
            // WBP 路径下 CellRoot 是 SizeBox，序号没法叠进 SizeBox 里 —— 用 UniformGridSlot 上层叠一个
            // 做法：把 CellRoot 包进一个 Overlay，序号叠在 Overlay 上，CellRoot 铺满。
            UOverlay* Wrap = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
            UOverlaySlot* CellSlotInWrap = Wrap->AddChildToOverlay(CellRoot);
            CellSlotInWrap->SetHorizontalAlignment(HAlign_Fill);
            CellSlotInWrap->SetVerticalAlignment(VAlign_Fill);
            UOverlaySlot* OrderSlot = Wrap->AddChildToOverlay(Order);
            OrderSlot->SetHorizontalAlignment(HAlign_Right);
            OrderSlot->SetVerticalAlignment(VAlign_Top);
            OrderSlot->SetPadding(FMargin(0.f, 2.f, 6.f, 0.f));
            CellRoot = Wrap;
        }

        // 点击绑定：优先格子内部按钮；都没有则整个条目根节点也接受点击（若可点）
        UCharaTeamAction* Action = NewObject<UCharaTeamAction>(this);
        Action->ActionType = 1; // TogglePick
        Action->Arg = i;
        Action->Owner = this;
        Actions.Add(Action);
        if (ClickTarget)
        {
            ClickTarget->OnClicked.AddDynamic(Action, &UCharaTeamAction::OnClicked);
        }

        UUniformGridSlot* GridSlotSlot = PickGrid->AddChildToUniformGrid(CellRoot, i / GridColumns, i % GridColumns);
        if (GridSlotSlot)
        {
            // ★ 槽位尺寸由 UniformGrid 决定（=max(MinDesiredSlot, 容器÷列数)），
            //   用 Fill 会把格子拉到槽位大小（容器铺满/格子算多时格子被放大）。
            //   改 Center：内容（USizeBox 钉死 SlotSize）在槽内居中、不被拉伸，格子恒为正方形。
            GridSlotSlot->SetHorizontalAlignment(HAlign_Center);
            GridSlotSlot->SetVerticalAlignment(VAlign_Center);
        }
        PickEntries.Add(CellRoot);

        // 选中态：序号 + 金框
        const int32 PickOrder = PendingPickIndexes.IndexOfByKey(i);
        if (PickOrder != INDEX_NONE)
        {
            Order->SetText(FText::AsNumber(PickOrder + 1));
            // ★ 选中框要显示，但必须是 HitTestInvisible，否则会挡住「再点一次取消」的点击。
            if (FrameImg) FrameImg->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            Order->SetText(FText::GetEmpty());
            if (FrameImg) FrameImg->SetVisibility(ESlateVisibility::Hidden);
        }
    }
}
