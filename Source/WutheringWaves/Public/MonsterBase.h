#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "BagTypes.h"
#include "MonsterBase.generated.h"

class UWidgetComponent;
class UUserWidget;
class UTextBlock;
class UWidgetAnimation;
class UCameraComponent;
class UAnimMontage;
class UPointLightComponent;
class ABossSting;

// 怪物等级：小怪 / Boss
UENUM(BlueprintType)
enum class EMonsterRank : uint8
{
    Minion UMETA(DisplayName = "小怪"),
    Boss UMETA(DisplayName = "Boss")
};

// AI 状态机
UENUM(BlueprintType)
enum class EMonsterAIState : uint8
{
    Idle,       // 待机（小怪：等待巡逻；Boss：原地驻守）
    Patrol,     // 小怪巡逻中
    Chase,      // 追踪角色
    Attack,     // 攻击角色
    Return,     // 小怪脱战回家
    Dead        // 死亡
};

// 单段攻击配置：每段攻击可独立设置蒙太奇、攻击速度、判定范围、是否可弹刀
USTRUCT(BlueprintType)
struct FMonsterAttackConfig
{
    GENERATED_BODY()

    // 该段攻击蒙太奇（留空则该段不参与随机选择）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    TObjectPtr<UAnimMontage> Montage;

    // 该段攻击速度（蒙太奇播放速率）：1=原速，2=两倍速（更快的前摇/伤害也来得更快）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0.05"))
    float PlayRate = 1.0f;

    // 该段攻击判定半径（cm）。0 = 使用全局 AttackHitRadius
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0.0"))
    float HitRadius = 0.0f;

    // 该段攻击判定扇形角度（度），360=全方位圆形。-1 = 使用全局 AttackHitArcAngle
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "-1.0", ClampMax = "360.0"))
    float HitArcAngle = -1.0f;

    // 该段攻击是否可被角色弹刀打断（前摇期间角色按弹刀键 = 打断该攻击并触发硬直）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    bool bParryable = false;

    // 该段是否为"大幅度攻击"：命中角色后触发角色的击飞蒙太奇（角色需配置 KnockbackMontage，
    // 未配置则保持普通受击反应）。false = 小幅度攻击：命中只触发角色原设定的受击动画蒙太奇
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    bool bHeavyAttack = false;

    // 该段攻击是否使用"模型接触判定"：开启后伤害不在固定延迟点一次性结算，
    // 而是在伤害窗口内持续检测玩家是否进入"模型接触半径"（贴近身体的较小判定），
    // 第一次接触才造成伤害；窗口内未接触则落空。关闭则用范围判定（半径+扇形）一次性结算
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
    bool bUseContactHit = false;

    // 该段模型接触判定半径（cm，贴近身体的较小值，需玩家真正靠近挥击范围才算命中）。
    // 0 = 使用全局 ContactHitRadius
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0.0"))
    float ContactHitRadius = 0.0f;

    // 该段模型接触判定窗口时长（秒，从 AttackDamageDelay 起持续检测该时长，
    // 窗口内第一次接触即造成伤害）。0 = 使用全局 ContactHitWindow
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0.0"))
    float ContactHitWindow = 0.0f;

    // ---- 受击判定时间窗口（蒙太奇时间轴，秒）----
    // 只有该段蒙太奇播放到 [HitWindowStartTime, HitWindowEndTime] 区间内，
    // 受击判定（范围判定 / 模型接触判定）才生效；窗口外即使角色贴着怪物也不扣血。
    // 时间单位为蒙太奇时间轴秒数（蒙太奇编辑器里看到的时间，与 PlayRate 无关）
    // -1 = 回落到全局 AttackDamageDelay（作为窗口起始，自动换算为蒙太奇时间）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "-1.0"))
    float HitWindowStartTime = -1.0f;

    // 受击判定窗口结束时间（蒙太奇时间轴秒数）。-1 = 回落：
    // 接触判定段 = 起始 + ContactHitWindow（换算为蒙太奇时间）；范围判定段 = 起始 + 0.1（近似单点结算）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "-1.0"))
    float HitWindowEndTime = -1.0f;

    // ---- 弹刀判定时间窗口（蒙太奇时间轴秒，需 bParryable=true 才生效）----
    // 只有该段蒙太奇播放到 [ParryWindowStartTime, ParryWindowEndTime] 区间内，
    // 角色攻击命中怪物才会触发弹刀（打断该次攻击 + 怪物播放弹刀反应蒙太奇）。
    // 窗口外即使 bParryable=true 也不会触发弹刀。
    // 时间单位为蒙太奇时间轴秒数（与 HitWindowStartTime/EndTime 同基准）
    // -1 = 回落：起始=0（蒙太奇开头），结束=HitWindowStartTime（受击判定窗口起始，
    //   即弹刀窗口默认覆盖"前摇阶段"——攻击挥出之前的蓄力/举刀动作）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "-1.0", EditCondition = "bParryable"))
    float ParryWindowStartTime = -1.0f;

    // 弹刀判定窗口结束时间（蒙太奇时间轴秒数）。-1 = 回落到 HitWindowStartTime
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "-1.0", EditCondition = "bParryable"))
    float ParryWindowEndTime = -1.0f;
};

// ---- Boss 狂暴技能的"段"配置 ----
// 血量首次跌破阈值 → 回血阶段（无敌/回满血/不移动）→ 结束后立刻释放的多段蒙太奇技能。
// 每段可独立配置：蒙太奇、播放速率、有效攻击时间段（蒙太奇时间轴秒）、每次伤害、
// 重复伤害间隔、该段期间的减伤比例。技能全程不可被打断（霸体），可被攻击（按减伤结算）
USTRUCT(BlueprintType)
struct FBossEnrageSegment
{
    GENERATED_BODY()

    // 该段蒙太奇（留空则该段被跳过）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage")
    TObjectPtr<UAnimMontage> Montage = nullptr;

    // 该段播放速率（1=原速，2=两倍速）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage", meta = (ClampMin = "0.05"))
    float PlayRate = 1.0f;

    // 有效攻击时间段起始（蒙太奇时间轴秒，蒙太奇编辑器里看到的时间，与 PlayRate 无关）。
    // 只有播放位置进入 [起始, 结束] 区间内才对攻击范围内（600cm 半球体）的玩家结算伤害。
    // -1 = 从头开始（0）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage", meta = (ClampMin = "-1.0"))
    float AttackWindowStartTime = -1.0f;

    // 有效攻击时间段结束（蒙太奇时间轴秒）。-1 = 蒙太奇全长
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage", meta = (ClampMin = "-1.0"))
    float AttackWindowEndTime = -1.0f;

    // 该段攻击时间段内每次命中对玩家造成的伤害。0 = 使用全局 AttackDamage
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage", meta = (ClampMin = "0.0"))
    float DamagePerHit = 0.0f;

    // 对玩家两次伤害的最小间隔（秒）。持续处于攻击范围内时按该间隔重复结算。
    // 0 = 该段内只结算一次
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage", meta = (ClampMin = "0.0"))
    float HitInterval = 0.5f;

    // 该段期间的减伤比例（0~1）：怪物受到的伤害乘以 (1-该值)。0 = 无减伤，1 = 完全免疫
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enrage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float DamageReduction = 0.5f;
};

// 摄像机贴近怪物时的遮挡处理方式
UENUM(BlueprintType)
enum class EMonsterOcclusionMode : uint8
{
    // 渐变半透明（默认）：摄像机靠近时怪物逐渐变透明，不遮挡视野。
    // 需要怪物材质配合：Blend Mode=Translucent 且提供名为 FadeOpacity 的标量参数连到 Opacity
    Fade UMETA(DisplayName = "渐变半透明"),
    // 直接隐藏：摄像机距离内怪物模型整体消失（旧方案，无需改材质）
    Hide UMETA(DisplayName = "直接隐藏")
};

UCLASS()
class WUTHERINGWAVES_API AMonsterBase : public ACharacter
{
    GENERATED_BODY()

public:
    AMonsterBase();

    // 受击扣血：受击立即进入战斗（反击仇恨），血量归零死亡
    virtual float TakeDamage(float DamageAmount, const struct FDamageEvent& DamageEvent, class AController* EventInstigator, AActor* DamageCauser) override;

    // ---- 伤害飘字（角色打怪时弹出，与头顶血条同机制：屏幕空间 WidgetComponent 挂在怪物身上）----
    // 弹出一条伤害飘字：每次调用弹出一条全新实例（新伤害不会覆盖上一条），显示
    // DamageNumberLifetime 秒（默认 1 秒，蓝图可调）后自动淡出销毁。
    // bCrit=true 用暴击飘字类 DamageNumberCritWidgetClass（未配置时回落普通类），否则用普通类。
    // 只显示 DamageAmount 的整数部分；<=0（无敌/死亡等无效命中）不弹出。
    UFUNCTION(BlueprintCallable, Category = "UI|DamageNumber")
    void ShowDamageNumber(float DamageAmount, bool bCrit);

    // 角色侧伤害结算（ComputeFinalDamage）后、ApplyPointDamage 前调用：告知本次命中的暴击结果。
    // TakeDamage 弹出飘字时立即消费并复位——未设置的命中一律按非暴击处理，标志不残留。
    void SetPendingDamageNumberCrit(bool bCrit) { bPendingDamageNumberCrit = bCrit; }

