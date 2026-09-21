// Copyright Epic Games, Inc. All Rights Reserved.

#include "InventoryComponent.h"

#include "Engine/DataTable.h"
#include "Engine/Texture2D.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"

// =====================================================================
// 反射读取辅助：把蓝图结构体（UserDefinedStruct）的任意字段读成 C++ 原生值。
// 好处：不依赖字段的具体属性类型（Int/Name/Str/Text/Object/SoftObject/Bool/Enum
// 都能读），也不需要把 Stru_bag_attribute 重建成 C++ 结构体。
// =====================================================================
namespace BagTableReader
{
    /** 字段名匹配：用前缀匹配以兼容 UserDefinedStruct 的「<字段名>_<序号>_<GUID>」属性名。 */
    static bool NameIs(const FProperty* Prop, const TCHAR* BaseName)
    {
        return Prop && Prop->GetName().StartsWith(BaseName, ESearchCase::IgnoreCase);
    }

    static bool ReadInt(const FProperty* Prop, const void* Addr, int32& OutValue)
    {
        if (const FNumericProperty* Num = CastField<FNumericProperty>(Prop))
        {
            if (Num->IsInteger())
            {
                OutValue = static_cast<int32>(Num->GetSignedIntPropertyValue(Addr));
                return true;
            }
            if (Num->IsFloatingPoint())
            {
                OutValue = FMath::RoundToInt(Num->GetFloatingPointPropertyValue(Addr));
                return true;
            }
        }
        // 蓝图枚举（FEnumProperty，底层通常是 uint8）
        if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
        {
            if (const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty())
            {
                OutValue = static_cast<int32>(Underlying->GetSignedIntPropertyValue(Addr));
                return true;
            }
        }
        if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
        {
            OutValue = BoolProp->GetPropertyValue(Addr) ? 1 : 0;
            return true;
        }
        if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
        {
            OutValue = FCString::Atoi(*StrProp->GetPropertyValue(Addr));
            return true;
        }
        return false;
    }

    static bool ReadBool(const FProperty* Prop, const void* Addr, bool& OutValue)
    {
        if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
        {
            OutValue = BoolProp->GetPropertyValue(Addr);
            return true;
        }
        int32 AsInt = 0;
        if (ReadInt(Prop, Addr, AsInt))
        {
            OutValue = (AsInt != 0);
            return true;
        }
        if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
        {
            OutValue = StrProp->GetPropertyValue(Addr).ToBool();
            return true;
        }
        return false;
    }

    static bool ReadString(const FProperty* Prop, const void* Addr, FString& OutValue)
    {
        if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
        {
            OutValue = StrProp->GetPropertyValue(Addr);
            return true;
        }
        if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
        {
            OutValue = TextProp->GetPropertyValue(Addr).ToString();
            return true;
        }
        if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
        {
            OutValue = NameProp->GetPropertyValue(Addr).ToString();
            return true;
        }
        int32 AsInt = 0;
        if (ReadInt(Prop, Addr, AsInt))
        {
            OutValue = FString::FromInt(AsInt);
            return true;
        }
        return false;
    }

    static bool ReadName(const FProperty* Prop, const void* Addr, FName& OutValue)
    {
        FString AsString;
        if (ReadString(Prop, Addr, AsString))
        {
            OutValue = FName(*AsString);
            return true;
        }
        return false;
    }

    static UObject* ReadObject(const FProperty* Prop, const void* Addr)
    {
        // 先判软引用：FSoftObjectProperty 也派生自 FObjectPropertyBase，
        // 若用 GetObjectPropertyValue 直接读会把软引用路径当指针解析。
        if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
        {
            const FSoftObjectPtr SoftValue = SoftProp->GetPropertyValue(Addr);
            return SoftValue.LoadSynchronous();
        }
        if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
        {
            return ObjProp->GetObjectPropertyValue(Addr);
        }
        return nullptr;
    }
} // namespace BagTableReader

// =====================================================================
// UInventoryComponent
// =====================================================================

UInventoryComponent::UInventoryComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

    // 品质 1~5 星配色（灰 / 绿 / 蓝 / 紫 / 金），可在角色蓝图里覆盖
    StarColors = {
        FLinearColor(0.55f, 0.56f, 0.60f, 1.0f), // 1★ 灰
        FLinearColor(0.32f, 0.72f, 0.40f, 1.0f), // 2★ 绿
        FLinearColor(0.28f, 0.56f, 0.92f, 1.0f), // 3★ 蓝
        FLinearColor(0.68f, 0.38f, 0.92f, 1.0f), // 4★ 紫
        FLinearColor(0.95f, 0.76f, 0.32f, 1.0f)  // 5★ 金
    };
}

void UInventoryComponent::BeginPlay()
{
    Super::BeginPlay();

    // 启动时先读一次，保证蓝图里随时能拿到数据
    ReloadItems();

    // 规则自检：在临时数组上跑真实算法，不碰背包数据。
    // 放在 ReloadItems 之后 —— 它会用到 KindCount / UnsetStackLimit 等配置。
    if (bRunStackRuleSelfTest)
    {
        RunStackRuleSelfTest();
    }
}

bool UInventoryComponent::EnsureToolTable()
{
    if (ToolTable)
    {
        return true;
    }

    if (ToolTablePath.IsEmpty())
    {
        return false;
    }

    ToolTable = LoadObject<UDataTable>(nullptr, *ToolTablePath);
    if (ToolTable)
    {
        UE_LOG(LogTemp, Log, TEXT("[Bag] ToolTable lazy-loaded: %s"), *ToolTablePath);
    }
    return ToolTable != nullptr;
}

bool UInventoryComponent::EnsureDropItemTable()
{
    if (DropItemTable)
    {
        return true;
    }

    if (DropItemTablePath.IsEmpty())
    {
        return false;
    }

    DropItemTable = LoadObject<UDataTable>(nullptr, *DropItemTablePath);
    if (DropItemTable)
    {
        UE_LOG(LogTemp, Log, TEXT("[Bag] DropItemTable lazy-loaded: %s"), *DropItemTablePath);
    }
    return DropItemTable != nullptr;
}

