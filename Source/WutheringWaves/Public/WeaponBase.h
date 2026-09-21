#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponBase.generated.h"

class UStaticMeshComponent;
class ABattleCharacter;
class AProjectileBase;

/**
 * 武器类别（职位系统用的大类，三分类）。
 *
 * ★ 2026-09-16 起【唯一】的武器类型划分：不再采用鸣潮原生的五类细分（阔刃/迅刀/手枪/臂铠/音感仪），
 *   统一用下面三分类，由它决定：
 *     ① 该武器属于哪个职位（职位约束用的判据）
 *     ② 该武器是近战还是远程（攻击行为）
 *     ③ 该武器应吸附到哪个插槽（插槽归属）
 *   旧的 EWeaponType（鸣潮五类）已整体移除，不再保留 —— 项目不采用那么多种类。
 *
 * ★ 枚举值默认值问题：uint8 枚举默认 0 = LightBlade（轻剑）。
 *    任何【没显式填类别】的武器蓝图都会被当成「轻剑」，从而被「剑士」职位放行。
 *    这符合直觉（默认剑士用轻剑），但新武器蓝图务必显式填 WeaponCategory，
 *    避免「没填类别」被误判成轻剑。
 */
UENUM(BlueprintType)
enum class EWeaponCategory : uint8
{
    LightBlade  UMETA(DisplayName = "轻剑"),
    MagicTool   UMETA(DisplayName = "法器"),
    Firearm     UMETA(DisplayName = "枪械")
};

/**
 * 角色职位（Job Class）。
 *
 * ★ 职位决定两件事：
 *    ① 角色只能用对应武器类别的 Weapon Class（装备时拦截 + 武器背包列表过滤）
 *    ② 武器吸附插槽（剑士/枪手 → HandGrip_R，法师 → magic_WeaponSocket）
 *
 *  职位在角色蓝图（如 BP_PlayerCharacter）的 Details 里设置。
 *
 * ★ 枪手是占位：职位枚举已建好、映射关系已建好，但枪械类武器暂未开发，
 *    角色选「枪手」时武器背包暂时没有可用武器（后续补枪械武器蓝图即可，无需改代码）。
 *
 * ★ 枚举值默认值：uint8 默认 0 = BladeMaster（剑士）。
 *    角色蓝图务必显式设置职位，避免「没填」被当成剑士。
 */
UENUM(BlueprintType)
enum class EJobClass : uint8
{
    BladeMaster UMETA(DisplayName = "剑士"),
    Mage        UMETA(DisplayName = "法师"),
    Gunner      UMETA(DisplayName = "枪手")
};

/** 职位系统工具：职位 ↔ 武器类别 ↔ 插槽 的映射。 */
class WUTHERINGWAVES_API FJobWeaponRules
{
public:
    /** 该职位可用的武器类别（剑士→轻剑，法师→法器，枪手→枪械）。 */
    static EWeaponCategory GetCategoryForJob(EJobClass Job);

    /** 该职位可用的武器类别集合（当前一对一，保留扩展性：未来一个职位可配多类）。 */
    static bool CanJobUseCategory(EJobClass Job, EWeaponCategory Category);

    /** 该职位对应的武器吸附插槽名（剑士/枪手→HandGrip_R，法师→magic_WeaponSocket）。 */
    static FName GetSocketForJob(EJobClass Job);
};

/**
 * 武器基类：
 * - 装备时吸附到角色 Mesh 的手部骨骼/插槽（默认 HandGrip_R）
 * - 近战武器（阔刃/迅刀/臂铠）：攻击时延迟做球形扫掠命中判定
 * - 远程武器（手枪/音感仪）：攻击时朝角色前方发射弹丸
 * 具体武器在蓝图里派生（如 BP_Sword），配置 Mesh 与数值
 */
UCLASS()
class WUTHERINGWAVES_API AWeaponBase : public AActor
{
    GENERATED_BODY()

public:
    AWeaponBase();

    // ---- 装备 ----
    // 装备：吸附到角色 Mesh 的骨骼/插槽上
    void OnEquipped(ABattleCharacter* NewOwner);

    // 卸下：脱离角色
    void OnUnequipped();

    // ---- 攻击（由角色连击系统调用）----
    // 远程武器立即发射弹丸；近战武器延迟到打击帧做命中判定
    void BeginAttack(int32 ComboStep);

    // 结束攻击：取消尚未执行的命中判定（如被闪避打断）
    void EndAttack();