    // ---- 攻击前摇查询（完美闪避判定用）----
    // 是否处于攻击前摇窗口（攻击已发起、伤害尚未结算）。
    // 玩家在此窗口内闪避 = 完美闪避（不扣耐力 + 专用蒙太奇 + 短暂闪避锁定）
    UFUNCTION(BlueprintPure, Category = "Monster|Combat")
    bool IsTelegraphingAttack() const { return bIsTelegraphing; }

    // ---- 完美闪避判定窗口查询（角色闪避时用）----
    // 覆盖范围比 IsTelegraphingAttack 更宽：前摇窗口 + 接触判定窗口 + 优雅期(GracePeriod)。
    // 接触模式下前摇会延伸到接触窗口结束；优雅期在前摇结束后再放宽一段时间，
    // 使玩家即使在伤害结算后才闪避仍能触发完美闪避——更贴近《鸣潮》手感
    UFUNCTION(BlueprintPure, Category = "Monster|Combat")
    bool IsInPerfectDodgeWindow() const;

    // ---- 死亡查询（相机推开等系统排除死亡怪物用）----
    UFUNCTION(BlueprintPure, Category = "Monster|Health")
    bool IsDead() const { return AIState == EMonsterAIState::Dead; }

    // ---- 战斗状态查询（供角色侧判断「是否已引起怪物仇恨/进入战斗」用）----
    // 返回该怪物是否处于战斗状态（bInCombat）。角色切人自动普攻等逻辑用此判定：
    // 只有附近存在已进入战斗的怪物（仇恨已建立）才触发，避免脱战状态误触发。
    UFUNCTION(BlueprintPure, Category = "Monster|Combat")
    bool IsInCombat() const { return bInCombat; }

    // ---- 怪物等级（伤害结算的等级系数用）----
    // 返回怪物数值等级（LV_num），供玩家伤害公式的等级系数 (100+角色等级)/(199+角色等级+怪物等级) 使用
    UFUNCTION(BlueprintPure, Category = "Monster")
    int32 GetMonsterLevel() const { return MonsterLevel; }

    // ---- 弹刀接口 ----
    // 当前是否处于"可弹刀攻击"的弹刀时间窗口内（该段配置了 bParryable=true 且
    // 蒙太奇播放位置在 [ParryWindowStart, ParryWindowEnd] 区间内）。
    // 角色在此窗口内攻击命中怪物 = 弹刀成功，打断该次攻击
    UFUNCTION(BlueprintPure, Category = "Monster|Combat")
    bool IsTelegraphingParryableAttack() const { return bParryWindowOpen && bCurrentAttackParryable; }

    // 弹刀打断当前攻击：停攻击蒙太奇、取消伤害结算、播放弹刀反应蒙太奇、进入硬直（硬直期内无法再次攻击）。
    // 仅当当前处于弹刀时间窗口（蒙太奇播放位置在 [ParryWindowStart, ParryWindowEnd] 内）时生效，成功返回 true
    UFUNCTION(BlueprintCallable, Category = "Monster|Combat")
    bool InterruptAttackByParry(AActor* Parrier);

    // 弹刀提示事件：怪物攻击蒙太奇播放进入弹刀时间窗口时触发（蓝图实现弹刀提示 UI/音效，如怪物头顶闪黄光）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Combat")
    void OnParryableTelegraph(int32 AttackIndex);

    // 弹刀窗口结束事件：蒙太奇播放离开弹刀时间窗口 / 攻击被打断 / 死亡时触发（蓝图隐藏弹刀提示）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Combat")
    void OnParryWindowEnd();

    // 出伤预警事件：攻击蒙太奇播放到出伤时间前 DamageWarningLeadTime 秒时触发
    // （蓝图实现屏幕边缘警告/音效/震屏等系统提示；头顶预警 UI 由 C++ 自动显示）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Combat")
    void OnDamageWarning();

    // 出伤预警结束事件：到达出伤时间开始结算伤害 / 攻击被打断 / 死亡时触发（蓝图隐藏警告）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Combat")
    void OnDamageWarningEnd();

    // 弹刀成功事件：攻击被角色弹刀打断时触发（蓝图实现火花特效/音效/怪物受击反馈）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Combat")
    void OnParried(AActor* Parrier);

    // ---- 完美闪避反馈 ----
    // 被玩家完美闪避时调用：不打断怪物的攻击动画，攻击继续播完。
    // 仅触发 OnPerfectDodged 事件供蓝图加时停/慢动作/特效，并返回建议的角色无敌时长。
    // 返回值 = max(Duration, 该次攻击判定窗口剩余时间)：
    // 保证"完美闪避触发 → 该段攻击受击判定时间结束"全程无敌
    UFUNCTION(BlueprintCallable, Category = "Monster|Combat")
    float StaggerByPerfectDodge(float Duration);

    // 完美闪避停滞（时停）：冻结怪物时间流速（动画/移动/AI 计时全部暂停，动作不打断）
    void ApplyPerfectDodgeFreeze();

    // 解除完美闪避停滞：恢复时间流速（世界时间定时器驱动——怪物自身 Tick 已被冻结无法计时）
    void RestoreFromPerfectDodgeFreeze();

    // ---- 大招停滞（时停）：角色大招施放期间冻结怪物（动画/移动/AI 暂停，攻击不打断）----
    // 冻结怪物时间流速（CustomTimeDilation=0）：蒙太奇暂停而非停止——怪物攻击动作不会被打断，
    // 恢复后从暂停处继续播放。与完美闪避时停不同：大招停滞无固定时长，由角色大招
    // EndUltimate 主动调用 RestoreFromUltimateFreeze 恢复（覆盖整个大招多段蒙太奇全程）
    void ApplyUltimateFreeze();

    // 解除大招停滞：恢复时间流速，怪物从暂停处继续动作
    void RestoreFromUltimateFreeze();

    // 完美闪避命中事件：怪物被完美闪避时触发（蓝图实现时停/慢动作/特效/音效）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Combat")
    void OnPerfectDodged();

    // ---- Boss 狂暴系统（血量首次跌破阈值 → 无敌回血 → 多段狂暴技能 → 增伤+红光）----
    // 回血阶段进行中（无敌 + 逐步回满血 + 不移动）
    UFUNCTION(BlueprintPure, Category = "Monster|Boss|Enrage")
    bool IsEnrageHealing() const { return bIsEnrageHealing; }

    // 狂暴技能施放中（霸体不可打断，可被攻击但按逐段减伤结算）
    UFUNCTION(BlueprintPure, Category = "Monster|Boss|Enrage")
    bool IsEnrageSkillCasting() const { return bIsEnrageSkillCasting; }

    // 狂暴技能声波攻击阶段进行中（声波模式开启且狂暴技能段正在播放）
    UFUNCTION(BlueprintPure, Category = "Monster|Boss|Enrage")
    bool IsEnrageSoundWaveStage() const { return bEnrageSoundWaveStage; }

    // 声波扩散范围半径（cm，角色完美闪避探测半径需覆盖到它）
    float GetEnrageSoundWaveRadius() const { return EnrageSoundWaveRadius; }

    // ---- Boss 二阶段毒刺远程攻击 ----
    // 毒刺发射动画/飞行阶段进行中（远程蒙太奇播放中或毒刺在飞）
    UFUNCTION(BlueprintPure, Category = "Monster|Boss|Enrage|Sting")
    bool IsBossStingStage() const { return bBossStingStage; }

    // 毒刺飞行中（用于完美闪避窗口判定扩展）
    bool IsBossStingFlying() const { return bBossStingFlying; }

    // 当前在飞毒刺是否处于「可完美闪避」状态（毒刺距玩家 > 阈值）
    bool IsBossStingPerfectDodgeWindow() const;

    // 毒刺最大有效范围（cm，超出自毁）。独立配置值；0 表示不限距离（永不因射程自毁）
    float GetBossStingMaxFlightDistance() const
    {
        return BossStingMaxFlightDistance;
    }

    // 毒刺命中结果回调（由 ABossSting::Resolve 调用）：命中玩家 → 瞬移背刺连击；未命中 → 瞬移背后不打断攻击频率
    void OnBossStingResolved(bool bHitPlayer);

    // 狂暴已完成（后续攻击增伤生效 + 身上红光亮起）
    UFUNCTION(BlueprintPure, Category = "Monster|Boss|Enrage")
    bool IsEnraged() const { return bEnraged; }

    // 狂暴完成事件：增伤生效 + 红光亮起时触发（蓝图可追加 Niagara 特效/音效/换色等）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Boss|Enrage")
    void OnEnraged();

    // 声波发出事件：每隔 EnrageSoundWaveInterval 秒、每个声波圈扩散开始时触发一次。
    // （蓝图在此实现声波特效/音效：从怪物中心生成一个圆环/光波并扩散到 Radius 半径；
    //  Radius = 该波最大扩散半径，ExpandSpeed = 扩散速度 cm/s，用于驱动特效同步速率）
    UFUNCTION(BlueprintImplementableEvent, Category = "Monster|Boss|Enrage")
    void OnEnrageSoundWaveEmitted(float Radius, float ExpandSpeed);

