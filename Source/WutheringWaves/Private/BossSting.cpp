#include "BossSting.h"
#include "BattleCharacter.h"
#include "MonsterBase.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"

ABossSting::ABossSting()
{
    PrimaryActorTick.bCanEverTick = true;

    // ---- 碰撞球体（根组件）----
    // 关键设计：毒刺是远程弹道，只对「玩家」敏感，且**不能被任何物体物理阻挡**。
    // 因此对 Pawn（玩家/小怪）用 Overlap（重叠检测）而非 Block：Overlap 不产生物理阻挡，
    // 毒刺可穿过一切物体（玩家/小怪/地形），同时通过 OnComponentBeginOverlap 检测玩家进入。
    // 对其它通道一律 Ignore（穿透）——毒刺飞行途中不会被地面/墙壁/柱子/其它怪物挡住。
    // 半径用蓝图可调的 HitDetectionRadius（默认 100cm，较原 50cm 扩大一倍），
    // 同时缓解高速（3000cm/s）下的隧道效应；蓝图改值后由 ApplyHitDetectionRadius 重新应用。
    CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
    SetRootComponent(CollisionSphere);
    CollisionSphere->InitSphereRadius(HitDetectionRadius);
    CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    CollisionSphere->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
    CollisionSphere->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
    CollisionSphere->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Overlap);
    CollisionSphere->SetGenerateOverlapEvents(true);
    CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &ABossSting::OnStingBeginOverlap);

    // ---- 可视网格：默认引擎圆锥便于测试（正式毒刺模型在派生蓝图里替换）----
    StingMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StingMesh"));
    StingMesh->SetupAttachment(CollisionSphere);
    StingMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMeshAsset(TEXT("/Engine/BasicShapes/Cone.Cone"));
    if (ConeMeshAsset.Succeeded())
    {
        StingMesh->SetStaticMesh(ConeMeshAsset.Object);
        StingMesh->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f)); // 圆锥尖朝飞行方向（X 正轴）
    }

    // ---- 飞行组件：直线飞行（无重力、不反弹）----
    ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
    ProjectileMovement->SetUpdatedComponent(CollisionSphere);
    ProjectileMovement->bRotationFollowsVelocity = true;
    ProjectileMovement->bShouldBounce = false;
    ProjectileMovement->ProjectileGravityScale = 0.0f;
    ProjectileMovement->InitialSpeed = 3000.0f;
    ProjectileMovement->MaxSpeed = 3000.0f;
}

void ABossSting::BeginPlay()
{
    Super::BeginPlay();

    SpawnLocation = GetActorLocation();

    // 应用蓝图配置的模型（若派生蓝图指定了 StingMeshAsset）
    if (StingMeshAsset)
    {
        StingMesh->SetStaticMesh(StingMeshAsset);
    }
    StingMesh->SetRelativeScale3D(StingMeshScale);

    // 应用攻击判定半径（蓝图改 HitDetectionRadius 后在此生效）
    ApplyHitDetectionRadius();
}

// 应用「攻击判定半径」到碰撞球：半径越大，毒刺的攻击判定范围越宽松（越容易命中玩家）
void ABossSting::ApplyHitDetectionRadius()
{
    if (CollisionSphere)
    {
        CollisionSphere->SetSphereRadius(FMath::Max(1.0f, HitDetectionRadius), true);
    }
}

