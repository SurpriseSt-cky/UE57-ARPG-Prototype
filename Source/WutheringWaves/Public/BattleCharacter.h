#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "InputActionValue.h"
#include "BagTypes.h"
#include "WeaponBase.h"
#include "CharaInfo.h"
#include "BattleCharacter.generated.h"

class UAbilitySystemComponent;
class UUserWidget;
class UImage;
class UTextBlock;
class UInputAction;
class UInputMappingContext;
class UBattleAttributeSet;
class UInventoryComponent;
class UPanelWidget;
class UDamageGameplayEffect;
class UUltimateAbility;
class UInvincibilityGameplayEffect;
class UDodgeAbility;
class UPoisonGameplayEffect;
class UDamageBoostGameplayEffect;
class UInputMappingContext;
class UInputAction;
class USpringArmComponent;
class UCameraComponent;
class AMonsterBase;
class UUserWidget;
class UTextBlock;
class UProgressBar;
class UWidget;
class UAnimMontage;
class UMaterialInterface;
class AGhostAfterimage;

// ---- 多段蒙太奇技能/大招的"段"配置 ----
// 技能（E）与大招（R）都由若干段动画蒙太奇构成，按数组顺序播放一次后结束。
// 每段可独立配置：播放速率、有效攻击时间段（蒙太奇时间轴秒）、接触伤害、
// 伤害重复间隔、是否可被闪避打断（仅技能生效，大招恒不可打断）
USTRUCT(BlueprintType)
struct FComboMontageSegment
{
    GENERATED_BODY()

    // 该段蒙太奇（留空则该段被跳过）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment")
    TObjectPtr<UAnimMontage> Montage = nullptr;

    // 该段播放速率（1=原速，2=两倍速）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment", meta = (ClampMin = "0.05"))
    float PlayRate = 1.0f;

    // 有效攻击时间段起始（蒙太奇时间轴秒，蒙太奇编辑器里看到的时间，与 PlayRate 无关）。
    // 只有播放位置进入 [起始, 结束] 区间内，与怪物模型接触才造成伤害。-1 = 0（从头开始）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment", meta = (ClampMin = "-1.0"))
    float AttackWindowStartTime = -1.0f;

    // 有效攻击时间段结束（蒙太奇时间轴秒）。-1 = 蒙太奇全长
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment", meta = (ClampMin = "-1.0"))
    float AttackWindowEndTime = -1.0f;

    // 技能倍率：攻击时间段内角色与怪物模型接触时，每次结算造成的伤害倍率。
    // 规整叫法：原「技能伤害(DamagePerHit)」统一改为「技能倍率(Multiplier)」，
    // 后续伤害结算将基于该倍率（当前默认 1.0 表示 1 倍基准，数值含义待接入攻击力基数）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment", meta = (ClampMin = "0.0"))
    float Multiplier = 1.0f;

    // 对同一怪物两次伤害的最小间隔（秒）。持续接触时按该间隔重复结算。
    // 0 = 该段内每个怪物只结算一次伤害
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment", meta = (ClampMin = "0.0"))
    float HitInterval = 0.5f;

    // 该段是否可被闪避打断（仅技能生效：技能播放该段期间按闪避 → 打断技能并进入冷却；
    // 大招忽略此标志，任何段都不可被打断）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment")
    bool bDodgeInterruptible = true;
};

/**
 * 普攻连段中的一段。
 *
 * ★★ 为什么单独定义、不复用上面的 FComboMontageSegment：
 *    那个结构体是给技能 / 大招 / 能量技用的「接触伤害段」——带技能倍率、命中间隔、
 *    攻击时间段这些字段，它们由 C++ 的 ApplySegmentContactDamage 逐帧结算。
 *    而普攻的伤害走的是武器侧（AWeaponBase::BeginAttack → 球形扫掠 / 发射弹丸），
 *    上面那些字段在普攻段里【填了也不会生效】。
 *    共用会带来两个后果：① 蓝图面板里出现一排「填了没用」的字段；
 *    ② 让人误以为「普攻也能按段配倍率」，于是照着填、然后发现没效果再来查一轮。
 *    → 语义不同就分开定义，各自只暴露真正生效的字段。
 *
 * ★ 普攻连段真正需要按段配的只有两件事：播哪段蒙太奇、多久之后能被下一段接上。
 */
USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FAttackComboSegment
{
    GENERATED_BODY()

    // 该段的普攻蒙太奇。留空 = 该段没有动画，但【仍然占一个段号】（段数始终 = 数组元素个数）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment")
    TObjectPtr<UAnimMontage> Montage = nullptr;

    // 该段的可打断窗口（蒙太奇时间轴秒）：播放进度 >= 此值后，才允许被【下一段普攻】打断衔接。
    // 值越大越"滞后"（接近播完才能接），越小越"跟手"。
    // -1 = 立即可打断（旧行为：普攻之间不受动画进度限制，只受 AttackCooldown 约束）。
    // ★ 只约束「普攻打断普攻」；闪避 / 技能打断普攻不受它限制（那走 bDodgeInterruptible 体系）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segment", meta = (ClampMin = "-1.0"))
    float CancelWindowTime = -1.0f;
};

UCLASS()
class WUTHERINGWAVES_API ABattleCharacter : public ACharacter, public IAbilitySystemInterface
{
    GENERATED_BODY()

public:
    ABattleCharacter();

    // ---- IAbilitySystemInterface ----
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
    virtual void Landed(const FHitResult& Hit) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // ---- 组件 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UAbilitySystemComponent* AbilitySystemComponent;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UBattleAttributeSet> AttributeSet;

    // ---- 输入 ----
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputMappingContext* DefaultMappingContext;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* LightAttackAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* DodgeAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* SkillAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* UltimateAction;

    // 能量技（Q 键：4 格能量满后释放。需在蓝图里创建对应 IA 并在 IMC_Player 绑定 Q 键）
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* EnergySkillAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* JumpAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* ResetCameraAction;

    // 疾跑（右键）：移动中点击触发，持续耗耐力直到耗尽
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* SprintAction;

    // 角色面板（C 键）：打开/关闭角色面板 UI（WBP_Character_imf），需在蓝图里创建对应 IA 并在 IMC_Player 绑定 C 键
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
    UInputAction* CharacterPanelAction;

    // ---- 背包（B 键）：输入动作/映射由 C++ 运行时创建并挂载，无需在编辑器配置 ----
    // 关键：bTriggerWhenPaused = true，使背包打开（游戏已暂停）后仍能用 B 关闭。
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "Input")
    UInputAction* BagAction;

    UPROPERTY(Transient, VisibleInstanceOnly, Category = "Input")
    UInputMappingContext* BagMappingContext;

    // 背包 B 键映射是否已加入本地子系统（防重复挂载）
    bool bBagContextAdded = false;

    // ---- 角色编队（L 键）：输入动作/映射由 C++ 运行时创建并挂载，无需在编辑器配置 ----
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "Input")
    UInputAction* CharaTeamAction;

    UPROPERTY(Transient, VisibleInstanceOnly, Category = "Input")
    UInputMappingContext* CharaTeamMappingContext;

    // 编队 L 键映射是否已加入本地子系统（防重复挂载）
    bool bCharaTeamContextAdded = false;

    // ---- 步行切换（Ctrl）：输入动作/映射由 C++ 运行时创建并挂载，无需在编辑器配置 ----
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "Input")
    UInputAction* WalkToggleAction;

    UPROPERTY(Transient, VisibleInstanceOnly, Category = "Input")
    UInputMappingContext* WalkToggleMappingContext;

    // 步行 Ctrl 映射是否已加入本地子系统（防重复挂载）
    bool bWalkContextAdded = false;