    // 攻击距离（完美闪避/弹刀判定用：只有怪物攻击范围内的玩家才算被瞄准）。
    // 返回实际触发距离：取 AttackRange、AttackHitRadius 与各段 HitRadius 的最大值
    UFUNCTION(BlueprintPure, Category = "Monster|Combat")
    float GetAttackRange() const
    {
        float Result = FMath::Max(AttackRange, AttackHitRadius);
        for (const FMonsterAttackConfig& Cfg : AttackConfigs)
        {
            Result = FMath::Max(Result, Cfg.HitRadius);
        }
        return Result;
    }

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

    // ---- 等级 ----
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Monster")
    EMonsterRank Rank = EMonsterRank::Minion;

    // 怪物数值等级（LV_num 显示用）：在怪物蓝图里设置，实时同步到 WBP_BossHealthBar 的 LV_num 文本
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster", meta = (ClampMin = "1", ClampMax = "999"))
    int32 MonsterLevel = 1;

    // ---- 生命 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Health")
    float MaxHealth = 100.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Monster|Health")
    float CurrentHealth = 100.0f;

    // ---- 掉落物（击败后自动进背包，不需要拾取）----
    // 在怪物蓝图的 Details → Monster|Drop 里配置。击败怪物时按此清单结算：
    // 逐条判定概率 → 取数量 → 直接加进玩家背包 → 屏幕左侧弹出「物品名 ×N」提示。
    // 无需生成掉落物 Actor、无需拾取交互。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Drop")
    bool bEnableDrops = true;

    // 掉落物数据表路径：Item Row Name 下拉框的选项来源，也是「掉多少个」的依据。
    // 正常与背包侧（InventoryComponent::DropItemTablePath）指向同一张表。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Drop")
    FString DropTablePath = TEXT("/Game/UI/bag_sys/Data_diaoluo_boss.Data_diaoluo_boss");

    // 掉落清单：可配多条 = 掉落多种物品，每条各自设置概率与数量。
    // 例：3 条 = 必定掉「boss掉落物」、必定再掉一种、30% 概率掉第三种。
    // 物品从 Data_diaoluo_boss（上面的路径）里选；数量默认也取表里那一行的 tool_num。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Drop")
    TArray<FMonsterDropEntry> DropItems;

    // 共享掉落表（可选）：指定后使用该资产里的清单，DropItems 被忽略。
    // 适合「多只怪共用同一套掉落」；单只怪单独配置时留空即可
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Drop")
    TObjectPtr<UMonsterDropTable> SharedDropTable = nullptr;

    // 本次生命周期的掉落是否已结算（防止同一只怪重复掉落）。
    // Die() 时置位；怪物脱战回原点复活（ResetHealth）时复位，复活后可再次掉落
    bool bDropsGranted = false;

    // ---- 小怪：巡逻/仇恨参数（单位 cm，UE 中 1m = 100cm）----
    // 巡逻半径：在出生点周围该范围内随机游走（默认 150m）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float PatrolRadius = 15000.0f;

    // 发现角色半径：角色进入该范围即追踪（默认 150m）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float DetectRadius = 15000.0f;

    // 脱战距离：角色超出该距离后开始计脱战时间（默认 300m）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float LeashRadius = 30000.0f;

    // 脱战判定时间：角色持续超出 LeashRadius 多少秒后，小怪回家并重置血量（默认 5s）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float OutOfCombatDelay = 5.0f;

    // 巡逻到达目标点后的停留时间
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float PatrolWaitTime = 2.0f;

    // 巡逻移速
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float PatrolSpeed = 300.0f;

    // 追踪/攻击移速
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Minion")
    float ChaseSpeed = 500.0f;

    // ---- Boss：仇恨参数 ----
    // 触发战斗范围（cm）：角色进入以 Boss 为中心的该范围即触发战斗（默认 400m）。
    // 独立于追击/脱战边界（AggroRadius），专用于 Boss 的「进战触发」判定，方便单独调整。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss", meta = (ClampMin = "0.0"))
    float BossCombatTriggerRadius = 40000.0f;

    // 仇恨半径（cm）：Boss 的追击/脱战保持边界语义，角色越过 AggroRadius 与 DetectRadius
    // 的较大值（GetBossChaseRadius）才脱战原地驻留。进战触发请改用 BossCombatTriggerRadius。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss", meta = (ClampMin = "0.0"))
    float AggroRadius = 40000.0f;

    // ---- 攻击参数 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    float AttackDamage = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    float AttackRange = 200.0f;

    // ---- 攻击触发距离（攻击 vs 追击的切换半径）----
    // 玩家距离 <= 该值 → 原地攻击；> 该值 → 追击（Chase）。
    // 默认 -1 = 自动回落近战触发基线 AttackRange：保证怪物会追到近身才攻击。
    // 注意：伤害判定半径（AttackHitRadius / 逐段 HitRadius）只决定"能否命中"，
    // 不再决定"是否追击"——防止大范围 AOE 判定半径导致 Boss 永远原地攻击不追人。
    // 远程型怪物可显式设大该值，让它在远处原地输出（不追击）。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "-1.0"))
    float AttackTriggerDistance = -1.0f;

    // ---- 大范围攻击判定（伤害结算用）----
    // 攻击判定半径（cm）：伤害结算时以怪物为圆心，玩家落在该半径内即受击。
    // 独立于 AttackRange（近身触发基线）与 AttackTriggerDistance（攻击/追击切换）——
    // 该值只决定"这一击能否命中"，不决定"怪物是否追过去打"。
    // 因此可放心调大获得大范围 AOE 攻击，不会让 Boss 因此永远原地攻击不追人。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float AttackHitRadius = 350.0f;

    // 攻击判定扇形角度（度）：以怪物攻击时的朝向为中心。
    // 360 = 全方位圆形判定（默认，大范围 AOE）；更小值 = 只打身前扇形（如 120 为挥砍类近战）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0", ClampMax = "360.0"))
    float AttackHitArcAngle = 360.0f;

    // 攻击判定高度容差（cm）：玩家与怪物的高度差在该值内才算命中（防止下落攻击从头顶蹭判定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float AttackHitHeightTolerance = 200.0f;

    // ---- 模型接触判定（逐段 bUseContactHit=true 时使用）----
    // 模型接触判定半径（cm，全局默认；逐段 ContactHitRadius>0 时覆盖）。
    // 贴近身体的较小值（区别于 AttackHitRadius 的大范围 AOE），需玩家真正靠近怪物挥击范围才算命中
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float ContactHitRadius = 120.0f;

    // 模型接触判定窗口时长（秒，全局默认；逐段 ContactHitWindow>0 时覆盖）。
    // 从 AttackDamageDelay 起持续检测该时长，窗口内第一次接触即造成伤害，窗口结束未接触则落空
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float ContactHitWindow = 0.25f;

    // 接触判定多段伤害间隔（秒）：玩家持续停留在接触半径内时，每隔该时间造成一次伤害。
    // 玩家离开接触半径再进入则立即结算一次（不等待间隔）。0=每次检测都结算（极高频）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float ContactHitDamageCooldown = 0.5f;

    // 完美闪避优雅期（秒）：前摇窗口结束后仍允许该时间内闪避触发完美闪避。
    // 使玩家即使在伤害已结算后闪避也能获得完美闪避反馈——更贴近《鸣潮》手感
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float PerfectDodgeGracePeriod = 0.3f;

    // ---- 完美闪避停滞（时停）----
    // 触发完美闪避时怪物整体停滞：CustomTimeDilation=0 冻结动画/移动/AI 计时。
    // 蒙太奇是暂停而非停止——不会打断怪物动作，恢复后从暂停处继续播放
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    bool bPerfectDodgeFreeze = true;

    // 停滞时长（秒，真实时间；怪物自身时间被冻结，由世界时间定时器负责恢复）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float PerfectDodgeFreezeDuration = 0.5f;

    // 调试绘制攻击判定范围（战斗中每帧画判定圆/扇形边界，方便调参数）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    bool bDebugDrawAttackHit = false;

    // 攻击间隔（秒，从攻击发起开始计），默认 4s
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    float AttackCooldown = 4.0f;

    // ---- 攻击间隔随机区间：每次攻击发起后从 [MinAttackInterval, MaxAttackInterval] 随机取值 ----
    // （默认 1-2.5s；Min>Max 或负数等异常配置回落到固定 AttackCooldown）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float MinAttackInterval = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float MaxAttackInterval = 2.5f;

    // 攻击判定延迟（秒）：未配置逐段 HitWindowStartTime 时的判定窗口起始（真实秒，
    // 内部自动换算为蒙太奇时间轴秒数；判定窗口的其余部分由 ContactHitWindow 等回落值决定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    float AttackDamageDelay = 0.3f;

    // ---- 动画蒙太奇 ----
    // 移动蒙太奇（可选）：巡逻/追踪移动时循环播放，停下即停止。
    // 注意：蒙太奇资产内需设置循环段（如 Default 循环）才能循环
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    TObjectPtr<UAnimMontage> MoveMontage;

    // 移动蒙太奇原生速度（cm/s）：该动画制作时对应的前进速度（如跑步动画 300）。
    // 播放速率按「当前实际移速 / 原生速度」每帧动态同步，使脚步速度与世界位移速度
    // 一致，消除滑步。ChaseSpeed 越快播放越快，反之亦然。
    // -1 = 自动从蒙太奇内动画序列的根运动位移推导；In-Place 动画（无根运动）回落 300
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation", meta = (ClampMin = "-1.0"))
    float MoveMontageNativeSpeed = -1.0f;