void ABossSting::InitSting(const FVector& Direction, float DamageAmount, float InStingSpeed, AMonsterBase* InOwnerBoss,
    AController* InInstigatorController, float InMaxFlightDistance, float InPerfectDodgeDistanceThreshold)
{
    Damage = DamageAmount;
    StingSpeed = FMath::Max(1.0f, InStingSpeed);
    OwnerBoss = InOwnerBoss;
    InstigatorCtrl = InInstigatorController;
    // 最大有效范围：<=0 表示不限距离（永不因射程自毁），否则直接采用配置值
    MaxFlightDistance = InMaxFlightDistance;
    PerfectDodgeDistanceThreshold = FMath::Max(0.0f, InPerfectDodgeDistanceThreshold);

    // 发射点兜底：确保飞行距离计算的基准点已初始化（正常情况下 BeginPlay 已赋值，
    // 若生成/初始化的调用时序异常导致 SpawnLocation 仍为零向量，则飞行距离会算成
    // 到世界原点(0,0,0)的巨大值而瞬间「超射程」自毁——此处补齐防御）
    if (SpawnLocation == FVector::ZeroVector)
    {
        SpawnLocation = GetActorLocation();
    }

    // 不与发射 Boss 碰撞（避免刚发射就打到自己）
    if (InOwnerBoss)
    {
        CollisionSphere->IgnoreActorWhenMoving(InOwnerBoss, true);
    }

    // 设定飞行方向与速度（蓝图可调的 StingSpeed 覆盖飞行组件速度）
    const float Speed = FMath::Max(1.0f, StingSpeed);
    ProjectileMovement->InitialSpeed = Speed;
    ProjectileMovement->MaxSpeed = Speed;
    ProjectileMovement->Velocity = Direction.GetSafeNormal() * Speed;

    // 记录当前飞行方向（用于嵌入刺入方向 + Tick 玩家接近检测兜底）
    FlightDirection = Direction.GetSafeNormal();

    // 应用攻击判定半径（Boss 透传 HitDetectionRadius 后在此生效）
    ApplyHitDetectionRadius();
}

bool ABossSting::CanPerfectDodge() const
{
    if (bResolved)
        return false;

    APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!Player)
        return false;

    // 已接触玩家、处于「嵌入中」（未完全进入结算）→ 玩家此时闪避 = 完美闪避（躲开毒刺）
    if (bContactingPlayer)
        return true;

    // ---- 未接触玩家：计算「毒刺球心到玩家贴身边缘」的真实间隙 ----
    // 距离基准按玩家胶囊表面算，而非球心到球心：减去玩家胶囊半径，得到毒刺表面到
    // 玩家身体的实际空隙，更贴合「是否来得及躲」的观感
    float GapToPlayer = FVector::Dist(GetActorLocation(), Player->GetActorLocation());
    if (const ABattleCharacter* PlayerChar = Cast<ABattleCharacter>(Player))
    {
        if (const UCapsuleComponent* Capsule = PlayerChar->GetCapsuleComponent())
        {
            GapToPlayer -= Capsule->GetUnscaledCapsuleRadius();
        }
    }
    GapToPlayer = FMath::Max(0.0f, GapToPlayer);

    // 1) 远距离蹭闪：毒刺还远（间隙 > 阈值）→ 有充裕反应时间，闪避 = 完美闪避
    if (GapToPlayer > PerfectDodgeDistanceThreshold)
        return true;

    // 2) 近身蹭闪：毒刺已贴近（间隙 <= CloseDodgeDistance）但尚未接触 → 主动接近后闪避 = 完美闪避
    //    （险中求胜：中间区 (CloseDodgeDistance, PerfectDodgeDistanceThreshold] 不可完美闪避）
    if (GapToPlayer <= CloseDodgeDistance)
        return true;

    // 中间危险区：距离不近不远，既不够远预判、也还没到贴身极限，此时闪避不算完美闪避
    return false;
}

void ABossSting::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (bResolved)
        return;

    // 已接触玩家（嵌入判定中）：减速刺入 + 深度判定，不再按飞行距离销毁
    if (bContactingPlayer)
    {
        UpdateEmbed(DeltaTime);
        return;
    }

    // 飞行中刷新当前飞行方向（速度非零时）
    if (!ProjectileMovement->Velocity.IsNearlyZero())
    {
        FlightDirection = ProjectileMovement->Velocity.GetSafeNormal();
    }

    // ---- 玩家接近检测兜底 ----
    // 毒刺对 Pawn 用 Overlap 检测玩家（OnStingBeginOverlap），但若因高速隧道效应或
    // Overlap 未触发而漏检，这里用距离检测兜底：飞行中若毒刺球心已足够接近玩家胶囊
    // （侵入到「接触」范围），主动进入嵌入流程，保证不因漏检而穿过玩家不结算。
    if (ABattleCharacter* PlayerChar = Cast<ABattleCharacter>(UGameplayStatics::GetPlayerPawn(this, 0)))
    {
        if (!PlayerChar->IsInvincibleNow())
        {
            const float DistToPlayer = FVector::Dist(GetActorLocation(), PlayerChar->GetActorLocation());
            // 接触阈值 = 毒刺攻击判定半径（HitDetectionRadius）+ 玩家胶囊半径
            const float CapsuleRadius = PlayerChar->GetCapsuleComponent()
                ? PlayerChar->GetCapsuleComponent()->GetUnscaledCapsuleRadius()
                : 35.0f;
            if (DistToPlayer <= (HitDetectionRadius + CapsuleRadius))
            {
                BeginEmbed(PlayerChar);
                return;
            }
        }
    }

    // 累加飞行距离
    FlightDistance = FVector::Dist(GetActorLocation(), SpawnLocation);

    // 飞行距离超出最大有效范围（仅当 MaxFlightDistance > 0 时生效）→ 未命中，自动销毁
    if (MaxFlightDistance > 0.0f && FlightDistance >= MaxFlightDistance)
    {
        Resolve(false);
    }
}