    // 打断前结算（近战）：闪避等动作打断攻击的瞬间调用。若近战命中判定已到出伤时刻
    // （延迟计时本帧内应触发），立即执行一次球形扫掠——前方有怪物接触才实际出伤，
    // 实现"打断时恰在出伤时间且有模型接触 → 先出伤后打断"；随后判定被清除。
    // 返回是否实际执行了出伤结算。远程武器/无待执行判定返回 false
    bool TryResolveDueMeleeHit();

    // ---- 战斗动作显形（技能/大招用，不影响攻击判定）----
    // 请求武器在 Duration 秒内保持显形（闲置隐藏逻辑暂缓）。角色施放技能/大招时调用，
    // 施放期间每帧刷新 → 全程武器可见；结束后回落到原有闲置自动隐藏节奏
    void ShowForCombatAction(float Duration);

    UFUNCTION(BlueprintPure, Category = "Weapon")
    bool IsRanged() const;

    // 武器类别（职位系统用的大类：轻剑/法器/枪械）。
    // 决定「属于哪个职位」「近战还是远程」「该吸附到哪个插槽」。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    EWeaponCategory GetWeaponCategory() const { return WeaponCategory; }

    // 该武器类别对应的默认吸附插槽名（轻剑/枪械 → HandGrip_R，法器 → magic_WeaponSocket）。
    // 供「按职位自动套插槽」逻辑使用（见 ABattleCharacter::ResolveEquipSocketName）。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    FName GetDefaultSocketForCategory() const;

    // 静态工具：把武器类别换算成默认吸附插槽名。蓝图也可直接调用。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    static FName GetSocketNameForCategory(EWeaponCategory Category);

    // ---- 插槽名读写（供「按职位自动套插槽」用）----
    // 装备时由角色侧按职位覆盖插槽名（见 ABattleCharacter::ResolveEquipSocketName）。
    // 单独 getter/setter 而不是把字段公开：让「谁改插槽」这件事有个明确入口，
    // 排查「武器吸错位置」时能一眼看出是职位套的、还是蓝图自己填的。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    FName GetAttachSocketName() const { return AttachSocketName; }

    void SetAttachSocketName(FName InSocket) { AttachSocketName = InSocket; }

    // 武器攻击力（装备加成）：角色装备此武器后，总攻击力额外增加该数值。
    // 在武器蓝图（如 test_knife）里设置。最终伤害基数 = 角色基础攻击力 + 武器攻击力 + 装备总攻击力。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    float GetAttackPower() const { return AttackPower; }

    // ---- 背包关联取值器 ----
    // 为什么用取值器而不是把字段放 public：与 AttackPower 等配置项保持一致
    // （都放 protected），而攻击力那边也是用 GetAttackPower() 对外暴露的 —— 同一种风格。
    // 角色侧读的永远是【蓝图默认值（CDO）】：声明写死在蓝图上，装出来的实例才有意义。

    /** 本武器声明自己对应背包数据表的哪一行（None = 没声明）。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Bag")
    FName GetBagItemRow() const { return BagItemRow; }

    /** 本武器声明的物品 tool_id（None = 没声明）。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Bag")
    FName GetBagItemToolId() const { return BagItemToolId; }

    /** 是否声明了背包归属（两个字段任一填了即算）。 */
    UFUNCTION(BlueprintPure, Category = "Weapon|Bag")
    bool HasBagBinding() const { return !BagItemRow.IsNone() || !BagItemToolId.IsNone(); }

    // ---- 枪口位置 ----
    // 供外部系统取「武器枪口」世界坐标使用（与 FireProjectile 用同一套取法）。
    // 返回 Mesh 上 MuzzleSocketName 插槽的世界位置；插槽不存在时 GetSocketLocation 会
    // 自动退回组件位置（引擎行为），因此不会返回无效值，调用方无需再判空。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    FVector GetMuzzleLocation() const;

    // 枪口插槽名（默认 "Muzzle"）。武器蓝图可按自己的 Mesh 调整。
    UFUNCTION(BlueprintPure, Category = "Weapon")
    FName GetMuzzleSocketName() const { return MuzzleSocketName; }

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

    // 近战命中判定：以角色为中心、沿面朝方向做球形扫掠
    void PerformMeleeTrace();

    // 远程发射弹丸
    void FireProjectile(ABattleCharacter* Wielder);

    // 命中单个目标时触发（蓝图可扩展打击特效/音效）
    UFUNCTION(BlueprintNativeEvent, Category = "Weapon")
    void OnAttackHit(AActor* HitActor, float DamageAmount);
    virtual void OnAttackHit_Implementation(AActor* HitActor, float DamageAmount);

    // ---- 组件 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UStaticMeshComponent* WeaponMesh;