// 把一行数据读成 FBagItemEntry。抽成独立函数是因为它现在有两个调用方：
// ReloadItems 读 Data_tool（建 AllItems）与读 Data_diaoluo_boss（建 ItemCatalog），
// 两边必须用完全一样的字段解析规则，否则会出现「同一件物品在两处显示不一样」。
bool UInventoryComponent::ReadRowEntry(const UScriptStruct* RowStruct, uint8* RowData, FName RowName,
    FBagItemEntry& OutEntry, TArray<FString>* OutFieldTypes) const
{
    if (!RowStruct || !RowData)
    {
        return false;
    }

    // 顺序与 FieldNames 一致：tool_id / tool_num / tool_name / tool_introdu /
    //                     tool_ima / duidie? / max_duidie / kind_tool / star
    if (OutFieldTypes)
    {
        OutFieldTypes->Reset();
        OutFieldTypes->SetNumZeroed(9);
    }

    OutEntry = FBagItemEntry();
    OutEntry.RowName = RowName;
    OutEntry.ToolId = RowName;

    for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
    {
        FProperty* Prop = *It;
        const void* Addr = Prop->ContainerPtrToValuePtr<void>(RowData);

        // 注意：max_duidie 必须在 duidie 之前判断（后者是前者的子串关系）
        if (BagTableReader::NameIs(Prop, TEXT("max_duidie")))
        {
            BagTableReader::ReadInt(Prop, Addr, OutEntry.MaxStack);
            if (OutFieldTypes) { (*OutFieldTypes)[6] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("duidie")))
        {
            BagTableReader::ReadBool(Prop, Addr, OutEntry.bStackable);
            if (OutFieldTypes) { (*OutFieldTypes)[5] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("tool_introdu")))
        {
            BagTableReader::ReadString(Prop, Addr, OutEntry.ToolIntrodu);
            if (OutFieldTypes) { (*OutFieldTypes)[3] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("tool_ima")))
        {
            OutEntry.ToolImage = Cast<UTexture2D>(BagTableReader::ReadObject(Prop, Addr));
            if (OutFieldTypes) { (*OutFieldTypes)[4] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("tool_id")))
        {
            BagTableReader::ReadName(Prop, Addr, OutEntry.ToolId);
            if (OutFieldTypes) { (*OutFieldTypes)[0] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("tool_name")))
        {
            BagTableReader::ReadString(Prop, Addr, OutEntry.ToolName);
            if (OutFieldTypes) { (*OutFieldTypes)[2] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("tool_num")))
        {
            BagTableReader::ReadInt(Prop, Addr, OutEntry.ToolNum);
            if (OutFieldTypes) { (*OutFieldTypes)[1] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("kind_tool")))
        {
            BagTableReader::ReadInt(Prop, Addr, OutEntry.KindTool);
            if (OutFieldTypes) { (*OutFieldTypes)[7] = Prop->GetCPPType(); }
        }
        else if (BagTableReader::NameIs(Prop, TEXT("star")))
        {
            BagTableReader::ReadInt(Prop, Addr, OutEntry.Star);
            if (OutFieldTypes) { (*OutFieldTypes)[8] = Prop->GetCPPType(); }
        }
    }

    if (OutEntry.MaxStack <= 0)
    {
        OutEntry.MaxStack = 1;
    }
    OutEntry.KindTool = FMath::Clamp(OutEntry.KindTool, 0, FMath::Max(0, KindCount - 1));
    OutEntry.bValid = true;
    return true;
}