    // ---- Boss 脱战行为（待机/寻找/回位）----
    // 总开关：true = Boss 脱战时启用待机循环 + 脱战寻找 + 回原点重置；false = 旧行为（脱战原地驻留）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Idle")
    bool bEnableBossIdleBehavior = true;

    // 待机蒙太奇（可选）：脱战（未触发战斗）时反复循环播放，直到进入战斗。
    // 需在蒙太奇资产内设置循环段才会循环。留空则脱战不播待机动画（保持旧驻留表现）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Idle")
    TObjectPtr<UAnimMontage> IdleMontage;

    // 寻找蒙太奇（可选）：脱战延迟结束后原地播放（表现「寻找玩家」），播完再回原点。
    // 留空则跳过寻找动画，直接回原点
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Idle")
    TObjectPtr<UAnimMontage> SearchMontage;

    // 寻找蒙太奇播放时长（秒）：原地播放寻找动画的时长。<=0 时不播寻找，直接回原点
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Idle", meta = (ClampMin = "0.0"))
    float SearchMontageDuration = 3.0f;

    // 脱战延迟（秒）：角色脱离追击范围（GetBossChaseRadius）后持续多少秒，才进入寻找/回位流程。
    // 与「立即脱战（LeaveCombat）」区分：立即脱战只停止追击原地驻留；此延迟满后才触发「寻找→回原点」
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Idle", meta = (ClampMin = "0.0"))
    float OutOfCombatSearchDelay = 3.0f;

    // 回原点移速（cm/s）：播完寻找动画后走回出生点的速度（默认复用 ChaseSpeed 若 <=0）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Idle", meta = (ClampMin = "0.0"))
    float ReturnHomeSpeed = 0.0f;

    // ---- 逐段攻击配置（推荐）----
    // 每段攻击独立配置：蒙太奇、攻击速度（PlayRate）、判定范围（HitRadius/HitArcAngle）、是否可弹刀（bParryable）。
    // 非空时优先使用（旧的 AttackMontages 与全局速度/范围被忽略）；留空则回落到 AttackMontages + 全局参数
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    TArray<FMonsterAttackConfig> AttackConfigs;

    // ---- 旧版攻击蒙太奇（4段）：AttackConfigs 为空时使用 ----
    // 每次攻击随机选一段播放，且不与上一次播放的重复
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    TArray<TObjectPtr<UAnimMontage>> AttackMontages;

    // 旧版攻击蒙太奇统一播放速率（仅 AttackConfigs 为空时生效）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    float AttackMontagePlayRate = 1.0f;

    // 旧版攻击蒙太奇（AttackConfigs 为空时的回落路径）是否为"大幅度攻击"：
    // true = 命中角色触发角色击飞蒙太奇；false = 小幅度攻击（普通受击反应）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    bool bLegacyAttackIsHeavy = false;

    // 死亡蒙太奇（可选）：怪物死亡时播放。未配置则保持旧行为（直接停止动画 → 3s 后销毁）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    TObjectPtr<UAnimMontage> DeathMontage;

    // 死亡蒙太奇播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    float DeathMontagePlayRate = 1.0f;

    // 死亡后销毁延迟（秒）：仅在未配置死亡蒙太奇时生效（尸体保留时间）。
    // 配置了死亡蒙太奇时：蒙太奇内容播完的瞬间直接销毁模型（不保留尸体、不会站起来）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    float DeathDestroyDelay = 3.0f;

    // ---- 受击反应蒙太奇 ----
    // 受击时随机播放其中一段。留空 = 不播放受击动画（只扣血）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    TArray<TObjectPtr<UAnimMontage>> HitReactionMontages;

    // 受击反应蒙太奇播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation", meta = (ClampMin = "0.05"))
    float HitReactionPlayRate = 1.0f;

    // 受击反应是否打断进行中的攻击（true=受击硬直打断攻击；false=霸体，攻击继续不受影响）。
    // 小怪默认 true（受击即被打断）；Boss 可设 false 实现霸体效果（攻击期间受击不中断）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    bool bHitReactionInterruptsAttack = true;

    // 受击反应冷却（秒）：两次受击反应的最小间隔。0=每次受击都播放（可被高频多段攻击刷屏）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float HitReactionCooldown = 0.0f;

    // ---- 弹刀反应蒙太奇 ----
    // 怪物被角色弹刀（InterruptAttackByParry）成功后播放的蒙太奇。
    // 播放时攻击蒙太奇已被停止（Montage_Stop），弹刀蒙太奇在硬直期间继续播放
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation")
    TObjectPtr<UAnimMontage> ParryReactionMontage;

    // 弹刀反应蒙太奇播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Animation", meta = (ClampMin = "0.05"))
    float ParryReactionPlayRate = 1.0f;

    // ---- Boss 狂暴系统（仅 Rank=Boss 生效；血量首次跌破阈值触发一次）----
    // 总开关：血量首次低于 EnrageHealthPercent 时打断自身动作进入狂暴流程
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    bool bEnableEnrage = true;

    // 触发血量百分比（0~1）：当前血量 / 最大血量 首次低于该值时触发（默认 30%）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EnrageHealthPercent = 0.3f;

    // 回血阶段动画蒙太奇：触发狂暴时打断自身动作立即播放（期间无敌、不移动）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    TObjectPtr<UAnimMontage> EnrageHealMontage;

    // 回血阶段播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.05"))
    float EnrageHealPlayRate = 1.0f;

    // 回血阶段总时长（秒）：期间无敌并从触发时血量逐步恢复至满血；蒙太奇短于该时长会循环重播
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.1"))
    float EnrageHealDuration = 5.0f;

    // 狂暴技能多段配置：回血结束后立刻朝玩家按顺序播放一次（全程不可被打断）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    TArray<FBossEnrageSegment> EnrageSkillSegments;

    // 狂暴技能攻击范围半径（cm）：以怪物为球心的上半球体范围，范围内的玩家受击（默认 600cm）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.0"))
    float EnrageSkillRadius = 600.0f;

    // 调试绘制狂暴技能作用范围：狂暴技能释放期间（bIsEnrageSkillCasting）每帧画出
    // EnrageSkillRadius 的判定圆（紫色）+ 朝向指示线，方便调范围参数。蓝图可开关
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    bool bDebugDrawEnrageRange = false;

    // ---- 狂暴技能：声波扩散攻击（开启后替代原"攻击窗口内节流范围伤害"）----
    // 以怪物为圆心周期性扩散声波圈：每个 EnrageSoundWaveInterval 秒发一波，波前从圆心以
    // EnrageSoundWaveExpandSpeed 向外扩散到 EnrageSoundWaveRadius 后消失；波前扫过玩家瞬间
    // 结算一次 EnrageSoundWaveDamage 伤害；波前距玩家 <= EnrageSoundWavePerfectDodgeMargin
    // （含 GraceSeconds 提前缓冲换算距离）的时机内闪避 = 完美闪避
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    bool bEnrageSoundWaveSkill = true;

    // 声波从第几个狂暴技能段开始触发（0 = 第一段即发波，1 = 第二段起才发波）。
    // 用于「第一段为空中/位移动作、第二段为攻击动作」的分段设计：第一段保持干净演出，
    // 声波只在指定段（默认第二段）内周期性扩散，避免第一段空中就发波导致手感突兀、难以闪避。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0"))
    int32 EnrageSoundWaveStartSegmentIndex = 1;

    // 声波释放频率：相邻两波发出的间隔（秒）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.05"))
    float EnrageSoundWaveInterval = 1.2f;

    // 声波扩散范围半径（cm，默认 600）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "1.0"))
    float EnrageSoundWaveRadius = 600.0f;

    // 单波伤害（<=0 时回落到当前狂暴段 DamagePerHit，再回落全局 AttackDamage）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.0"))
    float EnrageSoundWaveDamage = 0.0f;

    // 声波扩散速度（cm/s，600cm 圈约需 Radius/Speed 秒扩散完；越小观感越慢越从容）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "50.0"))
    float EnrageSoundWaveExpandSpeed = 1000.0f;

    // 完美闪避基础容差（cm）：波前到玩家的径向距离 <= 该值视为"声波到达模型"（默认 10cm）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.0"))
    float EnrageSoundWavePerfectDodgeMargin = 10.0f;

    // 完美闪避判定提前缓冲（秒）：容差不足一帧会漏判，给波前到达玩家前额外一段提前量，
    // 实际生效容差 = Margin + ExpandSpeed × GraceSeconds（默认 0.15s ≈ 150cm @1000cm/s）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.0"))
    float EnrageSoundWaveGraceSeconds = 0.15f;

    // ---- 声波运行期状态（非序列化）----
    // 当前狂暴技能是否处于声波攻击阶段（技能段播放中，EndEnrageSkill/死亡复位）
    bool bEnrageSoundWaveStage = false;
    // 当前是否存在正在扩散的声波圈
    bool bEnrageWaveInFlight = false;
    // 当前波是否已结算过伤害（每波只命中一次）
    bool bEnrageWaveHitResolved = false;
    // 当前波已扩散时长（秒）
    float EnrageWaveAge = 0.0f;
    // 距下一波发出的剩余冷却（秒）
    float EnrageWaveCooldownRemaining = 0.0f;
    // 本帧波前是否处于玩家完美闪避窗口（由 Tick 内 UpdateEnrageSoundWave 维护）
    bool bSoundWavePerfectWindow = false;

