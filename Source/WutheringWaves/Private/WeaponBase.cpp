#include "WeaponBase.h"
#include "BattleCharacter.h"
#include "MonsterBase.h"
#include "ProjectileBase.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"

AWeaponBase::AWeaponBase()
{
    PrimaryActorTick.bCanEverTick = true;

    WeaponMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponMesh"));
    SetRootComponent(WeaponMesh);
    // 武器本身不参与物理碰撞，命中判定由攻击扫掠负责
    WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AWeaponBase::BeginPlay()
{
    Super::BeginPlay();

    // 闲置隐藏计时起点：开局 IdleHideDelay 秒内未攻击同样会隐藏
    if (UWorld* World = GetWorld())
    {
        LastAttackTime = World->GetTimeSeconds();
    }
}

void AWeaponBase::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // ---- 战斗动作强制显形：技能/大招期间武器保持可见，优先于闲置隐藏 ----
    if (CombatShowRemaining > 0.0f)
    {
        CombatShowRemaining -= DeltaTime;
        if (CombatShowRemaining < 0.0f)
        {
            CombatShowRemaining = 0.0f;
        }
        if (IsHidden())
        {
            SetActorHiddenInGame(false);
        }
        return;
    }

    // ---- 闲置自动隐藏：未攻击超过 IdleHideDelay 秒则隐藏武器模型 ----
    if (!bAutoHideWhenIdle || IdleHideDelay <= 0.0f || !WeaponMesh)
        return;

    UWorld* World = GetWorld();
    if (!World)
        return;

    const bool bShouldHide = (World->GetTimeSeconds() - LastAttackTime) >= IdleHideDelay;
    if (bShouldHide != IsHidden())
    {
        SetActorHiddenInGame(bShouldHide);
    }
}

bool AWeaponBase::IsRanged() const
{
    // 法器为远程武器；轻剑/枪械近战（枪械占位，暂按近战处理，后续开发远程射击）。
    // 按 WeaponCategory（职位系统的大类）判断。
    return WeaponCategory == EWeaponCategory::MagicTool;
}

// ---- 枪口位置 ----
FVector AWeaponBase::GetMuzzleLocation() const
{
    if (!WeaponMesh)
    {
        // 组件理论上不会为空（构造函数创建），这里只是防御；退回 Actor 位置
        return GetActorLocation();
    }
    // 与 FireProjectile 完全一致的取法：Mesh 的 Muzzle 插槽世界位置。
    // 插槽不存在时引擎会退回组件自身位置，所以不会拿到无效值。
    return WeaponMesh->GetSocketLocation(MuzzleSocketName);
}

// ---- 类别 → 默认插槽 ----
FName AWeaponBase::GetSocketNameForCategory(EWeaponCategory Category)
{
    switch (Category)
    {
    case EWeaponCategory::MagicTool:
        // 法器吸附到角色的 magic_WeaponSocket（法师专属插槽）
        return TEXT("magic_WeaponSocket");
    case EWeaponCategory::Firearm:
    case EWeaponCategory::LightBlade:
    default:
        // 轻剑 / 枪械都吸附到 HandGrip_R（右手握持插槽）
        return TEXT("HandGrip_R");
    }
}

FName AWeaponBase::GetDefaultSocketForCategory() const
{
    return GetSocketNameForCategory(WeaponCategory);
}

// ---- 职位系统映射 ----
EWeaponCategory FJobWeaponRules::GetCategoryForJob(EJobClass Job)
{
    switch (Job)
    {
    case EJobClass::Mage:
        return EWeaponCategory::MagicTool;   // 法师 → 法器
    case EJobClass::Gunner:
        return EWeaponCategory::Firearm;     // 枪手 → 枪械
    case EJobClass::BladeMaster:
    default:
        return EWeaponCategory::LightBlade;  // 剑士 → 轻剑
    }
}