void UInventoryComponent::ReloadItems()
{
    AllItems.Reset();
    ItemCatalog.Reset();
    DropCatalog.Reset();
    ConflictingRowNames.Reset();
    ToolTableRowNames.Reset();
    bLoaded = false;

    if (!EnsureToolTable() || !ToolTable)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] ToolTable 未找到（%s）。请在 BP_PlayerCharacter 的 InventoryComponent 上指定数据表。"),
            *ToolTablePath);
        return;
    }

    const UScriptStruct* RowStruct = ToolTable->GetRowStruct();
    if (!RowStruct)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Bag] Data_tool 未设置行结构体（Row Struct）。"));
        return;
    }

    const TArray<FName> RowNames = ToolTable->GetRowNames();

    // 字段自检：对第一行输出「每个字段是否识别到 + 它在该结构体里的真实 C++ 类型」。
    // 用于排查「数据表填了但背包里是空的」这类问题（列名拼错 / 类型不受支持）。
    static const TCHAR* FieldNames[9] = {
        TEXT("tool_id"), TEXT("tool_num"), TEXT("tool_name"), TEXT("tool_introdu"),
        TEXT("tool_ima"), TEXT("duidie?"), TEXT("max_duidie"), TEXT("kind_tool"), TEXT("star") };
    bool bFieldReportDone = false;

    // ---- 1) 基础物品：Data_tool 的每一行都是玩家【已拥有】的物品 ----
    for (const FName& RowName : RowNames)
    {
        uint8* RowData = ToolTable->FindRowUnchecked(RowName);
        if (!RowData)
        {
            continue;
        }

        FBagItemEntry Entry;
        TArray<FString> FieldTypes;
        if (!ReadRowEntry(RowStruct, RowData, RowName, Entry, &FieldTypes))
        {
            continue;
        }

        if (!bFieldReportDone)
        {
            bFieldReportDone = true;
            FString Report;
            for (int32 FieldIndex = 0; FieldIndex < 9; ++FieldIndex)
            {
                const bool bFound = FieldTypes.IsValidIndex(FieldIndex) && !FieldTypes[FieldIndex].IsEmpty();
                Report += FString::Printf(TEXT("\n    %-13s = %s"),
                    FieldNames[FieldIndex],
                    bFound ? *FieldTypes[FieldIndex] : TEXT("<未找到>"));
            }
            UE_LOG(LogTemp, Log, TEXT("[Bag] 数据表字段识别结果（%s）：%s"),
                *GetNameSafe(RowStruct), *Report);
        }

        AllItems.Add(Entry);
        ItemCatalog.Add(RowName, Entry);
        // 记下「这一行属于背包物品表」—— 掉落表的行**不进**这个集合。
        // 用途见 IsToolTableRow：武器蓝图声明的行名必须落在这里，否则就是填错了表。
        ToolTableRowNames.Add(RowName);
    }

    // ---- 1.5) 表里的初始数量超过「单格上限」时，明确报出来 ----
    // 为什么只报不改：数据表里 tool_num 填的是「初始拥有几个」，它不经过入包逻辑，
    // 所以不会被拆成多格（拆了会把一张表瞬间铺满格子，还可能触发「背包满」把东西丢掉）。
    // 但这确实是个数据不一致点：不可堆叠物品填 tool_num=5，界面上却是「1 格 ×5」，
    // 之后再获得才会开始一格一件。不说清楚，使用者会以为是 bug。
    {
        TArray<FString> OverflowNames;
        for (const FBagItemEntry& Entry : AllItems)
        {
            const int32 Limit = GetEffectiveStackLimit(Entry);
            if (Entry.ToolNum > Limit)
            {
                OverflowNames.AddUnique(FString::Printf(TEXT("%s(tool_num=%d > 上限 %d)"),
                    *(Entry.ToolName.IsEmpty() ? Entry.RowName.ToString() : Entry.ToolName),
                    Entry.ToolNum, Limit));
            }
        }

        if (OverflowNames.Num() > 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] 数据表里有 %d 行的初始 tool_num 超过了单格上限：%s\n"
                     "      行为：表里的初始数量【不拆格】（仍显示为 1 格 ×N），"
                     "只有之后「获得」到的数量才按堆叠规则分格。\n"
                     "      建议：不可堆叠物品（duidie? 未勾选）把 tool_num 填成 1，"
                     "需要多件就直接加多行或改用掉落/奖励发放。"),
                OverflowNames.Num(), *FString::Join(OverflowNames, TEXT(" / ")));
        }
    }

    // ---- 2) 掉落物定义：Data_diaoluo_boss 只进物品字典，不进背包 ----
    //     为什么：这张表装的是「掉落物」这一类物品，它们要等玩家真的打出来才该出现在背包里。
    //     所以这里只把定义收进 ItemCatalog；真正的入包在第 3 步（逐条重放运行期获得记录）。
    int32 DropDefCount = 0;
    if (EnsureDropItemTable() && DropItemTable)
    {
        const UScriptStruct* DropRowStruct = DropItemTable->GetRowStruct();
        if (DropRowStruct)
        {
            for (const FName& DropRowName : DropItemTable->GetRowNames())
            {
                uint8* DropRowData = DropItemTable->FindRowUnchecked(DropRowName);
                if (!DropRowData)
                {
                    continue;
                }

                FBagItemEntry DropEntry;
                if (!ReadRowEntry(DropRowStruct, DropRowData, DropRowName, DropEntry))
                {
                    continue;
                }

                // 掉落物定义单独存一份：行名撞车时，只有这一份能说清「这一行是掉落表的哪一行」。
                DropCatalog.Add(DropRowName, DropEntry);
                ++DropDefCount;

                // ---- 行名撞车检测（本次两个 bug 之一的根因就在这里）----
                // DataTable 的行名**不是全局唯一**的：两张表各自从 1 开始编号是常态
                // （新建行的默认名就是 1、2、3…）。而 ItemCatalog 是「按行名跨表合并」的字典，
                // 一旦重名，它就只认得出其中一张表的那一行 ——
                // 表现是「怪物配的是掉落表的行，打出来却在提示里显示 Data_tool 的名称和图标，
                // 数量也加到了 Data_tool 那件物品上」，表面看像 UI 或数据表绑定错了。
                //
                // 这里选择「先写先赢」（Data_tool 的条目保持不动）+ 报警，而不是静默覆盖：
                // 行名是跨表的物品标识，改哪一边是设计决定，代码不应该替使用者做。
                if (ItemCatalog.Contains(DropRowName))
                {
                    ConflictingRowNames.AddUnique(DropRowName);
                    continue;
                }

                ItemCatalog.Add(DropRowName, DropEntry);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] %s 没有设置行结构体（Row Struct），掉落物定义读不出来。"
                     "修法：打开该数据表，把 Row Structure 选成 Stru_bag_attribute。"),
                *GetNameSafe(DropItemTable));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 掉落物数据表未找到（%s），掉落物将没有名称/图标可显示。"
                 "修法：确认 Content/UI/bag_sys 下有 Data_diaoluo_boss，"
                 "或在 InventoryComponent 上直接指定 Drop Item Table。"),
            *DropItemTablePath);
    }

    // ---- 3) 重放「运行期获得」记录 ----
    //     ★ 必须是【逐条重放】，不能只按行名加一个总数。
    //       因为不可堆叠物品是一格一件：每次获得都要占一个新的空白格，
    //       只有按获得的先后逐条塞格子，才能复原出「击杀 4 次 = 占 4 格」。
    //       若只叠加总数，无论打到几个都只剩 1 格，看起来就像掉落被吞了。
    //     ★ 这一步同时替代了旧版的「补条目」：掉落物专有物品（定义在
    //       Data_diaoluo_boss、不在 Data_tool 里）会在重放时按需现场建格，
    //       所以不会再出现「数量加进去了但背包里看不见」。
    int32 ReplayApplied = 0;    // 重放进背包的总数量
    int32 ReplayNewSlots = 0;   // 重放新占用的格子数
    int32 ReplaySkipped = 0;    // 定义已失效、跳过不记的条数
    int32 ReplayFullLost = 0;   // 重放时因背包满而放不下的数量

    for (const FBagAcquisition& Record : RuntimeAcquisitions)
    {
        if (Record.Count <= 0)
        {
            continue;
        }

        const FBagItemEntry* Definition = ResolveItemDefinition(Record.RowName, Record.bDropSource);
        if (!Definition)
        {
            // 定义没了（行被改名 / 表被换掉）→ 用背包里已有的同名格子兜底。
            // 这样至少不会把玩家已经拿到的物品凭空抹掉，只会在日志里留一条记录。
            Definition = AllItems.FindByPredicate(
                [&Record](const FBagItemEntry& Entry) { return Entry.RowName == Record.RowName; });
        }

        if (!Definition)
        {
            ++ReplaySkipped;
            continue;
        }

        int32 NewSlots = 0;
        int32 AddedToOld = 0;
        const int32 Added = FillSlots(*Definition, Record.Count, NewSlots, AddedToOld, /*bVerbose=*/false);
        ReplayApplied += Added;
        ReplayNewSlots += NewSlots;
        ReplayFullLost += (Record.Count - Added);
    }

    // 汇总视图（派生量）：行名 → 运行期获得总数，蓝图里查「这个物品一共打到了多少」很方便
    BonusCounts.Reset();
    for (const FBagAcquisition& Record : RuntimeAcquisitions)
    {
        if (Record.Count > 0)
        {
            BonusCounts.FindOrAdd(Record.RowName) += Record.Count;
        }
    }

    SortItems();
    bLoaded = true;

    if (ReplayApplied > 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Bag] 已重放 %d 条运行期获得记录：合计 %d 个，其中 %d 个占用新格子。"),
            RuntimeAcquisitions.Num(), ReplayApplied, ReplayNewSlots);
    }

    if (ReplaySkipped > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 有 %d 条运行期获得记录对应的物品定义已失效（行被改名 / 表格换过），"
                 "这些物品无法重建，已跳过。修法：确认掉落表的行名没有被改过，"
                 "或调用 ClearRuntimeItems() 清掉这些陈旧的记录。"),
            ReplaySkipped);
    }

    if (ReplayFullLost > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 重建时背包已满，有 %d 个物品放不进格子。"
                 "修法：加大 InventoryComponent 的 Slot Count，"
                 "或把该物品在数据表里设成可堆叠（duidie? 打勾 + 填 max_duidie）。"),
            ReplayFullLost);
    }

    UE_LOG(LogTemp, Log,
        TEXT("[Bag] 已从 %s 导入 %d 个格子（%d 种物品，槽位上限 %d）。"),
        *GetNameSafe(ToolTable), AllItems.Num(), ItemCatalog.Num(), SlotCount);

    if (DropDefCount > 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Bag] 掉落物表 %s 提供 %d 条物品定义；其中 %d 个已打出来的物品占用 %d 个格子。"),
            *GetNameSafe(DropItemTable), DropDefCount, ReplayApplied, ReplayNewSlots);
    }

    // ---- 行名冲突报告：不报出来的话，现象是「掉落提示显示成 Data_tool 的物品」这类玄学 bug ----
    if (ConflictingRowNames.Num() > 0)
    {
        TArray<FString> ConflictList;
        for (const FName& ConflictingName : ConflictingRowNames)
        {
            ConflictList.Add(ConflictingName.ToString());
        }

        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] ★ 两张数据表有 %d 个重名行：%s\n"
                 "      后果：怪物掉落这些行时，名称 / 图标 / 数量都会取到 Data_tool 那一行，\n"
                 "            看起来就像「配的是 Data_diaoluo_boss，显示的是 Data_tool」。\n"
                 "      临时行为：掉落时 C++ 会按「掉落表优先」刷新显示信息（见 [Drop] 重名 那条日志），\n"
                 "            但两件不同的物品仍然共用同一个背包格子，数量会混在一起。\n"
                 "      修法（推荐，一步到位）：打开 %s，把重名的行改成不会与 Data_tool 重名的名字\n"
                 "            （例如 boss掉落物），再回怪物蓝图 Monster|Drop → Drop Items 的下拉框重新选一次。"),
            ConflictingRowNames.Num(), *FString::Join(ConflictList, TEXT(" / ")),
            *GetNameSafe(DropItemTable));
    }

    // 逐条摘要（最多 5 条）：直接看出「分类 / 品质 / 数量 / 排序」是否符合预期。
    // 为什么需要：数据表里**没填过的列会保持默认值**（kind_tool=0 → 第 1 个分类，star=0 → 1★，
    // duidie?=false → 不可堆叠）。于是「所有物品都挤在第一个分类、而且全是 1★」这种问题
    // 从 UI 上很难判断是代码错还是数据没填 —— 这里一眼就能看出来。
    const int32 PreviewCount = FMath::Min(AllItems.Num(), 5);
    for (int32 Index = 0; Index < PreviewCount; ++Index)
    {
        const FBagItemEntry& Item = AllItems[Index];
        const int32 Limit = GetEffectiveStackLimit(Item);

        FString StackDesc;
        if (!Item.bStackable)
        {
            StackDesc = TEXT("不可堆叠·一格一件");
        }
        else if (Item.MaxStack <= 1)
        {
            // duidie? 打勾了但 max_duidie 没填 —— 这是最常见的「少填一列」，
            // 所以描述里要显式写「未填」，而不是伪装成一个正常的上限值
            StackDesc = TEXT("可堆叠·上限未填");
        }
        else
        {
            StackDesc = FString::Printf(TEXT("可堆叠·上限%d"), Item.MaxStack);
        }

        UE_LOG(LogTemp, Log,
            TEXT("[Bag]   #%d 格 行=%s | 分类=%d | 品质=%d(%d★) | 本格数量=%d | 有效上限=%d（%s）| 名称=%s"),
            Index + 1, *Item.RowName.ToString(), Item.KindTool,
            Item.Star, GetStarLevel(Item.Star), Item.ToolNum, Limit, *StackDesc, *Item.ToolName);
    }

    // ---- 堆叠规则自检：把「哪些物品会一格一件」直接列出来 ----
    // 为什么必须报：不可堆叠物品会快速吃掉格子，而「duidie? 没打勾」在数据表里
    // 是一个不显眼的空白 —— 不报的话现象是「打了几次怪，背包里同一件东西占了好几格」，
    // 很难联想到是数据表少填了一列。
    {
        TArray<FString> UnstackableNames;   // 不可堆叠 → 一格一件
        TArray<FString> UnsetLimitNames;    // 可堆叠但 max_duidie 没填
        TArray<FString> StackableNames;     // 正常可堆叠

        for (const FBagItemEntry& Entry : AllItems)
        {
            const FString Label = Entry.ToolName.IsEmpty() ? Entry.RowName.ToString() : Entry.ToolName;
            if (!Entry.bStackable)
            {
                UnstackableNames.AddUnique(Label);
            }
            else if (Entry.MaxStack <= 1)
            {
                UnsetLimitNames.AddUnique(Label);
            }
            else
            {
                StackableNames.AddUnique(Label);
            }
        }

        UE_LOG(LogTemp, Log,
            TEXT("[Bag] 堆叠规则：一格一件的物品 %d 种｜可堆叠的 %d 种｜当前占格 %d / %d"
                 "（开关 bUnstackableTakesNewSlot = %s）"),
            UnstackableNames.Num(), StackableNames.Num() + UnsetLimitNames.Num(),
            AllItems.Num(), SlotCount,
            bUnstackableTakesNewSlot ? TEXT("开") : TEXT("关（所有物品都往同一格叠加）"));

        if (UnstackableNames.Num() > 0)
        {
            UE_LOG(LogTemp, Log,
                TEXT("[Bag]   · 不可堆叠（每次获得各占一格）：%s"),
                *FString::Join(UnstackableNames, TEXT(" / ")));
        }
        else
        {
            // ★ 这个分支必须存在。数据表 9 列都填过时，最可能的结果就是「全部可堆叠」——
            //   此时新规则在界面上**完全看不出来**（所有物品都只占 1 格）。
            //   不明确说一句，使用者会以为「功能没生效」，然后去查代码而不是去查数据表。
            UE_LOG(LogTemp, Log,
                TEXT("[Bag]   · 当前【没有任何物品是不可堆叠的】→ 不会出现「一格一件」，"
                     "每件物品始终只占 1 格（这是数据表的 duidie? 决定的，不是代码没生效）。\n"
                     "        想试「一格一件」：打开数据表，把该物品那一行的 duidie? 取消勾选；"
                     "改完不用重启，下次打开背包即生效。\n"
                     "        也可以先用只读预览确认：DescribeAcquisition(行名, 数量)"));
        }

        if (UnsetLimitNames.Num() > 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag]   · 这些物品勾了「可堆叠」但 max_duidie 没填（<=1），"
                     "本次按「无上限 %d」处理，不会拆格。\n"
                     "        修法：在数据表里给它们填上 max_duidie（例如 999），"
                     "否则改动物品数量时没法体现堆叠上限：%s"),
                UnsetStackLimit, *FString::Join(UnsetLimitNames, TEXT(" / ")));
        }
    }
}

