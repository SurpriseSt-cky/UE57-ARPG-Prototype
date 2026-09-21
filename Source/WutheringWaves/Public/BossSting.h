#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BossSting.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;
class AMonsterBase;
class ABattleCharacter;

/**
 * Boss 二阶段远程毒刺投射物：
 * - 球体碰撞 + 直线弹道（无重力、不反弹）
 * - 模型 / 伤害 / 发射速度 蓝图可调
 * - 命中判定：毒刺接触玩家后减速「嵌入」，逐帧判定球心是否侵入玩家胶囊足够深
 *   （达到 PenetrationDepthForDamage）才结算伤害并销毁；一碰即结算 → 改为「完全进入才结算」
 * - 仇恨范围内（射程内未消失）角色被毒刺击中（含角色主动接触）均造成伤害
 * - 未命中玩家但飞行距离超出仇恨范围：自动销毁
 * - 完美闪避判定：毒刺「接触玩家 → 完全进入结算」期间，玩家闪避 = 完美闪避
 *   （闪避进入无敌帧 → 毒刺视为被躲开，销毁且不结算伤害）
 * - 命中玩家不触发受击动画、不触发击飞动画（走静默扣血路径）
 *
 * 发射由 MonsterBase 调用 SpawnBossSting 完成；命中/未命中结果通过委托回调给 Boss，
 * 由 Boss 决定后续「瞬移背刺 + 连击」。
 */
DECLARE_DELEGATE_OneParam(FOnBossStingResolved, bool /*bHitPlayer*/);

UCLASS()
class WUTHERINGWAVES_API ABossSting : public AActor
{
    GENERATED_BODY()

public:
    ABossSting();

    // 初始化毒刺：发射方向、伤害、速度、来源 Boss、控制器、射程（超出自动销毁）、完美闪避距离阈值。
    // 命中/未命中结果通过 OnResolved 回调通知发射方（bHitPlayer 表示是否命中玩家）
    UFUNCTION(BlueprintCallable, Category = "BossSting")
    void InitSting(const FVector& Direction, float DamageAmount, float InStingSpeed, class AMonsterBase* InOwnerBoss,
        AController* InstigatorController, float InMaxFlightDistance, float InPerfectDodgeDistanceThreshold);

    // 结果回调（Boss 绑定，用于触发瞬移背刺/连击）
    FOnBossStingResolved OnResolved;

    // 应用「攻击判定半径」到碰撞球（构造 / BeginPlay / InitSting / Boss 透传后调用）。
    // 半径越大 → 毒刺越容易判定命中玩家（攻击判定范围更宽松）
    void ApplyHitDetectionRadius();

    // 是否已结算（命中玩家 / 超射程销毁 / 超时销毁后为 true，防止重复回调）
    bool IsResolved() const { return bResolved; }

    // 毒刺是否仍处于「可完美闪避」状态（蹭闪判定）：
    // 1) 远距离：毒刺距玩家贴身边缘 > PerfectDodgeDistanceThreshold → 闪避可完美闪避
    // 2) 近身蹭闪：毒刺距玩家贴身边缘 <= CloseDodgeDistance（未接触）→ 闪避可完美闪避
    // 3) 已接触玩家但未完全进入（嵌入中）→ 闪避可完美闪避（躲开毒刺）
    // 中间区 (CloseDodgeDistance, PerfectDodgeDistanceThreshold] 不可完美闪避
    bool CanPerfectDodge() const;

    // 当前飞行距离（从发射点算起，cm）
    float GetFlightDistance() const { return FlightDistance; }

    // 毒刺是否已接触玩家（嵌入判定中，未完全进入）
    bool IsContactingPlayer() const { return bContactingPlayer; }

    // 近身蹭闪范围（cm）：毒刺距玩家「贴身边缘」<= 该值（尚未接触/嵌入）时，玩家主动
    // 接近毒刺后闪避 = 完美闪避（险中求胜）。与 PerfectDodgeDistanceThreshold 之间形成
    // 「中间危险区」：距离落在 (CloseDodgeDistance, PerfectDodgeDistanceThreshold] 区间内
    // 不可完美闪避，逼玩家要么远距离预判、要么贴身极限操作，杜绝「随便蹭一下」。
    // 距离基准 = 毒刺球心到玩家胶囊表面的真实间隙（已减胶囊半径），非球心到球心。
    // 由 Boss（MonsterBase::EmitBossStingProjectile）发射时从 Boss 蓝图配置透传。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    float CloseDodgeDistance = 120.0f;

