// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Engine/DataAsset.h"
#include "BagTypes.generated.h"

class UTexture2D;
class ABattleCharacter;

/**
 * 背包物品运行时条目。
 *
 * 字段与蓝图结构体 Stru_bag_attribute（/Game/UI/bag_sys）一一对应：
 *   tool_id / tool_num / tool_name / tool_introdu / tool_ima / duidie? / max_duidie / kind_tool / star
 *
 * 注意：UserDefinedStruct 的成员属性名在资产里其实是「<字段名>_<序号>_<GUID>」的形式
 * （例如 tool_id_2_E01378974A6CF28897C70689610CE8F9），所以 UInventoryComponent 导入时
 * 用「字段名前缀」匹配而不是全等匹配，既兼容后缀形式也兼容纯字段名。
 */
USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FBagItemEntry
{
    GENERATED_BODY()

    /** 物品 ID（tool_id） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    FName ToolId;

    /** 数据表行名（默认排序的稳定次序依据） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    FName RowName;

    /**
     * 同名物品中的【实例序号】（0 基）。可堆叠物品恒为 0；
     * 不可堆叠物品（duidie? 未勾选）每次获得都占一个新格子，这一批格子里第 0、1、2… 把。
     *
     * ★ 为什么需要它（武器「唯一装备」的根基）：
     *   同一行名（同类）的不可堆叠武器会在背包里占多个格子，光靠 RowName 无法区分
     *   「这一把 vs 那一把」。而「同类不同把的武器只能被一个角色装备」这个规则，
     *   必须能定位到【具体哪一把】—— 靠「RowName + InstanceNo」这个组合键。
     *   它是跨 ReloadItems 稳定的（重建时按同一规则重新分配），不依赖会变动的数组下标。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    int32 InstanceNo = 0;

    /** 物品数量（tool_num） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    int32 ToolNum = 0;

    /** 物品名称（tool_name） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    FString ToolName;

    /** 物品简介（tool_introdu） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    FString ToolIntrodu;

    /** 物品图片（tool_ima） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    TObjectPtr<UTexture2D> ToolImage = nullptr;

    /** 是否可堆叠（duidie?） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    bool bStackable = false;

    /** 堆叠上限（max_duidie） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    int32 MaxStack = 1;

    /** 物品种类（kind_tool，对应 E_kind_tool 的枚举值） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    int32 KindTool = 0;

    /** 物品品质（star，对应 E_star 的枚举值） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    int32 Star = 0;

    /** 是否为有效物品（空槽位为 false） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    bool bValid = false;
};

/**
 * 一次「运行期获得」的记录（掉落 / 任务奖励）。
 *
 * 为什么需要它：背包里的东西有两个来源 —— ① 数据表里写死的初始数量（tool_num）
 * ② 运行期打怪掉进来的数量。而 ReloadItems() 每次打开背包都会把物品列表
 * **整个重建回数据表的值**，所以运行期拿到的东西必须单独留一份记录，
 * 建成后再重放回列表（否则表现为「打开一次背包，掉落物就少一次」）。
 *
 * ★ 它同时是「不可堆叠物品占几格」的唯一真相来源：
 *   不可堆叠物品每次获得都要占一个新格子，重放时必须**逐条**进行，
 *   才能复原出「每一次掉落各自占了一格」的状态。
 *   所以这里存的是「一次一条」，而不是「同名物品的总数」。
 */
USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FBagAcquisition
{
    GENERATED_BODY()

    /** 数据表行名 */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    FName RowName;

    /** 这一次【实际】进入背包的数量（背包满时可能少于掉落数量） */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    int32 Count = 0;

    /** 记录时该物品是否可堆叠 —— 可堆叠才允许把相邻记录合并，避免列表无限增长 */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    bool bStackable = false;

    /** 定义来源：true = 以掉落表（Data_diaoluo_boss）优先，false = 合并字典。
     *  重放时要沿用同一条规则，否则行名撞车时重放出来的名称会和当时不一致。 */
    UPROPERTY(BlueprintReadOnly, Category = "Bag")
    bool bDropSource = false;
};

/** 背包 UI 内的交互类型：一个载体类覆盖所有动态按钮。 */
UENUM()
enum class EBagActionType : uint8
{
    /** 关闭背包（btn_close） */
    Close = 0,
    /** 左侧分类按钮（Index = 分类序号，-1 = 「全部」） */
    Kind = 1,
    /** 道具格子（Index = 格子序号） */
    Slot = 2,
    /** 详情面板「使用」按钮（Index = 当前选中格子序号） */
    Use = 3,
    /** 武器背包的「装备」按钮（Index 忽略，装的是当前选中的那一格）。
     *  为什么要单独一个动作，而不复用 Use：Use 的语义是「使用物品」（消耗、加血…），
     *  武器模式下虽然也走装备，但「点按钮装备」与「点格子即装备」是两种不同的交互 ——
     *  日志里能分开，玩家也能在不改变选中状态的前提下反复确认。 */
    Equip = 4,
    /** 角色面板 chara_pitc_head 下动态生成的头像按钮（Index = 角色槽位序号）。
     *  头像按钮是运行时按「拥有的角色」自动生成的（数量不定），
     *  与格子按钮一样走载体转发：Index = 点的是第几个角色。 */
    CharaHead = 5
};

/**
 * 动态按钮回调载体。
 *
 * UButton::OnClicked 是 DYNAMIC 多播委托，AddDynamic 只能接收「成员函数名字面量」，
 * 因此无法为运行时动态生成的 N 个格子各写一个 UFUNCTION 回调。
 * 标准解法是引入这个中间 UObject：每个按钮绑定一个实例，实例内保存 ActionType + Index，
 * 点击后统一转发给 ABattleCharacter::HandleBagAction()。
 */
UCLASS()
class WUTHERINGWAVES_API UBagWidgetAction : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY()
    EBagActionType ActionType = EBagActionType::Slot;

    UPROPERTY()
    int32 Index = INDEX_NONE;

    UPROPERTY()
    TWeakObjectPtr<ABattleCharacter> Owner;

    UFUNCTION()
    void OnClicked();
};