// ------------------------------------------------------------------
// 运行时获得物品（掉落 / 奖励）
// ------------------------------------------------------------------

bool UInventoryComponent::IsToolTableRow(FName RowName) const
{
    if (RowName.IsNone())
    {
        return false;
    }

    // ★ 宽松规则（很重要，别改成严格）：数据还没读进内存时一律放行。
    //   这个函数只用来【否决】明显属于掉落表的行名，不能把「还不知道」当成「不属于」——
    //   否则在 ReloadItems 之前（或读表失败时）会把所有武器都判成不可装备，
    //   那比原来那个 bug 更严重（原来只是显示错件，那会变成完全装备不上）。
    if (!bLoaded || ToolTableRowNames.Num() == 0)
    {
        return true;
    }

    return ToolTableRowNames.Contains(RowName);
}

bool UInventoryComponent::GetItemByRow(FName RowName, FBagItemEntry& OutItem) const
{
    if (RowName.IsNone())
    {
        return false;
    }

    // ★ 不可堆叠物品会有多个同名格子（一格一件），所以这里不能只找第一条就返回：
    //   展示字段（名称/图标/品质）取第一条，但数量必须是【所有同名格子求和】，
    //   否则「按行名查数量」会得到 1，而实际上玩家有 5 个。
    const FBagItemEntry* First = nullptr;
    int32 TotalCount = 0;

    for (const FBagItemEntry& Entry : AllItems)
    {
        if (Entry.RowName == RowName)
        {
            if (!First)
            {
                First = &Entry;
            }
            TotalCount += Entry.ToolNum;
        }
    }

    if (!First)
    {
        return false;
    }

    OutItem = *First;
    OutItem.ToolNum = TotalCount;
    return true;
}