    // 攻击判定半径（cm）：毒刺「命中判定球」半径。
    // 毒刺球心与玩家胶囊表面的间隙 <= 该值时，判定为「接触玩家」→ 进入嵌入流程并最终结算伤害。
    // 调大 = 毒刺的攻击判定范围更宽松（更容易命中）；调小 = 更严格（更难命中）。
    // 同时作用于：① 碰撞球半径（Overlap 接触检测）② Tick 玩家接近兜底判定。
    // 由 Boss（MonsterBase::EmitBossStingProjectile）发射时从 Boss 蓝图配置透传，本蓝图亦可单独覆盖。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting", meta = (ClampMin = "1.0"))
    float HitDetectionRadius = 100.0f;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

    UFUNCTION()
    void OnStingBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
        UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

    // 启动嵌入流程：接触玩家后进入嵌入状态（记录玩家、减速、关闭碰撞），
    // 之后由 UpdateEmbed 逐帧判定侵入深度结算伤害
    void BeginEmbed(ABattleCharacter* PlayerChar);

    // 嵌入判定：毒刺接触玩家后每帧调用，判定球心是否侵入玩家胶囊足够深
    // 完全进入 → 结算伤害并销毁；期间玩家无敌（闪避）→ 视为完美闪避躲开
    void UpdateEmbed(float DeltaTime);

    // 结算（命中玩家 bHitPlayer=true；超射程/未命中 bHitPlayer=false），销毁并回调
    void Resolve(bool bHitPlayer);

    // ---- 组件 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    USphereComponent* CollisionSphere;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UStaticMeshComponent* StingMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UProjectileMovementComponent* ProjectileMovement;

    // ---- 数值（蓝图可调）----
    // 毒刺伤害（发射时由 Boss 覆盖；<=0 回落到 Boss 的毒刺伤害配置）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    float Damage = 40.0f;

    // 毒刺发射速度（cm/s，蓝图可调；发射时由 Boss 覆盖）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    float StingSpeed = 3000.0f;

    // 毒刺模型（默认引擎圆锥便于测试；正式毒刺模型在派生蓝图里替换）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    UStaticMesh* StingMeshAsset = nullptr;

    // 模型缩放
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    FVector StingMeshScale = FVector(1.0f);

    // 完美闪避距离阈值（cm）：毒刺未接触玩家时，距玩家「贴身边缘」> 该值则闪避 = 完美闪避
    // （远距离蹭闪：毒刺还远、有充裕反应时间，此时闪避视为完美闪避）
    float PerfectDodgeDistanceThreshold = 800.0f;

    // 最大有效范围（cm）：超出自动销毁（未命中）。<=0 表示不限距离，永不因射程自毁（由 InitSting 从 Boss 配置传入）
    float MaxFlightDistance = 0.0f;

    // 嵌入深度阈值（cm）：毒刺接触玩家后，球心侵入玩家胶囊的深度达到该值才结算伤害。
    // 从接触 → 达到此深度 之间的时间窗内玩家闪避 = 完美闪避
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    float PenetrationDepthForDamage = 30.0f;

    // 嵌入推进速度（cm/s）：接触玩家后毒刺减速到该速度继续刺入，控制「接触→完全进入」的反应窗时长
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    float EmbedSpeed = 600.0f;

    // 嵌入超时（秒）：接触玩家后若超过该时长仍未完全进入，视为未命中销毁（防止毒刺永久卡住）
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "BossSting")
    float EmbedTimeout = 0.6f;

    // ---- 运行期状态 ----
    // 发射点（飞行距离计算基准）
    FVector SpawnLocation = FVector::ZeroVector;

    // 已飞行距离（cm，Tick 累加）
    float FlightDistance = 0.0f;

    // 嵌入已持续时长（秒，接触玩家后累加，用于超时兜底）
    float EmbedElapsed = 0.0f;

    // 是否已结算
    bool bResolved = false;

    // 是否已接触玩家（嵌入判定中，未完全进入结算）
    bool bContactingPlayer = false;

    // 嵌入方向（接触瞬间的飞行方向单位向量，接触后沿此方向继续刺入）
    FVector EmbedDirection = FVector::ZeroVector;

    // 当前飞行方向（单位向量，飞行中每帧刷新，InitSting 时初始化为发射方向）。
    // 用于：进入嵌入状态时作为刺入方向（此时 Velocity 已因停止移动清零）；
    // 以及 Tick 里的玩家接近检测兜底。
    FVector FlightDirection = FVector::ZeroVector;

    // 被接触的玩家（嵌入判定目标）
    TWeakObjectPtr<class ABattleCharacter> ContactedPlayer;

    // 伤害来源 Boss
    TWeakObjectPtr<AMonsterBase> OwnerBoss;

    // 伤害归属控制器
    TWeakObjectPtr<AController> InstigatorCtrl;
};