bool FJobWeaponRules::CanJobUseCategory(EJobClass Job, EWeaponCategory Category)
{
    // 当前一对一映射，直接比较；未来若一个职位可配多类，在这里扩展。
    return GetCategoryForJob(Job) == Category;
}

FName FJobWeaponRules::GetSocketForJob(EJobClass Job)
{
    // 插槽规则：法师 → magic_WeaponSocket，其余（剑士/枪手）→ HandGrip_R。
    // 与 AWeaponBase::GetSocketNameForCategory 保持同一套规则（按类别换算）。
    return AWeaponBase::GetSocketNameForCategory(GetCategoryForJob(Job));
}

// ---- 装备 ----
void AWeaponBase::OnEquipped(ABattleCharacter* NewOwner)
{
    if (!NewOwner)
        return;

    OwningCharacter = NewOwner;

    // 吸附到角色 Mesh 的骨骼/插槽（骨骼名与插槽名均可）
    AttachToComponent(NewOwner->GetMesh(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, AttachSocketName);
}

void AWeaponBase::OnUnequipped()
{
    DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    OwningCharacter = nullptr;
}

// ---- 攻击 ----
void AWeaponBase::BeginAttack(int32 ComboStep)
{
    ABattleCharacter* Wielder = OwningCharacter.Get();
    if (!Wielder || !GetWorld())
        return;

    // ---- 攻击触发：武器立即显形，并刷新闲置隐藏计时 ----
    if (GetWorld())
    {
        LastAttackTime = GetWorld()->GetTimeSeconds();
    }
    if (IsHidden())
    {
        SetActorHiddenInGame(false);
    }

    if (IsRanged())
    {
        // 远程：立即发射弹丸
        FireProjectile(Wielder);
    }
    else
    {
        // 近战：延迟到攻击动画的打击帧再做命中判定
        GetWorld()->GetTimerManager().ClearTimer(MeleeTraceTimer);
        GetWorld()->GetTimerManager().SetTimer(MeleeTraceTimer, this, &AWeaponBase::PerformMeleeTrace, FMath::Max(AttackHitDelay, 0.01f), false);
    }
}

void AWeaponBase::EndAttack()
{
    // 取消未执行的命中判定（攻击被闪避等动作打断时调用）
    if (GetWorld())
    {
        GetWorld()->GetTimerManager().ClearTimer(MeleeTraceTimer);
    }
}

bool AWeaponBase::TryResolveDueMeleeHit()
{
    UWorld* World = GetWorld();
    if (!World || !OwningCharacter.IsValid())
        return false;

    // 远程武器弹丸已即时发射，没有待结算的延迟命中判定
    if (IsRanged())
        return false;

    // 没有待执行的近战判定：要么未在攻击、要么延迟已到并已出伤
    if (!World->GetTimerManager().IsTimerActive(MeleeTraceTimer))
        return false;

    const float Remaining = World->GetTimerManager().GetTimerRemaining(MeleeTraceTimer);
    // 无论是否到出伤时刻，本次打断都取消该判定
    World->GetTimerManager().ClearTimer(MeleeTraceTimer);

    // 打断瞬间已到出伤时刻（剩余时间 ≤ 一帧预算，命中判定本帧内即应触发）→ 先结算。
    // 结算即"前方接触判定"：有怪物在武器扫掠范围内才实际掉血，没有接触则无事发生
    const float FrameBudget = FMath::Max(World->GetDeltaSeconds(), 0.016f) + 0.02f;
    if (Remaining <= FrameBudget)
    {
        PerformMeleeTrace();
        return true;
    }

    return false;
}

// ---- 战斗动作显形（技能/大招）：期间闲置隐藏暂缓 ----
void AWeaponBase::ShowForCombatAction(float Duration)
{
    CombatShowRemaining = FMath::Max(CombatShowRemaining, Duration);
    if (IsHidden())
    {
        SetActorHiddenInGame(false);
    }
}

void AWeaponBase::FireProjectile(ABattleCharacter* Wielder)
{
    if (!ProjectileClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponBase: ProjectileClass not set! Ranged weapon needs one (e.g. BP_Projectile)."));
        return;
    }

    // 发射位置：武器 Mesh 的 Muzzle 插槽（未创建则退回武器自身位置）
    const FVector SpawnLocation = WeaponMesh->GetSocketLocation(MuzzleSocketName);
    // 发射方向：角色面朝方向
    const FVector Direction = Wielder->GetActorForwardVector();

    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    if (AProjectileBase* Projectile = GetWorld()->SpawnActor<AProjectileBase>(ProjectileClass, SpawnLocation, Direction.Rotation(), Params))
    {
        // 远程弹丸：传入倍率(ProjectileDamage)，命中时由弹丸按统一伤害公式结算
        // （攻击力×弹丸倍率×[暴击]×(1+增伤)×等级系数×0.9，暴击/等级系数需命中目标后才可知）
        Projectile->InitProjectileWithMultiplier(Direction, ProjectileDamage, Wielder, Wielder->GetController());
    }
}

// ---- 近战命中判定 ----
void AWeaponBase::PerformMeleeTrace()
{
    ABattleCharacter* Wielder = OwningCharacter.Get();
    UWorld* World = GetWorld();
    if (!Wielder || !World)
        return;

    // 以角色为中心、沿面朝方向做球形扫掠
    const FVector Start = Wielder->GetActorLocation();
    const FVector End = Start + Wielder->GetActorForwardVector() * AttackRange;

    FCollisionQueryParams QueryParams;
    QueryParams.AddIgnoredActor(Wielder);
    QueryParams.AddIgnoredActor(this);

    TArray<FHitResult> Hits;
    World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(AttackRadius), QueryParams);

    // 同一次攻击对同一目标只结算一次伤害
    TSet<AActor*> AlreadyHit;
    for (const FHitResult& Hit : Hits)
    {
        AActor* HitActor = Hit.GetActor();
        if (!HitActor || AlreadyHit.Contains(HitActor))
            continue;

        AlreadyHit.Add(HitActor);

        // ApplyPointDamage 返回目标 TakeDamage 的实际结算伤害。
        // 普攻伤害 = 统一伤害公式（攻击力×普攻倍率×[暴击]×(1+增伤)×等级系数×0.9）
        AMonsterBase* HitMonster = Cast<AMonsterBase>(HitActor);
        bool bWasCrit = false;
        const float FinalDamage = Wielder->ComputeFinalDamage(BaseDamage, HitMonster, bWasCrit);
        // 预设暴击标志：怪物 TakeDamage 按实际扣血量弹出（暴击/普通）伤害飘字
        if (HitMonster)
        {
            HitMonster->SetPendingDamageNumberCrit(bWasCrit);
        }
        const float ActualDamage = UGameplayStatics::ApplyPointDamage(
            HitActor,
            FinalDamage,
            Wielder->GetActorForwardVector(),
            Hit,
            Wielder->GetController(),
            this,
            UDamageType::StaticClass());

        // 能量增伤窗口内按实际伤害吸血回血（角色侧判定，无 buff 时零开销）
        Wielder->ApplyEnergyBuffLifesteal(ActualDamage);

        // 命中处于可弹刀前摇窗口的怪物 → 自动触发弹刀，打断其攻击
        if (HitMonster)
        {
            Wielder->TryParryOnHit(HitMonster);
        }

        OnAttackHit(HitActor, FinalDamage);
    }

    if (AlreadyHit.Num() > 0)
    {
        // 镜头振动仅由大招段命中触发（UpdateUltimateSegmentCombat），近战命中不再振动
        UE_LOG(LogTemp, Warning, TEXT("Melee hit %d target(s), %.1f damage each."), AlreadyHit.Num(), BaseDamage);
    }
}

void AWeaponBase::OnAttackHit_Implementation(AActor* HitActor, float DamageAmount)
{
    // 默认空实现：蓝图里可加打击特效/音效
}