/**
 * 获得物品通知接口。
 *
 * 用途：「击败怪物 → 物品自动进背包 → 屏幕左侧弹出「物品名 ×N」提示」这条链路里，
 * 提示 UI（WBP_HealthBar）实现本接口，C++ 在入包后自动调用，**不需要在蓝图里连线绑定**。
 *
 * 为什么用接口而不是事件绑定：本工程既有模式是「C++ 推数据 / 蓝图做表现」
 * （见 ABattleCharacter::UpdateHealthBar 通过 GetWidgetFromName 操作 WBP_HealthBar 内部控件）。
 * 接口可以在 C++ 侧按类型查找后直接调用，省掉蓝图里「Get Player Character → Cast → Bind」
 * 三步连线 —— 那三步任意一处写错都是静默失效（提示不出现但没有任何报错），很难排查。
 */
UINTERFACE(BlueprintType, meta = (DisplayName = "Item Pickup Listener"))
class WUTHERINGWAVES_API UItemPickupListener : public UInterface
{
    GENERATED_BODY()
};

class WUTHERINGWAVES_API IItemPickupListener
{
    GENERATED_BODY()

public:
    /**
     * 获得物品时由 C++ 调用（在 WBP_HealthBar 里实现本事件来显示提示）。
     *
     * @param Item           获得后的物品条目：ToolName / ToolImage / Star 等可直接使用；
     *                       Item.ToolNum = 该物品在背包里的【总数】（可能分散在多个格子里 ——
     *                       不可堆叠物品是一格一件，想知道占了几格用
     *                       UInventoryComponent::GetSlotCountForItem）
     * @param ObtainedCount  本次获得的数量 —— 提示上显示的「×N」就是这个值
     *
     * 蓝图侧典型做法：把一条提示加进左侧列表（VerticalBox），播放淡入动画，N 秒后移除。
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Bag|UI")
    void OnItemObtained(const FBagItemEntry& Item, int32 ObtainedCount);
};

/**
 * 单条掉落配置：一种物品 + 它各自的数量与概率。
 *
 * 在怪物蓝图的 Details → Monster|Drop → Drop Items 里添加任意多条，
 * 即可实现「一次掉落一种或多种物品」；每条独立设置数量与概率。
 * 例：3 条 = 必定掉「boss掉落物」、必定再掉一种、30% 概率掉第三种。
 *
 * ★ 物品从哪来：Content/UI/bag_sys/Data_diaoluo_boss 这张表。
 *   它的行结构与 Data_tool 完全相同（Stru_bag_attribute），物品的名称/图标/品质/
 *   分类都从那一行读 —— 即使该物品不在 Data_tool 里，进入背包时也会作为
 *   新物品补进去（见 UInventoryComponent::AddItemByRow）。
 */
USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FMonsterDropEntry
{
    GENERATED_BODY()

    /** 掉落的物品：选 Data_diaoluo_boss 的【行名】。
     *  编辑器里是下拉框（选项由 AMonsterBase::GetDropRowOptions 提供），填不错；
     *  留空则本条被跳过并在日志里说明。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drop",
        meta = (GetOptions = "GetDropRowOptions"))
    FName ItemRowName;

    /** 是否用下面的数量范围覆盖数据表那一行里的 tool_num。
     *  不勾（默认）= 数量直接取数据表的 tool_num，数量只维护一处；
     *  勾上 = 每次击杀在范围内随机，可做「2~4 个」这种浮动掉落。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drop")
    bool bOverrideCount = false;

    /** 掉落数量下限（仅在勾选 Override Count 时生效） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drop",
        meta = (ClampMin = "1", EditCondition = "bOverrideCount"))
    int32 MinCount = 1;

    /** 掉落数量上限（与下限相同 = 每次固定数量） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drop",
        meta = (ClampMin = "1", EditCondition = "bOverrideCount"))
    int32 MaxCount = 1;

    /** 掉落概率 0~1（1 = 每次击杀必定掉落；0.3 = 30% 概率掉落） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drop", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float DropChance = 1.0f;
};

/**
 * 可复用的掉落表资产（可选）。
 *
 * 「多只怪共用同一套掉落」时用：Content 右键 → Miscellaneous → Data Asset →
 * 选 MonsterDropTable → 配好清单 → 在各怪物蓝图里指定 Shared Drop Table。
 * 单只怪单独配置时直接用怪物自身的 Drop Items 数组即可，不需要本资产。
 */
UCLASS(BlueprintType)
class WUTHERINGWAVES_API UMonsterDropTable : public UDataAsset
{
    GENERATED_BODY()

public:
    /** 掉落清单 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drop")
    TArray<FMonsterDropEntry> Drops;
};