    // ---- 武器配置 ----
    // 武器类别（职位系统用的大类，三分类：轻剑/法器/枪械）。
    // ★ 新武器蓝图务必显式填写，否则默认 0 = LightBlade（轻剑），会被「剑士」职位放行。
    //   由它决定：属于哪个职位 / 近战还是远程 / 吸附到哪个插槽。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
    EWeaponCategory WeaponCategory = EWeaponCategory::LightBlade;

    // ---- 背包关联：武器蓝图【自声明】自己对应背包里的哪一件 ----
    // 为什么声明在这里而不是在角色蓝图里维护一张「行名 → 武器类」的表：
    //   做一把新武器时你本来就要打开它的蓝图配 Mesh / 攻击力，
    //   顺手在这一页填上「我是背包里的哪一行」，角色侧就完全不用再配东西。
    //   （角色上的 Weapon Class By Item Row 仍然保留，作用是【覆盖】和【兜底】。）
    //
    // 填好之后：「打开武器背包 → 点这一行 → 装备成这把武器 → 角色蓝图的 Weapon Class 也跟着变」。
    //
    // 行名怎么找：打开 Data_tool，左侧第一列就是行名（本工程目前是 1 / 2 / 3 / 4 / 5）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Bag")
    FName BagItemRow;

    // 备选写法：用物品的 tool_id 声明（有些工程的行名是随手起的数字、tool_id 才是人看得懂的）。
    // 两个都填时【行名优先】；反查时两个 key 都建索引，用哪个查得到。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Bag")
    FName BagItemToolId;

    // 吸附到角色 Mesh 的骨骼/插槽名（默认 HandGrip_R 插槽）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
    FName AttachSocketName = TEXT("HandGrip_R");

    // 远程武器的发射口插槽名（武器 Mesh 上未创建该插槽时，用武器自身位置）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
    FName MuzzleSocketName = TEXT("Muzzle");

    // ---- 收刀/显形 ----
    // 启用闲置自动隐藏：一段时间未攻击则隐藏武器模型，攻击时立即显示
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
    bool bAutoHideWhenIdle = true;

    // 未攻击多少秒后隐藏武器模型（秒）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon", meta = (ClampMin = "0.0"))
    float IdleHideDelay = 0.8f;

    // 上次攻击的时刻（世界时间），用于闲置隐藏计时
    float LastAttackTime = 0.0f;

    // 战斗动作强制显形剩余时间（秒）：>0 时闲置隐藏不生效（Tick 递减）
    float CombatShowRemaining = 0.0f;

    // ---- 战斗数值 ----
    // 武器攻击力（装备加成）：角色装备后，总攻击力 = 角色基础攻击力 + 武器攻击力 + 装备总攻击力。
    // 该数值直接叠加到伤害基数（不参与倍率），例如 test_knife 设 648 即装备后攻击力 +648。
    // 在武器蓝图里设置（默认 0 = 无攻击力加成）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat", meta = (ClampMin = "0.0"))
    float AttackPower = 0.0f;

    // 普攻倍率：近战/远程普攻每次命中的伤害倍率。规整叫法——原「普攻伤害(BaseDamage)」
    // 改为「普攻倍率」，最终伤害 = 角色攻击力(GetAttackPower) × 普攻倍率 × 增伤乘数。
    // 默认 0.25 表示 1/4 攻击力（攻击力 100 → 25 伤害，与旧固定 25 对齐）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat", meta = (ClampMin = "0.0"))
    float BaseDamage = 0.25f;

    // 近战攻击判定距离（cm）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat")
    float AttackRange = 180.0f;

    // 近战攻击判定半径（cm）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat")
    float AttackRadius = 60.0f;

    // 攻击开始到命中判定的延迟（秒），用于对齐攻击动画的打击帧
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat")
    float AttackHitDelay = 0.2f;

    // 远程弹丸类（手枪/音感仪必须配置）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat")
    TSubclassOf<AProjectileBase> ProjectileClass;

    // 远程弹丸倍率：远程普攻每次命中的伤害倍率。规整叫法——原「弹丸伤害」改为「弹丸倍率」，
    // 最终伤害 = 角色攻击力(GetAttackPower) × 弹丸倍率 × 增伤乘数。默认 0.2（攻击力 100 → 20 伤害）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Combat", meta = (ClampMin = "0.0"))
    float ProjectileDamage = 0.2f;

    // 持械角色
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
    TWeakObjectPtr<ABattleCharacter> OwningCharacter;

    // 近战命中判定定时器
    FTimerHandle MeleeTraceTimer;
};