void ABossSting::OnStingBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
    if (bResolved)
        return;

    // 已进入嵌入状态：忽略后续 overlap（嵌入推进期间碰撞已关闭，正常不会触发）
    if (bContactingPlayer)
        return;

    // 只对玩家敏感；小怪/其它 Pawn/非玩家 Actor 一律穿透（Overlap 本就不阻挡物理）
    ABattleCharacter* PlayerChar = Cast<ABattleCharacter>(OtherActor);
    if (!PlayerChar)
        return;

    // 玩家处于无敌帧（普通闪避/完美闪避/大招）：毒刺穿过继续飞，不嵌入、不结算伤害。
    // （Overlap 不阻挡物理，毒刺本就继续飞行，无需额外处理，直接 return 即可）
    if (PlayerChar->IsInvincibleNow())
    {
        UE_LOG(LogTemp, Warning, TEXT("Boss sting overlapped invincible player '%s', passing through."),
            *OtherActor->GetName());
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("BossSting Overlap player '%s', embedding..."), *OtherActor->GetName());
    BeginEmbed(PlayerChar);
}

// 启动嵌入流程：接触玩家后进入「嵌入」状态——
// 记录被接触玩家 + 嵌入方向（沿当前飞行方向继续刺入），减速嵌入，关闭碰撞。
// 之后由 UpdateEmbed 逐帧判定毒刺球心是否侵入玩家胶囊足够深才结算伤害。
void ABossSting::BeginEmbed(ABattleCharacter* PlayerChar)
{
    if (!PlayerChar || bContactingPlayer || bResolved)
        return;

    bContactingPlayer = true;
    ContactedPlayer = PlayerChar;

    // 嵌入方向优先用记录的飞行方向（Overlap/距离兜底触发时 Velocity 可能已被清零）
    EmbedDirection = FlightDirection;
    if (EmbedDirection.IsNearlyZero())
    {
        // 兜底：取毒刺朝向玩家方向
        EmbedDirection = (PlayerChar->GetActorLocation() - GetActorLocation()).GetSafeNormal();
    }

    // 减速嵌入：飞行组件停止（速度清零），由 UpdateEmbed 逐帧按 EmbedSpeed 推进
    ProjectileMovement->StopMovementImmediately();
    ProjectileMovement->Velocity = FVector::ZeroVector;
    ProjectileMovement->InitialSpeed = 0.0f;

    // 关闭碰撞：嵌入阶段判定由 UpdateEmbed 的空间计算驱动，毒刺需自由「刺入」玩家体内，
    // 不能被玩家胶囊挡住推进
    CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    UE_LOG(LogTemp, Warning, TEXT("Boss sting CONTACT player '%s', embedding... (dodge now = perfect dodge)"),
        *PlayerChar->GetName());
}