public:
    // ---- 输入处理函数（public：供 GameplayAbility 外壳及输入绑定调用）----
    void LightAttack();
    void Dodge();
    void Skill();
    void Ultimate();
    void EnergySkill();
    void PerformJump();
    void PerformDoubleJump();
    void StartSprint();
    void StopSprint();
    void ToggleWalkState();
    bool EnsureWalkToggleInput();

    // ---- 角色面板（C 键）----
    // 切换角色面板开关：打开→关闭、关闭→打开（由 C 键输入调用）
    void ToggleCharacterPanel();
    // 打开角色面板：暂停游戏 + 显示鼠标（UI Only 输入模式）
    void OpenCharacterPanel();
    // 关闭角色面板：恢复游戏 + 隐藏鼠标（恢复 Game 输入模式）。
    // BlueprintCallable：供 WBP_Character_imf 的 Esc_butt 按钮 OnClicked 调用
    UFUNCTION(BlueprintCallable, Category = "CharacterPanel")
    void CloseCharacterPanel();

    // 打开角色面板时，把实时属性（攻击力/最大血量/暴击率/暴击伤害）写入 WBP_Character_imf 的四个文本。
    // 在 OpenCharacterPanel() 创建并 AddToViewport 面板后调用。
    void UpdateCharacterPanelStats();

    // ---- 角色面板 Tab 切换（mod_butt 内四个按钮）----
    // 角色面板子页签类型：change_chara(角色)/change_arm(武器)/change_yiqi(装备)/change_mingzuo(命座)
    enum class ECharacterPanelTab : uint8
    {
        Chara   = 0,  // change_chara —— 角色面板展示
        Arm     = 1,  // change_arm   —— 角色武器展示
        Yiqi    = 2,  // change_yiqi  —— 角色装备展示
        Mingzuo = 3   // change_mingzuo —— 命座展示
    };

    // 绑定角色面板内 mod_butt 四个按钮的 OnClicked 到 C++ 处理函数。
    // 在 OpenCharacterPanel() 创建并 AddToViewport 面板后调用（面板每次打开都重建，需重新绑定）。
    void BindCharacterPanelTabs();

    // 四个页签按钮的 OnClicked 回调（无参数委托，需各自独立的 UFUNCTION）。
    // 供 BindCharacterPanelTabs 用 AddDynamic 绑定。
    UFUNCTION()
    void OnTabCharaClicked();
    UFUNCTION()
    void OnTabArmClicked();
    UFUNCTION()
    void OnTabYiqiClicked();
    UFUNCTION()
    void OnTabMingzuoClicked();

    // 切换到指定页签：设置对应组件可见、隐藏其余组件、更新 something_show 背景图。
    // 供上面四个 OnClicked 回调及默认选中逻辑调用。
    UFUNCTION(BlueprintCallable, Category = "CharacterPanel")
    void SwitchCharacterPanelTab(int32 TabIndex);

    // ---- 角色槽位选择（chara_pitc_head 内的角色头像按钮 chara_1、chara_2...）----
    // 用指定角色的属性填充面板：chara_name(名称)、pict_chara/something_show(头像)、
    // attack_num(总攻击值)、health_num(最大生命值)、critical_hit_num(暴击率)、critical_hit_damage_num(暴击伤害)。
    // Source 为 nullptr 时填默认空值：数值显示 0（暴击率/伤害显示 0%）、头像为空、名称为 "Null"。
    void ApplyCharacterPanelFrom(class ABattleCharacter* Source);

    // 应用第 SlotIndex 个角色槽位（0 → chara_1，1 → chara_2 ...）：
    // 取 CharacterSlotClasses[SlotIndex] 的 CDO（类默认对象）属性填充面板。
    // 槽位越界或类为空（未绑定）→ 按 Source=nullptr 填默认空值。
    void ApplyCharacterSlot(int32 SlotIndex);

    // 绑定 chara_pitc_head 内角色头像按钮（chara_1、chara_2...）的 OnClicked 到对应回调。
    // 在 OpenCharacterPanel() 里与 BindCharacterPanelTabs() 一并调用（面板每次打开重建，需重新绑定）。
    void BindCharacterSlotButtons();

    // 各角色槽位按钮的 OnClicked 回调（无参委托，需各自独立的 UFUNCTION）。
    // chara_1 → OnCharaSlot1Clicked（槽位 0），chara_2 → OnCharaSlot2Clicked（槽位 1）...
    UFUNCTION()
    void OnCharaSlot1Clicked();
    UFUNCTION()
    void OnCharaSlot2Clicked();
    UFUNCTION()
    void OnCharaSlot3Clicked();
    UFUNCTION()
    void OnCharaSlot4Clicked();

    // ==================================================================
    // ---- 角色信息表（Data_chara_imfor）：面板头像与编队共用的角色列表 ----
    // ==================================================================
    /** 表有效时返回表对象；无效时尝试按约定路径懒加载 /Game/UI/chara_imf/Data_chara_imfor。
     *  加载失败返回 nullptr（调用方各自兜底：面板回退 CharacterSlotClasses，编队显示空队伍）。 */
    UDataTable* EnsureCharaInfoTable();

    /** 按行名取角色信息（未配置/行不存在返回 false，OutInfo 不被写入）。
     *  用输出参数而不是返回指针：UHT 不允许 UFUNCTION 暴露指向 USTRUCT 的裸指针。 */
    UFUNCTION(BlueprintPure, Category = "CharacterPanel")
    bool GetCharaInfoByRow(FName RowName, FCharaInfoEntry& OutInfo) const;

    /** C++ 内部用：同上但返回指针（无需拷贝）；行不存在返回 nullptr。 */
    const FCharaInfoEntry* FindCharaInfo(FName RowName) const;

    // ---- 角色基础信息取值器（protected 字段的对外只读口；编队 UI 等外部类从这里读）----
    UFUNCTION(BlueprintPure, Category = "Character")
    FString GetCharacterDisplayName() const { return CharacterName; }

    UFUNCTION(BlueprintPure, Category = "Character")
    int32 GetCharacterLevelValue() const { return CharacterLevel; }

    /** 「拥有的角色」行名列表（面板头像与编队列表都用它）。
     *  OwnedCharaRows 为空 → 表里全部行；非空 → 按 OwnedCharaRows 顺序过滤表。
     *  表也无效 → 回退 CharacterSlotClasses（旧行为，此时行名是 NAME_None 占位）。 */
    UFUNCTION(BlueprintPure, Category = "CharacterPanel")
    TArray<FName> GetOwnedCharaRows() const;

    /** 按「拥有的角色」重建 chara_pitc_head 下的头像格子（自动添加 + 选中框）。
     *  与背包系统同一套模式：每个头像格子用 CharaHeadSlotClass 独立 CreateWidget 出来
     *  （套一层 USizeBox 钉死尺寸防塌陷），加到容器后，再按拥有角色逐格填充
     *  头像图 + 选中框（见 ApplyCharaHeadSlotVisual）。
     *  运行时清空容器旧子控件（含 WBP 里硬编码的 chara_1/chara_2），再动态生成。
     *  在 OpenCharacterPanel 里调用（面板每次打开重建）。 */
    void RebuildCharaHeadList();

    /** 填充第 SlotIndex 个头像格子：头像图 + 选中框 + 等级（按约定控件名取）。
     *  bSelected = true 时显示金色选中框。与背包 ApplyBagSlotVisual 同一模式。 */
    void ApplyCharaHeadSlotVisual(int32 SlotIndex, class UTexture2D* HeadTex, bool bSelected, int32 Level);

    /** 本次生成的头像格子实例（下标与槽位对齐）。 */
    UPROPERTY()
    TArray<TObjectPtr<class UUserWidget>> CharaHeadSlotWidgets;

    /** 动态头像按钮的载体对象（防 GC）。 */
    UPROPERTY()
    TArray<TObjectPtr<class UBagWidgetAction>> CharaHeadActions;

    /** 刷新动态头像的选中态：第 CurrentCharaSlotIndex 个显示金框，其余隐藏。 */
    void RefreshCharaHeadSelection();

    /** 解析「当前正在看的角色」实例：
     *  CurrentCharaSlotIndex < 0 → nullptr（没选，显示自身语义由调用方决定）；
     *  选中槽位对应的角色类 == this 的类 → this（实时实例）；
     *  否则 → 该角色类 CDO（其他角色默认属性）。
     *  数据表优先，表无效回退 CharacterSlotClasses 旧数组。
     *  非 const：可能触发 Data_chara_imfor 的懒加载。 */
    ABattleCharacter* ResolveSelectedCharaSource();

    /** 武器背包职位筛选应使用的职位。
     *  在「角色面板→打开武器背包」语境下，玩家是在给【面板选中的角色】挑武器，
     *  所以筛选用选中角色的 JobClass（而非当前操控角色 this 的职位）。
     *  没有有效选中角色时回退 this->JobClass（保持旧语义）。 */
    EJobClass GetWeaponBagFilterJobClass();

    // ==================================================================
    // ---- 角色编队（L 键打开；纯 C++ 构建界面，无需 WBP 资产）----
    // ==================================================================
    /** 第 TeamIndex 支队伍的成员行名（不足补 None / 超出截断到 3；Teams 未初始化时自动补 8 队）。 */
    UFUNCTION(BlueprintPure, Category = "CharacterPanel")
    const TArray<FName>& GetTeamMembers(int32 TeamIndex);

    /** Teams 不足 8 支时自动补齐（惰性初始化）。 */
    void EnsureTeamsArray();

    /** 写回一支队伍：逐位覆盖 —— NewMembers[i] 有效则覆盖第 i 位；不足 3 个时其余位保持原样。 */
    UFUNCTION(BlueprintCallable, Category = "CharacterPanel")
    void SetTeamMembers(int32 TeamIndex, const TArray<FName>& NewMembers);

    /** 整体覆盖一支队伍（快速编队「完成」用）：本轮选几个写几个，不足 3 位的其余位**清空（None）**。
     *  与 SetTeamMembers（逐位覆盖、未选保持原样）语义不同 —— 重新编队是「新一轮整体替换」，
     *  不是「只补空缺」。 */
    UFUNCTION(BlueprintCallable, Category = "CharacterPanel")
    void OverwriteTeamMembers(int32 TeamIndex, const TArray<FName>& NewMembers);

    /** 解析「玩家自己」在 Data_chara_imfor 里的行名：遍历表找 CharaClass == GetClass() 的行。
     *  用于 slot_0 默认角色（未编队时 slot_0 = BP_CharacterPlayer 自己）。
     *  找不到（玩家没在表里 / 表未建）→ 返回 NAME_None。 */
    FName ResolveDefaultMemberRow() const;

    /** 惰性创建 L 键输入（运行时自建 IA/IMC，同 EnsureBagInput 模式；bTriggerWhenPaused）。 */
    bool EnsureCharaTeamInput();

    /** 切换编队界面：打开↔关闭（由 L 键调用）。 */
    void ToggleCharaTeamUI();

    /** 打开编队界面：创建 UCharaTeamWidget（C++ 构建控件树）+ 暂停游戏 + 唤起鼠标。 */
    void OpenCharaTeamUI();

    /** 关闭编队界面：移除控件 + 恢复游戏。 */
    void CloseCharaTeamUI();


    // ==================================================================
    // ---- 背包系统（B 键打开 / 关闭；打开时暂停游戏 + 显示鼠标）----
    // ==================================================================
    // 惰性创建 B 键输入（运行时自建 UInputAction + UInputMappingContext 并挂到本地子系统，
    // 无需在编辑器创建 IA/IMC 资源）。动作带 bTriggerWhenPaused，暂停时仍可响应。
    bool EnsureBagInput();

    // 切换背包开关：打开→关闭、关闭→打开（由 B 键输入调用）
    void ToggleBag();

    // 打开背包：创建 WBP_Bag + 生成全部格子 + 填充数据 + 暂停游戏 + 显示鼠标
    void OpenBag();

    // 关闭背包：销毁控件 + 恢复游戏 + 隐藏鼠标。
    // BlueprintCallable：供 WBP_Bag 的 btn_close 按钮 OnClicked 调用。
    UFUNCTION(BlueprintCallable, Category = "Bag")
    void CloseBag();

    // 重新读表并刷新「左侧分类 → 网格 → 详情 → 顶栏容量」
    UFUNCTION(BlueprintCallable, Category = "Bag")
    void RefreshBag();

    // 切换当前分类（Kind < 0 表示「全部」）
    UFUNCTION(BlueprintCallable, Category = "Bag")
    void SetBagKind(int32 Kind);

    // 取背包数据组件（可能为 nullptr）
    UFUNCTION(BlueprintCallable, Category = "Bag")
    class UInventoryComponent* GetInventory() const;

    // 背包 UI 内所有动态按钮的统一入口（由 UBagWidgetAction 转发）
    void HandleBagAction(EBagActionType ActionType, int32 Index);

    // 背包内点击「使用」时触发，供蓝图扩展实际使用效果（消耗品/物资箱等）
    UFUNCTION(BlueprintImplementableEvent, Category = "Bag")
    void OnBagItemUsed(const FBagItemEntry& Item);

    // ==================================================================
    // ---- 武器页（arm_imfor）与武器背包：切换角色武器类 ----
    // ==================================================================
    // 需求：角色面板的「武器」页里用一个按钮打开【武器背包】，在背包里选中一把武器
    //       → 切换角色当前装备的武器类（AWeaponBase 子类），并且武器页与背包保持同步。
    //
    // 三个设计决策（为什么这么做）：
    //  ① 武器背包 = 【复用 WBP_Bag 的「武器」分类模式】，不新建资产。
    //     背包本来就有 kind_0(武器) 分类按钮与 GetItemsForKind()；另建一个只装武器的控件
    //     等于把分类筛选/格子/详情重写一遍，而且「和背包同步」会退化成两套数据。
    //     这里只加一个模式标志 bBagInWeaponMode。
    //  ② 背包行的武器类从哪来：Stru_bag_attribute 里没有「武器蓝图类」这一列，
    //     所以用 WeaponClassByItemRow 在角色上做映射（行名 → AWeaponBase 子类）。
    //     不动数据表结构 = 不用重编资产、不影响已有字段的消费方。
    //  ③ 装备状态要记住【行名】（EquippedWeaponRow）—— 否则回答不了
    //     「背包里哪一格是当前装备的」；「和背包同步」这条需求全靠它。

    /** 行名 → 武器蓝图类。在 BP_PlayerCharacter 里配：Data_tool 里武器那一行的行名 → characters/arms 下的武器蓝图。
     *  没配时按约定兜底：拿行名 / tool_id 去 /Game/characters/arms/<名字> 找同名武器蓝图（成功失败都打日志）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Switch")
    TMap<FName, TSubclassOf<class AWeaponBase>> WeaponClassByItemRow;

    /** 哪个 kind_tool 值算「武器」。
     *  E_kind_tool 的显示名（按值序）：0=武器 1=声骸 2=消耗品 3=材料 4=箱匣 5=特殊。
     *  做成配置项而不是写死 0：以后往枚举里插新分类时不用改代码。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Switch", meta = (ClampMin = "0"))
    int32 WeaponKindTool = 0;

    /** 武器背包里【点格子就装备】（默认【关】）。
     *  关闭（推荐，也是《鸣潮》的做法）：点格子只选中 → 看右侧详情确认 → 点「装备」按钮才装。
     *  开启：点一下格子立刻换武器，适合快速试装。
     *  ★ 用法：详情面板那个按钮（btn_use / btn_equip）在武器模式下就是「装备」，
     *    所以关掉本开关不会让「怎么装备」变得不可发现。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Switch")
    bool bEquipOnWeaponSlotClick = false;

    /** 【装备】一件武器时，是否要求这件物品能解析出武器蓝图类（默认开）。
     *
     *  ★ 这个开关只作用于【装备路径】，不再作用于武器背包的列表过滤 —— 这一点是修出来的：
     *
     *    它原来写在 `IsWeaponItem` 里（「算武器 ⇔ kind_tool 是武器 且 有武器蓝图」），
     *    动机是对的：`kind_tool` 的默认值就是 0（武器），所以任何【没显式填分类】的物品
     *    （掉落物首当其冲）都会被动地满足 `KindTool == WeaponKindTool`。
     *    但它把判据放在了一个 fail-closed 的位置上，一旦解析链断了（实测：自动扫描返回 0 个
     *    候选 → 索引空），**所有**物品都被判成「不是武器」→ 整个武器背包被清空。
     *    也就是说：一个配置没配好，被放大成了功能整体不可用。
     *
     *    现在拆成两条判据，各归其位：
     *      · `IsWeaponItem`（列表用）    = kind_tool 是武器 【且】行名属于 Data_tool —— 只看数据，不会因配置缺失而清空列表
     *      · `CanEquipWeaponItem`（装备用）= 上面两条 【且】（本开关开启时）能解析出武器蓝图类
     *    也就是说「不知道它是不是武器」≠「它不是武器」：列表照常显示，装备时才拦，
     *    而且拦住的时候会打出「该去哪个蓝图填哪个字段」的修法。
     *
     *  关掉：装备时也不再要求武器蓝图类 —— 只有在你确认解析链本身有问题、想临时放行时才关。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Switch")
    bool bRequireWeaponClassForWeaponItem = true;

    /** 关掉武器背包后自动回到角色面板的「武器页」（默认开）。
     *  为什么需要：角色面板与背包互斥（OpenBag 会先收起面板），不自动回来的话
     *  玩家切换完武器就看不到结果 —— 那是「同步」最直观的一环。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Switch")
    bool bReopenPanelAfterWeaponBag = true;

    /** 当前装备的武器来自背包的哪一行（NAME_None = 没有记录）。
     *  ★「背包 ↔ 武器页」同步的唯一依据：背包靠它打「已装备」标记，物品没了也靠它自动卸下。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Switch")
    FName EquippedWeaponRow;

    /** 当前装备的武器是「同名武器中的第几把」（0 基，配合 EquippedWeaponRow 组成唯一实例键）。
     *  ★ 为什么需要（武器「唯一装备」）：同一行名（同类）的不可堆叠武器会占多个格子，
     *    光靠行名区分不了「这一把 vs 那一把」。这个实例号 + 行名 = 唯一的那一把，
     *    装备时据此校验「这把武器有没有被别的角色占用」。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Switch")
    int32 EquippedWeaponInstanceNo = 0;

    /** 是否处于「武器背包」模式（打开的背包被锁到武器分类，点格子 = 装备） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|UI")
    bool bBagInWeaponMode = false;

    /** 打开【武器背包】：复用 WBP_Bag，进入武器模式（分类锁到武器 + 点格子=装备）。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|UI")
    void OpenWeaponBag();

    /** 关闭武器背包。语义入口；实际收尾（恢复游戏 + 自动回武器页）都在 CloseBag 里，
     *  保证「B 键关 / btn_close 关 / 武器页按钮关」三条路行为一致。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|UI")
    void CloseWeaponBag();

    /** 按背包行名装备武器（= 切换角色武器类）。找不到映射时给出「两种修法 + 当前可选行名」的日志。
     *  @param InstanceNo 同名武器中的第几把（0 基）；蓝图直接调时默认 0（数据表初始那把）。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    bool EquipWeaponFromItemRow(FName RowName, int32 InstanceNo = 0);

    /** 装备【当前选中的那一格】—— 三条装备路径（点格子 / 详情面板按钮 / btn_equip）都汇到这里。
     *  统一入口的好处：校验与日志只有一份，「点格子能装、点按钮装不了」这种路径差异不会出现。
     *  @param TriggerName 触发来源，只用于日志（排查时能一眼看出玩家点的是哪个按钮）。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    bool EquipSelectedBagWeapon(const FString& TriggerName);

    /** 卸下武器并清空 EquippedWeaponRow（UnequipWeapon 只处理 Actor，不清记录） */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    void UnequipWeaponAndClearRow();

    /** 解析「背包行名 → 武器蓝图类」：先查 WeaponClassByItemRow，再按约定路径兜底。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    TSubclassOf<class AWeaponBase> ResolveWeaponClassForItemRow(FName RowName) const;

    /** 这件物品算不算「背包里的一件武器」—— 只看数据，不问配置。
     *  = kind_tool 是武器分类 【且】它的行名属于背包物品表（Data_tool）。
     *
     *  ★ 这里【故意】不要求「能解析出武器蓝图类」。理由见 bRequireWeaponClassForWeaponItem
     *    的注释：那条要求一旦放在这里，解析链断掉就会把整个武器背包清空。
     *    调用方如果想问「能不能装备」，用 CanEquipWeaponItem。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    bool IsWeaponItem(const struct FBagItemEntry& Item) const;

    /** 这件物品能不能【装备】（= 有可 Spawn 的武器蓝图类）。
     *  = IsWeaponItem 且（默认）能解析出武器蓝图类。装备路径用这个；列表过滤不要用。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    bool CanEquipWeaponItem(const struct FBagItemEntry& Item) const;

    /** 这一格是不是【当前真正装备着的那把武器】。
     *
     *  ★ 所有「装备中 / 已装备」的显示与判定都必须走这一个函数。
     *    之前是各处各写一遍 `Entry->RowName == EquippedWeaponRow`，两种后果：
     *      ① 有的地方漏了 `!EquippedWeaponRow.IsNone()`（行号为空的物品会被显示成已装备）；
     *      ② 行号相等并不等于「就是它」—— 行号与实物脱钩时（掉落物、声明回填）
     *         背包会说「装备中」，而角色手上是另一把刀。
     *    所以这里一次判完：行号非空 + 行名属于 Data_tool + （能解析出武器类时）手上的 Actor 就是这个类。
     *
     *  ★ 解析不出武器类时【不再直接判否】，而是回退到「记录行 + 展示行一致」的判据 ——
     *    否则「已装备」标记会跟着解析链一起失灵（实测：自动扫描返回 0 个候选时，
     *    明明装备着 sword、那一格却不显示「装备中」）。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Switch")
    bool IsWeaponRowEquipped(FName RowName, int32 InstanceNo = -1) const;

    /** 打开武器背包后自检：把「kind_tool 是武器、但没有武器蓝图」的物品列出来。
     *  这是本轮两个 bug 的判定口 —— 不报的话，玩家只会看到「武器背包里少了一件/多了一件」。 */
    void LogWeaponItemFilterReport() const;

    /** 「当前装备的武器」对应背包的哪一行 —— 背包高亮与武器页显示都用它。
     *
     *  ★ 为什么不能直接用 EquippedWeaponRow：那是「记录」，会被武器蓝图声明回填、
     *    也会被别的装备入口改写；而真正生效的是手上的武器 Actor。
     *    一旦两者脱钩，背包高亮/武器页就会指向另一件东西（本轮 bug 的第二个现象）。
     *  顺序：① 手上那个武器类自己声明的行（最可信，类才是生效的东西）
     *        → ② EquippedWeaponRow（仅在「它解析出的类正是手上这把」时才采信）
     *        → ③ 原样返回 EquippedWeaponRow（没有武器 Actor 时保持旧行为）。*/
    UFUNCTION(BlueprintPure, Category = "Weapon|Switch")
    FName GetEquippedWeaponDisplayRow() const;

    // ==================================================================
    // ---- 武器蓝图 ↔ 背包数据 的绑定（武器蓝图自声明 + 自动发现 + 同步）----
    // ==================================================================
    // 【整条链路】（每句话对应需求里的一个诉求）
    //   武器蓝图 BP_sword → Weapon|Bag → Bag Item Row = '3'
    //        ↓ RebuildWeaponRowIndex() 读每个候选蓝图的默认值，建索引
    //   行名 '3' → BP_sword
    //        ↓ 打开武器背包，点第 3 件
    //   装备 BP_sword，同时 EquippedWeaponRow = '3'（背包那格标「装备中」）
    //        ↓ SyncDefaultWeaponClass()
    //   角色蓝图的 Weapon Class（Default Weapon Class）也变成 BP_sword
    //
    // 【三层优先级】ResolveWeaponClassForItemRow
    //   ① Weapon Class By Item Row（角色上手工覆盖，最高）
    //   ② 武器蓝图自声明的 Bag Item Row / Bag Item Tool Id  ← 日常走这条
    //   ③ /Game/characters/arms/<行名或 tool_id> 同名资产（兜底）

    /** 候选武器蓝图：打开武器背包时，C++ 读这些蓝图的【默认值】拿到它们各自声明的背包行。
     *
     *  为什么要有这个列表（而不是纯靠自动扫描）：打包时未被任何东西引用的资产会被裁剪掉 ——
     *  把武器加进这里 = 建立硬引用，打包后也能正常对应。想省事可以先只靠自动扫描。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Bag")
    TArray<TSubclassOf<class AWeaponBase>> WeaponClassCandidates;

    /** 编辑器便利：自动扫描下面那些目录里的武器蓝图，不用手工往上面那个列表里加。
     *  依赖资产注册表；打包后的发现结果取决于资产有没有被 cook 保留 →
     *  想让打包版也稳定，把武器加进 Weapon Class Candidates。默认开。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Bag")
    bool bAutoScanWeaponBlueprints = true;

    /** 自动扫描的目录。留空 = 用默认 /Game/characters/arms（本工程武器蓝图所在处）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Bag")
    TArray<FString> WeaponBlueprintSearchPaths;

    /** ★ 装备 / 卸下武器时，把武器类同步写回角色蓝图的 Weapon Class（Default Weapon Class）。
     *
     *  这就是需求里的「装备后会同步更换角色蓝图中的 Weapon Class」。
     *  实现上既改本次实例、也改蓝图默认值对象（CDO）—— 否则停掉 PIE 再打开
     *  BP_PlayerCharacter，Details 里还是旧值，看起来像同步没生效。
     *  副作用：编辑器里该蓝图会变成「未保存」，Save All 才持久化（日志里会写）。想关掉就把这个设 false。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Bag")
    bool bSyncDefaultWeaponClass = true;

    /** 当前装备的武器是不是【从背包某一格点出来的】（决定「那一格没了要不要自动卸下」）。
     *
     *  为什么要区分：EquipWeapon 也会按武器蓝图声明的行回填 EquippedWeaponRow，
     *  但那种装备（比如开局用 Default Weapon Class 装的那把）不代表「这把武器是背包给的」。
     *  若也参与自动卸下，开局自带的武器会在第一次打开背包时被静默卸掉 —— 这是个真回归。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Bag")
    bool bEquippedWeaponRowFromBag = false;

    /** 重建「背包行名 → 武器蓝图类」索引。
     *  改了武器蓝图的自声明 / 候选列表之后手动调一次；首次解析时会自动建一次。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Bag")
    void RebuildWeaponRowIndex();

    /** 把武器类同步到角色蓝图的 Weapon Class（Default Weapon Class）。
     *  传 nullptr = 同步成空手（卸下武器时用）。返回是否真的改动了值。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Bag")
    bool SyncDefaultWeaponClass(TSubclassOf<class AWeaponBase> WeaponClass);

    /** 反查：这个武器蓝图声明的是背包哪一行（读蓝图默认值；没声明返回 None）。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Bag")
    FName GetBagItemRowForWeaponClass(TSubclassOf<class AWeaponBase> WeaponClass) const;

    /** 把「武器蓝图声明的 key」换算成【背包的真实行名】。
     *
     *  ★ 为什么需要这个函数：声明可以填行名（Bag Item Row）也可以填 tool_id（Bag Item Tool Id），
     *    但「背包哪一格是装备中」必须用**行名**去查，两者不能混着比 ——
     *    混着比的后果是「用 tool_id 声明时背包不高亮」+「误报『声明的是别的行』」。
     *  规则：Key 本身是拥有的行名 → 原样返回；否则按 tool_id 找对应行；找不到则原样返回。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Bag")
    FName ResolveBagKeyToInventoryRow(FName Key) const;

    /** 当前已建立多少条「背包行 → 武器蓝图」绑定（只读诊断，排「装不上」时先看它）。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Bag")
    int32 GetWeaponRowBindingCount() const { return WeaponRowToClassIndex.Num(); }

    /** 刷新武器页 arm_imfor：当前武器的图标/名称/品质/类型/攻击力 + 未装备提示。 */
    UFUNCTION(BlueprintCallable, Category = "Weapon|UI")
    void RefreshArmPanel();

    /** 用指定角色（其他角色 CDO）的默认武器信息填武器页。
     *  选中其他角色时，RefreshArmPanel 走这里 —— 显示该角色 DefaultWeaponClass 的 CDO 属性，
     *  而不是 this 手上实装的那把。 */
    void RefreshArmPanelForCharacter(const ABattleCharacter* Selected);

    /** 按「当前正在看的角色」重算面板属性（attack_num / health_num / 暴击率 / 暴击伤害）。
     *
     *  ★ 与换武器联动：attack_num 显示的是 GetTotalAttackPower() = 基础 + 武器 + 装备，
     *    换掉武器会改变它 —— 不重算的话现象是「武器页换了刀，切回角色页攻击力还是旧的」。
     *  调用点：SwitchCharacterPanelTab（切回角色页时）、装备/卸下武器之后。 */
    UFUNCTION(BlueprintCallable, Category = "CharacterPanel")
    void RefreshCharacterPanelStats();

    /** 当前正在看的角色槽位序号（-1 = 没选过 / 未配槽位 → 显示当前角色自身）。
     *  在 ApplyCharacterSlot 里记录；RefreshCharacterPanelStats 靠它把显示还原成
     *  「和当初选那个槽位时完全一致」的结果（含槽位越界/未绑定 → 显示空值这种情况）。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CharacterPanel")
    int32 CurrentCharaSlotIndex = -1;

    /** arm_imfor 里「打开武器背包」按钮的 OnClicked 回调。
     *  为什么绑定也放 C++：面板每次打开都重建，蓝图连线容易漏（见 BindCharacterPanelTabs 的说明）。 */
    UFUNCTION()
    void OnOpenWeaponBagClicked();

    // ---- 武器页内部实现 ----
    // 绑定 arm_imfor 内的「打开武器背包」按钮（面板每次打开都重建 → 每次都要重绑）
    void BindArmPanelButtons();
    // 把 arm_imfor 控件树的真实结构（父链 + 全部子控件 名字/类名）打进日志。
    // 为什么需要：C++ 不猜布局 —— 控件名对不上时，与其来回猜，不如一次把「实际有什么」打出来对齐。
    void LogArmPanelStructure();
    // 按名字找一个武器页控件。只在「整棵树里都没有」时返回 nullptr（不报错、不刷日志）。
    class UWidget* FindArmPanelWidget(const TCHAR* WidgetName) const;

    // ---- 获得物品通知（掉落物自动入包时推送）----
    // 把「获得物品」事件推给实现了 IItemPickupListener 的 UI（默认就是 WBP_HealthBar）。
    //
    // 为什么不用事件绑定：本工程既有模式是「C++ 推数据 / 蓝图做表现」（见 UpdateHealthBar）。
    // 这里的提示 UI 是挂在本角色身上的 UWidgetComponent（HealthBarWidget），C++ 可以直接
    // 取到它的 UserWidget 并按接口类型调用 —— 省掉蓝图里「Get Player Character → Cast →
    // Bind Event」三步连线，而那三步任意一处写错都是静默失效，极难排查。
    void NotifyItemObtained(const FBagItemEntry& Item, int32 ObtainedCount);

    // ---- GAS 能力封装访问器（供 GameplayAbility 外壳轮询/判定）----
    // 大招是否正在施放（多段蒙太奇播放期间为 true）
    bool IsUltimateCasting() const { return bIsUltimateCasting; }
    // 技能是否正在施放
    bool IsSkillCasting() const { return bIsSkillCasting; }
    // 大招冷却是否可用（冷却归零且无冲突状态）
    bool CanCastUltimate() const { return !bIsUltimateCasting && !bIsAttacking && !bIsSkillCasting && !bIsHitReaction && !bIsEnergySkillCasting && CurrentUltimateCooldown <= 0.0f; }

    // ---- 攻击力/增伤（public：供 WeaponBase 及 GameplayAbility 外壳计算伤害）----
    // 角色当前攻击力：读 GAS AttributeSet->Attack（唯一真源），未初始化时回落 BaseAttack。
    // 作为「技能倍率(Multiplier)」结算伤害的基数
    float GetAttackPower() const;

    // 当前武器提供的攻击力加成（无武器/空手时返回 0）。
    // 读 CurrentWeapon->GetAttackPower()，让武器蓝图（如 test_knife）配置的攻击力叠加生效。
    float GetWeaponAttackPower() const;

    // 装备总攻击力加成（护符/饰品等装备系统，尚未开发）：暂固定返回 0，后续装备系统落地后接入。
    float GetEquipmentAttackPower() const;

    // 角色总攻击值（= 基础攻击力 + 武器攻击力 + 装备总攻击力）：WBP_Character_imf 的 attack_num 显示的数值。
    // 作为面板显示与最终伤害结算的统一基数。
    float GetTotalAttackPower() const;

    // 玩家造成伤害的统一乘数（能量技增伤期间 ×(1+EnergySkillDamageBoost)）
    float GetOutgoingDamageMultiplier() const;

    // 角色当前暴击率（整数百分比，如 5 = 5%）：读 GAS AttributeSet->CritRate，未初始化回落 BaseCritRate
    float GetCritRate() const;

    // 角色当前暴击伤害（整数百分比，如 150 = 150%）：读 GAS AttributeSet->CritDamage，未初始化回落 BaseCritDamage
    float GetCritDamage() const;

    // ---- 统一伤害结算：套用完整伤害公式 ----
    // 最终伤害 = 总攻击力 × 倍率 × [暴击伤害(仅暴击)] × (1+伤害提升) × 等级系数 × 0.9
    //   - 总攻击力 = GetTotalAttackPower()（= 基础攻击力 + 武器攻击力 + 装备总攻击力）
    //   - 倍率 = 传入的 Multiplier（段倍率/普攻倍率/弹丸倍率/下落攻击倍率）
    //   - 暴击伤害：按 GetCritRate() 概率判定，触发暴击时 × (GetCritDamage()/100)，否则 ×1
    //   - (1+伤害提升) = GetOutgoingDamageMultiplier()（能量技/大招增伤，无增伤时为 1）
    //   - 等级系数 = (100 + 角色等级) / (199 + 角色等级 + 怪物等级)
    //   - ×0.9 固定系数
    // TargetMonster 用于读取怪物等级；为 nullptr 时等级系数按怪物等级=1 回落
    float ComputeFinalDamage(float Multiplier, class AMonsterBase* TargetMonster) const;

    // 同上（带暴击输出的重载）：bOutWasCrit 返回本次结算是否触发暴击。
    // 伤害飘字用：调用后把暴击结果 SetPendingDamageNumberCrit 写到目标怪物，
    // 再 ApplyPointDamage → 怪物 TakeDamage 按实际扣血量弹出对应（暴击/普通）飘字
    float ComputeFinalDamage(float Multiplier, class AMonsterBase* TargetMonster, bool& bOutWasCrit) const;