    // 狂暴完成后后续攻击的伤害倍率（默认 1.25 = 提升 25%）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.1"))
    float EnrageDamageMultiplier = 1.25f;

    // 狂暴完成后身上是否散发红光（点光源挂在模型上 + 尝试设置材质参数 EnrageGlow/EnrageGlowColor）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    bool bEnrageRedGlow = true;

    // 红光颜色
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage")
    FLinearColor EnrageGlowColor = FLinearColor(1.0f, 0.05f, 0.05f, 1.0f);

    // 红光强度（点光源亮度）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.0"))
    float EnrageGlowIntensity = 8000.0f;

    // 【狂暴技能后旋转守卫时长】技能结束后该时长内每帧压平 Actor 的 Pitch/Roll（Yaw 保留）。
    // 原因：蒙太奇结束后仍有 BlendOut 混合输出期（时长由蒙太奇资产决定，常见 0.25s+），
    // 根运动旋转会以递减权重继续施加到 Actor；固定 0.16s 定时补一次可能早于混合结束，
    // 导致模型残留歪斜（Boss 闲置时无 Controller 旋转兜底，残留会一直留到移动/攻击）。
    // 若蒙太奇 BlendOut 较长导致仍歪斜，调大该值即可。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage", meta = (ClampMin = "0.1"))
    float EnrageRotationGuardDuration = 0.8f;

    // 红光点光源（挂在怪物网格体上，狂暴完成时亮起）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Monster|Boss|Enrage")
    UPointLightComponent* EnrageLight = nullptr;

    // ---- Boss 二阶段毒刺远程攻击（狂暴完成后 bEnraged 生效）----
    // 总开关：二阶段战斗中，玩家在仇恨范围内但距离 > BossStingTriggerDistance 时，
    // 怪物播放远程攻击蒙太奇（蒙太奇本身不接触判定）并朝玩家发射一根毒刺。
    // 毒刺自身接触玩家 = 命中（命中即销毁，不触发受击/击飞动画）；未命中且飞行距离
    // 超出射程（默认 = 仇恨半径）则消失。命中后瞬移背刺连击，未命中瞬移背后不打断攻击频率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting")
    bool bEnableBossSting = true;

    // 触发距离（cm）：二阶段战斗中玩家距离 > 该值（且仍在仇恨范围内）才发射毒刺（默认 400m）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingTriggerDistance = 40000.0f;

    // 毒刺发射动画蒙太奇（怪物释放技能本身的动画，不接触判定；留空则跳过动画直接发射）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting")
    TObjectPtr<UAnimMontage> BossStingMontage;

    // 毒刺发射动画播放速率
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.05"))
    float BossStingMontagePlayRate = 1.0f;

    // 毒刺伤害
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingDamage = 40.0f;

    // 毒刺发射速度（cm/s）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "1.0"))
    float BossStingSpeed = 3000.0f;

    // 毒刺投射物类（默认加载 /Game/Blueprints 下的 BP_BossSting；未命中时可在怪物蓝图手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting")
    TSubclassOf<ABossSting> BossStingClass;

    // 毒刺最大有效范围（cm）：未命中玩家时超出该距离自动销毁。独立配置，不再由仇恨范围决定。
    // 默认 150m，可在蓝图任意调整（设为 0 表示不限距离，永不因射程自毁）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingMaxFlightDistance = 15000.0f;

    // 毒刺攻击判定半径（cm）：毒刺「命中判定球」半径，决定毒刺多近算接触玩家并结算伤害。
    // 调大 = 攻击判定范围更宽松（更容易命中，也更难靠走位擦身躲开）。
    // 发射时透传给 ABossSting::HitDetectionRadius（毒刺蓝图亦可单独覆盖）。默认 100cm。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "1.0"))
    float BossStingHitDetectionRadius = 100.0f;

    // 完美闪避距离阈值（cm）：毒刺距玩家 > 该值时，玩家闪避 = 完美闪避（不被视为被毒刺击中）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingPerfectDodgeDistanceThreshold = 800.0f;

    // 近身蹭闪范围（cm）：毒刺距玩家贴身边缘 <= 该值（尚未接触）时，玩家主动接近毒刺后
    // 闪避 = 完美闪避（险中求胜）。与 BossStingPerfectDodgeDistanceThreshold 之间为中间危险区
    //（不可完美闪避）。默认 120cm，可在蓝图调整。0 = 关闭近身蹭闪（仅保留远距离+嵌入蹭闪）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingCloseDodgeDistance = 120.0f;

    // 毒刺发射冷却（秒）：两次毒刺攻击的最小间隔，防止连续发射刷屏
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingCooldown = 4.0f;

    // 命中后延迟瞬移时间（秒）：毒刺命中玩家后延迟该秒数才瞬移至角色背后背刺（默认 1s，用于毒刺刺入演出）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting", meta = (ClampMin = "0.0"))
    float BossStingTeleportDelay = 1.0f;

    // 毒刺飞行中/发射动画期间怪物是否停止追击（true = 原地发射，false = 可边追边发射）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Boss|Enrage|Sting")
    bool bBossStingHoldStill = true;

    // ---- 毒刺运行期状态（非序列化）----
    // 毒刺发射动画/飞行阶段进行中
    bool bBossStingStage = false;
    // 毒刺飞行中（已发射、未结算）
    bool bBossStingFlying = false;
    // 毒刺发射冷却剩余（Tick 递减）
    float BossStingCooldownRemaining = 0.0f;
    // 当前在飞毒刺（完美闪避窗口判定查询用）
    TWeakObjectPtr<ABossSting> CurrentSting;

    // 命中后延迟瞬移：毒刺命中玩家后延迟 BossStingTeleportDelay 秒才瞬移背刺（等待毒刺刺入演出）
    // 运行期状态：待执行延迟瞬移的剩余时间（>0 表示命中已结算、等待倒计时）
    float BossStingTeleportDelayRemaining = 0.0f;

    // ---- Boss 二阶段毒刺内部函数 ----
    // 二阶段距离判定：满足条件则触发毒刺远程攻击（UpdateBossAI 调用）
    void TryTriggerBossSting(float DeltaTime);
    // 发射毒刺（播放远程蒙太奇 + 生成毒刺投射物）
    void SpawnBossSting();
    // 实际生成毒刺投射物（发射时刻调用）
    void EmitBossStingProjectile();
    // 瞬移到玩家背后（背刺起点）
    void TeleportBehindPlayer();
    // 命中后连击：依次播放所有「不可弹刀」的攻击段（每段带完整判定），播完恢复普通攻击频率
    void StartStingBackstabCombo();
    // 播放背刺连击当前段（依次播放不可弹刀攻击段，段播完自动推进下一段）
    void PlayBackstabComboSegment();
    // 毒刺阶段 Tick 驱动（蒙太奇播放中朝向玩家 / 毒刺飞行距离完美闪避窗口维护）
    void UpdateBossSting(float DeltaTime);
    // 背刺连击运行时状态：待播放的不可弹刀攻击段索引列表
    TArray<int32> BackstabComboIndices;
    // 背刺连击当前段索引
    int32 BackstabComboCursor = 0;
    // 背刺连击进行中
    bool bBackstabComboActive = false;
    // 播放指定索引的攻击段（逐段配置路径，复用 PlayRandomAttackMontage 的配置刷新逻辑）
    void PlaySpecificAttackConfig(int32 ConfigIndex);
    // 播放指定索引的旧版攻击蒙太奇
    void PlaySpecificAttackMontage(int32 MontageIndex);

    // ---- 摄像机防遮挡（怪物贴近镜头时）----
    // 处理方式：Fade=渐变半透明（默认，怪物逐渐变透明不挡视野，需材质配合）；
    // Hide=距离内直接隐藏（无需改材质，但怪物会整体消失）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Camera")
    EMonsterOcclusionMode CameraOcclusionMode = EMonsterOcclusionMode::Fade;

    // Fade 模式：摄像机距离小于该值时开始变透明（渐变起点）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Camera", meta = (EditCondition = "CameraOcclusionMode == EMonsterOcclusionMode::Fade"))
    float CameraFadeStartDistance = 400.0f;

    // Fade 模式：摄像机距离小于该值时达到最透明（渐变终点）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Camera", meta = (EditCondition = "CameraOcclusionMode == EMonsterOcclusionMode::Fade"))
    float CameraFadeEndDistance = 150.0f;

    // Fade 模式：最透明时的不透明度（0=全透明 ~ 1=不透明）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Camera", meta = (EditCondition = "CameraOcclusionMode == EMonsterOcclusionMode::Fade", ClampMin = "0.0", ClampMax = "1.0"))
    float MinFadeOpacity = 0.15f;

    // Hide 模式：摄像机与怪物距离小于该值时直接隐藏模型
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Camera", meta = (EditCondition = "CameraOcclusionMode == EMonsterOcclusionMode::Hide"))
    float CameraHideDistance = 250.0f;