int32 UInventoryComponent::GetEffectiveStackLimit(const FBagItemEntry& Item) const
{
    // 关掉开关 = 退回旧行为：所有物品都往同一格累加，永远不拆格
    if (!bUnstackableTakesNewSlot)
    {
        return TNumericLimits<int32>::Max();
    }

    // 不可堆叠 → 一格一件（这正是「再次获得占新空白格」的实现）
    if (!Item.bStackable)
    {
        return 1;
    }

    // 可堆叠但上限没填 → 当作无上限，而不是 1（见 UnsetStackLimit 的说明）
    if (Item.MaxStack <= 1)
    {
        return FMath::Max(1, UnsetStackLimit);
    }

    return Item.MaxStack;
}

int32 UInventoryComponent::GetSlotCountForItem(FName RowName) const
{
    int32 Count = 0;
    for (const FBagItemEntry& Entry : AllItems)
    {
        if (Entry.RowName == RowName)
        {
            ++Count;
        }
    }
    return Count;
}

int32 UInventoryComponent::GetTotalCountForItem(FName RowName) const
{
    int32 Total = 0;
    for (const FBagItemEntry& Entry : AllItems)
    {
        if (Entry.RowName == RowName)
        {
            Total += Entry.ToolNum;
        }
    }
    return Total;
}

void UInventoryComponent::ClearRuntimeItems()
{
    RuntimeAcquisitions.Reset();
    BonusCounts.Reset();
    ReloadItems();

    UE_LOG(LogTemp, Log, TEXT("[Bag] 已清空运行期获得的物品，背包恢复为数据表的初始状态。"));
}

bool UInventoryComponent::GetItemDefinition(FName RowName, FBagItemEntry& OutDef) const
{
    if (RowName.IsNone())
    {
        return false;
    }

    // 注意用 const 版本查找：ItemCatalog 在只读查询里不应被「顺手插入空条目」
    if (const FBagItemEntry* Found = ItemCatalog.Find(RowName))
    {
        OutDef = *Found;
        return true;
    }
    return false;
}

bool UInventoryComponent::GetDropItemDefinition(FName RowName, FBagItemEntry& OutDef) const
{
    if (RowName.IsNone())
    {
        return false;
    }

    // ★ 只查 DropCatalog（= 只有 Data_diaoluo_boss 的行）。
    //   掉落结算读 tool_num 必须走这里：用合并字典的话，行名一撞车就会读到
    //   Data_tool 那一行的数量，表现为「掉落数量莫名其妙跟着背包基础表的数字走」。
    if (const FBagItemEntry* Found = DropCatalog.Find(RowName))
    {
        OutDef = *Found;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------
// 入包：堆叠规则真正落地的地方
// ------------------------------------------------------------------

const FBagItemEntry* UInventoryComponent::ResolveItemDefinition(FName RowName, bool bDropSource) const
{
    if (RowName.IsNone())
    {
        return nullptr;
    }

    // 掉落来源：先查掉落表。行名撞车时只有这一份说得清「这一行是掉落表的哪一行」
    if (bDropSource)
    {
        if (const FBagItemEntry* DropDef = DropCatalog.Find(RowName))
        {
            return DropDef;
        }
    }

    return ItemCatalog.Find(RowName);
}

// 把 Count 个物品塞进【传入的格子数组】。
// ★ 「不可堆叠物品每次获得占一个新的空白格」这条规则完全落在这个函数里：
//   它们的有效堆叠上限是 1，上面永远找不到「还没装满」的同名格子，
//   于是每次都会走到「新建格子」分支，各自占一格。
//
// 为什么参数是「格子数组」而不是直接用 AllItems：
//   ① 实时入包传 AllItems
//   ② ReloadItems 重放传 AllItems（同一个函数 → 结果天然一致）
//   ③ 「只读预览」（DescribeAcquisition）传一份 **拷贝** → 不改变任何真实数据，
//      却仍然跑的是同一套分格算法。三处复用同一实现，不会漂移。
int32 UInventoryComponent::FillSlotsIn(TArray<FBagItemEntry>& Slots, const FBagItemEntry& Definition,
    int32 Count, int32 SlotLimit, int32& OutNewSlots, int32& OutAddedToOld, bool bVerbose) const
{
    OutNewSlots = 0;
    OutAddedToOld = 0;

    const int32 Total = FMath::Max(0, Count);
    int32 Remaining = Total;

    while (Remaining > 0)
    {
        // ---- 1) 找一个「同名、且还没装满」的格子 ----
        FBagItemEntry* Target = nullptr;
        for (FBagItemEntry& Entry : Slots)
        {
            if (Entry.RowName == Definition.RowName && Entry.ToolNum < GetEffectiveStackLimit(Entry))
            {
                Target = &Entry;
                break;
            }
        }

        bool bCreatedNewSlot = false;

        // ---- 2) 没有能装的格子 → 占用一个新的空白格 ----
        if (!Target)
        {
            if (bRejectWhenBagFull && Slots.Num() >= SlotLimit)
            {
                // 没有任何空白格了 —— 必须明确写出「丢了多少」，而不是静默吞掉。
                // ⚠️ 只在「实时入包」（bVerbose）时打：重放时背包满会每条记录都命中这里，
                //    逐条打会把日志刷爆；重放路径由 ReloadItems 汇总成一条报告。
                if (bVerbose)
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Bag] ★ 背包已满：%d 个格子全被占用，'%s' 还有 %d 个没能入包（本次共获得 %d 个）。\n"
                             "      修法（三选一）：\n"
                             "      1) 加大 InventoryComponent 的 Slot Count（当前 %d）\n"
                             "      2) 把该物品在数据表里改成可堆叠：duidie? 打勾 + max_duidie 填个上限\n"
                             "      3) 关掉 InventoryComponent 的 Reject When Bag Full"
                             "（物品仍然会进背包，但 UI 只显示前 %d 格）"),
                        Slots.Num(), *Definition.ToolName, Remaining, Total, SlotCount, SlotLimit);
                }
                break;
            }

            FBagItemEntry NewEntry = Definition;
            NewEntry.ToolNum = 0;
            NewEntry.bValid = true;
            // ★ 分配实例序号：同名（同 RowName）条目里，取已有最大 InstanceNo + 1。
            //   这样「同类多把武器」各自有稳定序号（0、1、2…），装备/占用能定位到具体哪一把。
            {
                int32 MaxInstance = -1;
                for (const FBagItemEntry& E : Slots)
                {
                    if (E.RowName == NewEntry.RowName)
                    {
                        MaxInstance = FMath::Max(MaxInstance, E.InstanceNo);
                    }
                }
                NewEntry.InstanceNo = MaxInstance + 1;
            }
            Slots.Add(NewEntry);
            Target = &Slots.Last();      // ★ Add 后必须重新取指针（可能触发数组重分配）
            bCreatedNewSlot = true;
            ++OutNewSlots;
        }

        // ---- 3) 往这个格子里塞，最多塞到有效上限 ----
        const int32 Limit = GetEffectiveStackLimit(*Target);
        const int32 Space = FMath::Max(0, Limit - Target->ToolNum);
        const int32 Put = FMath::Min(Remaining, Space);

        if (Put <= 0)
        {
            // 理论上到不了这里（上面已判过空间）。加一道防线避免死循环。
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] 内部异常：'%s' 的格子既找不到空间也无法新建（有效上限=%d，现有=%d），停止入包。"),
                *Definition.ToolName, Limit, Target->ToolNum);
            break;
        }

        Target->ToolNum += Put;
        Remaining -= Put;

        if (bCreatedNewSlot)
        {
            if (bVerbose)
            {
                UE_LOG(LogTemp, Log,
                    TEXT("[Bag]   占用新空白格 → 第 %d 格：%s ×%d（有效上限 %d%s）"),
                    Slots.Num(), *Definition.ToolName, Put, Limit,
                    Definition.bStackable ? TEXT("") : TEXT("，不可堆叠 → 一格一件"));
            }
        }
        else
        {
            OutAddedToOld += Put;
        }
    }

    return Total - Remaining;
}