protected:
    // ---- 战斗状态 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsAttacking = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsDodging = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsSkillCasting = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsUltimateCasting = false;

    // 旧无敌帧布尔（P3 起改为镜像：真实无敌状态由 GAS 的 State.Invincible Tag 驱动，
    // 此字段仅作兼容保留，不再作为判定依据）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsInvincible = false;

    // ---- 无敌 GameplayEffect（P3）：闪避/完美闪避通过该 GE Grant State.Invincible Tag ----
    // 时长由 SetByCaller 动态传入（普通闪避 0.3s，完美闪避为动态无敌时长）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge")
    TSubclassOf<UInvincibilityGameplayEffect> InvincibilityEffectClass;

    // 受击硬直中（受击蒙太奇播放期间为 true，播放结束自动解除，期间禁止一切操作）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsHitReaction = false;

    // 下落攻击进行中（空中普攻触发，落地结算伤害）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    bool bIsFallAttacking = false;

    // ---- 下落攻击参数 ----
    // 落地冲击倍率：下落攻击落地时对范围内所有怪物造成的伤害倍率。规整叫法——
    // 原「下落攻击伤害(FallAttackDamage)」改为「下落攻击倍率」，最终伤害 = 攻击力 × 倍率 × 增伤乘数。
    // 默认 0.4（攻击力 100 → 40 伤害，与旧固定 40 对齐）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|FallAttack", meta = (ClampMin = "0.0"))
    float FallAttackDamage = 0.4f;

    // 落地冲击范围（cm，以角色为中心）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|FallAttack")
    float FallAttackImpactRadius = 350.0f;

    // 触发下落攻击后的下坠速度（cm/s，正值越大下坠越快）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|FallAttack")
    float FallAttackDiveSpeed = 1800.0f;

    // 下落攻击触发高度（cm）：角色空中使用普攻时，距地面高度需超过该值才触发下落攻击。
    // 默认 600 = 单次跳跃高度 JumpHeight，需高于一次跳跃才可触发（防止低空误触）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|FallAttack", meta = (ClampMin = "-1.0"))
    float FallAttackTriggerHeight = -1.0f;

    // 实际可达的最大跳跃高度（cm，脚部基准）：按真实物理从 JumpZVelocity 与重力推导
    // = 一段跳顶点 + 二段跳顶点。FallAttackTriggerHeight <= 0 时作为触发门槛自动使用
    float GetMaxJumpHeight() const;

    // ---- 完美闪避参数 ----
    // 闪避 GameplayAbility（P3：外壳式封装）。配置后 BeginPlay 会通过 ASC 授予该能力，
    // ActivateAbility 时回调 Dodge()。留空则不授予（闪避仍走原有按键直调逻辑）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge")
    TSubclassOf<UDodgeAbility> DodgeAbilityClass;

    // 完美闪避的基础无敌时间（实际无敌 = max(该值, 被停滞怪物该段攻击判定窗口的剩余时间)，
    // 即从完美闪避触发直到该段攻击受击判定时间结束全程无敌；比普通闪避的 0.3s 长）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge")
    float PerfectDodgeInvincibilityTime = 0.5f;

    // 完美闪避触发后的闪避锁定时间（期间无法再次闪避）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge")
    float PerfectDodgeLockoutTime = 0.2f;

    // 完美闪避球体半径（cm，以角色为中心）：怪物攻击前摇内、其模型进入该球体的怪物可被完美闪避。
    // 替代旧的"攻击范围×2"判定，改为固定球体范围，更直观、更易调
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge", meta = (ClampMin = "0.0"))
    float PerfectDodgeRadius = 800.0f;

    // 完美闪避建议无敌时长下限（秒）：取 max(该值, 怪物该段攻击判定窗口剩余时间) 作为角色实际无敌时长。
    // 怪物攻击不打断，角色靠无敌帧规避伤害
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge", meta = (ClampMin = "0.0"))
    float PerfectDodgeStaggerDuration = 0.5f;

    // 闪避锁定剩余时间（>0 时无法闪避，Tick 递减）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Dodge")
    float CurrentDodgeLockout = 0.0f;

    // ---- 完美闪避蓝色残影 ----
    // 是否启用完美闪避残影（触发完美闪避时角色拖出蓝色半透明残影）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost")
    bool bPerfectDodgeGhost = true;

    // 残影总时长（秒）：从完美闪避触发起持续生成残影，整体效果持续该时长
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost", meta = (ClampMin = "0.1"))
    float PerfectDodgeGhostDuration = 1.0f;

    // 残影数量（整个时长内按间隔生成这么多份，越多拖尾越密）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost", meta = (ClampMin = "1"))
    int32 PerfectDodgeGhostCount = 6;

    // 残影颜色（默认蓝色）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost")
    FLinearColor PerfectDodgeGhostColor = FLinearColor(0.15f, 0.5f, 1.0f, 0.6f);

    // 残影材质（需半透明，推荐 Unlit Translucent；支持标量参数 GhostAlpha 与向量参数 GhostColor）。
    // 未指定时运行时自动尝试加载 /Game/Effects/M_DodgeGhost，仍未找到则不生成残影并在日志提示
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost")
    UMaterialInterface* PerfectDodgeGhostMaterial;

    // 残影相对角色网格的前后偏移（厘米，沿闪避/移动方向）：残影挂接在角色骨骼网格上随动（贴身），
    // 正值 = 残影落在角色身后（默认 20，形成《鸣潮》式残像感）；负值 = 残影落在角色身前；0 = 完全贴身。
    // 可同时叠加 GhostSideOffset 实现左右偏移
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost")
    float GhostBackOffset = 20.0f;

    // 残影相对角色网格的左右偏移（厘米）：正值 = 残影偏角色右侧；负值 = 偏角色左侧；0 = 居中（正前/正后方）。
    // 左右以角色移动方向为基准（无移动时以角色朝向为基准）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|DodgeGhost")
    float GhostSideOffset = 0.0f;

    // 残影生成状态（Tick 驱动）
    float GhostTrailTimeRemaining = 0.0f;
    float GhostTrailSpawnAccumulator = 0.0f;

    // 开始生成完美闪避残影（完美闪避触发时调用）
    void StartPerfectDodgeGhosts();
    // Tick 更新：按间隔生成残影直到时长耗尽
    void UpdatePerfectDodgeGhosts(float DeltaTime);
    // 在当前位置生成一份残影
    void SpawnDodgeGhost();

    // ---- 弹刀参数 ----
    // 弹刀无需专门按键：角色任意攻击（近战/远程/下落攻击）命中处于可弹刀前摇窗口的怪物
    // 即自动触发弹刀，打断该次攻击（怪物硬直）。此冷却限制两次成功弹刀的最小间隔
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Parry")
    float ParryCooldown = 0.8f;

    // 弹刀冷却剩余（Tick 递减；仅弹刀成功时进入冷却）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Parry")
    float CurrentParryCooldown = 0.0f;

    // ---- 耐力 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamina")
    float MaxStamina = 120.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Stamina")
    float CurrentStamina = 120.0f;

    // ---- 耐力恢复 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamina")
    float StaminaRegenTime = 3.0f;

    float StaminaRegenRate = 40.0f;

    // ---- 协奏能量 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Concerto")
    float ConcertoEnergy = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Concerto")
    float MaxConcertoEnergy = 100.0f;

    // ---- 血量燃烧能量技（Q 键）----
    // 使用技能（E）实际施放时扣除最大生命的 EnergyHPCostPercent（默认 15%），
    // 血量不足时保底扣至 1 点（不会因放技能死亡）；每次扣除累积 1 格能量，
    // 点亮 WBP_HealthBar 的 Passive_eng_1~4 进度条。4 格满 + 不在冷却 → 按 Q
    // 释放能量技：播放攻击蒙太奇 + EnergySkillBuffDuration 秒内伤害提升，
    // 增伤结束后清空全部能量（进度条清空，需重新累积）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EnergyHPCostPercent = 0.15f;

    // 能量最大格数（满格才可释放 Q 技能；对应 Passive_eng_1~4 共 4 格）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy", meta = (ClampMin = "1"))
    int32 MaxEnergyStacks = 4;

    // 当前能量格数（0~MaxEnergyStacks：每实际施放一次技能 +1，增伤 buff 结束清零）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Energy")
    int32 EnergyStacks = 0;

    // 能量技攻击段（蒙太奇/攻击时间段/每击接触伤害/重复伤害间隔均在此配置）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy")
    FComboMontageSegment EnergySkillSegment;

    // 能量技冷却（秒）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy", meta = (ClampMin = "0.0"))
    float EnergySkillCooldown = 30.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Energy")
    float CurrentEnergySkillCooldown = 0.0f;

    // 能量技增伤幅度（0.2 = +20%），自释放瞬间起持续 EnergySkillBuffDuration 秒
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy", meta = (ClampMin = "0.0"))
    float EnergySkillDamageBoost = 0.2f;

    // 增伤持续时长（秒；结束后关闭增伤并清空全部能量）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy", meta = (ClampMin = "0.1"))
    float EnergySkillBuffDuration = 20.0f;

    // 增伤期间的吸血比例（0.2 = 增伤窗口内每次实际造成伤害回复其 20% 的血量）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EnergyBuffLifestealRatio = 0.2f;

    // 能量技增伤是否生效中（生效期间不可再次释放 Q 技能）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Energy")
    bool bEnergyBuffActive = false;

    // 能量技是否正在施放（播放蒙太奇期间）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Energy")
    bool bIsEnergySkillCasting = false;

    // 能量技霸体（可配置）：Q 技能蒙太奇施放期间不播受击反应、不被击飞打断，
    // 伤害照常结算（与技能霸体 bSkillHyperArmor 同款行为）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy")
    bool bEnergySkillHyperArmor = true;

    // 当前能量技蒙太奇（攻击窗口判定用）
    UPROPERTY(Transient)
    TObjectPtr<UAnimMontage> CurrentEnergyMontage = nullptr;

    // 能量技蒙太奇静默计时（兜底：结束回调丢失时自动结束，防止操作永久锁死）
    float EnergyMontageSilentTime = 0.0f;

    // 能量技段内各怪物最近受击时间戳（控制 HitInterval 重复伤害节奏）
    TMap<TWeakObjectPtr<AMonsterBase>, float> EnergySegmentHitTimes;

    // 增伤结束定时器（到期关闭增伤并清空全部能量）
    FTimerHandle EnergyBuffTimerHandle;

    // ---- 能量进度条帧驱动同步（WBP_HealthBar 的 Passive_eng_1~4）----
    // Tick 每帧调用 UpdateEnergyBars()：格数未变化时一次整型比较直接返回（零开销），
    // 变化时才写进度条——任何代码路径增减/清空能量，UI 都必然当帧同步
    TWeakObjectPtr<UUserWidget> EnergyBarHostWidget;      // 宿主 widget 缓存（重建检测）
    TArray<TWeakObjectPtr<UProgressBar>> EnergyBarCache;  // Passive_eng_1~4 控件缓存
    int32 LastSyncedEnergyStacks = -1;                    // 上次同步到 UI 的格数（短路用）

    // ---- 冷却时间 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float AttackCooldown = 0.5f;

    // ---- 攻击索敌 ----
    // 攻击索敌半径（cm）：以角色为中心，该范围内存在怪物时，角色发起攻击的同时
    // 自动转向最近的怪物方向（范围内无怪物则保持原朝向）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float AttackAimRadius = 200.0f;

    // ---- 普攻根运动（修复连击段间位置回跳）----
    // 根因与技能/大招当年相同：普攻段动画序列未启用 Root Motion，根骨骼位移只渲染在
    // 姿势上（模型视觉移动但胶囊不动），段结束姿势重置时模型瞬移回原位置。
    // 开启后运行时为普攻蒙太奇（ComboAttackSegments 里的每一段）内动画序列启用根运动，位移跨段保留
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack")
    bool bEnableAttackRootMotion = true;

    // 普攻结束后旋转守卫时长（秒）：期间无蒙太奇播放时每帧清除根运动残留的 Pitch/Roll，
    // 覆盖蒙太奇 BlendOut 混合输出期（时长由蒙太奇资产决定），修复普攻后模型歪斜
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack", meta = (ClampMin = "0.1"))
    float AttackRotationGuardDuration = 0.8f;

    // ---- 普攻期间位移输入 → 只改攻击朝向 ----
    // 普攻蒙太奇播放期间按方向键：不驱动移动（不触发行走），而是平滑转向输入方向，
    // 后续普攻/根运动突进方向随之改变（= 改变攻击方向）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack")
    bool bAttackAimOnMoveInput = true;

    // 普攻期间按输入方向转向的平滑速度（值越大转得越快）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack", meta = (ClampMin = "0.5"))
    float AttackAimInterpSpeed = 10.0f;

    // 普攻根运动补丁是否已执行
    // ★ 注意它【不是】"每局只打一次、打完就永久跳过"的开关（以前那样用过，段数改成可配之后会出问题）：
    //   用户随时可能在蓝图里加一段（第 5 段、第 6 段…），新段的动画序列同样需要开根运动，
    //   否则新段的位移只渲染在姿势骨骼上、胶囊不动 → 表现为"打这一段时角色滑出去又瞬移回来"。
    //   所以配对下面的 AttackRootMotionPatchedComboCount 一起用：段数变了就重跑一遍补丁。
    bool bAttackRootMotionPatched = false;

    // 上一次执行根运动补丁时覆盖的普攻段数。-1 = 还没跑过。
    // 与当前段数不一致 → 重新补一遍（新增的段才能拿到根运动）。
    int32 AttackRootMotionPatchedComboCount = -1;

    // ---- 闪避/完美闪避根运动（修复闪避动画段末位置回跳）----
    // 根因与普攻/技能/大招相同：闪避蒙太奇动画序列未启用 Root Motion，闪避位移只渲染在
    // 模型姿势上（胶囊不动），蒙太奇结束姿势重置时模型瞬移回原位置。
    // 开启后运行时为 Dodge/Dash/PerfectDodge 蒙太奇内动画序列启用根运动，位移真实保留
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Dodge")
    bool bEnableDodgeRootMotion = true;

    // 闪避根运动补丁是否已执行（每局一次）
    bool bDodgeRootMotionPatched = false;

    // 运行时为闪避/疾跑/完美闪避蒙太奇动画序列启用根运动（Dodge() 内调用）
    void EnsureDodgeRootMotion();

    // 普攻旋转守卫剩余时间（Tick 递减，>0 且无蒙太奇播放时每帧清残留旋转）
    float AttackRotationGuardRemaining = 0.0f;

    // 是否有普攻蒙太奇（ComboAttackSegments 里任意一段 / FallAttackMontage）正在播放
    bool IsAttackMontagePlaying() const;

    // 运行时为普攻蒙太奇动画序列启用根运动（与技能/大招段根运动补丁同方案）
    void EnsureAttackRootMotion();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float SkillCooldown = 3.0f;

    // ---- 技能霸体（E 施放期间受击不播硬直动画、不打断技能，但伤害照常结算）----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Skill")
    bool bSkillHyperArmor = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float UltimateCooldown = 15.0f;

    // ---- 大招 GameplayAbility（P2：外壳式封装样板）----
    // 大招的 GAS 能力外壳（UUltimateAbility）。配置后 BeginPlay 会通过 ASC 授予该能力，
    // ActivateAbility 时回调 Ultimate()。留空则不授予（大招仍走原有 R 键直调逻辑）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    TSubclassOf<UUltimateAbility> UltimateAbilityClass;

    // ---- 大招无敌（R 施放期间完全免疫伤害：不扣血、不播受击动画、不触发受伤UI）----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    bool bUltimateInvincible = true;

    // ---- 大招停滞怪物（施放期间冻结所有怪物，不打断怪物攻击，大招结束恢复）----
    // true = 大招施放瞬间停滞所有存活怪物（CustomTimeDilation=0，动画/移动/AI 暂停、
    // 攻击蒙太奇暂停而非停止），直到 EndUltimate 才恢复。营造"大招时间停止"的演出效果
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    bool bUltimateFreezeMonsters = true;

    // ---- 大招镜头效果（施放期间画面放大 + 锁定视角旋转，结束后自动恢复）----
    // 画面放大倍数（1.0=不放大；1.5=画面放大 1.5 倍，FOV 按透视关系精确缩小）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate", meta = (ClampMin = "1.0"))
    float UltimateZoomFovMultiplier = 1.5f;

    // 大招期间是否锁定相机视角旋转（true = 视角固定不可转动）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    bool bUltimateLockCameraRotation = true;

    // 大招施放前记录的原始 FOV（EndUltimate 恢复用；首次施放时记录，避免 BP 覆盖值丢失）
    float DefaultFieldOfView = 90.0f;

    // 是否已记录原始 FOV（多次施放只记录一次，防累积误差）
    bool bUltimateFovRecorded = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    float CurrentAttackCooldown = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    float CurrentSkillCooldown = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    float CurrentUltimateCooldown = 0.0f;

    // ---- 多段蒙太奇技能（E 键）----
    // 技能段配置：数组长度=段数，按顺序播放一次后结束技能并进入冷却。
    // 每段可独立配置攻击时间段/接触伤害/是否可被闪避打断（见 FComboMontageSegment）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Skill")
    TArray<FComboMontageSegment> SkillSegments;

    // 当前正在播放的技能段索引（-1=未在播放）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Skill")
    int32 CurrentSkillSegmentIndex = -1;

    // 当前技能段蒙太奇（窗口判定/推进用）
    UPROPERTY(Transient)
    TObjectPtr<UAnimMontage> CurrentSkillMontage = nullptr;

    // 当前技能段内各怪物最近受击时间戳（控制 HitInterval 重复伤害节奏）
    TMap<TWeakObjectPtr<AMonsterBase>, float> SkillSegmentHitTimes;

    // 技能蒙太奇静默计时（兜底：结束回调丢失时自动结束技能，防止卡死）
    float SkillMontageSilentTime = 0.0f;

    // ---- 多段蒙太奇大招（R 键）----
    // 大招段配置：数组长度=段数，按顺序播放一次后结束大招并进入冷却。
    // 大招全程不可被打断（期间闪避无效、受击完全免疫=无敌，见 bUltimateInvincible）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    TArray<FComboMontageSegment> UltimateSegments;

    // 当前正在播放的大招段索引（-1=未在播放）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Ultimate")
    int32 CurrentUltimateSegmentIndex = -1;

    // 当前大招段蒙太奇
    UPROPERTY(Transient)
    TObjectPtr<UAnimMontage> CurrentUltimateMontage = nullptr;

    // 当前大招段内各怪物最近受击时间戳
    TMap<TWeakObjectPtr<AMonsterBase>, float> UltimateSegmentHitTimes;

    // 大招蒙太奇静默计时（兜底保护）
    float UltimateMontageSilentTime = 0.0f;

    // ---- 段根运动（修复技能/大招段间位置回跳）----
    // 根因：段动画序列默认未启用 Root Motion，根骨骼位移只渲染在骨骼姿势上
    // （模型视觉移动但胶囊不动），段结束姿势重置时模型瞬移回胶囊原位置。
    // 启用后位移由移动组件以根运动驱动整个角色，位置跨段保留。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Skill")
    bool bEnableSegmentRootMotion = true;

    // 段根运动补丁是否已执行（每局一次）
    bool bSegmentRootMotionInitialized = false;

    // 运行时为技能/大招段动画序列启用根运动并输出诊断日志
    void EnsureSegmentRootMotion();

    // ---- 根运动旋转残留清除（修复技能/大招结束后模型歪斜）----
    // 根因：启用段根运动后，root 骨骼的旋转增量（俯仰/侧倾，如冲刺前倾）
    // 会被应用到 Actor 旋转上；蒙太奇结束后 Pitch/Roll 残留导致模型看起来
    // 是歪的，直到移动/攻击的朝向逻辑把旋转重置为纯 Yaw 才恢复。
    // Mesh 相对变换基准值（BeginPlay 记录，用于清除残留偏移）
    FVector BaseMeshRelativeLocation = FVector::ZeroVector;
    FRotator BaseMeshRelativeRotation = FRotator::ZeroRotator;

    // 延迟补一次旋转清除的定时器（等待蒙太奇混合输出完成）
    FTimerHandle RootRotationResetTimer;

    // 清除根运动残留旋转：Actor 只保留 Yaw + Mesh 恢复基准相对变换
    void ResetMontageRotation();

    // ---- 技能/大招自动索敌转向（联动索敌机制）----
    // 施放期间持续追踪范围内最近的存活怪物，平滑旋转角色朝向
    // 总开关：false = 只在起手瞬间转向一次（旧行为）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Skill")
    bool bSkillAutoAim = true;

    // 自动索敌搜索半径（cm）：技能伤害范围通常大于普攻，默认比 AttackAimRadius(200) 大
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Skill", meta = (ClampMin = "0.0"))
    float SkillAimRadius = 600.0f;

    // 索敌转向平滑速度（越大转得越快）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Skill", meta = (ClampMin = "0.1"))
    float SkillAimInterpSpeed = 10.0f;

    // Tick：技能/大招施放期间平滑转向范围内最近怪物
    void UpdateSkillAim(float DeltaTime);

    // 在指定半径内寻找最近存活怪物并平滑转向（返回是否找到目标）
    bool AimAtNearestMonsterInRange(float Radius, float DeltaTime, float InterpSpeed);

    // ---- 索敌锁定系统（自动索敌 + 攻击自动转向 + 追踪 UI）----
    // 总开关：false = 不索敌、攻击不转向、不显示追踪提示（完全回退旧行为）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|LockOn")
    bool bEnableLockOn = true;

    // 索敌范围（cm，水平距离）：范围内有存活怪物时自动锁定最近的单位；目标超出范围立即丢失
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|LockOn", meta = (ClampMin = "0.0"))
    float LockOnRadius = 2000.0f;

    // 换锁阈值（秒）：某怪物比当前索敌目标更近、且一直保持更近达到该时长 → 更换索敌目标
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|LockOn", meta = (ClampMin = "0.1"))
    float LockOnRetargetTime = 5.0f;

    // 索敌提示 UI 类（在引擎内自建 Widget Blueprint，如 WBP_LockOnIndicator 并在此引用）。
    // 锁定目标后自动创建并绑定到目标身上：屏幕空间投影定位（不随镜头缩放/目标距离变化），
    // 目标未出现在屏幕上（背后/移出视野）时自动隐藏。未配置则不显示追踪提示
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|LockOn")
    TSubclassOf<UUserWidget> LockOnIndicatorClass;

    // 追踪点兜底抬升高度（cm）：索敌 UI 默认对准怪物碰撞胶囊中心（≈ 模型中心）；
    // 仅当目标怪物没有碰撞胶囊组件时才退回"脚底上方该高度"作为追踪点
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|LockOn", meta = (ClampMin = "0.0"))
    float LockOnIndicatorHeight = 160.0f;

    // 索敌扫描间隔（秒，内部）：越小对"更近怪物换锁"响应越快，但每帧扫描开销越大
    float LockOnScanInterval = 0.15f;

    // 当前索敌目标（有且只有一个；死亡/无效/超出范围时自动更换为范围内最近怪物）
    TWeakObjectPtr<AMonsterBase> LockedTarget;
    // 比当前目标更近的候选（持续比目标近 LockOnRetargetTime 秒后上位成为新目标）
    TWeakObjectPtr<AMonsterBase> CloserCandidate;
    float CloserCandidateTime = 0.0f;
    // 扫描计时（内部）
    float LockOnScanTimer = 0.0f;

    // 索敌追踪 Widget（首次锁定目标时懒创建；随目标/视野刷新位置与显隐）
    UUserWidget* LockOnIndicatorWidget = nullptr;
    bool bLockOnIndicatorVisible = false;

    // ---- 警觉朝向（Upper Body Facing Target）----
    // 范围内存在锁定目标时，上半身自动朝目标，下半身维持移动朝向（Orient to Movement），
    // 模拟"警觉观察"效果。范围、Sprint 退出等行为在 BP 中可调
    //
    // 总开关：true = 启用警觉（不再整身转 Actor，走 ABP Warping）；false = 旧行为（整身转目标）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|AlertFacing")
    bool bEnableUpperBodyFacingTarget = true;

    // 警觉触发距离（cm，水平）：锁定目标距离 ≤ 该值 → 启用警觉上半身朝向；超出 → 退出警觉
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|AlertFacing", meta = (ClampMin = "0.0"))
    float AlertFacingRadius = 1500.0f;

    // Sprint 时是否退出警觉（true：sprint 期间不旋转上半身，让整身跟速度走；与图片中"切 sprint 会退出靶向移动"一致）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|AlertFacing")
    bool bExitAlertOnSprint = true;

    // 上半身 YawOffset 平滑速度（度/秒）：警觉过渡插值速率，越大越"硬切"
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|AlertFacing", meta = (ClampMin = "0.0"))
    float AlertFacingInterpSpeed = 360.0f;

    // 警觉激活权重（0/1，由 Tick 中按条件计算）：0 = 完全不警觉（上半身跟 Actor 朝向）；1 = 完全警觉（上半身跟目标）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|AlertFacing")
    float UpperBodyYawWeight = 0.0f;

    // 目标方向相对 Actor 朝向的 Yaw 差（-180..180 度）：传给 ABP Warping 节点（GoalYaw）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|AlertFacing")
    float UpperBodyYawOffset = 0.0f;

    // Tick：每帧按条件计算警觉权重 + 平滑 YawOffset
    void UpdateUpperBodyFacingTarget(float DeltaTime);

    // 工具：把 ToTarget（水平）方向转成相对 Actor 的 Yaw 差并写入 UpperBodyYawOffset（带平滑）
    void SetUpperBodyYawOffsetToFaceTarget(const FVector& ToTargetFlat, float DeltaTime);

    // Tick：维护索敌目标（最近优先/更近者持续换锁）+ 每帧刷新追踪 UI 位置与显隐
    void UpdateLockOn(float DeltaTime);

    // 返回当前有效索敌目标：锁定存在、存活且在 LockOnRadius 内；否则 nullptr
    AMonsterBase* GetValidLockedTarget() const;

    // 每帧刷新索敌提示 UI：投影目标头顶追踪点到屏幕，离屏/背后隐藏
    void UpdateLockOnIndicator();

    // 懒创建索敌提示 Widget（AddToViewport + 中心对齐 + 默认隐藏）
    void CreateLockOnIndicatorWidget();

    // 隐藏索敌提示 UI（无目标/离屏/未配置类时调用；不销毁，复用实例）
    void HideLockOnIndicator();

    // ---- 技能/大招段播放与结算 ----
    // 播放技能第 Index 段（绑定段结束回调、重置段内伤害记录）
    void PlaySkillSegment(int32 Index);

    // 播放大招第 Index 段
    void PlayUltimateSegment(int32 Index);

    // 技能段蒙太奇结束回调：正常结束 → 推进下一段；被打断 → 结束技能进入冷却
    UFUNCTION()
    void OnSkillSegmentMontageEnded(UAnimMontage* Montage, bool bInterrupted);

    // 大招段蒙太奇结束回调：正常结束 → 推进下一段（大招不会被主动打断）
    UFUNCTION()
    void OnUltimateSegmentMontageEnded(UAnimMontage* Montage, bool bInterrupted);

    // 结束技能（正常播完/被打断均调用）：停止蒙太奇、解锁操作、进入冷却
    void EndSkill();

    // 结束大招：停止蒙太奇、解锁操作、进入冷却
    void EndUltimate();

    // ---- 能量技（Q 键）----
    // EnergySkill() 的输入处理声明在上方"输入处理函数"区（与 Skill/Ultimate 并列）
    // 技能实际施放成功时扣血并累积 1 格能量（血量不足保底扣至 1 点）
    void SpendHealthForEnergy();

    // 累积 1 格能量并刷新 Passive_eng_1~4（已满则忽略）
    void GainEnergyStack();

    // 刷新能量进度条（WBP_HealthBar 的 Passive_eng_1~4：有能量的格子=1，否则=0）
    void UpdateEnergyBars();

    // Tick：能量技攻击窗口检测（与技能/大招段同款接触伤害结算）
    void UpdateEnergySkillCombat(float DeltaTime);

    // 能量技蒙太奇结束回调（正常结束/被打断均结束施放状态）
    UFUNCTION()
    void OnEnergySkillMontageEnded(UAnimMontage* Montage, bool bInterrupted);

    // 结束能量技施放状态（清施放标记/蒙太奇引用）
    void EndEnergySkill();

    // 增伤结束：关闭 buff、清空全部能量并刷新进度条（定时器回调）
    void OnEnergyBuffEnded();

    // 能量技当前是否可释放（4 格能量 + 不在冷却 + 增伤未激活）
    bool IsEnergySkillUnlocked() const;

    // 应用大招镜头效果（画面放大 UltimateZoomFovMultiplier 倍 + 锁定视角旋转）
    void ApplyUltimateCameraEffect();

    // 恢复大招镜头效果（恢复原始 FOV、解锁视角旋转）
    void RestoreUltimateCameraEffect();

    // 当前技能段是否可被闪避打断（技能未播放中返回 false）
    bool IsCurrentSkillSegmentDodgeInterruptible() const;

    // Tick：技能段攻击窗口检测（窗口内与怪物模型接触 → 按 HitInterval 结算伤害）
    void UpdateSkillSegmentCombat(float DeltaTime);

    // Tick：大招段攻击窗口检测
    void UpdateUltimateSegmentCombat(float DeltaTime);

    // 段内接触伤害结算（技能/大招共用）：攻击窗口开启时检测角色胶囊与怪物模型包围盒
    // 是否接触，按段的 Multiplier（技能倍率）与 HitInterval 对每个怪物重复结算
    // 技能/大招/能量技段通用命中结算；返回本次实际命中（成功结算伤害）的怪物数
    int32 ApplySegmentContactDamage(const FComboMontageSegment& Segment, TMap<TWeakObjectPtr<AMonsterBase>, float>& HitTimes);

    // ---- 武器系统 ----
    // 当前装备的武器（null = 空手，攻击不附带武器判定）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
    class AWeaponBase* CurrentWeapon = nullptr;

    // 开局自动装备的武器类（在 BP_PlayerCharacter 里配置，留空则空手）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
    TSubclassOf<class AWeaponBase> DefaultWeaponClass;

    // ★ 每个角色类【当前装备的武器】（运行时、跨实例共享，绕开 CDO 继承链）。
    //
    // 为什么不能再用 CDO 的 DefaultWeaponClass 存「运行时切换后的装备」：
    //   UE 蓝图 CDO 有继承语义 —— BP_Chara_magic 继承自 BP_PlayerCharacter 时，
    //   子类 CDO 的 DefaultWeaponClass 会继承父类 CDO 的值（除非子类蓝图显式改过）。
    //   于是「给 BP_PlayerCharacter 切换武器 → 写父类 CDO → BP_Chara_magic 继承到」，
    //   两个角色的面板都显示同一把武器（串号）。这是本表要修掉的 bug。
    //
    // 规则：
    //   · key = 角色类（如 BP_PlayerCharacter_C），value = 该角色当前装备的武器类。
    //   · 只有「运行时切换武器」才写这里；开局自动装备仍用各角色类自己的 DefaultWeaponClass。
    //   · 面板查看某个角色时，先查这里，查不到再回退到该角色类 CDO 的 DefaultWeaponClass。
    //     （这样 BP_PlayerCharacter 切武器只写它自己的 key，BP_Chara_magic 面板读不到 → 不串号。）
    // static：其他角色（队友）只有 CDO 没有实例，装备状态要跨实例、全局一份才查得到。
    static TMap<UClass*, TSubclassOf<class AWeaponBase>> ClassEquippedWeapon;

    // ★ 武器实例占用表（「同类不同把只能一个角色装备」的判据）。
    //   key = "行名#实例号"（如 "1#0" 表示行 '1' 的第 0 把），value = 装备它的角色类。
    //   装备时查：这把武器实例若已被【别的角色类】占用 → 拒绝；卸下/换装时释放。
    //   同一角色类换装（先卸旧再装新）不视为冲突 —— 由「先卸下旧武器」的顺序保证。
    static TMap<FString, UClass*> WeaponInstanceOwner;

    // ---- 职位系统 ----
    // 角色职位（剑士 / 法师 / 枪手）。在 BP_PlayerCharacter 的 Details 里设置。
    // ★ 决定两件事：① 只能装备对应武器类别的武器（装备拦截 + 背包列表过滤）
    //              ② 武器吸附插槽（剑士/枪手 → HandGrip_R，法师 → magic_WeaponSocket）。
    // ★ 枪手为占位：枚举已建、映射已建，但枪械武器暂未开发。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Job")
    EJobClass JobClass = EJobClass::BladeMaster;

    // ---- 跳跃系统 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump")
    int32 MaxJumpCount = 2;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Jump")
    int32 CurrentJumpCount = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump")
    float JumpHeight = 600.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jump")
    float DoubleJumpHeight = 500.0f;

    // ---- 连击系统 ----
    // ★★ 段数怎么改（0 基础版）：
    //    打开 BP_PlayerCharacter → 右侧 Details 面板 → 搜 "Combo" → 找到
    //    【Combo Attack Segments】数组 → 点 + 号加一段 / 选中元素点 - 号删一段。
    //    数组里有几个元素就是几段，出招顺序 = 数组顺序（第 1 个元素 = 第 1 段）。
    //
    // ★ CurrentComboStep / MaxComboStep 都是【运行时派生值】，只读显示、不用手填：
    //    MaxComboStep 由 EnsureComboSegments() 从 ComboAttackSegments 的长度算出。
    //    两个真相源必然打架，所以现在只留【数组】一个真相源。
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combo")
    int32 CurrentComboStep = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combo")
    int32 MaxComboStep = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
    float ComboResetTime = 1.5f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combo")
    float ComboTimer = 0.0f;

    // ★ 普攻连段的段列表 —— 这就是「段数」的唯一真相源。
    // 加元素 = 加一段（数组长度即段数）。每段可配：蒙太奇 + 可打断窗口。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
    TArray<FAttackComboSegment> ComboAttackSegments;

    // 是否启用普攻段间打断窗口（false = 回到旧行为：普攻之间不受动画进度限制，仅受 AttackCooldown 约束）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combo")
    bool bEnableComboCancelWindow = true;

    // ---- 摄像机系统 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    USpringArmComponent* CameraBoom;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    UCameraComponent* FollowCamera;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    float CameraWorldYaw = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    float CameraWorldPitch = -10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
    float CameraSensitivity = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
    float CameraMinDistance = 200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
    float CameraMaxDistance = 1200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
    float CameraZoomSpeed = 50.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
    float CameraResetSpeed = 5.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    bool bIsResettingCamera = false;

    float TargetYaw = 0.0f;
    float TargetPitch = -10.0f;

    // ---- 相机防贴脸推开 ----
    // 当摄像机与小怪/Boss 距离过近时，沿相机臂方向把摄像机往远处推开；
    // 距离恢复后摄像机自动回到玩家设定的原相机位置（推开期间玩家缩放仍生效）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Pushback")
    bool bEnableCameraPushback = true;

    // 触发距离（cm）：摄像机（原始位置）与小怪/Boss 距离小于该值时推开摄像机
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Pushback", meta = (ClampMin = "0.0"))
    float CameraPushbackTriggerDistance = 10.0f;

    // 推开距离（cm）：沿相机臂方向往远处拉开的量
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Pushback", meta = (ClampMin = "0.0"))
    float CameraPushbackAmount = 15.0f;

    // 推开/收回的平滑速度（值越大响应越快，0.01~20 合理区间）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Pushback", meta = (ClampMin = "0.01"))
    float CameraPushbackInterpSpeed = 10.0f;

    // 当前是否处于推开状态
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera|Pushback")
    bool bCameraPushbackActive = false;

    // 玩家设定的相机臂长（缩放只改这个值；推开偏移叠加在其上，收回时回到它）
    float UserCameraArmLength = 500.0f;

    // 当前推开偏移（平滑插值中）
    float CurrentCameraPushback = 0.0f;

    // ---- 弹刀镜头拉近 ----
    // 弹刀成功时相机沿臂方向拉近一段距离，持续设定时间后平滑恢复原距离。
    // 与玩家滚轮缩放、防贴脸推开共存：最终臂长 = 玩家设定 + 推开偏移 - 弹刀拉近
    // 拉近距离（cm）：0=禁用弹刀镜头拉近
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.0"))
    float ParryCameraZoomInDistance = 150.0f;

    // 拉近保持时间（秒）：时间到后相机平滑恢复原距离
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.0"))
    float ParryCameraZoomDuration = 0.6f;

    // 拉近/恢复的平滑速度（值越大切换越快，0.01~30 合理区间）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.01"))
    float ParryCameraInterpSpeed = 12.0f;

    // 拉近剩余保持时间（Tick 递减，>0 期间保持拉近，归零后平滑恢复）
    float ParryCameraZoomTimeRemaining = 0.0f;

    // 当前实际拉近量（平滑插值中）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Camera")
    float CurrentParryCameraZoomIn = 0.0f;

    // 弹刀成功时触发镜头拉近（距离/时长 <= 0 时无操作）
    void TriggerParryCameraZoom();

    // ---- 命中镜头振动（大招段攻击窗口命中怪物时触发，强化大招打击反馈）----
    // 触发后相机在自身右方向小幅左右往复振动（正弦衰减），持续设定时长后自然停止。
    // 与弹刀镜头拉近/防贴脸推开共存：振动只改 FollowCamera 相对位置，不动臂长。
    // 弹刀/普攻/技能/能量技命中敌方不触发振动
    // 总开关：false 时不产生任何镜头振动
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera")
    bool bEnableHitCameraShake = true;

    // 触发延迟（秒）：命中后延迟该时间才开始振动（0=立即振动）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.0"))
    float HitCameraShakeDelay = 0.0f;

    // 振动幅度（cm）：相机左右偏移的最大距离
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.0"))
    float HitCameraShakeAmplitude = 3.0f;

    // 振动持续时间（秒）：期间幅度线性衰减至 0，结束时相机回到原位
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.0"))
    float HitCameraShakeDuration = 0.2f;

    // 振动频率（Hz）：每秒左右往复次数，越大抖得越密集
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Camera", meta = (ClampMin = "0.1"))
    float HitCameraShakeFrequency = 30.0f;

    // 大招段命中时触发镜头振动（开关/幅度/时长不合法时无操作；振动中再次命中会重置重新计时）
    // 注：声明于类内 public 区（见 UpdateUltimateSegmentCombat）；唯一触发点是大招段命中结算

    // 每帧驱动命中振动（Tick 调用）：延迟计时 → 正弦衰减振动 → 归零复位相机相对位置
    void UpdateHitCameraShake(float DeltaTime);

    // 命中振动延迟剩余（>0 期间倒计时，归零后进入振动期）
    float HitShakeDelayRemaining = 0.0f;

    // 命中振动剩余时间（>0 期间相机按正弦衰减左右偏移）
    float HitShakeTimeRemaining = 0.0f;

    // 命中振动已进行时间（相位计算用）
    float HitShakeElapsed = 0.0f;

    // ---- 完美闪避 UI（屏幕空间）----
    // UI 在屏幕中心偏右的横向偏移（像素，正值向右）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|PerfectDodgeUI")
    float PerfectDodgeUIOffsetX = 160.0f;

    // UI 相对屏幕中心的纵向偏移（像素，正值向下）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|PerfectDodgeUI")
    float PerfectDodgeUIOffsetY = 0.0f;

    // UI 文字大小
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|PerfectDodgeUI", meta = (ClampMin = "8"))
    int32 PerfectDodgeUIFontSize = 46;

    // UI 文字颜色（默认青色）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|PerfectDodgeUI")
    FLinearColor PerfectDodgeUIColor = FLinearColor(0.35f, 1.0f, 1.0f, 1.0f);

    // 自定义完美闪避 UI 资产（Widget Blueprint 类）：配置后优先使用该资产，
    // 未配置时回落到内置 C++ 生成的文字控件
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|PerfectDodgeUI")
    TSubclassOf<UUserWidget> PerfectDodgeUIClass;

    // 是否应用内置淡入淡出/上移动画（自定资产带自身入场动画时可关闭）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|PerfectDodgeUI")
    bool bAnimatePerfectDodgeUI = true;

    // 当前活跃的 UI 控件（nullptr = 无 UI 在播放）
    UPROPERTY(Transient)
    UUserWidget* PerfectDodgeUIWidget = nullptr;

    // UI 内的文字块（用于动画更新）
    UPROPERTY(Transient)
    UTextBlock* PerfectDodgeUITextBlock = nullptr;

    // UI 已播放时长（秒，Tick 递增，超过总时长则移除控件）
    float PerfectDodgeTextElapsed = 0.0f;

    // 显示完美闪避 UI（纯 C++ 构建 CanvasPanel+TextBlock，屏幕中心偏右，淡入淡出后自动移除）
    void ShowPerfectDodgeText();
    // 每帧更新 UI（淡入 + 轻微上移 + 淡出 + 超时移除）
    void UpdatePerfectDodgeText(float DeltaTime);
    // 移除 UI（EndPlay/销毁时清理，防止控件残留在视口）
    void HidePerfectDodgeUI();

    // 每帧推开检测与应用（Tick 调用）
    void UpdateCameraPushback(float DeltaTime);

    // ---- 移动控制 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Movement")
    FVector2D MovementInputValue = FVector2D::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    bool bIsMovingInput = false;

    FVector CurrentMoveDirection = FVector::ZeroVector;

    // ---- 移动手感：起步加速 / 停步制动 / 转身速率（对标鸣潮的「有惯性但不粘滞」手感）----
    // 起步加速度（cm/s²）：从静止到满速所需时间 ≈ 移速 / 该值。
    // 越大起步越干脆（鸣潮偏干脆，推荐 4000~8000）；过小会显得「推不动」。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "100.0", UIMin = "2000.0", UIMax = "12000.0"))
    float MoveAcceleration = 5000.0f;

    // 停步制动力（cm/s²）：松开方向键后的减速速率，决定「滑行距离」。
    // 越大停得越干脆（鸣潮偏干脆，推荐 4000~9000）；过小会有明显滑步。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "100.0", UIMin = "2000.0", UIMax = "14000.0"))
    float MoveBrakingDeceleration = 6000.0f;

    // 移动转身速率（度/秒）：角色朝移动方向转身的速度。
    // 越大转身越快（鸣潮急转干脆，推荐 720~1440）；过小会「甩尾」迟滞。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "30.0", UIMin = "360.0", UIMax = "2160.0"))
    float MoveTurnSpeedDegPerSec = 1080.0f;

    // 移动中当前速度矢量（供惯性插值使用，避免每帧硬赋值导致起步/停步瞬变）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    FVector SmoothedMoveVelocity = FVector::ZeroVector;

    // ---- 疾跑系统 ----
    // 疾跑中（速度已提升、耐力持续消耗中）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    bool bIsSprinting = false;

    // 疾跑速度倍率（疾跑速度 = 基础移速 × 此倍率）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement")
    float SprintSpeedMultiplier = 1.5f;

    // 疾跑每秒消耗的耐力
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement")
    float SprintStaminaDrainPerSecond = 20.0f;

    // 疾跑最大持续时间（秒）：到达后自动停止加速
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "0.1"))
    float SprintDuration = 3.0f;

    // 疾跑已持续时间（Tick 递增，>= SprintDuration 时停止）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    float SprintElapsedTime = 0.0f;

    // 基础移速（BeginPlay 时从 CharacterMovement 记录，疾跑结束后恢复到该值）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    float BaseWalkSpeed = 600.0f;

    // BeginPlay 时记录的 bOrientRotationToMovement 初值：
    // 任何需要临时关掉「朝向移动方向」的功能，结束时都应按此初值恢复（否则会永久改掉蓝图配置）。
    bool bEnableOrientRotationToMovementDefault = false;


    // ---- 步行系统（Ctrl 切换）----
    // 步行中：移速锁定为 WalkSpeed（绝对速度），不受疾跑/其他因素影响
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    bool bWalking = false;

    // 步行锁定速度（cm/s）：直接填想要的步行速度，自由调整，不再按基础移速比例折算
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "1.0", ClampMax = "3000.0", UIMin = "50.0", UIMax = "1200.0"))
    float WalkSpeed = 300.0f;

    // 步行动画播放速率倍率（仅步行期间生效）：作用于整个角色动画的全局播放速率。
    // 1.0 = 正常速度，>1 加快脚步，<1 放慢脚步（慢动作）。与 WalkSpeed（移动速度）相互独立，
    // 可分别调节「实际移动快慢」与「动画播放快慢」。退出步行后自动恢复 1.0。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "0.1", ClampMax = "5.0", UIMin = "0.5", UIMax = "2.0"))
    float WalkAnimPlayRate = 1.0f;

    // ---- UI组件 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    class UWidgetComponent* StaminaBarWidget;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> StaminaBarWidgetClass;

    // 耐力条固定绘制尺寸（屏幕像素，不随摄像机缩放变化）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D StaminaBarDrawSize = FVector2D(200.0f, 30.0f);

    // 耐力条相对"屏幕中心"的偏移（单位：屏幕像素）。
    // X：正值=屏幕右方，负值=左方；Y：正值=屏幕下方，负值=上方。
    // 耐力条锁定在摄像机画面上（屏幕中心+此偏移），与角色/胶囊体位置无关。
    // 调整方式：打开 BP_PlayerCharacter → Details → UI → Stamina Bar Screen Offset
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D StaminaBarScreenOffset = FVector2D(300.0f, 0.0f);

    // 当前耐力百分比（用于更新UI）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    float StaminaPercent = 1.0f;

    // ---- 耐力条显隐控制 ----
    // 耐力无变动多长时间后隐藏UI（秒）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float StaminaBarHideDelay = 2.0f;

    // 上次耐力变动的时间戳
    float LastStaminaChangeTime = 0.0f;

    // 上次记录的耐力值（用于检测变动）
    float LastStaminaValue = -1.0f;

    // 耐力条当前是否可见
    bool bStaminaBarVisible = false;

    // ---- 生命值系统 ----
    // 伤害 GameplayEffect（P1 GAS 化）：受击/静默伤害通过该 GE 的 Execution 结算（护盾吸收+扣血）。
    // 默认使用 C++ 内置 UDamageGameplayEffect；可在角色蓝图替换为自定义 GE。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
    TSubclassOf<UDamageGameplayEffect> DamageEffectClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
    float MaxHealth = 100.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
    float CurrentHealth = 100.0f;

    // ---- 攻击力（技能倍率的伤害基数）----
    // 角色基础攻击力，作为「技能倍率(Multiplier)」结算伤害的基数：
    //   最终伤害 = 攻击力 × 技能倍率 × 增伤buff
    // 在角色蓝图里自行设置（默认 100，与 GAS Attack 属性对齐）。
    // BeginPlay 时写入 AttributeSet->Attack（GAS 唯一真源），伤害结算读 AttributeSet->GetAttack()。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack", meta = (ClampMin = "0.0"))
    float BaseAttack = 100.0f;

    // ---- 暴击属性（整数百分比语义，皆可在蓝图调整）----
    // 暴击率（整数百分比，5 = 5% 概率触发暴击）：触发暴击时伤害 × 暴击伤害。
    // 暴击伤害（整数百分比，150 = 150% = 1.5 倍伤害）：仅在触发暴击时才计入伤害公式。
    // 两者 BeginPlay 时写入 AttributeSet（GAS 唯一真源），伤害结算读 AttributeSet->GetCritRate()/GetCritDamage()。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack", meta = (ClampMin = "0.0"))
    float BaseCritRate = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Attack", meta = (ClampMin = "0.0"))
    float BaseCritDamage = 150.0f;

    // 角色数值等级（char_LV_num 显示用）：在角色蓝图里设置，实时同步到 WBP_HealthBar 的 char_LV_num 文本
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health", meta = (ClampMin = "1", ClampMax = "999"))
    int32 CharacterLevel = 1;

    // ---- 角色基础信息（角色面板展示用）----
    // 角色游戏名称：在角色蓝图里设置，显示到 WBP_Character_imf 的 chara_name 文本控件
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character")
    FString CharacterName = TEXT("");

    // 角色图像（头像/立绘，UTexture2D）：在角色蓝图里设置，显示到 WBP_Character_imf 的 pict_chara 图像控件
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character")
    class UTexture2D* CharacterPortrait = nullptr;

    // chara_pitc_head 内角色头像按钮（chara_1、chara_2...）绑定的角色类。
    // 索引 0 → chara_1，索引 1 → chara_2 ... 点击按钮时读取对应 CDO 默认属性
    // （CharacterName/CharacterPortrait/总攻击值/最大生命值/暴击率/暴击伤害）填充面板。
    // 元素留空表示该槽位未绑定角色。
    // ★【兼容角色】Data_chara_imfor 数据表配置好后，头像列表以表为准；本数组退为「表无效时的兜底」。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character")
    TArray<TSubclassOf<ABattleCharacter>> CharacterSlotClasses;

    // ---- 角色信息表与编队（Data_chara_imfor）----
    // ★ 角色面板头像列表 / 编队界面（L 键）都从这一张表同步：
    //   每行 = 一个角色（蓝图类 + 显示名 + 头像 + 立绘）。
    //   新增角色 = 表里加一行，两处 UI 自动出现，无需改任何代码。
    //
    // 留空 → 首次使用时懒加载 /Game/UI/chara_imf/Data_chara_imfor（约定路径）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character|CharaInfo")
    UDataTable* CharaInfoTable = nullptr;

    /** 「拥有的角色」行名列表（过滤 Data_chara_imfor）。
     *  留空 = 表里所有行都算拥有（最省配置：建好表填好角色立即全部生效）；
     *  非空 = 只显示列表里的行（做角色获取/解锁时用）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character|CharaInfo")
    TArray<FName> OwnedCharaRows;

    /** 各队伍的成员（默认 8 队，界面从左往右 = MemberRows[0..2]）。
     *  行名对应 Data_chara_imfor；None = 该槽位为空。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character|CharaTeam")
    TArray<FCharaTeamMembers> Teams;

    /** 当前查看/编辑的队伍下标（0 基；界面标题显示 +1） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Character|CharaTeam")
    int32 CurrentTeamIndex = 0;

public:
    /** 当前队伍下标（编队 UI 从这里读 —— 字段在 protected 区） */
    UFUNCTION(BlueprintPure, Category = "CharacterPanel")
    int32 GetCurrentTeamIndex() const { return CurrentTeamIndex; }

    /** 切换查看/编辑的队伍（自动夹到有效范围）。 */
    UFUNCTION(BlueprintCallable, Category = "CharacterPanel")
    void SetCurrentTeamIndex(int32 NewIndex)
    {
        EnsureTeamsArray();
        CurrentTeamIndex = FMath::Clamp(NewIndex, 0, Teams.Num() - 1);
    }

    /** 头像格子控件类 getter（供编队 UI 复用 WBP_CharaHead_Slot 建快速编队网格）。
     *  内部懒加载约定路径 /Game/UI/WBP_CharaHead_Slot（与 RebuildCharaHeadList 同一份）。 */
    TSubclassOf<UUserWidget> GetCharaHeadSlotClass();

    // =====================================================================
    // ---- 切人系统（1/2/3 键；鸣潮式切人）----
    // =====================================================================
    // 按键 N → 当前队伍（Teams[CurrentTeamIndex]）第 N-1 槽的成员行名 →
    // Data_chara_imfor 查角色类 → spawn 新实例 + PlayerController Possess 切换。
    //
    // 三种切人形态（对应鸣潮）：
    //   ① 技能中（E/Q，大招除外）切人：操控立刻转移；旧角色留在原地继续把技能放完，
    //      放完自动销毁（EndSkill 收敛点）；新角色出现在【怪物附近】且不与旧角色重合的点。
    //   ② 普攻中切人：仅在当前段的可打断窗口（CancelWindowTime，蓝图已配）内允许；
    //      新角色出现在旧角色位置并自动打出第 SwitchInAttackComboStep+1 段普攻（蓝图可配）；
    //      旧角色立即销毁。
    //   ③ 其余（含移动中）：原地切换；新角色继承旧角色的移速向量 → 移动动画自然衔接；
    //      旧角色立即销毁。
    // 拒绝条件：目标在切人 CD（0.5s，按角色行名相互独立）｜大招中｜受击硬直中｜死亡｜
    //           槽位为空｜目标就是当前角色。
    //
    // 每次成功切人后：换上 + 换下的两个角色行名都进 0.5s CD（右上角编队 HUD 显示遮罩+秒数）。