    // ---- 小怪头顶血条（绑定在怪物身上，脱战隐藏）----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    UWidgetComponent* HealthBarWidget;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> HealthBarWidgetClass;

    // 头顶血条固定绘制尺寸（屏幕像素）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D HealthBarDrawSize = FVector2D(150.0f, 18.0f);

    // 血条相对胶囊顶部的抬高距离
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float HealthBarHeadOffset = 30.0f;

    // ---- Boss 屏幕血条（战斗时挂到玩家摄像机，屏幕顶部居中）----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    UWidgetComponent* BossBarWidget;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> BossBarWidgetClass;

    // Boss 血条固定绘制尺寸（屏幕像素，缩放/旋转不影响大小）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D BossBarDrawSize = FVector2D(600.0f, 40.0f);

    // Boss 血条上边缘距屏幕顶部的距离（像素）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float BossBarTopMargin = 40.0f;

    // Boss 血条水平偏移（像素，正值右移、负值左移，0 为水平居中）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float BossBarHorizontalOffset = 0.0f;

    // 上一帧视口尺寸（检测窗口拉伸/分辨率切换，触发屏幕空间 UI 强制刷新）
    int32 LastViewportSizeX = 0;
    int32 LastViewportSizeY = 0;

    // 视口尺寸变化检测：resize 后强制重建屏幕空间 UI 控件（修复控件消失无法自适应）
    void CheckViewportResize();

    // 强制重建屏幕空间 WidgetComponent 的 Slate 控件（保留隐藏状态与配置）
    void ForceRefreshScreenWidget(UWidgetComponent* WidgetComp);

    // ---- 弹刀提示（怪物头顶，由 C++ 控制：进入战斗 + 攻击蒙太奇进入弹刀时间窗口才显示）----
    // 命名刻意避开 "ParryPromptWidget"：若蓝图里手动添加了同名组件/变量会与原生属性撞名，
    // 导致蓝图骨架编译失败（Internal Compiler Error: property already exists）
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UI")
    UWidgetComponent* ParryPromptUI = nullptr;

    // 出伤预警提示控件（屏幕空间，挂在怪物头顶）
    UPROPERTY(VisibleAnywhere, Category = "UI")
    UWidgetComponent* DamageWarningUI = nullptr;

    // 弹刀提示控件类（默认自动加载 /Content/UI/WBP_ParryPrompt；未命中时可在怪物蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> ParryPromptWidgetClass;

    // 弹刀提示固定绘制尺寸（屏幕像素）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D ParryPromptDrawSize = FVector2D(120.0f, 60.0f);

    // 弹刀提示相对胶囊顶部的抬高距离（需比血条 HealthBarHeadOffset 更高，避免重叠）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float ParryPromptHeadOffset = 70.0f;

    // 弹刀提示入场动画名（控件蓝图 Timeline 里做的动画名；留空 = 播放控件里第一个动画）。
    // 每次进入弹刀窗口都会重新播放一次
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FString ParryPromptAnimationName;

    // ---- 出伤预警（提示即将造成伤害）----
    // 出伤预警提前量（秒）：攻击蒙太奇播放到出伤时间（判定窗口起始）前该秒数时，
    // 显示头顶系统预警提示并触发 OnDamageWarning；到达出伤时间自动收起。
    // 预警期间（提示出现 → 出伤开始）玩家在攻击范围内闪避 = 完美闪避
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat", meta = (ClampMin = "0.0"))
    float DamageWarningLeadTime = 0.2f;

    // 完美闪避仅限预警窗口：开启 = 只有"提示出现 → 出伤开始"期间闪避才算完美闪避；
    // 关闭 = 回到旧行为（整个前摇 + 判定窗口 + 优雅期内闪避都算完美闪避）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    bool bPerfectDodgeOnlyInWarningWindow = true;

    // 出伤预警提示控件类（默认自动加载 /Content/UI/WBP_DamageWarning；未命中时可在怪物蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> DamageWarningWidgetClass;

    // 出伤预警固定绘制尺寸（屏幕像素）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    FVector2D DamageWarningDrawSize = FVector2D(140.0f, 60.0f);

    // 出伤预警相对胶囊顶部的抬高距离（需比弹刀提示更高，避免重叠）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
    float DamageWarningHeadOffset = 110.0f;

    // ---- 伤害飘字（角色对怪物造成伤害时弹出，暴击/非暴击各用一套控件类）----
    // 普通伤害飘字控件类（默认自动加载 /Content/UI/WBP_DamageNumber_Normal；未命中时可在怪物蓝图里手动指定）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber")
    TSubclassOf<UUserWidget> DamageNumberNormalWidgetClass;

    // 暴击伤害飘字控件类（默认自动加载 /Content/UI/WBP_DamageNumber_Crit；未配置时回落普通飘字类）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber")
    TSubclassOf<UUserWidget> DamageNumberCritWidgetClass;

    // 单条飘字显示时长（秒，蓝图可调，默认 1 秒）：末尾 0.25 秒线性淡出后销毁
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber", meta = (ClampMin = "0.1"))
    float DamageNumberLifetime = 1.0f;

    // 飘字固定绘制尺寸（屏幕像素；弹出时设置一次，不每帧改——每帧改会重排抖动）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber")
    FVector2D DamageNumberDrawSize = FVector2D(180.0f, 90.0f);

    // 飘字相对胶囊顶部的抬高距离（需比出伤预警 DamageWarningHeadOffset 更高，避免重叠）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber")
    float DamageNumberHeadOffset = 130.0f;

    // 连续飘字水平随机散布半径（厘米）：多段攻击的每条飘字彼此错开、不完全叠在一起
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber", meta = (ClampMin = "0.0"))
    float DamageNumberJitter = 25.0f;

    // 飘字上飘速度（厘米/秒，0 = 原地显示到消失）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI|DamageNumber")
    float DamageNumberRiseSpeed = 60.0f;

    // 存活中的飘字条目（Tick 里推进寿命：上飘 + 末段淡出 + 到时销毁）
    struct FDamageNumberEntry
    {
        TWeakObjectPtr<UWidgetComponent> WidgetComp;
        TWeakObjectPtr<UUserWidget> WidgetInstance;
        float Age = 0.0f;
        float Lifetime = 1.0f;
        float RiseSpeed = 0.0f;
    };
    TArray<FDamageNumberEntry> ActiveDamageNumbers;

    // 飘字控件类未配置只告警一次的标志（避免每次受击刷屏）
    bool bDamageNumberClassWarned = false;

    // 角色侧预设的本次命中暴击标志（SetPendingDamageNumberCrit 写入，TakeDamage 消费后立即复位）
    bool bPendingDamageNumberCrit = false;

    // 伤害飘字寿命推进：上飘 + 末段淡出 + 到时销毁（Tick 每帧调用）
    void UpdateDamageNumbers(float DeltaTime);

    // 在飘字控件里找数字 TextBlock：约定名 txt_damage_num，对不上时前缀兜底
    // （UMG 复制控件会自动加序号后缀，精确名会 miss）
    UTextBlock* FindDamageNumberTextBlock(UUserWidget* Widget) const;

    // ---- 内部状态（非蓝图可配）----
    EMonsterAIState AIState = EMonsterAIState::Idle;

    FVector SpawnLocation = FVector::ZeroVector;

    // 出生朝向（BeginPlay 记录，ResetToHome 回原点时恢复，修复 Boss 回原点后朝向没重置）
    FRotator SpawnRotation = FRotator::ZeroRotator;

    FVector PatrolTarget = FVector::ZeroVector;

    // 巡逻停留计时
    float PatrolWaitTimer = 0.0f;

    // 脱战计时（角色持续超出 LeashRadius 的时间）
    float OutOfCombatTimer = 0.0f;

    // 攻击冷却剩余
    float CurrentAttackCd = 0.0f;

    // 是否处于战斗（控制血条显隐）
    bool bInCombat = false;

    // 玩家摄像机缓存（Boss 血条挂载用）
    UCameraComponent* CachedPlayerCamera = nullptr;

    // ---- 动画内部状态 ----
    // 上一次播放的攻击蒙太奇索引（随机选段时避免重复）
    int32 LastAttackMontageIndex = INDEX_NONE;

    // 移动蒙太奇当前是否在播放
    bool bMoveMontageActive = false;

    // 移动蒙太奇原生速度缓存（首次使用时解析，避免每帧重算）
    float CachedMoveMontageNativeSpeed = -1.0f;

    // ---- Boss 脱战行为内部状态 ----
    // 待机蒙太奇当前是否在播放（脱战循环）
    bool bIdleMontageActive = false;
    // 寻找蒙太奇当前是否在播放（寻找阶段）
    bool bSearchMontageActive = false;
    // 脱战延迟计时（角色超出追击范围后累计；>= OutOfCombatSearchDelay 进入寻找）
    float OutOfCombatSearchTimer = 0.0f;
    // 寻找阶段剩余时长（>0 表示正在原地播放寻找动画）
    float SearchRemaining = 0.0f;

    // 受击判定窗口循环检测定时器（0.02s，按蒙太奇时间轴推进）
    FTimerHandle HitWindowTimerHandle;

    // 死亡姿势冻结定时器（蒙太奇播完前暂停动画，防止混合回 Idle 站立）
    FTimerHandle DeathFreezeTimerHandle;

