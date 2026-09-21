// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BagTypes.h"
#include "InventoryComponent.generated.h"

class UDataTable;

/**
 * 背包数据组件（挂在 ABattleCharacter 上）。
 *
 * 数据源：数据表 Data_tool（/Game/UI/bag_sys），行结构为蓝图结构体 Stru_bag_attribute。
 * 由于蓝图结构体无法作为 C++ 原生类型使用，本组件在导入时通过 UE 反射逐行读取字段，
 * 转成原生结构 FBagItemEntry。因此：
 *   - 不需要改动已有的 E_kind_tool / E_star / Stru_bag_attribute / Data_tool 资产
 *   - 数据表里有多少行，背包里就有多少种物品（tool_num 即为拥有数量）
 */
UCLASS(ClassGroup = (WutheringWaves), meta = (BlueprintSpawnableComponent))
class WUTHERINGWAVES_API UInventoryComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UInventoryComponent();

    // ------------------------------------------------------------------
    // 数据源
    // ------------------------------------------------------------------

    /** 物品数据表。留空时按 ToolTablePath 在运行时懒加载（默认指向 Data_tool）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Data")
    TObjectPtr<UDataTable> ToolTable = nullptr;

    /** 数据表资产路径（ToolTable 为空时按此路径懒加载） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Data")
    FString ToolTablePath = TEXT("/Game/UI/bag_sys/Data_tool.Data_tool");

    /**
     * 掉落物数据表（默认 Data_diaoluo_boss）。
     *
     * 行结构与 Data_tool 完全相同（Stru_bag_attribute），但它装的是「掉落物」这一类物品。
     * 它不需要出现在背包的基础物品列表里 —— 只有玩家真正打出来过，才会被补进背包。
     * 这解决了「掉落物不在 Data_tool 里 → 打到了也看不见」的问题。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Data")
    TObjectPtr<UDataTable> DropItemTable = nullptr;

    /** 掉落物数据表路径（DropItemTable 为空时按此路径懒加载） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Data")
    FString DropItemTablePath = TEXT("/Game/UI/bag_sys/Data_diaoluo_boss.Data_diaoluo_boss");

    /** 分类枚举资产路径（用于读取分类显示名） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Data")
    FString KindEnumPath = TEXT("/Game/UI/bag_sys/E_kind_tool.E_kind_tool");

    /** star 字段是否为「从 0 开始的枚举值」（E_star 共 5 项 → 0~4 对应 1~5 星）。
     *  若你直接在数据表里填 1~5，请取消勾选。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Data")
    bool bStarIsZeroBased = true;

    // ------------------------------------------------------------------
    // 网格布局
    // ------------------------------------------------------------------

    /** 网格列数（鸣潮背包为 8 列） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Grid", meta = (ClampMin = "1"))
    int32 GridColumns = 8;

    /** 格子总数：没有物品的格子也会存在（空槽占位） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Grid", meta = (ClampMin = "1"))
    int32 SlotCount = 40;

    /** 单个格子的边长（像素）。
     *  C++ 会在每个格子外面套一层 SizeBox 强制成 这个尺寸 —— 因为 UniformGridPanel 的
     *  格宽/格高是由子控件的「期望尺寸」撑开的，如果 WBP_Bag_Slot 根节点没有固定尺寸，
     *  所有格子会一起塌成 0×0，表现就是「背包打开了但一个格子都没有」。
     *  这里设成你 WBP_Bag_Slot 的设计尺寸即可（默认 90 = 手册里的推荐值）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Grid", meta = (ClampMin = "8"))
    float CellSize = 90.f;

    /** 分类数量（对应 E_kind_tool 的枚举项数） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Grid", meta = (ClampMin = "1"))
    int32 KindCount = 6;

    /** 每个分类的容量上限（顶栏「拥有数/上限」用）。数量不足时按 DefaultCategoryCapacity 补齐。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Grid")
    TArray<int32> CategoryCapacities;

    /** 未单独配置容量时的默认分类容量 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Grid", meta = (ClampMin = "1"))
    int32 DefaultCategoryCapacity = 2000;

    // ------------------------------------------------------------------
    // 堆叠规则
    // ------------------------------------------------------------------

    /**
     * ★ 不可堆叠物品「一格一件」。
     *
     * 打开（默认）= 数据表 duidie? 没打勾的物品，每次获得都占一个**新的空白格**：
     *   已有 1 格「龙鳞 ×1」，再打到 3 个 → 变成 4 格，每格 1 个。
     * 关闭 = 退回旧行为：所有物品都往同一个格子里累加，不拆格。
     *
     * 可堆叠物品（duidie? 打勾）不受本开关影响，永远按 max_duidie 上限叠加，
     * 装满了才溢出到下一个空格子。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Stack")
    bool bUnstackableTakesNewSlot = true;

    /**
     * 可堆叠物品，但数据表的 max_duidie 没填（<=1）时，视为「无上限」= 这个值。
     *
     * 为什么不能直接当成 1：那样「duidie? 打勾了但忘了填上限」会静默变成
     * 一格一件 —— 使用者只是少填一个数字，表现却是背包被同一个物品塞满，
     * 极难联想到原因。这里取一个足够大的数 + 启动时打警告，暴露问题但不破坏玩法。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Stack", meta = (ClampMin = "1"))
    int32 UnsetStackLimit = 9999;

    /**
     * 格子用满后，新物品是否拒收（默认拒收）。
     *
     * 开启 = 没有空白格就放不下，多出来的部分会被丢掉并在日志里明确写出丢了多少；
     * 关闭 = 允许物品数超过格子数（UI 上超出 Slot Count 的部分不显示）。
     *
     * 不可堆叠物品会快速吃掉格子，所以这个上限必须存在，否则「背包满」这件事
     * 在界面上没有任何反馈。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Stack")
    bool bRejectWhenBagFull = true;

    // ------------------------------------------------------------------
    // 视觉配置
    // ------------------------------------------------------------------

    /** 品质 1~5 星对应的描边/底色（默认 灰 / 绿 / 蓝 / 紫 / 金） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Visual")
    TArray<FLinearColor> StarColors;

    /** 空槽位底色（深色半透明，保证在深色背包底上仍能看出格位） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Visual")
    FLinearColor EmptySlotColor = FLinearColor(0.20f, 0.21f, 0.25f, 0.90f);

    /** 选中格子的高亮色（作用在 WBP_Bag_Slot 的 img_slot_select 上） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Visual")
    FLinearColor SlotSelectColor = FLinearColor(0.90f, 0.75f, 0.38f, 0.55f);

    /** 分类按钮「选中」底色（金色高亮） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Visual")
    FLinearColor KindSelectedColor = FLinearColor(0.90f, 0.75f, 0.38f, 1.0f);

    /** 分类按钮「未选中」底色。
     *  注意 UButton::SetBackgroundColor 是「乘算染色」，按钮默认刷子是浅灰色，
     *  因此这里必须给暗色，否则未选中的分类按钮会变成刺眼的亮灰色块。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Visual")
    FLinearColor KindNormalColor = FLinearColor(0.16f, 0.17f, 0.21f, 0.85f);

    /** 分类显示名覆盖（留空则读取 E_kind_tool 枚举的显示名）。
     *  数量不足或为空时，回退到「枚举显示名」→「Category N」 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Visual")
    TArray<FText> KindNameOverrides;

    // ------------------------------------------------------------------
    // 运行时数据
    // ------------------------------------------------------------------

    /**
     * 背包里【当前占用】的全部格子（按品质从高到低排序）。
     *
     * ★ 一个 RowName 可能出现多条 —— 这是因为不可堆叠物品「一格一件」：
     *   同名物品的每一条各自占一个格子。UI 是按下标逐格画的，
     *   所以「多条同名条目」天然就是「多个格子」，不需要额外的槽位数组。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TArray<FBagItemEntry> AllItems;

    /** 是否已成功读取过数据表 */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    bool bLoaded = false;

    /**
     * 运行期获得的【逐次记录】（掉落 / 任务奖励），顺序 = 获得的先后。
     *
     * 这是「运行期拿到了什么」的唯一真相来源。ReloadItems() 重建物品列表后，
     * 会按这里的顺序**逐条重放**一遍入包逻辑 —— 只有逐条重放才能复原
     * 「不可堆叠物品每次获得各占一格」的结果；只存一个总数是复原不出来的。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TArray<FBagAcquisition> RuntimeAcquisitions;

    /**
     * 运行时额外获得的数量汇总：数据表行名 → 总数（由 RuntimeAcquisitions 派生）。
     *
     * 为什么必须单独存一层：数据表里的 tool_num 是「基础值」，而 ReloadItems()
     * 会把 AllItems 整个重建回基础值 —— 而每次打开背包都会走一遍 ReloadItems。
     * 如果掉落的数量直接写进 AllItems[].ToolNum，下一次打开背包就凭空消失了。
     *
     * 注意它只是【汇总视图】（蓝图里查「这个物品我一共打到了多少」很方便），
     * 真正用于重建的是 RuntimeAcquisitions。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TMap<FName, int32> BonusCounts;

    /**
     * 物品定义索引：行名 → 物品定义（Data_tool 为主，掉落表只补空缺）。
     *
     * 与 AllItems 的区别：AllItems 是「玩家现在拥有的东西」（会随掉落增删），
     * ItemCatalog 是「这世上存在哪些物品」的只读字典。掉落时先在 AllItems 里找，
     * 找不到就查这里 —— 查到说明是掉落物专有物品，用它补一条进 AllItems。
     *
     * ⚠️ 两张表行名撞车时这里是【先写先赢】（Data_tool 优先），
     *    所以破坏性覆盖不会发生；但真正的「这一行到底是谁」请用 DropCatalog 判断。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TMap<FName, FBagItemEntry> ItemCatalog;

    /**
     * 只属于 **Data_tool**（背包物品表）的行名集合 —— 掉落表的行**不在**这里。
     *
     * ★ 它解决的是一个具体的错位：两套行名体系是各自独立的（Data_tool 用 `1`~`5`，
     *   掉落表用 `100`/`101`）。而「武器蓝图声明的 Bag Item Row」「装备中的背包行」
     *   这类字段，语义上只可能指向【玩家背包里的物品】—— 也就是 Data_tool 的行。
     *   一旦有人把掉落表的行名填进武器蓝图（很容易发生：那两个行名也是数字），
     *   就会出现「拿 `100` 去找背包物品」的错位：找不到该找的东西，却找到了掉落物，
     *   于是掉落的物品那一格被标成「装备中」、武器页显示它的名字和图标。
     *   有了这个集合，「这一行到底属不属于背包表」就是一个可直接回答的问题。
     *
     * 只在 ReloadItems() 里填；表未加载时为空 —— 此时查询方应【不作否决】
     * （见 IsToolTableRow 的宽松规则）。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TSet<FName> ToolTableRowNames;

    /**
     * 掉落物定义索引：只装 Data_diaoluo_boss 的行，**不与 Data_tool 合并**。
     *
     * 为什么要单独一份：DataTable 的行名不是全局唯一的 —— 两张表各自从 `1` 开始
     * 编号是常态（新建行的默认名就是 1、2、3…）。如果只靠 ItemCatalog 这个
     * 「合并字典」去认物品，行名一撞车就只能认出一张表的那一行，
     * 表现就是「怪物配的是掉落表的行，打出来却在提示里显示 Data_tool 的
     * 名称和图标，数量也加到了 Data_tool 那件物品上」。
     *
     * 所以规则改成：**凡是从掉落表来的行，定义一律以 DropCatalog 为准。**
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TMap<FName, FBagItemEntry> DropCatalog;

    /**
     * 行名冲突清单：Data_tool 与 Data_diaoluo_boss 里同名的行。
     *
     * 非空 = 数据有问题，ReloadItems() 会打一条 ★ 级警告并在里面给出修法。
     * 为什么不自动改：行名是跨表的物品唯一标识，改哪一边是设计决定，
     * 代码只能把事实说清楚（改错了会把玩家已有的物品对错行）。
     */
    UPROPERTY(BlueprintReadOnly, Category = "Bag|Runtime")
    TArray<FName> ConflictingRowNames;

    // ------------------------------------------------------------------
    // API
    // ------------------------------------------------------------------

    /**
     * 往背包里添加物品（掉落 / 任务奖励等运行期获得）。
     *
     * 走的是「Data_tool 优先」的老规则，适合奖励类物品。
     * **掉落物请用 AddDropItemByRow** —— 行名撞车时它会以掉落表的定义为准。
     *
     * @param RowName  数据表行名
     * @param Count    要添加的数量（<=0 直接返回 0）
     * @param OutItem  输出：展示字段为物品定义，ToolNum = 该物品【全部格子之和】；
     *                 失败时不修改。提示 UI 只用它读名称/图标，数量请用「本次 ×N」那个入参。
     * @return 实际添加的数量；0 = 失败（行名不存在 / 参数非法 / 数据表未加载）
     */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    int32 AddItemByRow(FName RowName, int32 Count, FBagItemEntry& OutItem);

    /**
     * 掉落物专用入口：定义**一律以 Data_diaoluo_boss 为准**。
     *
     * 与 AddItemByRow 的唯一区别：行名若在两表里都存在（撞车），
     * 这里会用掉落表的名称/图标/分类/品质覆盖背包里那一条，
     * 保证「打出来的」和「提示里显示的」是同一件东西。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    int32 AddDropItemByRow(FName RowName, int32 Count, FBagItemEntry& OutItem);

    /**
     * 按行名查询物品条目。
     *
     * ★ 不可堆叠物品会出现多个同名格子，此时：
     *   OutItem  = 第一个格子（名称/图标/品质等展示字段都一样）
     *   ToolNum  = 所有同名格子的数量之和（所以它仍然等于「这个物品我有几个」）
     *   想知道占了几格用 GetSlotCountForItem()。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    bool GetItemByRow(FName RowName, FBagItemEntry& OutItem) const;

    /**
     * 这一行是不是【背包物品表（Data_tool）】的行？
     *
     * 为什么要问这个：行名只在**同一张表内**唯一。Data_tool 用 `1`~`5`，掉落表用
     * `100`/`101` —— 两套编号互不相干。而「武器蓝图声明的背包行」「当前装备的行号」
     * 这些字段天然只指向背包里的物品，所以拿到一个行名时必须能回答「它属于哪张表」，
     * 否则会出现「用掉落表的行名去背包里找物品」的错位（找到的是那件掉落物）。
     *
     * ★ 宽松规则：表还没加载（bLoaded=false 或集合为空）时返回 **true**。
     *   理由：这个函数只用来【否决】明显不属于背包表的行，不能反过来把「还不知道」
     *   当成「不属于」—— 否则数据未加载时会连正常武器都装备不上（比原 bug 更严重）。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    bool IsToolTableRow(FName RowName) const;

    /**
     * 查询物品【定义】（Data_tool 与 Data_diaoluo_boss 的合并字典）。
     *
     * 与 GetItemByRow 的区别：这个不关心玩家是否拥有，只要表里有就能查到。
     * 掉落结算用它读「这一行在表里写的 tool_num / 名称 / 图标」——
     * 所以数量只需要在数据表里维护一处，怪物侧默认不用重复填。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    bool GetItemDefinition(FName RowName, FBagItemEntry& OutDef) const;

    /**
     * 查询【掉落物】定义（只查 Data_diaoluo_boss）。
     *
     * 掉落结算读 tool_num 时必须用这个，不能用 GetItemDefinition ——
     * 后者在行名撞车时会返回 Data_tool 那一行的数量。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    bool GetDropItemDefinition(FName RowName, FBagItemEntry& OutDef) const;

    /** 重新读取数据表并重建 AllItems（按品质降序排序，并重放运行期获得记录） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    void ReloadItems();

    /**
     * 这件物品的【有效堆叠上限】—— 一格最多能放几个。
     *
     *   不可堆叠（duidie? 未勾选）        → 1（即一格一件）
     *   可堆叠 + 填了 max_duidie          → 那个数
     *   可堆叠 + max_duidie 没填（<=1）   → UnsetStackLimit（默认 9999，并会打警告）
     *   bUnstackableTakesNewSlot 关掉     → 无上限（退回「永远叠加」的旧行为）
     */
    UFUNCTION(BlueprintCallable, Category = "Bag|Stack")
    int32 GetEffectiveStackLimit(const FBagItemEntry& Item) const;

    /** 该物品当前占了几个格子（不可堆叠物品 = 件数） */
    UFUNCTION(BlueprintCallable, Category = "Bag|Stack")
    int32 GetSlotCountForItem(FName RowName) const;

    /** 该物品的总数量（把所有同名格子加起来） */
    UFUNCTION(BlueprintCallable, Category = "Bag|Stack")
    int32 GetTotalCountForItem(FName RowName) const;

    /**
     * 清空运行期获得记录 + 把背包恢复成数据表的初始状态（调试 / 读档时用）。
     * 注意：这会把掉落物专有物品的格子一并清掉（它们本来就不是表里的初始内容）。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag|Stack")
    void ClearRuntimeItems();

    /**
     * 【只读预览】「现在再获得 RowName ×Count 会怎么分格」——**不改动任何真实数据**。
     *
     * 在 AllItems 的一份拷贝上跑同一套分格算法，所以结果必然与真的去获得一致。
     * 用途：不用打死怪就能验证堆叠规则（在蓝图里 Print String 出来，或看 `[Bag] [预览…]` 日志）。
     *
     * 例：某物品「现在 1 格 → 再获得 3 个后 4 格（新占 3 格 / 叠进已有格 0 个，共 3 个）」
     *     —— 说明它是不可堆叠的（一格一件）；若显示「新占 0 格 / 叠进已有格 3 个」则是可堆叠。
     */
    UFUNCTION(BlueprintCallable, Category = "Bag|Stack")
    FString DescribeAcquisition(FName RowName, int32 Count) const;

    /**
     * 启动时跑一遍堆叠规则自检（在临时数组上跑**真实的** FillSlotsIn，不动背包数据）。
     *
     * 为什么需要：分格规则是本系统最容易「改着改着就悄悄错了」的一块 ——
     * 错的表现是「某件物品该占 3 格却占了 1 格」，而它在界面上很难一眼看出。
     * 这里把文档里的规则表变成可执行断言，只打一行结论，失败才逐条列出「期望 vs 实际」。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|Stack")
    bool bRunStackRuleSelfTest = true;

    /** 取指定分类的物品（Kind < 0 表示不过滤 = 全部） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    TArray<FBagItemEntry> GetItemsForKind(int32 Kind) const;

    /** 指定分类的物品总数量（tool_num 求和） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    int32 GetOwnedCountForKind(int32 Kind) const;

    /** 分类显示名列表（长度 = KindCount） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    TArray<FText> GetKindDisplayNames() const;

    /** 品质对应颜色（传入原始 star 字段值） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    FLinearColor GetStarColor(int32 RawStar) const;

    /** 品质文本，例如 "5★"（传入原始 star 字段值） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    FText GetStarText(int32 RawStar) const;

    /** 品质等级 1~5（传入原始 star 字段值，已做 0 基/1 基换算与钳制） */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    int32 GetStarLevel(int32 RawStar) const;

    /** 分类容量上限 */
    UFUNCTION(BlueprintCallable, Category = "Bag")
    int32 GetCapacityForKind(int32 Kind) const;