public:
    /** 请求切换到当前队伍的第 SlotIndex 槽（0 基；1/2/3 键 → 0/1/2）。 */
    UFUNCTION(BlueprintCallable, Category = "Character|Switch")
    void RequestSwitchToTeamSlot(int32 SlotIndex);

    /** 该角色行名是否在切人 CD 中（右上角 HUD 与按键拦截共用）。 */
    UFUNCTION(BlueprintPure, Category = "Character|Switch")
    bool IsSwitchCooldownActive(FName CharaRow) const;

    /** 该角色行名的切人 CD 剩余秒数（0 = 可切）。 */
    UFUNCTION(BlueprintPure, Category = "Character|Switch")
    float GetSwitchCooldownRemaining(FName CharaRow) const;

protected:
    // ---- 切人输入（动态 IMC：数字键 1/2/3，与背包 B 键同一套 Ensure 幂等模式）----
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Switch|Input")
    UInputAction* SwitchSlotAction1 = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Switch|Input")
    UInputAction* SwitchSlotAction2 = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Switch|Input")
    UInputAction* SwitchSlotAction3 = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Switch|Input")
    UInputMappingContext* SwitchMappingContext = nullptr;
    bool bSwitchContextAdded = false;

    /** ★ 输入绑定已完成的显式标志：SetupPlayerInputComponent 执行后置 true。
     *  不能拿「InputComponent 是否非空」当判据——切人 spawn 的新角色可能在别的路径
     *  里被建出了 InputComponent，但 SetupPlayerInputComponent 从未被调用，那样
     *  「补绑」会被误判为「已绑」而跳过，1/2/3 键整体失灵（切不回去）。 */
    bool bPlayerInputBound = false;

    /** 幂等创建 1/2/3 的 IA/IMC 并挂到本地子系统（挂上后返回 true）。 */
    bool EnsureSwitchInput();

    /** 幂等补绑输入组件：确保 InputComponent 已创建 + SetupPlayerInputComponent 已执行 + 已注册。
     *  ★ 切人修复的核心：AController::Possess 走 DispatchRestart(false) → 只调 Restart()，
     *    不调 PawnClientRestart() → SetupPlayerInputComponent 根本不会被调用 →
     *    切人 spawn 的新角色 C++ 输入（含 1/2/3 切人键）整体丢失（「切不回去」根因）。
     *    这里在 Possess 后手动补上这条链（幂等：已绑定则跳过）。 */
    void EnsurePlayerInputBound();

    /** 切人后下一帧的输入自愈校验（ExecuteSwitchToRow 用 SetTimerForNextTick 调）：
     *  复核本实例的 1/2/3 映射是否真的在 EnhancedInput 输入栈里；不在（被旧实例残留
     *  映射污染挤掉/清理失效）就补回并打告警。幂等，只补缺不重复加。 */
    void VerifySwitchInputAfterPossess();

    /** 移除本实例的切人 1/2/3 输入映射（形态①技能保留切人、旧角色留场退役时调用）。
     *  ★ 根因：形态①旧角色留场放技能、不销毁，其 SwitchMappingContext（优先级 2）仍挂在
     *    EnhancedInput 输入栈里，与新角色同优先级同键（1/2/3）的映射互相遮蔽 —— 旧角色
     *    先注册、遮蔽新角色，导致技能期间切回键回调只触发旧角色（已被 UnPossess、静默
     *    return）而新角色不触发 → 技能期间「切不回去」。留场时就要摘掉旧角色映射，不能等
     *    EndPlay（旧角色技能放完才销毁，期间映射一直残留遮蔽）。 */
    void RemoveSwitchInputMapping();

    UFUNCTION()
    void OnSwitchSlot1Pressed();
    UFUNCTION()
    void OnSwitchSlot2Pressed();
    UFUNCTION()
    void OnSwitchSlot3Pressed();

    // ---- 切人配置（角色蓝图可调）----
    /** 切人 CD（秒）：成功切人后，换上/换下两个角色各自独立进入该 CD（per-row 独立计时，互不影响）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Switch", meta = (ClampMin = "0.0"))
    float SwitchCooldown = 1.0f;

    /** 普攻中切人时新角色自动打出的普切段号（0 基：0=第1段，1=第2段…）。各角色蓝图可各自设置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Switch", meta = (ClampMin = "0"))
    int32 SwitchInAttackComboStep = 1;

    /** 技能中切人：新角色出生点距怪物中心的距离（cm）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Switch", meta = (ClampMin = "50.0"))
    float SwitchSpawnDistanceFromMonster = 220.0f;

    /** 技能中切人：新角色出生点与旧角色位置的最小间隔（cm），保证「不与原角色重合」。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Switch", meta = (ClampMin = "0.0"))
    float SwitchMinSeparation = 200.0f;

    /** 技能中切人：搜索「怪物附近」的半径（cm）。找不到存活怪物 → 退化为旧位置侧移出生。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Switch", meta = (ClampMin = "100.0"))
    float SwitchCombatMonsterSearchRadius = 1500.0f;

    // ---- 切人内部流程 ----
    /** 切人主入口的共享实现（区分技能保留 / 普攻衔接 / 原地切换三种形态）。 */
    bool ExecuteSwitchToRow(int32 SlotIndex, FName TargetRow);

    /** ★ 把操控权归还给「已在场存活」的目标实例（技能中切人旧角色留场放技能、切回时不 spawn
     *  新实例而是重新 Possess 它）。成功返回 true。负责：取消待自毁标志、重新 Possess、
     *  补绑输入、重建 HUD、恢复血条显示、记录 CD、处置当前实例。 */
    bool ResumeControlToExistingInstance(ABattleCharacter* ExistingChar, FName TargetRow);

    /** 反查：这个角色类对应 Data_chara_imfor 的哪一行（表里没有 → NAME_None）。 */
    FName FindCharaRowForClass(const UClass* CharaClass);

    /** 当前实例在编队里的行名（缓存；表里没有 → NAME_None）。 */
    FName GetMyCharaRow();

    /** 半径内最近的存活怪物（切人进场点用；锁定目标优先）。 */
    class AMonsterBase* FindNearestAliveMonsterForSwitch(float Radius);

    /** 技能中切人的进场点：怪物周围取「不与旧角色重合」的点（失败 → 旧位置侧移）。 */
    FVector FindSwitchSpawnPoint(const FVector& OldLoc, bool& bOutNearMonster);

    /** 把「玩家级」状态从旧实例搬到新实例：编队数据 + 背包运行时数据 + 角色表缓存。 */
    void TransferPlayerStateTo(ABattleCharacter* Target);

    /** 退役前释放武器占用并把装备行名记到 per-class 表（切回来时恢复装备）。 */
    void ReleaseWeaponClaimForRetire();

    /** 移除本实例创建的屏幕 UI（技能图标 HUD 等），防止切人后残留叠加。 */
    void CleanupRetiredScreenUI();

    /** 恢复该角色类上次装备的背包武器（切回时由 BeginPlay 调用）。 */
    void TryRestoreEquippedWeaponFromClaim();

    /** 技能保留切人：旧角色技能放完后自毁的标志（EndSkill 收敛点检查）。 */
    bool bPendingDestroyAfterSkillFinish = false;

    /** 切人重入保护（一次切人流程未完成前忽略新请求）。 */
    bool bSwitchInProgress = false;

    /** 本实例在编队里的行名缓存（首次 GetMyCharaRow 时反查表并缓存）。 */
    FName CachedMyCharaRow = NAME_None;

    // ---- 切人 CD 与装备保持（玩家级状态，跨角色实例共享 → static）----
    /** 行名 → CD 结束的世界时间（秒）。static：切人后新实例必须能看到旧角色设下的 CD。 */
    static TMap<FName, double> SwitchCooldownUntilRow;
    /** 角色类 → 上次装备的背包行名（切出时记录，切回时恢复）。 */
    static TMap<UClass*, FName> ClassEquippedBagRow;
    /** ★ 行名 → 当前在场存活的角色实例（static 注册表）。用于切人时判断「目标角色是否已有一份
     *  实例在场」——技能中切人（形态①）旧角色留场放技能不销毁，切回时若仍存活就直接把操控权
     *  归还给它，而不是再 spawn 一个重复实例（否则会出现「原角色多出一个新的」）。
     *  spawn/初始角色注册，EndPlay 注销（value==this 才移除，防止 A 销毁时误删 B 的登记）。 */
    static TMap<FName, class ABattleCharacter*> ActiveCharaInstances;

    // ---- 右上角编队 HUD（头像 + 当前高亮框 + CD 进度条/秒数 + 槽位数字）----
    // 两套路径：
    //   ① WBP 路径（推荐）：蓝图上填 SwitchHUDClass = WBP_SwitchHUD，C++ 按名字 FindWidget 填数据。
    //      控件约定名（WBP 内）：slot_0~2（每个 Overlay 格子，内含 frame/img_icon/txt_key/cd_mask/txt_cd）。
    //   ② 纯 C++ 兜底（没配 WBP 时）：代码搭树，样式是简易的色块 + 头像。
    /** BeginPlay / 切人 Possess 后构建；编队成员变更需重建（重新调用本函数）。 */
    void BuildSwitchTeamHUD();
    /** WBP 路径：配了 SwitchHUDClass 时，从 WBP 按名字取控件并填数据。 */
    void BuildSwitchTeamHUD_FromWBP(const TArray<FName>& Rows);
    /** 每帧刷新：CD 进度条比例/秒数、当前操控高亮框。 */
    void UpdateSwitchTeamHUD();
    /** 移除并清空 HUD（实例退役时调用，防止残留）。 */
    void RemoveSwitchTeamHUD();

    /** HUD 展示用的队伍成员（含「未编队默认出战」兜底）：队伍全空时把当前角色自己
     *  填进 slot_0。Build / Update 两处共用同一兜底逻辑，避免比较不一致导致每帧重建。 */
    TArray<FName> GetEffectiveTeamMembersForHUD();

    /** 切人 HUD 的 Widget Blueprint 类（可选）：填了就走 WBP 路径（可视化 UI），
     *  留空走纯 C++ 兜底。在 BP_PlayerCharacter / BP_Chara_magic 的 Switch 分类里配。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Switch")
    TSubclassOf<UUserWidget> SwitchHUDClass = nullptr;

    UPROPERTY()
    TObjectPtr<UUserWidget> SwitchTeamHUDWidget = nullptr;
    // ★ bUsingWBP 标记：区分「WBP 路径（FindWidget 拿控件）」与「纯 C++ 兜底路径（代码搭树）」
    bool bSwitchHUDUsesWBP = false;
    UPROPERTY()
    TArray<TObjectPtr<UWidget>> SwitchHudSlotWidgets;   // 每个 slot 格子容器（空槽隐藏整格）
    UPROPERTY()
    TArray<TObjectPtr<UWidget>> SwitchHudFrameWidgets;  // 当前操控高亮框（金框，WBP 路径为任意 UWidget）
    UPROPERTY()
    TArray<TObjectPtr<UWidget>> SwitchHudIconWidgets;   // 头像（UImage）
    UPROPERTY()
    TArray<TObjectPtr<UWidget>> SwitchHudMaskWidgets;   // CD 进度条（UProgressBar，WBP 路径）或遮罩（兜底）
    UPROPERTY()
    TArray<TObjectPtr<UWidget>> SwitchHudCdTextWidgets; // CD 剩余秒数（UTextBlock）
    UPROPERTY()
    TArray<FName> SwitchHudRows;                        // 每格对应的编队行名

protected:
    // ---- 护盾系统（E 技能施放时获得）----
    // 护盾量 = 最大血量 × 护盾比例（默认 8%）：施放 E 技能成功时获得。
    // 受击时优先扣除护盾，超出部分才扣血；护盾无法叠加，每次施放 E 刷新为满值。
    // E 技能的自耗血（SpendHealthForEnergy）直接改 CurrentHealth，不经过 TakeDamage，天然不扣护盾。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Shield", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ShieldMaxHPRatio = 0.08f;

    // 当前护盾量（护盾扣完归 0，护盾状态随之消失）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health|Shield")
    float CurrentShield = 0.0f;

    // 护盾状态图标资产（UTexture2D）：显示在 WBP_HealthBar 的 state_01~04 中，
    // 默认加载 /Game/UI/tip_UI/state_UI/shield_up，可在角色蓝图自由替换
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Shield")
    class UTexture2D* ShieldIconTexture = nullptr;

    // 护盾占用的状态图标槽位索引（0~3 对应 state_01~04；-1 = 未占用；由 RefreshStatusIcons 动态分配）
    int32 ShieldSlotIndex = -1;

    // 护盾获得时间戳（世界时间，用于「按获得时间顺序」分配状态图标槽位）
    float ShieldApplyTime = 0.0f;

    // ---- 中毒状态（Boss 毒刺命中施加）----
    // 中毒 GameplayEffect（P4）：替代旧 bIsPoisoned 布尔 + UpdatePoison Tick 扣血。
    // 应用该 GE（HasDuration + Periodic）→ 目标拥有 State.Poisoned Tag，周期结算毒伤，
    // 到期自动移除 Tag。配置后 ApplyPoison 通过 ASC 应用该 GE；留空则回退旧手写逻辑。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Poison")
    TSubclassOf<UPoisonGameplayEffect> PoisonEffectClass;

    // 中毒持续时间（秒）：期间持续扣除最大血量百分比（默认 10s）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Poison", meta = (ClampMin = "0.0"))
    float PoisonDuration = 10.0f;

    // 中毒每秒扣除的最大血量百分比（0.03 = 每秒扣 3% 最大血量）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Poison", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PoisonDamagePerSecondRatio = 0.03f;

    // 中毒扣血间隔（秒）：每隔该秒数结算一次毒伤（默认 1s，避免每帧扣血刷屏）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Poison", meta = (ClampMin = "0.05"))
    float PoisonTickInterval = 1.0f;

    // 中毒运行态：是否处于中毒中（无法重复叠加，中毒期间再次命中不会重置/叠加）
    bool bIsPoisoned = false;

    // 中毒剩余时间（秒，Tick 递减）
    float PoisonRemaining = 0.0f;

    // 中毒扣血累计计时（达到 PoisonTickInterval 结算一次毒伤）
    float PoisonTickTimer = 0.0f;

    // 中毒状态图标资产（UTexture2D）：显示在 WBP_HealthBar 的 state_01~04 中，
    // 默认加载 /Game/UI/tip_UI/state_UI/poisoning，可在角色蓝图自由替换
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Poison")
    class UTexture2D* PoisonIconTexture = nullptr;

    // 中毒占用的状态图标槽位索引（0~3 对应 state_01~04；-1 = 未占用；由 RefreshStatusIcons 动态分配）
    int32 PoisonSlotIndex = -1;

    // 中毒获得时间戳（世界时间，用于「按获得时间顺序」分配状态图标槽位）
    float PoisonApplyTime = 0.0f;

    // ---- 大招增伤状态（Ultimate 施放后获得）----
    // 增伤 GameplayEffect（P4）：替代旧 bUltimateBuffActive 布尔 + UltimateBuffTimerHandle 定时器。
    // 应用该 GE（HasDuration）→ 目标拥有 Buff.DamageBoost Tag，GetOutgoingDamageMultiplier 据此增伤，
    // 到期自动移除 Tag。配置后 ApplyUltimateDamageBuff 通过 ASC 应用该 GE；留空则回退旧手写逻辑。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    TSubclassOf<UDamageBoostGameplayEffect> DamageBoostEffectClass;

    // 增伤幅度（0.1 = +10%）：大招施放后持续 UltimateBuffDuration 秒内自身伤害提升
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate", meta = (ClampMin = "0.0"))
    float UltimateDamageBoost = 0.1f;

    // 大招增伤持续时间（秒；结束后关闭增伤）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate", meta = (ClampMin = "0.1"))
    float UltimateBuffDuration = 10.0f;

    // 大招增伤是否生效中（无法重复叠加：生效期间再次释放大招不重置/叠加）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Ultimate")
    bool bUltimateBuffActive = false;

    // 大招增伤结束定时器（到期关闭增伤 + 清除状态图标）
    FTimerHandle UltimateBuffTimerHandle;

    // 大招增伤状态图标资产（UTexture2D）：默认加载 /Game/UI/tip_UI/state_UI/attack_up，
    // 可在角色蓝图自由替换
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Ultimate")
    class UTexture2D* UltimateBuffIconTexture = nullptr;

    // 大招增伤占用的状态图标槽位索引（0~3 对应 state_01~04；-1 = 未占用；由 RefreshStatusIcons 动态分配）
    int32 UltimateBuffSlotIndex = -1;

    // 大招增伤获得时间戳（世界时间，用于「按获得时间顺序」分配状态图标槽位）
    float UltimateBuffApplyTime = 0.0f;

    // ---- 吸血状态（能量技 buff 期间：攻击回复伤害量百分比）----
    // 吸血状态图标资产（UTexture2D）：显示在 WBP_HealthBar 的 state_01~04 中，
    // 默认加载 /Game/UI/tip_UI/state_UI/suck_blood，可在角色蓝图自由替换。
    // 吸血状态与能量技增伤共用 bEnergyBuffActive 生命周期（能量技释放→持续 EnergySkillBuffDuration 秒）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Energy")
    class UTexture2D* LifestealIconTexture = nullptr;

    // 吸血占用的状态图标槽位索引（0~3 对应 state_01~04；-1 = 未占用；由 RefreshStatusIcons 动态分配）
    int32 LifestealSlotIndex = -1;

    // 吸血获得时间戳（世界时间，用于「按获得时间顺序」分配状态图标槽位）
    float LifestealApplyTime = 0.0f;

    // ---- 血条UI组件 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    class UWidgetComponent* HealthBarWidget;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> HealthBarWidgetClass;

    // 血条固定绘制尺寸（屏幕像素，不随摄像机缩放变化）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D HealthBarDrawSize = FVector2D(400.0f, 40.0f);

    // 血条下边缘距屏幕底部的距离（像素）
    // 调整方式：打开 BP_PlayerCharacter → Details → UI → Health Bar Bottom Margin
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float HealthBarBottomMargin = 60.0f;

    // 血条相对屏幕水平中心的偏移（像素，正值=右移，负值=左移，0=水平居中）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float HealthBarHorizontalOffset = 0.0f;

    // 当前生命百分比（用于更新UI）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    float HealthPercent = 1.0f;

    // ---- 受伤 UI（绑定相机：位置不受视野移动/缩放影响）----
    // 受伤提示控件（屏幕空间，挂在 FollowCamera 上零偏移 → 永远位于屏幕中心）
    UPROPERTY(VisibleAnywhere, Category = "UI")
    class UWidgetComponent* HurtUIWidget;

    // 受伤提示控件类（默认自动加载 /Content/UI/WBP_HurtScreen；未命中时可在蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> HurtUIWidgetClass;

    // 受伤提示固定绘制尺寸（屏幕像素；做成全屏红边只需在控件里铺满 + 调大该值）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D HurtUIDrawSize = FVector2D(1600.0f, 900.0f);

    // 受伤提示显示时长（秒）：受到伤害时显示，经过该时长后隐藏直到下次受伤
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI", meta = (ClampMin = "0.0"))
    float HurtUIDisplayDuration = 0.05f;

    // 受伤提示隐藏定时器
    FTimerHandle HurtUITimerHandle;

    // ---- 获得物品提示 UI（全屏，绑定相机：位置锁屏幕中心 + 绘制尺寸跟随视口）----
    // 与受伤红屏（HurtUIWidget）同一套定位模式：组件全屏铺满视口、控件内部各元素用【锚点】定位，
    // 因此换分辨率也不会歪。
    //
    // 为什么不复用 WBP_HealthBar：血条那个 WidgetComponent 的绘制区域只有 400×40，放不下
    // 屏幕左侧的提示列表（控件超出该区域的内容会被裁掉）。所以要提示就必须另开一块全屏画布。
    //
    // 「击败怪物 → 物品自动进背包 → 屏幕左侧弹出「物品名 ×N」」的提示列表就做在这个控件里
    // （对应的蓝图 WBP_ItemPickupTips，它的 Parent Class 必须是 ItemPickupTipsWidget）。
    // 显示逻辑全部由 C++ 驱动：NotifyItemObtained → UItemPickupTipsWidget::AddTip
    // → CreateWidget(WBP_ItemTip) → 到点自动淡出移除。蓝图侧不需要加接口、不需要连事件。
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    class UWidgetComponent* ItemPickupTipWidget;

    // 提示控件类（留空时按 ItemPickupTipWidgetPath 在运行时懒加载；
    // 也可以在这里直接指定，覆盖路径加载）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> ItemPickupTipWidgetClass;

    // 提示控件类资产路径（ItemPickupTipWidgetClass 为空时按此路径懒加载）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FString ItemPickupTipWidgetPath = TEXT("/Game/UI/WBP_ItemPickupTips.WBP_ItemPickupTips_C");

    // 提示控件绘制尺寸（仅作为初始值 —— 每帧会被同步成实际视口尺寸，通常不需要手调）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D ItemPickupTipDrawSize = FVector2D(1920.0f, 1080.0f);

    // 显示受伤提示（受伤时调用）；到时自动隐藏
    void ShowHurtUI();
    // 隐藏受伤提示
    void HideHurtUI();
    // 定位受伤提示到屏幕中心并铺满视口（与血条同一公式：FOV 解析、每帧刚性锁定）
    void UpdateHurtUIPosition();

    // 定位获得物品提示控件到屏幕中心并铺满视口（与受伤提示同一套公式，分辨率自适应）
    void UpdateItemPickupTipPosition();

    // ---- 技能/大招图标 UI（常驻屏幕显示）----
    // 技能/大招图标控件类（默认自动加载 /Content/UI/WBP_Skill；未命中时可在蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> SkillIconUIClass;

    // 当前常驻的技能/大招图标控件（BeginPlay 创建，AddToViewport 后一直显示）
    UPROPERTY(Transient)
    UUserWidget* SkillIconUIWidget = nullptr;

    // WBP_Skill 内缓存：技能冷却倒计时文本（控件名 skill_cd）
    UPROPERTY(Transient)
    UTextBlock* SkillCDText = nullptr;

    // WBP_Skill 内缓存：大招冷却倒计时文本（控件名 UIminate_cd）
    UPROPERTY(Transient)
    UTextBlock* UltimateCDText = nullptr;

    // WBP_Skill 内缓存：能量技（Q）冷却倒计时文本（控件名 passive_cd）
    UPROPERTY(Transient)
    UTextBlock* PassiveCDText = nullptr;

    // WBP_Skill 内缓存：能量技（Q）冷却进度条（passive_cd 若为 ProgressBar 类型时启用）
    UPROPERTY(Transient)
    UProgressBar* PassiveCDProgress = nullptr;

    // WBP_Skill 内缓存：能量技就绪遮罩图像（控件名 sock；
    // 未满足释放条件【能量未满/冷却未结束/增伤中】时显示，可释放时隐藏）
    UPROPERTY(Transient)
    UWidget* SockWidget = nullptr;

    // 上次显示值缓存（避免每帧 SetText 重建 FText 的无谓开销）
    float LastShownSkillCD = -1.0f;
    float LastShownUltimateCD = -1.0f;
    float LastShownPassiveCD = -1.0f;

    // 初始化技能/大招图标 UI（BeginPlay 调用：创建控件并常驻屏幕）
    void InitSkillIconUI();

    // 每帧更新技能/大招冷却倒计时：冷却中显示剩余秒数，未冷却隐藏文本
    void UpdateSkillIconCooldownUI();

    // ---- 角色面板 UI（C 键打开，暂停游戏 + 显示鼠标）----
    // 角色面板控件类（默认自动加载 /Content/UI/WBP_Character_imf；未命中时可在蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> CharacterPanelClass;

    // 头像格子控件类（默认自动加载 /Game/UI/WBP_CharaHead_Slot）。
    // 与背包格子 WBP_Bag_Slot 同一个模式：每个头像格子用它 CreateWidget 出来，
    // 再按「拥有的角色」往里面填充头像图 + 选中框（ApplyCharaHeadSlotVisual）。
    // 内部约定控件名：img_head_icon（头像图）、img_select_frame（金色选中框）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> CharaHeadSlotClass;

    // 角色面板运行时实例（打开时 AddToViewport，关闭时 RemoveFromParent）
    UPROPERTY(Transient)
    UUserWidget* CharacterPanelWidget = nullptr;

    // 角色面板是否已打开（打开=暂停游戏+显示鼠标）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    bool bCharacterPanelOpen = false;

    // 当前激活的角色面板页签（默认 Chara=角色）。面板每次打开时重置为 Chara。
    ECharacterPanelTab CurrentPanelTab = ECharacterPanelTab::Chara;

    // ==================================================================
    // ---- 背包 UI（B 键打开，暂停游戏 + 显示鼠标）----
    // ==================================================================
    // 背包主面板控件类（默认自动加载 /Game/UI/bag_sys/WBP_Bag；未命中时可在蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|UI")
    TSubclassOf<UUserWidget> BagWidgetClass;

    // 背包格子控件类（默认自动加载 /Game/UI/bag_sys/WBP_Bag_Slot）。
    // 网格里的每个格子都用它创建，空槽位也是同一个控件（只是内容置空）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|UI")
    TSubclassOf<UUserWidget> BagSlotWidgetClass;

    // 本次生成的格子容器（UniformGridPanel / GridPanel），仅用于打开后做一次尺寸自检
    UPROPERTY(Transient)
    TObjectPtr<UPanelWidget> BagGridPanel = nullptr;

    // BagGridPanel 是 UniformGridPanel（true）还是普通 GridPanel（false）
    bool bBagGridIsUniform = true;

    // 「网格被拉伸」的收敛开关。
    // 引擎规则（SUniformGridPanel::OnArrangeChildren）：
    //     格子尺寸 = 网格分到的空间 ÷ 列数（高度 ÷ 行数）
    // 所以网格一旦被容器撑到全屏，格子就会按「全屏 ÷ 列数」均分到整屏 —— 表现是「格子占满屏幕、挡住其他 UI」。
    // 打开时：仅在「实测明显大于期望（>1.15 倍）」这种客观被拉伸的情况下，把 grid_slots 改成按内容尺寸；
    // 关闭时：只写日志、不改动任何布局。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|UI")
    bool bAutoFitStretchedBagGrid = true;

    // 本次打开是否已处理过「网格被拉伸」（防止反复修改）
    bool bBagGridStretchHandled = false;

    // 本次打开是否已把「网格在 ScrollBox 槽内的横向对齐」由 Fill 收敛为 Left
    // （ScrollBox 交叉轴默认 Fill 会把内容撑满，格子随之摊开；改成 Left 后只占内容宽度）
    bool bBagGridCrossAxisFixed = false;

    // 本次打开的尺寸采样次数：首帧几何为 0 时用于重采样，避免把「尚未排版」误判成「尺寸为 0」
    int32 BagGridSampleTries = 0;

    // 「WBP_HealthBar 尚未实现 IItemPickupListener」的诊断日志是否已提示过。
    // 掉落提示会频繁触发，只在第一次说明原因，避免日志刷屏
    bool bItemPickupListenerReported = false;

    // 提示列表的「布局体检」是否已经打过。
    // 为什么要等：控件未被绘制过时 GetCachedGeometry 恒为 0×0，
    // 拿它当结论会得出「尺寸坏了」的误判 —— 所以要先重采样几帧、确认真的画过了再报。
    // ★ 注意它【不是】「只打一次」的开关（原来那样用过，吃过亏）：
    //   「提示位置错位 / 显示不全」只在【列表非空】时才看得出来，而首帧列表必然是空的。
    //   现在配合下面两个成员，体检时机 = 首次绘制后 + 每次列表多出条目（限流）+ 视口变化后。
    bool bItemPickupLayoutDiagnosed = false;

    // 上一次体检时列表里有几条。用来判断「刚刚多了一条」→ 该再体检一次。
    int32 LastDiagnosedTipCount = -1;

    // 上一次体检的时间戳（秒）。给「每次出内容都体检」加个限流，避免一次掉 6 件刷 6 遍。
    float LastItemPickupDiagTime = -1.0f;

    // 背包数据组件（构造函数中创建，指向同名子对象）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bag")
    TObjectPtr<UInventoryComponent> InventoryComponent = nullptr;

    // 背包主面板运行时实例（打开时 AddToViewport，关闭时 RemoveFromParent）
    UPROPERTY(Transient)
    UUserWidget* BagWidget = nullptr;

    // 全部格子控件（长度 = InventoryComponent->SlotCount，含空槽）
    UPROPERTY(Transient)
    TArray<TObjectPtr<UUserWidget>> BagSlotWidgets;

    // 动态按钮回调载体（格子/分类/关闭/使用），与控件同生命周期
    UPROPERTY(Transient)
    TArray<TObjectPtr<UBagWidgetAction>> BagActions;

    // 当前分类筛选结果（与格子序号一一对应，供点击/详情/使用查询）
    UPROPERTY(Transient)
    TArray<FBagItemEntry> CurrentBagItems;

    // 背包是否已打开（打开=暂停游戏+显示鼠标）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bag|UI")
    bool bBagOpen = false;

    // 当前分类（-1 = 全部）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bag|UI")
    int32 CurrentBagKind = -1;

    // 当前选中的格子序号（-1 = 未选中）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Bag|UI")
    int32 SelectedBagIndex = -1;

    // 「打开背包 / 切换分类时默认选中第一个物品」的开关（默认开）。
    // 关闭后：选中项完全交给玩家点击决定 —— 打开与切换分类都保持未选中、详情区留空。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bag|UI")
    bool bAutoSelectFirstBagItem = true;

    // 「下次刷新时自动选中第一个物品」的待办标记。
    // 打开背包 / 切换分类时置 true；玩家手动点格子后立即清除 —— 玩家手选优先，
    // 绝不能用默认值把玩家刚点的那一格覆盖掉。
    bool bBagSelectFirstPending = false;

    // ---- 角色编队（L 键）----
    // 编队界面是否已打开
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CharacterPanel")
    bool bCharaTeamOpen = false;

    // 编队界面控件类（默认懒加载 /Game/UI/WBP_CharaTeam.WBP_CharaTeam_C，父类 = UCharaTeamWidget）。
    // 留空或加载失败 → 回退纯 C++ 构建（UCharaTeamWidget 自身），功能照常。
    // 建了 WBP_CharaTeam（继承 UCharaTeamWidget）并在设计器里搭好控件 → 走 WBP 设计树；
    // 只建了空 WBP（没拖控件）→ 仍回退 C++ 构建，不会白屏。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CharacterPanel")
    TSubclassOf<class UCharaTeamWidget> CharaTeamWidgetClass;

    // 编队界面控件（打开时创建、关闭时销毁）
    UPROPERTY(Transient)
    class UCharaTeamWidget* CharaTeamWidget = nullptr;

    // ---- 背包内部实现 ----
    // 生成全部格子控件并挂到 grid_slots（UUniformGridPanel）上，绑定每格点击
    void BuildBagGrid();
    // 尺寸自检：把「网格 / 第一个格子 / 上层容器」的尺寸打进日志，直接指出尺寸链断在哪一层
    void LogBagGridDiagnostics();
    // 尺寸自检第二步：隔帧读「真正画出来的尺寸」，并检测「网格被拉伸」（= 格子占满屏幕的根因）。
    // 注意：GetCachedGeometry() 在首帧（尚未绘制过）恒为 0，所以这里会重采样几帧再下结论，
    //       绝不根据 0 值去改动布局 —— 上一版的「0 尺寸自动修复」正是这样把网格撑满全屏的。
    void LogBagGridRealSize();
    // 结构快照：只对第 1 个格子打印「根节点 + 直接子控件」的类名/控件名/槽位类型/期望尺寸
    void LogBagSlotStructure();
    // 绑定分类按钮（kind_all / kind_0..kind_5）、关闭按钮、使用按钮
    void BindBagButtons();
    // 内部刷新：bReloadData = true 时先重新读表
    void RefreshBagInternal(bool bReloadData);
    // 刷新单个格子的视觉（Entry 为 nullptr 表示空槽）
    void ApplyBagSlotVisual(int32 SlotIndex, const FBagItemEntry* Entry);
    // 刷新右侧详情面板（Entry 为 nullptr 表示无选中）
    void ShowBagItemDetail(const FBagItemEntry* Entry);
    // 刷新顶栏「拥有数 / 上限」
    void UpdateBagCapacityText();
    // 刷新左侧分类按钮的选中高亮与文字
    void UpdateBagKindHighlight();
    // 创建并登记一个动态按钮回调载体
    UBagWidgetAction* MakeBagAction(EBagActionType ActionType, int32 Index);

    // ---- 武器页（arm_imfor）状态 ----
    // arm_imfor 里的按钮是否已绑定。
    // 做成 UPROPERTY 是为了能在 PIE 的 Details 面板里直接看到结论 ——
    // 「按钮点了没反应」时，第一个要确认的就是它到底绑上没绑。
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|UI")
    bool bArmPanelButtonsBound = false;

    // 「arm_imfor 结构快照」是否已打过（避免每次切页都刷一遍控件树）
    bool bArmPanelStructureLogged = false;

    // 「角色面板正在打开流程中」的重入保护。
    // 为什么需要（这是个真实的撞车，不是防御性代码）：OpenCharacterPanel() 开头会
    // 「先收起背包」，而 CloseBag() 末尾又有「从武器背包退出 → 自动回武器页」——
    // 玩家开着武器背包按 C 时，就会在 OpenCharacterPanel 内部再调一次 OpenCharacterPanel：
    // AddToViewport 被调两次，而且内层末尾的「默认选中角色页」会把武器页刷掉（表现为页签乱跳）。
    bool bOpeningCharacterPanel = false;

    // 「按约定路径找到武器蓝图」这条提示是否已打过。
    // 为什么需要：ResolveWeaponClassForItemRow 每次刷新详情面板都会被调到
    // （ShowBagItemDetail → 判断这件武器能不能装备），不 gate 的话点一下刷一条日志。
    // mutable：该函数是 const（也要给蓝图的纯函数调用），但这条提示是"日志节流"而非逻辑状态。
    mutable bool bWeaponPathHintLogged = false;

    // ---- 武器蓝图 ↔ 背包行 索引 ----
    // 「背包行名 / tool_id → 武器蓝图类」，由 RebuildWeaponRowIndex() 从各蓝图的自声明生成。
    // 为什么是 mutable：ResolveWeaponClassForItemRow 是 const（要给蓝图的纯节点用），
    // 但这份索引是「首次解析时懒建一次」的缓存 —— 它是日志/查询用的派生数据，不是角色状态。
    mutable TMap<FName, TSubclassOf<class AWeaponBase>> WeaponRowToClassIndex;
    mutable bool bWeaponRowIndexBuilt = false;
    // 重建过程中再被调到就直接返回（扫描会加载资产，加载过程里可能触发别的刷新）
    mutable bool bWeaponRowIndexBuilding = false;
    // 「按武器蓝图自声明命中」这条提示打过没（同 bWeaponPathHintLogged：该函数会被反复调到）
    mutable bool bWeaponDeclaredHintLogged = false;

    // 「所有路径都解析不出武器蓝图类」这条追踪打过没。
    // ★ 为什么必须有这条日志（只报一次）：解析失败是静默返回 nullptr 的，
    //   而它以前会连锁成「整个武器背包被清空、装备按钮点了没反应」这种大现象。
    //   把「试了哪几条路径、各自什么结果、索引里几条」摆出来，
    //   才能一眼分出是「索引是空的」还是「路径名不对」—— 这两种修法完全不同。
    mutable bool bWeaponResolveFailLogged = false;
    // 懒构建：索引还没建时建一次
    void EnsureWeaponRowIndex() const;

    // ---- 定时器 ----
    FTimerHandle InvincibilityTimer;

    // ---- 内部函数 ----
    void Jump();
    void DoubleJump();
    void UpdateMovement(float DeltaTime);
    void UpdateCameraRotation();
    void ResetCombo();
    void PerformComboAttack(int32 Step);
    void OnComboAttackEnd();

    // ---- 普攻连段段列表（段数 = ComboAttackSegments 元素个数）----
    // 启动时调用一次：
    //   ① 按数组长度同步 MaxComboStep；
    //   ② 把「实际有几段、每段播什么、窗口多少」打一条日志 —— 段数不对时一眼能看出来，
    //      而不用去猜「到底读到几段」。
    // 幂等：只做同步与日志，不改数组内容。
    void EnsureComboSegments();

    // 当前生效的段数（= ComboAttackSegments 元素个数，至少 1）
    UFUNCTION(BlueprintCallable, Category = "Combo")
    int32 GetComboSegmentCount() const;

    // 第 Step 段（0 基）的蒙太奇；越界或该段没配蒙太奇返回 nullptr
    UFUNCTION(BlueprintCallable, Category = "Combo")
    UAnimMontage* GetComboMontage(int32 Step) const;

    // 第 Step 段（0 基）的可打断窗口秒数；越界返回 0（= 立即可打断）
    float GetComboCancelWindowTime(int32 Step) const;

    // 该蒙太奇属于第几段（0 基）；不属于任何段返回 INDEX_NONE
    int32 FindComboStepIndex(const UAnimMontage* Montage) const;

    // 返回当前正在播放的普攻蒙太奇（遍历 ComboAttackSegments），无则 nullptr
    UAnimMontage* GetActiveComboMontage() const;
    // 当前段是否已进入可打断窗口（播放进度 >= 该段的 CancelWindowTime）
    bool IsComboCancelWindowReached() const;

    // ---- 攻击索敌 ----
    // 发起攻击时调用：在 AttackAimRadius 范围内查找最近的存活怪物，存在则把角色朝向
    // 平滑转向该怪物（范围内无怪物则不改朝向）
    void AimAttackAtNearestMonster();

    // 切人自动普攻门控：附近是否存在「已进入战斗」的存活怪物（AttackAimRadius 范围内）。
    // 只有存在已建立仇恨（bInCombat=true）的怪物时，切人才自动打出配置段数的普攻；
    // 脱战/未引起仇恨时切人不自动普攻（避免空挥）。
    bool HasMonsterInCombat() const;

    // ---- 下落攻击 ----
    // 空中普攻触发：播放蒙太奇 + 加速下坠（伤害在落地时结算）
    void PerformFallAttack();
    // 落地冲击：对范围内所有怪物造成伤害
    void ApplyFallAttackImpact();

    // ---- 完美闪避 ----
    // 检测是否有怪物处于攻击前摇窗口内（是则本次闪避判定为完美闪避）
    bool CheckPerfectDodgeWindow() const;
    // 收集可被完美闪避的怪物：攻击前摇内 + 在角色为中心的球体范围（PerfectDodgeRadius）内
    void GatherPerfectDodgeTargets(TArray<AMonsterBase*>& OutTargets) const;

public:
    // ---- 弹刀（命中自动触发，无专门按键）----
    // 角色攻击命中怪物时由武器/弹丸/下落攻击结算处调用：
    // 若该怪物正处于可弹刀前摇窗口 → 自动触发弹刀（打断其攻击 + 怪物硬直 + 播放弹刀蒙太奇）
    // 返回是否触发成功
    bool TryParryOnHit(class AMonsterBase* HitMonster);

    // ---- 无敌帧查询（Boss 毒刺命中判定用：无敌期间毒刺穿过视为未命中）----
    // 返回当前是否处于无敌帧（完美闪避/大招期间）。
    // P3 GAS 化：查 ASC 的 State.Invincible Tag（由无敌 GE Grant），兼容大招无敌。
    UFUNCTION(BlueprintPure, Category = "Combat")
    bool IsInvincibleNow() const;

    // ---- 索敌锁定（蓝图只读查询）----
    // 当前索敌目标：未开启索敌 / 无目标 / 目标死亡或超出 LockOnRadius 时返回 nullptr
    UFUNCTION(BlueprintPure, Category = "Combat|LockOn")
    AMonsterBase* GetLockedTargetActor() const;

    // ---- 警觉朝向（蓝图只读查询，供 ABP Orientation Warping 使用）----
    // 目标方向相对 Actor 朝向的 Yaw 差（-180..180 度）：ABP Warping 节点的 Goal Yaw Offset 输入
    UFUNCTION(BlueprintPure, Category = "Combat|AlertFacing")
    float GetUpperBodyYawOffset() const;

    // 警觉激活权重（0..1）：ABP 可用于在"警觉朝向 vs Actor 朝向"间做 Lerp 过渡
    UFUNCTION(BlueprintPure, Category = "Combat|AlertFacing")
    float GetUpperBodyYawWeight() const;

public:
    // ---- 能量增伤吸血（Q 释放后 20s 增伤窗口内生效）----
    // 玩家任何出伤点（普攻近战扫掠/技能/大招/下落攻击）实际结算伤害后调用：
    // 按 EnergyBuffLifestealRatio 回血（直接改 CurrentHealth，不触发受伤 UI/硬直）
    void ApplyEnergyBuffLifesteal(float DamageDealt);

public:
    // ---- 命中镜头振动：大招段攻击窗口内实际命中怪物时触发（强化大招打击反馈）----
    // 触发点唯一：UpdateUltimateSegmentCombat 命中结算；弹刀/技能/能量技/普攻命中不触发。
    // 参数见 Combat|Camera 的 HitCameraShake* 配置（名称沿用历史，现用于大招段命中反馈）
    void TriggerHitCameraShake();

public:
    // ---- 击飞反应（怪物"大幅度攻击"命中时由 MonsterBase 调用）----
    // 播放击飞蒙太奇并锁定操作（时长 KnockbackStunDuration，复用受击硬直状态机）。
    // 未配置 KnockbackMontage 时无操作（保持 TakeDamage 已播的普通受击反应）
    void PlayKnockbackReaction(class AActor* Attacker);

    // Play the getup montage after a knockback stun ends (keeps input locked until done).
    void PlayGetupReaction();

public:
    // ---- 协奏系统接口 ----
    UFUNCTION(BlueprintCallable, Category = "Concerto")
    void AddConcertoEnergy(float Amount);

    UFUNCTION(BlueprintCallable, Category = "Concerto")
    bool IsConcertoFull() const;

    // ---- 闪避系统 ----
    UFUNCTION(BlueprintCallable, Category = "Combat")
    void PerformDodge();

    UFUNCTION(BlueprintNativeEvent, Category = "Combat")
    void OnLimitDodgeSuccess();

    // ---- 韧性系统 ----
    UFUNCTION(BlueprintCallable, Category = "Combat")
    void ApplyPoiseDamage(float Amount);

    UFUNCTION(BlueprintNativeEvent, Category = "Combat")
    void OnPoiseBroken();

    // ---- 攻击系统辅助 ----
    UFUNCTION(BlueprintCallable, Category = "Combat")
    bool CanAttack() const;

    // ---- 武器系统 ----
    // 装备指定类的武器（会先卸下当前武器）
    UFUNCTION(BlueprintCallable, Category = "Weapon")
    bool EquipWeapon(TSubclassOf<class AWeaponBase> WeaponClass);

    // 卸下当前武器（空手）
    UFUNCTION(BlueprintCallable, Category = "Weapon")
    void UnequipWeapon();

    UFUNCTION(BlueprintPure, Category = "Weapon")
    bool HasWeapon() const { return CurrentWeapon != nullptr; }

    UFUNCTION(BlueprintPure, Category = "Weapon")
    class AWeaponBase* GetCurrentWeapon() const { return CurrentWeapon; }

    // ---- 职位系统 ----
    // 当前职位（蓝图可读）
    UFUNCTION(BlueprintPure, Category = "Job")
    EJobClass GetJobClass() const { return JobClass; }

    // 设置职位（蓝图可调）。切换职位后，若当前武器不属于新职位 → 自动卸下（空手），
    // 避免「法师手上拿着剑」这种非法状态。
    UFUNCTION(BlueprintCallable, Category = "Job")
    void SetJobClass(EJobClass NewJob);

    // 当前职位对应的武器吸附插槽名（剑士/枪手→HandGrip_R，法师→magic_WeaponSocket）
    UFUNCTION(BlueprintPure, Category = "Job")
    FName GetJobSocketName() const;

    // 该武器类别是否与当前职位匹配（职位约束的判据入口）
    UFUNCTION(BlueprintPure, Category = "Job")
    bool CanUseWeaponCategory(EWeaponCategory Category) const;

    // 该武器蓝图是否与当前职位匹配（读蓝图 CDO 的 WeaponCategory 判）
    UFUNCTION(BlueprintPure, Category = "Job")
    bool CanEquipWeaponClassForJob(TSubclassOf<class AWeaponBase> WeaponClass) const;

    // 装备时按职位解析插槽名：优先用职位插槽（法师→magic_WeaponSocket），
    // 若武器蓝图显式填了插槽且非默认值则尊重蓝图（蓝图覆盖优先）。当前实现：职位插槽优先。
    FName ResolveEquipSocketName(class AWeaponBase* Weapon) const;

    // 武器装备/卸下事件（蓝图可扩展：音效/特效/属性加成）
    UFUNCTION(BlueprintNativeEvent, Category = "Weapon")
    void OnWeaponEquipped(class AWeaponBase* NewWeapon);
    virtual void OnWeaponEquipped_Implementation(class AWeaponBase* NewWeapon);

    UFUNCTION(BlueprintNativeEvent, Category = "Weapon")
    void OnWeaponUnequipped();
    virtual void OnWeaponUnequipped_Implementation();

    // ---- 跳跃控制 ----
    UFUNCTION(BlueprintCallable, Category = "Jump")
    bool CanPerformJump() const;

    UFUNCTION(BlueprintCallable, Category = "Jump")
    bool CanPerformDoubleJump() const;

    UFUNCTION(BlueprintCallable, Category = "Jump")
    void ResetJumpCount();

    UFUNCTION(BlueprintCallable, Category = "Jump")
    bool IsCharacterJumping() const;

    // ---- 连击系统 ----
    UFUNCTION(BlueprintCallable, Category = "Combo")
    int32 GetCurrentComboStep() const { return CurrentComboStep + 1; }

    UFUNCTION(BlueprintCallable, Category = "Combo")
    void ForceResetCombo();

    // ---- 摄像机控制 ----
    UFUNCTION(BlueprintCallable, Category = "Camera")
    void AddCameraRotation(float DeltaX, float DeltaY);

    UFUNCTION(BlueprintCallable, Category = "Camera")
    void ResetCameraView();

    UFUNCTION(BlueprintCallable, Category = "Camera")
    void ZoomCamera(float Delta);

    UFUNCTION(BlueprintCallable, Category = "Camera")
    void SetCameraToBackView();

    // ---- 移动控制 ----
    UFUNCTION(BlueprintCallable, Category = "Movement")
    void SetMovementInput(float ForwardValue, float RightValue);

    // ---- 判断是否移动 ----
    UFUNCTION(BlueprintCallable, Category = "Combat")
    bool IsMoving() const;

    // ---- 禁用无敌 ----
    void DisableInvincibility();

    // ---- 授予无敌帧（P3 GAS 化）：应用无敌 GameplayEffect，Grant State.Invincible Tag ----
    // Duration 秒后 GE 到期自动移除 Tag（无需手动 Timer）。未配置 GE 时回退旧 bIsInvincible+Timer 逻辑。
    void GrantInvincibility(float Duration);

    // ---- 受击反应 ----
    // 受击时随机播放一段 HitReactionMontages。按 bHitReactionInterruptsAttack 决定是否打断攻击。
    // 无敌/冷却中/霸体模式攻击进行中 跳过
    void PlayHitReaction();

    // 受击硬直计时结束：解除操作锁定并停止残余的受击蒙太奇。
    // 由 Tick 计时驱动（HitReactionDuration），与蒙太奇播放状态完全解耦，
    // 避免蒙太奇被打断/替换/播放失败时结束回调不触发导致操作永久锁死的 bug
    void EndHitReaction();

    // 当前正在播放的受击蒙太奇（硬直结束时用于精确停止）
    UAnimMontage* CurrentHitReactionMontage = nullptr;

    // 受击硬直剩余时间（Tick 递减，<=0 时调用 EndHitReaction 解除锁定）
    float HitReactionTimeRemaining = 0.0f;

    // 受击反应冷却剩余（Tick 递减，>0 时跳过受击反应播放）
    float HitReactionCooldownRemaining = 0.0f;

    // Runtime knockback/getup state (see Combat|Animation config above):
    // whether the current stun is a knockback (getup follows when it ends)
    bool bIsKnockback = false;

    // Getup phase active: playing the getup montage (input stays locked)
    bool bIsGettingUp = false;

    // 站立蒙太奇是否已开始播放（false = 仍处于 GetupDelay 等待期；true = 站立蒙太奇播放中）
    bool bGetupMontageStarted = false;

    // Remaining getup montage time (Tick counts down, <=0 ends the stun)
    float GetupTimeRemaining = 0.0f;

    // ---- 动画蒙太奇 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* AttackMontage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* DodgeMontage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* DashMontage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* JumpMontage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* DoubleJumpMontage;

    // 下落攻击蒙太奇：跳跃期间使用普攻时触发（空中按下普攻键）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* FallAttackMontage;

    // 完美闪避蒙太奇：在怪物攻击前摇内闪避成功时播放
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* PerfectDodgeMontage;

    // 弹刀蒙太奇：弹刀成功（打断怪物可弹刀攻击）时播放
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* ParryMontage;

    // ---- 受击反应蒙太奇 ----
    // 受击时随机播放其中一段。留空 = 不播放受击动画（只扣血）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    TArray<TObjectPtr<UAnimMontage>> HitReactionMontages;

    // 受击反应蒙太奇播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.05"))
    float HitReactionPlayRate = 1.0f;

    // 受击反应是否打断进行中的攻击（true=受击硬直打断攻击；false=霸体，攻击继续不受影响）。
    // 默认 false：玩家攻击被怪打中不中断攻击（保持操作手感）；设 true 可增加挑战性
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    bool bHitReactionInterruptsAttack = false;

    // 受击反应冷却（秒）：两次受击反应的最小间隔。0=每次受击都播放
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
    float HitReactionCooldown = 0.0f;

    // 受击硬直持续时间（秒）：受击后操作被锁定的僵直时长，与受击蒙太奇播放时长无关。
    // 硬直由 Tick 计时控制，时间到即解锁（蒙太奇若还没播完会被混合截停）。
    // 0=不产生硬直（仅播放受击动画，不锁定操作）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta=(ClampMin = "0.0"))
    float HitReactionDuration = 0.5f;

    // ---- 击飞反应（怪物"大幅度攻击"命中时触发）----
    // 击飞蒙太奇：怪物大幅度攻击命中角色后播放（覆盖普通受击反应）。
    // 未配置时回落为普通受击反应（HitReactionMontages）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* KnockbackMontage;

    // 击飞蒙太奇播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.05"))
    float KnockbackMontagePlayRate = 1.0f;

    // 击飞硬直持续时间（秒）：击飞后操作被锁定的时长（Tick 计时解除，与蒙太奇时长解耦）。
    // 0=不产生硬直（仅播放击飞动画）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
    float KnockbackStunDuration = 1.0f;

    // 击飞僵直开关：击飞硬直（KnockbackStunDuration）期间是否锁定全部操作。
    // true = 被击飞后无法进行任何操作（移动/攻击/闪避/技能/大招/跳跃/疾跑），
    // 硬直结束进入站立阶段（bGetupLockInput 继续决定是否锁定）；
    // false = 击飞期间不锁操作，仅播放击飞动画，玩家可提前行动打断击飞动作
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    bool bKnockbackLockInput = true;

    // 击飞位移初速度（cm/s，水平方向=远离攻击者）：>0 时击飞瞬间用 LaunchCharacter 把角色
    // 向后弹出并附带 KnockbackLaunchZ 的向上速度；0=不产生额外位移（仅蒙太奇自身根运动）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
    float KnockbackLaunchSpeed = 0.0f;

    // 击飞位移的向上初速度（cm/s，KnockbackLaunchSpeed > 0 时生效）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
    float KnockbackLaunchZ = 300.0f;

    // ---- 站立反应（击飞落地后自动播放）----
    // 站立蒙太奇：击飞硬直（KnockbackStunDuration）结束后自动播放并保持操作锁定，
    // 站立播完（按蒙太奇长度/播放速率计时）后解锁。未配置时硬直结束直接恢复操作。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    class UAnimMontage* GetupMontage;

    // 站立蒙太奇播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.05"))
    float GetupMontagePlayRate = 1.0f;

    // 击飞硬直结束 → 站立蒙太奇开始前的延迟（秒）：期间角色保持躺地/僵直状态，
    // 时间到后才开始播放站立蒙太奇。0 = 击飞硬直结束立即播放站立
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
    float GetupDelay = 0.0f;

    // 站立僵直开关：站立阶段（等待 GetupDelay + 站立蒙太奇播放期间）是否锁定全部操作。
    // true = 期间无法进行任何操作（移动/攻击/闪避/技能/大招/跳跃/疾跑）；
    // false = 站立阶段不锁操作，玩家可提前打断站立动作
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Animation")
    bool bGetupLockInput = true;

    // ---- UI更新 ----
    UFUNCTION(BlueprintCallable, Category = "UI")
    void UpdateStaminaBar();

    // 每帧把耐力条定位到屏幕中心+偏移处（绑定摄像机视角）
    void UpdateStaminaBarPosition();

    // ---- 耐力条显隐 ----
    UFUNCTION(BlueprintCallable, Category = "UI")
    void SetStaminaBarVisible(bool bVisible);

    // 检测耐力变动并处理显隐（每帧Tick调用）
    void UpdateStaminaBarVisibility();

    // ---- 血条UI更新 ----
    UFUNCTION(BlueprintCallable, Category = "UI")
    void UpdateHealthBar();

    // 每帧把血条定位到屏幕底部居中处（绑定摄像机视角）
    void UpdateHealthBarPosition();

    // ---- 视口尺寸变化检测：窗口拉伸/分辨率切换后强制刷新屏幕空间 UI ----
    // （UE 已知问题：Screen Space WidgetComponent 在视口 resize 后 Slate 窗口失效导致控件消失）
    void CheckViewportResize();

    // 强制重建屏幕空间 WidgetComponent 的 Slate 控件（保留隐藏状态与配置）
    void ForceRefreshScreenWidget(class UWidgetComponent* WidgetComp);

    // 上一帧视口尺寸（检测窗口拉伸/分辨率切换）
    int32 LastViewportSizeX = 0;
    int32 LastViewportSizeY = 0;

    // ---- 生命值操作 ----
    // 受击扣血并刷新血条（无敌状态免疫伤害）
    virtual float TakeDamage(float DamageAmount, const struct FDamageEvent& DamageEvent, class AController* EventInstigator, AActor* DamageCauser) override;

    // GAS 伤害结算（P1）：通过伤害 GameplayEffect（Execution）扣护盾/扣血，
    // 并同步 CurrentHealth/CurrentShield 镜像字段 + 刷新血条。未配置 GE 时回退旧逻辑。
    void ApplyDamageViaGAS(float DamageAmount);

    // 回血并刷新血条
    UFUNCTION(BlueprintCallable, Category = "Health")
    void HealHealth(float Amount);

    // ---- 静默扣血（Boss 二阶段毒刺命中用）----
    // 直接扣血 + 刷新血条 + 受伤 UI 闪现，但【不触发受击动画、不触发击飞动画】。
    // 毒刺命中要求「命中即扣血但不进入受击/击飞硬直」，与普通 TakeDamage 区分：
    // 无敌帧仍免疫（返回 0），命中后不打断玩家当前动作/不锁操作。
    // 返回实际扣血量（无敌免疫时返回 0）
    UFUNCTION(BlueprintCallable, Category = "Health")
    float ApplySilentDamage(float DamageAmount, class AActor* DamageCauser);

    // ---- 中毒状态（Boss 毒刺命中施加）----
    // 施加中毒：期间持续扣除最大血量百分比（PoisonDamagePerSecondRatio / PoisonTickInterval 节奏）。
    // 无法重复叠加：已中毒时再次命中不会重置计时/叠加层数（直接忽略）。
    // 返回是否成功施加（已中毒时返回 false）
    UFUNCTION(BlueprintCallable, Category = "Health|Poison")
    bool ApplyPoison();

    // 当前是否处于中毒状态（GAS 化：查 State.Poisoned Tag）
    UFUNCTION(BlueprintPure, Category = "Health|Poison")
    bool IsPoisoned() const;

    // 中毒剩余时间（秒，从 ASC 读取剩余时长；未配置 GE 时回退镜像字段）
    UFUNCTION(BlueprintPure, Category = "Health|Poison")
    float GetPoisonRemaining() const;

    // 中毒 Tick 驱动（Tick 内调用）：GAS 化后主要做到期清理 + 状态图标同步。
    // 毒伤由中毒 GE 的 Periodic Execution 自动结算，此函数仅负责检测到期并清理镜像。
    void UpdatePoison(float DeltaTime);

    // ---- 护盾状态（E 技能施放后获得）----
    // 获得护盾：护盾量 = 最大血量 × ShieldMaxHPRatio（默认 8%）。无法叠加，
    // 每次施放 E 刷新为满值（重新取最大血量 8%，而非叠加）。
    void ApplyShield();

    // 护盾吸收伤害：受击时优先扣护盾。返回被护盾吸收的伤害量（超出护盾部分需继续扣血）。
    float AbsorbDamageWithShield(float DamageAmount);

    // 刷新护盾进度条（WBP_HealthBar 的 shield_bar：护盾量 / 最大护盾量）
    void UpdateShieldBar();

    // 当前是否拥有护盾（护盾量 > 0）
    UFUNCTION(BlueprintPure, Category = "Health|Shield")
    bool HasShield() const { return CurrentShield > 0.0f; }

    // 当前护盾量
    UFUNCTION(BlueprintPure, Category = "Health|Shield")
    float GetCurrentShield() const { return CurrentShield; }

    // ---- 大招增伤状态（Ultimate 施放后获得）----
    // 激活增伤：置 bUltimateBuffActive + 启动到期定时器 + 申请状态图标槽位。
    // 无法重复叠加：已激活时再次调用不重置计时/叠加（直接忽略）。
    void ApplyUltimateDamageBuff();

    // 增伤到期回调：关闭增伤 + 释放状态图标槽位
    void OnUltimateBuffEnded();

    // 当前是否处于大招增伤状态（GAS 化：查 Buff.DamageBoost Tag）
    UFUNCTION(BlueprintPure, Category = "Combat|Ultimate")
    bool IsUltimateDamageBuffActive() const;

    // ---- 状态图标（WBP_HealthBar 的 state_01~04）----
    // 刷新 4 个状态图标槽位：按「获得状态的时间顺序」填充图像、未占用槽位清除图像并隐藏。
    // 通用槽位管理：中毒（PoisonSlotIndex）+ 大招增伤（UltimateBuffSlotIndex）+
    // 吸血（LifestealSlotIndex）三类状态，先获得的状态占更小的槽位号；未来可扩展更多状态。
    void RefreshStatusIcons();

    // 状态图标缓存（state_01~04 的 UImage 控件缓存）+ 宿主 widget 重建检测
    TWeakObjectPtr<UUserWidget> StatusIconHostWidget;
    TArray<TWeakObjectPtr<class UImage>> StatusIconCache;

};