    // 是否处于攻击前摇窗口（攻击发起 → 伤害结算前），完美闪避判定用
    bool bIsTelegraphing = false;

    // 本次攻击是否已实际命中过玩家（伤害未被无敌格挡，ApplyPointDamage 返回值 > 0）。
    // 命中后该次攻击不再允许触发完美闪避：优雅期/接触窗口内后续闪避只算普通闪避，
    // 避免"伤害已扣血却又弹出完美闪避 UI"的时序问题。下次 PerformAttack 时重置
    bool bAttackHitPlayer = false;

    // ---- 当前段攻击运行时状态（PerformAttack 时刷新）----
    // 当前段是否可弹刀
    bool bCurrentAttackParryable = false;

    // 当前段是否为"大幅度攻击"（命中角色时触发击飞蒙太奇；攻击窗口结束/被打断时复位）
    bool bCurrentAttackIsHeavy = false;

    // 弹刀提示是否正在显示（前摇窗口开始时置位、结束时清除，驱动 OnParryWindowEnd）
    bool bParryPromptActive = false;

    // 出伤预警窗口激活中（出伤时间前 DamageWarningLeadTime 秒 → 出伤开始）。
    // 此期间玩家闪避 = 完美闪避（攻击范围内），到达出伤时间自动关闭
    bool bDamageWarningActive = false;

    // 当前段生效的判定半径/扇形角度（逐段配置回落到全局值后的结果）
    float CurrentHitRadius = 0.0f;
    float CurrentHitArcAngle = 360.0f;

    // ---- 当前段受击判定窗口运行时状态（PlayRandomAttackMontage 时刷新）----
    // 当前段是否使用模型接触判定
    bool bCurrentUseContactHit = false;
    // 当前段生效的接触半径/窗口（逐段配置回落到全局值后的结果）
    float CurrentContactHitRadius = 120.0f;
    float CurrentContactHitWindow = 0.25f;

    // 当前正在播放的攻击蒙太奇（判定窗口按其时间轴推进；无蒙太奇时用真实时间）
    UAnimMontage* CurrentAttackMontage = nullptr;
    // 当前段攻击播放速率（判定窗口剩余时长换算真实秒用）
    float CurrentAttackPlayRate = 1.0f;
    // 攻击发起的真实时间戳（无蒙太奇时的判定窗口计时基准）
    float AttackStartTime = 0.0f;

    // 当前段判定窗口起止（蒙太奇时间轴秒数，逐段配置回落到全局后）
    float CurrentHitWindowStart = 0.0f;
    float CurrentHitWindowEnd = 0.0f;
    // 判定窗口检测是否进行中（攻击发起 → 窗口结束/被打断）
    bool bHitWindowRunning = false;
    // 蒙太奇播放位置当前处于判定区间内（判定生效中）
    bool bHitWindowOpen = false;
    // 范围判定模式：本次攻击已结算一次伤害（保持单次命中语义）
    bool bRangeHitDone = false;

    // ---- 当前段弹刀判定窗口运行时状态（PlayRandomAttackMontage 时刷新）----
    // 当前段弹刀窗口起止（蒙太奇时间轴秒数，逐段配置回落到全局后）
    float CurrentParryWindowStart = 0.0f;
    float CurrentParryWindowEnd = 0.0f;
    // 蒙太奇播放位置当前处于弹刀窗口内（弹刀可触发）
    bool bParryWindowOpen = false;

    // 上次接触伤害时间戳（ContactHitDamageCooldown 用）
    float LastContactDamageTime = 0.0f;
    // 玩家上一帧是否在接触半径内（检测"离开再进入"以立即结算伤害）
    bool bPlayerWasInContact = false;

    // 前摇窗口结束时间戳（完美闪避优雅期 GracePeriod 用）
    float TelegraphEndTime = 0.0f;

    // ---- 停滞（弹刀）----
    // 停滞剩余时间（秒）：>0 时冻结 AI 移动与攻击（Tick 递减，期间清零速度、跳过 UpdateAI）
    float StaggerRemaining = 0.0f;

    // ---- 完美闪避停滞（时停）运行时状态 ----
    // 是否处于完美闪避停滞中（CustomTimeDilation=0，世界时间定时器负责恢复）
    bool bPerfectDodgeFrozen = false;

    // 停滞前的时间流速（恢复时写回，防止覆盖其他系统对 dilation 的修改）
    float PreFreezeTimeDilation = 1.0f;

    // 停滞恢复定时器（世界时间驱动）
    FTimerHandle PerfectDodgeFreezeTimerHandle;

    // ---- 大招停滞（时停）运行时状态 ----
    // 是否处于大招停滞中（CustomTimeDilation=0，角色 EndUltimate 主动恢复，无定时器）
    bool bUltimateFrozen = false;

    // 大招停滞前的时间流速（恢复时写回，防止覆盖其他系统对 dilation 的修改）
    float PreUltimateTimeDilation = 1.0f;

    // 受击反应冷却剩余（Tick 递减，>0 时跳过受击反应播放）
    float HitReactionCooldownRemaining = 0.0f;

    // 弹刀成功后的硬直时间（秒）：硬直内不发起攻击（通过攻击冷却实现）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monster|Combat")
    float ParryStaggerDuration = 2.0f;

    // ---- Boss 狂暴运行时状态 ----
    // 是否已触发过狂暴（只触发一次：首次血量跌破阈值）
    bool bEnrageTriggered = false;

    // 回血阶段进行中（无敌 + 逐步回满血 + 不移动 + 循环播回血蒙太奇）
    bool bIsEnrageHealing = false;

    // 回血阶段剩余时间（Tick 递减）
    float EnrageHealTimeRemaining = 0.0f;

    // 回血起始血量（线性恢复到满血的插值起点）
    float EnrageHealStartHealth = 0.0f;

    // 狂暴技能施放中（多段蒙太奇按顺序播放，霸体不可打断）
    bool bIsEnrageSkillCasting = false;

    // 狂暴已完成（增伤 + 红光生效）
    bool bEnraged = false;

    // 当前狂暴技能段索引 / 蒙太奇
    int32 CurrentEnrageSegmentIndex = -1;
    UAnimMontage* CurrentEnrageMontage = nullptr;

    // 狂暴技能段兜底：蒙太奇停播未推进的累计时间（>0.3s 强制推进下一段，防回调丢失卡死）
    float EnrageMontageSilentTime = 0.0f;

    // 上次对玩家结算狂暴技能伤害的时间（HitInterval 节流）
    float EnrageLastHitTime = 0.0f;

    // Mesh 基准相对变换（BeginPlay 记录，清除根运动残留旋转时恢复）
    FVector BaseMeshRelativeLocation = FVector::ZeroVector;
    FRotator BaseMeshRelativeRotation = FRotator::ZeroRotator;

    // 根运动残留旋转补清除定时器（蒙太奇混合输出期间再补一次）
    FTimerHandle EnrageRootRotationResetTimer;

    // 【旋转守卫剩余时间】狂暴技能结束后 >0 期间每 Tick 压平 Pitch/Roll（见 EndEnrageSkill/Tick），
    // 覆盖蒙太奇 BlendOut 混合输出期持续施加的残余根运动旋转
    float EnrageRotationGuardRemaining = 0.0f;

    // 段根运动补丁是否已执行（每局一次）
    bool bEnrageRootMotionPatched = false;

    // ---- Boss 狂暴内部函数 ----
    // 血量跌破阈值检测（TakeDamage 扣血后调用）：满足条件则触发回血阶段
    void TryTriggerEnrage();
    // 触发回血阶段：打断自身动作、停移动、播回血蒙太奇、进入无敌
    void TriggerEnrageHeal();
    // 结束回血阶段：停回血蒙太奇、立刻朝玩家释放狂暴技能
    void EndEnrageHeal();
    // 释放狂暴技能：朝向玩家并播放第一个有效段
    void StartEnrageSkill();
    // 播放指定狂暴技能段（Play + 结束回调驱动推进）
    void PlayEnrageSegment(int32 Index);
    // 狂暴技能段蒙太奇结束回调：正常/被打断（兜底）都推进下一段，最后一段结束 → EndEnrageSkill
    UFUNCTION()
    void OnEnrageSegmentMontageEnded(UAnimMontage* Montage, bool bInterrupted);
    // 结束狂暴技能：停蒙太奇、清状态、进入狂暴（增伤 + 红光 + 清残留旋转）
    void EndEnrageSkill();
    // Tick 驱动：回血计时/回血量/蒙太奇循环 + 狂暴技能段推进兜底与攻击窗口伤害结算
    void UpdateEnrage(float DeltaTime);
    // 玩家是否在狂暴技能攻击范围内（以怪物为球心的上半球体）
    bool IsPlayerInEnrageHemisphere() const;
    // 对狂暴技能攻击范围内的玩家结算该段伤害（HitInterval 节流）
    void ApplyEnrageSkillDamage(const FBossEnrageSegment& Segment);
    // 声波攻击：周期波推进/波前扩散/命中结算/完美闪避窗口维护（声波模式每帧调用）
    void UpdateEnrageSoundWave(float DeltaTime);
    // 运行时为狂暴技能段动画序列启用根运动（修复段间位置回跳，与角色段根运动补丁同方案）
    void EnsureEnrageRootMotion();
    // 清除根运动残留旋转（修复狂暴技能结束后模型歪斜）
    void ResetEnrageMontageRotation();
    // 狂暴红光开关（亮起/熄灭点光源 + 尝试设置材质参数）
    void SetEnrageGlow(bool bEnabled);