// 实时入包 / 重放用的入口：直接作用于 AllItems
int32 UInventoryComponent::FillSlots(const FBagItemEntry& Definition, int32 Count,
    int32& OutNewSlots, int32& OutAddedToOld, bool bVerbose)
{
    return FillSlotsIn(AllItems, Definition, Count, FMath::Max(1, SlotCount),
        OutNewSlots, OutAddedToOld, bVerbose);
}

// 【只读预览】在 AllItems 的一份拷贝上跑同一套分格算法 —— 不改任何真实数据。
// 用途：不打架也能验证「这个物品再获得 3 个会占几格」。
FString UInventoryComponent::DescribeAcquisition(FName RowName, int32 Count) const
{
    if (RowName.IsNone() || Count <= 0)
    {
        return TEXT("参数无效：RowName 不能为空、Count 必须 > 0");
    }

    // 预览时两个字典都查（掉落表优先），再退到已有格子 —— 与入包时的解析顺序一致
    const FBagItemEntry* DropDef = DropCatalog.Find(RowName);
    const FBagItemEntry* Def = DropDef ? DropDef : ItemCatalog.Find(RowName);
    if (!Def)
    {
        Def = AllItems.FindByPredicate(
            [RowName](const FBagItemEntry& Entry) { return Entry.RowName == RowName; });
    }

    if (!Def)
    {
        return FString::Printf(
            TEXT("行名 '%s' 在 %s / %s / 背包里都找不到，无法预览"),
            *RowName.ToString(), *GetNameSafe(ToolTable), *GetNameSafe(DropItemTable));
    }

    const int32 SlotLimit = FMath::Max(1, SlotCount);
    const int32 SlotsBefore = GetSlotCountForItem(RowName);   // 真实数据，先记下来

    // ★ 拷贝上跑：真实 AllItems 一个字节都不会变
    TArray<FBagItemEntry> Simulated = AllItems;
    int32 NewSlots = 0;
    int32 AddedToOld = 0;
    const int32 Added = FillSlotsIn(Simulated, *Def, Count, SlotLimit, NewSlots, AddedToOld, /*bVerbose=*/false);

    int32 SlotsAfter = 0;
    for (const FBagItemEntry& Entry : Simulated)
    {
        if (Entry.RowName == RowName)
        {
            ++SlotsAfter;
        }
    }

    const int32 Limit = GetEffectiveStackLimit(*Def);
    FString StackDesc;
    if (!Def->bStackable)
    {
        StackDesc = TEXT("不可堆叠 → 一格一件");
    }
    else if (Def->MaxStack <= 1)
    {
        StackDesc = FString::Printf(TEXT("可堆叠·上限未填 → 视为无上限 %d"), Limit);
    }
    else
    {
        StackDesc = FString::Printf(TEXT("可堆叠·上限 %d"), Limit);
    }

    FString LostNote;
    if (Added < Count)
    {
        LostNote = FString::Printf(TEXT("　⚠️ 有 %d 个放不进（格子满 %d 格，已开启拒收）"),
            Count - Added, SlotLimit);
    }

    const FString Result = FString::Printf(
        TEXT("[预览·未改动数据] %s（行=%s）：%s；现在 %d 格 → 再获得 %d 个后 %d 格"
             "（新占 %d 格 / 叠进已有格 %d 个，共 %d 个）%s"),
        *Def->ToolName, *RowName.ToString(), *StackDesc,
        SlotsBefore, Count, SlotsAfter, NewSlots, AddedToOld, Added, *LostNote);

    // 只打 Log 不打 Warning：这是使用者主动要的诊断信息，不是异常
    UE_LOG(LogTemp, Log, TEXT("[Bag] %s"), *Result);
    return Result;
}