// 嵌入判定：接触玩家后每帧调用
// 判定毒刺球心是否侵入玩家胶囊足够深（达到 PenetrationDepthForDamage）→ 结算伤害+销毁；
// 期间玩家无敌（闪避完美闪避）→ 视为躲开，销毁不结算。
void ABossSting::UpdateEmbed(float DeltaTime)
{
    ABattleCharacter* PlayerChar = ContactedPlayer.Get();
    if (!PlayerChar)
    {
        // 玩家已销毁/失效 → 未命中
        Resolve(false);
        return;
    }

    // 嵌入超时兜底：接触后长时间未完全进入（玩家跑开/毒刺卡表面）→ 未命中销毁
    EmbedElapsed += DeltaTime;
    if (EmbedElapsed >= EmbedTimeout)
    {
        UE_LOG(LogTemp, Warning, TEXT("Boss sting embed timeout (%.2fs), missed player."), EmbedElapsed);
        Resolve(false);
        return;
    }

    // 嵌入期间玩家进入无敌帧（闪避 = 完美闪避）→ 毒刺被躲开，销毁且不结算伤害
    if (PlayerChar->IsInvincibleNow())
    {
        UE_LOG(LogTemp, Warning, TEXT("Boss sting DODGED during embed (perfect dodge)! No damage."));
        Resolve(false);
        return;
    }

    // 沿嵌入方向缓慢推进（刺入），营造「毒刺扎进角色」的观感
    // （碰撞已关闭，直接位移不 sweep，保证能自由穿入玩家体内）
    if (!EmbedDirection.IsNearlyZero())
    {
        AddActorWorldOffset(EmbedDirection * EmbedSpeed * DeltaTime, false);
    }

    // 计算毒刺球心相对玩家胶囊的侵入深度
    UCapsuleComponent* PlayerCapsule = PlayerChar->GetCapsuleComponent();
    if (!PlayerCapsule)
    {
        // 无胶囊体兜底：按距离判定
        const float DistToPlayer = FVector::Dist(GetActorLocation(), PlayerChar->GetActorLocation());
        if (DistToPlayer < 30.0f)
        {
            const float Actual = PlayerChar->ApplySilentDamage(Damage, OwnerBoss.Get());
            // 命中施加中毒（无法重复叠加，已中毒时 ApplyPoison 返回 false 忽略）
            PlayerChar->ApplyPoison();
            UE_LOG(LogTemp, Warning, TEXT("Boss sting EMBEDDED (fallback) player '%s'! Damage=%.1f (applied=%.1f)."),
                *PlayerChar->GetName(), Damage, Actual);
            Resolve(true);
        }
        return;
    }

    const float CapsuleRadius = PlayerCapsule->GetUnscaledCapsuleRadius();
    const float CapsuleHalfHeight = PlayerCapsule->GetUnscaledCapsuleHalfHeight();
    const FVector CapsuleCenter = PlayerCapsule->GetComponentLocation();
    const FVector StingCenter = GetActorLocation();

    // 玩家胶囊轴线沿 Z，球心相对胶囊中心的轴向与径向分量
    const float Dz = StingCenter.Z - CapsuleCenter.Z;
    const float DistH = FVector::Dist2D(StingCenter, CapsuleCenter); // 水平距离（X/Y 平面）

    // 只有球心落在胶囊圆柱段高度内才算「刺入」；超出上下半球顶则视为未进入
    const float CylinderHalfLength = FMath::Max(0.0f, CapsuleHalfHeight - CapsuleRadius);
    const float AbsDz = FMath::Abs(Dz);
    if (AbsDz > CylinderHalfLength)
    {
        // 球心高于/低于圆柱段（在半球顶区域），仍未完全进入，继续嵌入
        return;
    }

    // 侵入深度 = 胶囊表面（半径 CapsuleRadius）到球心的径向距离
    const float PenetrationDepth = CapsuleRadius - DistH;
    if (PenetrationDepth >= PenetrationDepthForDamage)
    {
        // 完全进入角色模型 → 结算伤害并销毁
        const float Actual = PlayerChar->ApplySilentDamage(Damage, OwnerBoss.Get());
        // 命中施加中毒（无法重复叠加，已中毒时 ApplyPoison 返回 false 忽略）
        PlayerChar->ApplyPoison();
        UE_LOG(LogTemp, Warning, TEXT("Boss sting EMBEDDED player '%s'! Damage=%.1f (applied=%.1f)."),
            *PlayerChar->GetName(), Damage, Actual);
        Resolve(true);
    }
}

void ABossSting::Resolve(bool bHitPlayer)
{
    if (bResolved)
        return;
    bResolved = true;

    // 通知 Boss（触发瞬移背刺/连击）
    if (AMonsterBase* Boss = OwnerBoss.Get())
    {
        Boss->OnBossStingResolved(bHitPlayer);
    }

    // 命中玩家即销毁毒刺；未命中（超射程/命中阻挡物）同样销毁
    Destroy();
}