    // 模型当前是否因摄像机贴近而隐藏（Hide 模式用，避免每帧重复 SetHiddenInGame）
    bool bMeshHiddenForCamera = false;

    // Fade 模式当前的不透明度（变化检测，避免每帧刷新材质参数）
    float LastFadeOpacity = 1.0f;

    // ---- 弹刀提示 UI（C++ 接管显示/隐藏与入场动画）----
    // 显示弹刀提示：仅在战斗中生效（bInCombat 门控），并播放控件蓝图里的入场动画
    void ShowParryPrompt();
    // 隐藏弹刀提示（脱战/离开弹刀窗口/攻击结束/被打断/死亡时调用）
    void HideParryPrompt();
    // 在控件蓝图的动画列表中查找入场动画（ParryPromptAnimationName 为空 = 第一个动画）
    UWidgetAnimation* FindParryPromptAnimation(UUserWidget* Widget) const;

    // ---- 出伤预警 UI（C++ 接管显示/隐藏）----
    // 更新出伤预警状态：TimelinePos 为攻击时间轴当前位置，DamageStartTime 为该次攻击的出伤时间
    void UpdateDamageWarning(float TimelinePos, float DamageStartTime);
    // 显示头顶出伤预警提示（战斗中才显示）
    void ShowDamageWarning();
    // 隐藏出伤预警提示
    void HideDamageWarning();

    // ---- 内部函数 ----
    // 实际攻击触发距离：攻击 vs 追击的切换半径（玩家距离 <= 该值 → 原地攻击，否则追击）。
    // 显式配置 AttackTriggerDistance（>=0）时以其为准（远程怪可设大值原地输出）；
    // 默认回落近身触发基线 AttackRange——注意：不再取 max(AttackRange, AttackHitRadius)，
    // 攻击判定半径（AttackHitRadius / 逐段 HitRadius）只决定"能否命中"，不参与"是否追击"，
    // 否则大范围 AOE 判定半径会让 Boss 永远原地攻击、从不追击玩家。
    float GetAttackTriggerDistance() const
    {
        if (AttackTriggerDistance >= 0.0f)
        {
            return AttackTriggerDistance;
        }
        return FMath::Max(AttackRange, 1.0f);
    }

    // Boss 追击保持半径（脱战边界）：取 AggroRadius 与 DetectRadius 的较大值。
    // 语义：AggroRadius = 进战触发半径；DetectRadius = 追击/保持仇恨半径。
    // 角色在两者较大值内 Boss 持续追击，超出才脱战（防止边界抖动）
    float GetBossChaseRadius() const { return FMath::Max(AggroRadius, DetectRadius); }

    void UpdateAI(float DeltaTime);
    void UpdateMinionAI(float DeltaTime);
    void UpdateBossAI(float DeltaTime);

    // 朝目标点移动（直接驱动 CharacterMovement，无需 NavMesh），到达返回 true
    bool MoveTowards(const FVector& Target, float Speed, float DeltaTime);

    // 清零水平速度（保留垂直速度），攻击时停住追踪遗留的滑行
    void StopHorizontalMovement();

    void PickNewPatrolTarget();
    void EnterCombat();
    void LeaveCombat(bool bResetHealth);
    void ReturnHome();
    void ResetHealth();
    void PerformAttack();
    void FaceTarget(const FVector& Target);

    void UpdateHealthBar();
    // Boss 血条定位到屏幕顶部居中（解析计算，防抖动）
    void UpdateBossBarPosition();
    void AttachBossBarToCamera();
    void DetachBossBar();

    // ---- 动画蒙太奇 ----
    // 随机选择一段攻击蒙太奇（不与上一次重复），播放并返回索引；无可用蒙太奇返回 INDEX_NONE
    int32 PlayRandomAttackMontage();
    // 结束攻击前摇窗口（伤害结算/落空/被弹刀打断/死亡时调用）：清除标志并触发 OnParryWindowEnd
    void EndTelegraphWindow();
    // 大范围区域判定：玩家是否落在攻击判定半径 + 扇形角度 + 高度容差内
    bool IsPlayerInAttackHitArea() const;
    // 模型接触判定：玩家是否落在当前段接触半径 + 扇形角度 + 高度容差内（贴身判定）
    bool IsPlayerInContactHitArea() const;
    // 通用命中区域判定（指定半径）：半径 + 扇形 + 高度容差。供上面两个接口复用
    bool IsPlayerInHitAreaWithRadius(float Radius) const;
    // 调试绘制攻击判定范围（判定圆 + 扇形两条边界线）
    void DrawAttackHitDebug() const;

    // 调试绘制狂暴技能作用范围（EnrageSkillRadius 判定圆 + 朝向线，狂暴技能释放期间调用）
    void DrawEnrageRangeDebug() const;

    // ---- 受击判定窗口（蒙太奇时间轴驱动）----
    // 攻击发起后启动循环检测（PerformAttack 调用）
    void StartHitWindow();
    // 循环检测（0.02s）：蒙太奇播放位置进入 [Start, End] 区间才执行受击判定。
    // 接触判定段：接触 + 周期多段结算；范围判定段：窗口内首次进入攻击范围结算一次
    void UpdateHitWindow();
    // 结束判定窗口（窗口到期/蒙太奇播完或被停/被打断/死亡时调用）
    void EndHitWindow(bool bHit);
    // 当前攻击判定窗口的剩余真实秒数（完美闪避无敌时长计算用；无进行中攻击返回 0）
    float GetRemainingHitWindowSeconds() const;
    // 实际对玩家造成攻击伤害（范围/接触两路共用，提取自原 ApplyAttackDamage）
    void DealAttackDamageToPlayer();

    // 通用停滞处理（弹刀用）：清攻击与接触定时器、结束前摇、停蒙太奇、
    // 停移动、设置 StaggerRemaining 与 CurrentAttackCd。不触发事件（由调用方按场景触发）
    void ApplyStagger(float Duration);
    // 受击反应：随机播放一段 HitReactionMontages。按 bHitReactionInterruptsAttack 决定是否打断攻击。
    // 停滞/死亡/冷却中跳过。非打断模式（霸体）下攻击进行中也不播放
    void PlayHitReaction();
    // 动画衔接空挡兜底：当怪物处于"非特殊状态、无任何蒙太奇在播、且未在移动"的空挡期时，
    // 主动刷新待机/移动蒙太奇，消除上一个动作播完到下一个动作接上之间的"站直僵住"帧。
    void UpdateAnimFallback();
    // 移动蒙太奇播放/停止管理
    void UpdateMoveMontage(bool bIsMoving);
    // 解析移动蒙太奇的原生前进速度（cm/s）：显式配置优先，否则从蒙太奇内
    // 动画序列的根运动位移自动推导（推导失败回落 300）
    float GetMoveMontageNativeSpeed();

    // ---- Boss 脱战行为（待机/寻找/回位）----
    // Tick 中驱动脱战行为状态机（仅 Boss + bEnableBossIdleBehavior 生效）：
    // 脱战→循环待机蒙太奇；脱战延迟满→寻找蒙太奇；寻找播完→回原点重置。
    void UpdateBossIdleBehavior(float DeltaTime);
    // 播放/停止待机蒙太奇（脱战循环，进入战斗/狂暴/死亡时停止）
    void UpdateIdleMontage(bool bShouldPlay);
    // 启动寻找阶段（脱战延迟满触发）：停待机、原地播寻找蒙太奇、计时
    void StartSearching();
    // 结束寻找阶段：停寻找蒙太奇、转回位状态（回原点）
    void FinishSearching();
    // 回原点完成：重置血量、状态、狂暴标记回第一阶段
    void ResetToHome();

    void Die();

    // ---- 掉落物结算 ----
    // 把 DropItems（或 SharedDropTable）按概率结算给玩家背包，并推送「获得物品」提示。
    // Die() 里调用一次；bDropsGranted 防止重复结算
    void GrantDropsToPlayer();
    // 取本次生效的掉落清单（SharedDropTable 优先于 DropItems）
    const TArray<FMonsterDropEntry>& GetActiveDropList() const;

    // 供编辑器下拉框使用：列出 DropTablePath 这张表的全部行名。
    // 被 FMonsterDropEntry::ItemRowName 的 meta = (GetOptions = "GetDropRowOptions") 引用 ——
    // 这个函数改名的话，那边也要一起改，否则下拉框会退化成普通文本框（不报错，只是不好用）。
    UFUNCTION()
    TArray<FName> GetDropRowOptions() const;

    // 冻结在死亡动画最后一帧：蒙太奇结束时动画蓝图会混合回 Idle（怪物“站起来”），
    // 在内容结束前暂停骨骼动画更新，尸体保持倒地姿势直到销毁
    void FreezeDeathPose();

    // 摄像机贴近时的遮挡处理：Fade=按距离渐变半透明（材质参数 FadeOpacity），
    // Hide=距离内直接隐藏模型（连同挂在网格体上的子组件如武器）
    void UpdateCameraOcclusion();

    APawn* GetPlayerPawn() const;
    float GetPlayerDistance() const;
};