// ------------------------------------------------------------------
// 堆叠规则自检
// ------------------------------------------------------------------
// 把文档《背包堆叠规则.md》里的规则表变成可执行断言。
// ★ 关键：跑的是**真实的 FillSlotsIn**，只是把目标数组换成临时数组 ——
//   所以它验证的是「实现」而不是「一份文档的副本」。
void UInventoryComponent::RunStackRuleSelfTest() const
{
    struct FStackCase
    {
        const TCHAR* Name;
        bool  bStackable;
        int32 MaxStack;
        int32 PreSlots;        // 预置格子数
        int32 PrePerSlot;      // 每个预置格子里的数量
        bool  bPreOtherRow;    // 预置格用「别的物品」占位（用来测「没有空白格」）
        int32 AddCount;        // 本次获得数量
        int32 SlotLimit;       // 背包容量
        int32 ExpectRowSlots;  // 期望：该物品最终占几格
        int32 ExpectAdded;     // 期望：实际入包数量
        bool  bNeedsNewSlotRule; // 依赖 bUnstackableTakesNewSlot 开着
    };

    static const FStackCase Cases[] =
    {
        // ---- 不可堆叠：一格一件（本次需求的核心）----
        { TEXT("不可堆叠·空背包获得1"),        false, 0, 0, 0, false, 1, 40, 1, 1, true },
        { TEXT("不可堆叠·已有1格再获得1"),      false, 0, 1, 1, false, 1, 40, 2, 1, true },
        { TEXT("不可堆叠·空背包一次获得3"),     false, 0, 0, 0, false, 3, 40, 3, 3, true },

        // ---- 可堆叠：按 max_duidie 叠加，装满才溢出 ----
        { TEXT("可堆叠上限3·空背包获得7"),      true,  3, 0, 0, false, 7, 40, 3, 7, false },
        { TEXT("可堆叠上限3·已有2个再加2"),     true,  3, 1, 2, false, 2, 40, 2, 2, false },
        { TEXT("可堆叠上限3·已满3个再加1"),     true,  3, 1, 3, false, 1, 40, 2, 1, false },

        // ---- 上限没填 → 视为无上限（不拆格）----
        { TEXT("可堆叠但上限未填·不拆格"),      true,  1, 0, 0, false, 7, 40, 1, 7, false },

        // ---- 没有空白格 → 拒收，且明确丢了多少 ----
        { TEXT("背包满(SlotLimit=1)·拒收"),     false, 0, 1, 1, true,  1,  1, 0, 0, false },
    };

    const int32 CaseCount = UE_ARRAY_COUNT(Cases);
    int32 Passed = 0;
    int32 Skipped = 0;
    TArray<FString> Failures;

    for (const FStackCase& Case : Cases)
    {
        // 开关关掉时「一格一件」的用例必然不成立 —— 跳过而不是误报失败
        if (Case.bNeedsNewSlotRule && !bUnstackableTakesNewSlot)
        {
            ++Skipped;
            continue;
        }

        FBagItemEntry Definition;
        Definition.RowName = FName(TEXT("__StackSelfTestRow"));
        Definition.ToolId = Definition.RowName;
        Definition.ToolName = TEXT("自检物品");
        Definition.bStackable = Case.bStackable;
        Definition.MaxStack = Case.MaxStack;
        Definition.bValid = true;

        // 临时数组：真实数据一个字节都不动
        TArray<FBagItemEntry> Slots;
        for (int32 Index = 0; Index < Case.PreSlots; ++Index)
        {
            FBagItemEntry Pre = Definition;
            Pre.ToolNum = Case.PrePerSlot;
            if (Case.bPreOtherRow)
            {
                Pre.RowName = FName(TEXT("__StackSelfTestOther"));
            }
            Slots.Add(Pre);
        }

        int32 NewSlots = 0;
        int32 AddedToOld = 0;
        const int32 Added = FillSlotsIn(Slots, Definition, Case.AddCount, Case.SlotLimit,
            NewSlots, AddedToOld, /*bVerbose=*/false);

        int32 RowSlots = 0;
        for (const FBagItemEntry& Entry : Slots)
        {
            if (Entry.RowName == Definition.RowName)
            {
                ++RowSlots;
            }
        }

        if (RowSlots == Case.ExpectRowSlots && Added == Case.ExpectAdded)
        {
            ++Passed;
        }
        else
        {
            Failures.Add(FString::Printf(
                TEXT("%s（可堆叠=%s 上限=%d 预置=%d格×%d 获得=%d 容量=%d）\n"
                     "        期望：该物品占 %d 格 / 入包 %d 个\n"
                     "        实际：该物品占 %d 格 / 入包 %d 个"),
                Case.Name,
                Case.bStackable ? TEXT("是") : TEXT("否"), Case.MaxStack,
                Case.PreSlots, Case.PrePerSlot, Case.AddCount, Case.SlotLimit,
                Case.ExpectRowSlots, Case.ExpectAdded, RowSlots, Added));
        }
    }

    if (Failures.Num() == 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Bag] 堆叠规则自检：%d/%d 通过%s（在临时数组上跑真实分格算法，未改动背包数据）"),
            Passed, CaseCount - Skipped,
            Skipped > 0 ? *FString::Printf(TEXT("（另有 %d 条因「一格一件」开关关闭而跳过）"), Skipped)
                        : TEXT(""));
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] ★ 堆叠规则自检失败 %d/%d —— 分格算法与预期不符。\n"
                 "      这是代码问题不是数据问题，请把下面这段整段发出来：\n      %s"),
            Passed, CaseCount - Skipped,
            *FString::Join(Failures, TEXT("\n      ")));
    }
}

void UInventoryComponent::RecordAcquisition(FName RowName, int32 Count, bool bStackable, bool bDropSource)
{
    if (RowName.IsNone() || Count <= 0)
    {
        return;
    }

    // 可堆叠物品：与「上一条同名同来源」的记录合并，避免打一千次存一千条。
    // ★ 只在【相邻】时合并 —— 中间夹了别的物品就不能并，否则重放顺序会变，
    //   不可堆叠物品各自占哪一格就对不上了。
    if (bStackable && RuntimeAcquisitions.Num() > 0)
    {
        FBagAcquisition& Last = RuntimeAcquisitions.Last();
        if (Last.RowName == RowName && Last.bDropSource == bDropSource && Last.bStackable)
        {
            Last.Count += Count;
            return;
        }
    }

    FBagAcquisition Record;
    Record.RowName = RowName;
    Record.Count = Count;
    Record.bStackable = bStackable;
    Record.bDropSource = bDropSource;
    RuntimeAcquisitions.Add(Record);
}

int32 UInventoryComponent::AddItemInternal(FName RowName, int32 Count, bool bDropSource, FBagItemEntry& OutItem)
{
    if (RowName.IsNone() || Count <= 0)
    {
        return 0;
    }

    // 兜底：还没成功读过表时先读一次（正常情况下 BeginPlay 已读过）
    if (!bLoaded)
    {
        ReloadItems();
    }

    // ★ 掉落物的定义**一律以 Data_diaoluo_boss 优先**。
    //   为什么：两张表的行名可能撞车（都从 1 开始编号），
    //   那样怪物配的明明是掉落表的行，提示里却会出现 Data_tool 的名称和图标。
    const FBagItemEntry* DropDef = bDropSource ? DropCatalog.Find(RowName) : nullptr;
    const FBagItemEntry* Def = DropDef ? DropDef : ItemCatalog.Find(RowName);

    // 背包里已有的同名格子（不可堆叠物品会有多条）
    FBagItemEntry* Existing = AllItems.FindByPredicate(
        [RowName](const FBagItemEntry& Entry) { return Entry.RowName == RowName; });

    if (!Def && !Existing)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop] 行名 '%s' 在 %s 和 %s 里都不存在，本次获得被忽略（共 %d 个）。"
                 "修法：怪物蓝图 Details → Monster|Drop → Drop Items 里的 Item Row Name "
                 "是下拉框，直接从 %s 里选一行即可。"),
            *RowName.ToString(), *GetNameSafe(ToolTable), *GetNameSafe(DropItemTable), Count,
            *GetNameSafe(DropItemTable));
        return 0;
    }

    // 新建格子用的模板；表里查不到时用已有格子的信息兜底（不抹掉玩家已拿到的物品）
    FBagItemEntry Definition;
    if (Def)
    {
        Definition = *Def;
    }
    else
    {
        Definition = *Existing;
        Definition.ToolNum = 0;
    }

    // ---- 掉落表优先：把所有同名格子的展示字段刷成掉落表的定义 ----
    // 两种情况会走到这里：① 之前打过这个掉落物（刷新一遍无副作用）
    //                    ② 行名与 Data_tool 撞车（那一格原本是 Data_tool 的物品）
    if (bDropSource && DropDef && Existing)
    {
        for (FBagItemEntry& Entry : AllItems)
        {
            if (Entry.RowName != RowName)
            {
                continue;
            }

            Entry.ToolId = DropDef->ToolId;
            Entry.ToolName = DropDef->ToolName;
            Entry.ToolIntrodu = DropDef->ToolIntrodu;
            Entry.ToolImage = DropDef->ToolImage;
            Entry.KindTool = DropDef->KindTool;
            Entry.Star = DropDef->Star;
            Entry.MaxStack = DropDef->MaxStack;
            Entry.bStackable = DropDef->bStackable;
        }
    }

    // ---- 塞格子（堆叠规则在这里生效）----
    int32 NewSlots = 0;
    int32 AddedToOld = 0;
    const int32 Added = FillSlots(Definition, Count, NewSlots, AddedToOld, /*bVerbose=*/true);

    if (Added <= 0)
    {
        // 最常见的原因是背包满 —— FillSlots 里已经打印了带修法的详细说明，这里不重复
        return 0;
    }

    // ★ 先塞格子、后记账：记录的是【实际】入包的数量（背包满时可能少于 Count）
    RecordAcquisition(RowName, Added, Definition.bStackable, bDropSource);
    BonusCounts.FindOrAdd(RowName) += Added;

    // 重新排序，保持与 ReloadItems 之后的状态一致
    SortItems();

    // 输出条目：展示字段取自定义，数量 = 全部同名格子之和（与 GetItemByRow 语义一致）
    OutItem = Definition;
    OutItem.ToolNum = GetTotalCountForItem(RowName);

    // 这条日志是「取了哪个物品 / 占了几格」的第一现场：
    // 名称 + 图标名 + 新增格数都打出来，一眼就能看出取到的是掉落表还是 Data_tool 的。
    UE_LOG(LogTemp, Log,
        TEXT("[Drop] 物品入包：行=%s | 名称=%s | 图标=%s | 本次 ×%d → 新占 %d 格 / 叠入已有格 %d 个 | 现有 %d 个，共占 %d 格"),
        *RowName.ToString(), *Definition.ToolName,
        Definition.ToolImage ? *Definition.ToolImage->GetName() : TEXT("<无图标>"),
        Count, NewSlots, AddedToOld, OutItem.ToolNum, GetSlotCountForItem(RowName));

    if (ConflictingRowNames.Contains(RowName))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop] 行=%s 与 Data_tool 重名，已按「掉落表优先」刷新显示信息（名称=%s）。"
                 "★ 两件不同的物品行名相同 → 判断「这是哪个物品」时仍会混淆，"
                 "要彻底分开请给掉落表这一行改名（见 [Bag] ★ 两张数据表有… 那条日志）。"),
            *RowName.ToString(), *Definition.ToolName);
    }

    return Added;
}