protected:
    virtual void BeginPlay() override;

    /** 按品质从高到低排序（同品质按分类、行名稳定排序；同名格子按数量降序保证可复现） */
    void SortItems();

    /**
     * 入包的公共实现。
     *
     * @param bDropSource  true = 定义以掉落表（Data_diaoluo_boss）优先，行名撞车时用掉落表那行的名称/图标
     */
    int32 AddItemInternal(FName RowName, int32 Count, bool bDropSource, FBagItemEntry& OutItem);

    /** 解析物品定义：bDropSource 时先查 DropCatalog，再回落到合并字典 */
    const FBagItemEntry* ResolveItemDefinition(FName RowName, bool bDropSource) const;

    /**
     * 把 Count 个物品【塞进格子】—— 堆叠规则真正落地的地方。
     *
     * 规则：先找「同名且还没装满」的格子；找不到就占用一个新的空白格。
     *   不可堆叠 → 有效上限 1 → 每个格子只放 1 个 → 每次获得都开新格
     *   可堆叠   → 按上限叠加，装满才溢出到下一个格子
     *
     * @param OutNewSlots     输出：本次新占了几个空白格
     * @param OutAddedToOld   输出：本次有几个是叠进已有格子的
     * @param bVerbose        是否打印逐格日志（ReloadItems 重放时传 false，避免刷屏）
     * @return 实际塞进去的数量（背包满时可能小于 Count）
     */
    int32 FillSlots(const FBagItemEntry& Definition, int32 Count,
        int32& OutNewSlots, int32& OutAddedToOld, bool bVerbose);

    /**
     * 在【指定数组】上执行分格 —— FillSlots 的通用版。
     *
     * 三处复用同一实现，保证不会漂移：
     *   ① 实时入包 → 传 AllItems
     *   ② ReloadItems 重放 → 传 AllItems
     *   ③ 只读预览 DescribeAcquisition → 传 AllItems 的**拷贝**（不改真实数据）
     *
     * @param Slots     目标格子数组；SlotLimit 是它的容量上限（背包为 SlotCount）
     */
    int32 FillSlotsIn(TArray<FBagItemEntry>& Slots, const FBagItemEntry& Definition, int32 Count,
        int32 SlotLimit, int32& OutNewSlots, int32& OutAddedToOld, bool bVerbose) const;

    /** 堆叠规则自检的实现（见 bRunStackRuleSelfTest 的说明） */
    void RunStackRuleSelfTest() const;

    /**
     * 记一条运行期获得记录（ReloadItems 靠它重建）。
     * 可堆叠物品会和「上一条同名记录」合并 —— 否则打到一万个就存一万条。
     */
    void RecordAcquisition(FName RowName, int32 Count, bool bStackable, bool bDropSource);

    /** 确保 ToolTable 有效（必要时按路径懒加载） */
    bool EnsureToolTable();

    /** 确保 DropItemTable 有效（必要时按路径懒加载；找不到不算致命，只是掉落物没有定义） */
    bool EnsureDropItemTable();

    /**
     * 把数据表的一行读成 FBagItemEntry。
     *
     * 字段名用「前缀匹配」而非全等 —— 蓝图 UserDefinedStruct 的成员属性名实际是
     * 「<字段名>_<序号>_<GUID>」（例如 tool_id_2_E01378974A6CF28897C70689610CE8F9）。
     *
     * @param RowStruct      行结构体（UScriptStruct）
     * @param RowData        行数据指针（FindRowUnchecked 的返回值，非 const 因为它读 TObjectPtr）
     * @param RowName        行名，写进 OutEntry.RowName
     * @param OutFieldTypes  可选：按 tool_id/tool_num/... 顺序输出各字段的真实 C++ 类型，
     *                       空字符串表示该字段没找到（排查「表填了但读不到」用）
     */
    bool ReadRowEntry(const UScriptStruct* RowStruct, uint8* RowData, FName RowName,
        FBagItemEntry& OutEntry, TArray<FString>* OutFieldTypes = nullptr) const;
};