int32 UInventoryComponent::AddItemByRow(FName RowName, int32 Count, FBagItemEntry& OutItem)
{
    // 奖励类物品：定义走「Data_tool 优先」的合并字典
    return AddItemInternal(RowName, Count, /*bDropSource=*/false, OutItem);
}

int32 UInventoryComponent::AddDropItemByRow(FName RowName, int32 Count, FBagItemEntry& OutItem)
{
    // 掉落物：定义以 Data_diaoluo_boss 为准（行名撞车时提示里显示的才是打出来的那件）
    return AddItemInternal(RowName, Count, /*bDropSource=*/true, OutItem);
}

void UInventoryComponent::SortItems()
{
    // 默认排序：品质从高到低；同品质按分类升序，再按行名。
    //
    // ★ 末尾必须再加一层「按数量降序」的兜底比较：
    //   不可堆叠物品会有【多个完全同名】的格子，而 TArray::Sort 不是稳定排序 ——
    //   少了这一层，同名格子之间的先后顺序在每次排序后都可能变（表现为「打开一次背包，
    //   格子的排列顺序就换了一次」）。有了它，整格（满的）排在前面、零头排最后，结果固定。
    AllItems.Sort([](const FBagItemEntry& A, const FBagItemEntry& B)
    {
        if (A.Star != B.Star)
        {
            return A.Star > B.Star;
        }
        if (A.KindTool != B.KindTool)
        {
            return A.KindTool < B.KindTool;
        }
        if (A.RowName != B.RowName)
        {
            return A.RowName.LexicalLess(B.RowName);
        }
        return A.ToolNum > B.ToolNum;
    });
}

TArray<FBagItemEntry> UInventoryComponent::GetItemsForKind(int32 Kind) const
{
    if (Kind < 0)
    {
        return AllItems;
    }

    TArray<FBagItemEntry> Result;
    for (const FBagItemEntry& Entry : AllItems)
    {
        if (Entry.KindTool == Kind)
        {
            Result.Add(Entry);
        }
    }
    return Result;
}

int32 UInventoryComponent::GetOwnedCountForKind(int32 Kind) const
{
    int32 Total = 0;
    for (const FBagItemEntry& Entry : AllItems)
    {
        if (Kind < 0 || Entry.KindTool == Kind)
        {
            Total += FMath::Max(0, Entry.ToolNum);
        }
    }
    return Total;
}

TArray<FText> UInventoryComponent::GetKindDisplayNames() const
{
    TArray<FText> Names;
    Names.Reserve(KindCount);

    const UEnum* KindEnum = KindEnumPath.IsEmpty()
        ? nullptr
        : LoadObject<UEnum>(nullptr, *KindEnumPath);

    for (int32 Index = 0; Index < KindCount; ++Index)
    {
        FText Name;

        // 1) 蓝图里显式配置的覆盖名
        if (KindNameOverrides.IsValidIndex(Index) && !KindNameOverrides[Index].IsEmpty())
        {
            Name = KindNameOverrides[Index];
        }
        else if (KindEnum)
        {
            // 2) 读 E_kind_tool 枚举的显示名（在枚举编辑器里改名即生效）
            const FText EnumDisplayName = KindEnum->GetDisplayNameTextByValue(Index);
            const FString Raw = EnumDisplayName.ToString();
            if (!Raw.IsEmpty() && !Raw.StartsWith(TEXT("NewEnumerator")))
            {
                Name = EnumDisplayName;
            }
        }

        // 3) 兜底
        if (Name.IsEmpty())
        {
            Name = FText::FromString(FString::Printf(TEXT("Category %d"), Index + 1));
        }

        Names.Add(Name);
    }

    return Names;
}

int32 UInventoryComponent::GetStarLevel(int32 RawStar) const
{
    const int32 Level = bStarIsZeroBased ? (RawStar + 1) : RawStar;
    return FMath::Clamp(Level, 1, 5);
}

FText UInventoryComponent::GetStarText(int32 RawStar) const
{
    return FText::FromString(FString::Printf(TEXT("%d\u2605"), GetStarLevel(RawStar)));
}

FLinearColor UInventoryComponent::GetStarColor(int32 RawStar) const
{
    const int32 ColorIndex = GetStarLevel(RawStar) - 1;
    if (StarColors.IsValidIndex(ColorIndex))
    {
        return StarColors[ColorIndex];
    }
    return FLinearColor::White;
}

int32 UInventoryComponent::GetCapacityForKind(int32 Kind) const
{
    if (Kind < 0)
    {
        // 「全部」视图：把各分类容量求和
        int32 Total = 0;
        for (int32 Index = 0; Index < KindCount; ++Index)
        {
            Total += GetCapacityForKind(Index);
        }
        return Total > 0 ? Total : DefaultCategoryCapacity;
    }

    if (CategoryCapacities.IsValidIndex(Kind) && CategoryCapacities[Kind] > 0)
    {
        return CategoryCapacities[Kind];
    }
    return DefaultCategoryCapacity;
}
