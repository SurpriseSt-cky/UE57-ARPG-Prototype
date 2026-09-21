#include "ProjectileBase.h"
#include "BattleCharacter.h"
#include "MonsterBase.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"

AProjectileBase::AProjectileBase()
{
    PrimaryActorTick.bCanEverTick = false;

    // ---- 碰撞球体（根组件）----
    CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
    SetRootComponent(CollisionSphere);
    CollisionSphere->InitSphereRadius(10.0f);
    CollisionSphere->SetCollisionProfileName(TEXT("Projectile"));
    CollisionSphere->OnComponentHit.AddDynamic(this, &AProjectileBase::OnHit);

    // ---- 可视网格：默认引擎小球便于测试（正式弹丸模型在派生 BP 里替换）----
    ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileMesh"));
    ProjectileMesh->SetupAttachment(CollisionSphere);
    ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshAsset(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (SphereMeshAsset.Succeeded())
    {
        ProjectileMesh->SetStaticMesh(SphereMeshAsset.Object);
        ProjectileMesh->SetRelativeScale3D(FVector(0.2f));
    }

    // ---- 飞行组件：直线飞行（无重力、不反弹）----
    ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
    ProjectileMovement->SetUpdatedComponent(CollisionSphere);
    ProjectileMovement->bRotationFollowsVelocity = true;
    ProjectileMovement->bShouldBounce = false;
    ProjectileMovement->ProjectileGravityScale = 0.0f;
    ProjectileMovement->InitialSpeed = 5000.0f;
    ProjectileMovement->MaxSpeed = 5000.0f;
}

void AProjectileBase::BeginPlay()
{
    Super::BeginPlay();

    // 超时自动销毁，防止弹丸永久存在
    SetLifeSpan(MaxLifeTime);
}

void AProjectileBase::InitProjectile(const FVector& Direction, float DamageAmount, AActor* Causer, AController* InstigatorController)
{
    Damage = DamageAmount;
    bUseMultiplier = false;
    DamageCauser = Causer;
    InstigatorCtrl = InstigatorController;

    // 不与发射者发生碰撞（避免刚发射就打到自己）
    if (Causer)
    {
        CollisionSphere->IgnoreActorWhenMoving(Causer, true);
    }

    // 设定飞行方向
    ProjectileMovement->Velocity = Direction.GetSafeNormal() * ProjectileMovement->InitialSpeed;
}

void AProjectileBase::InitProjectileWithMultiplier(const FVector& Direction, float Multiplier, AActor* Causer, AController* InstigatorController)
{
    // 倍率模式：Damage 字段存储倍率，命中时由发射者角色按统一公式结算
    Damage = Multiplier;
    bUseMultiplier = true;
    DamageCauser = Causer;
    InstigatorCtrl = InstigatorController;

    if (Causer)
    {
        CollisionSphere->IgnoreActorWhenMoving(Causer, true);
    }

    ProjectileMovement->Velocity = Direction.GetSafeNormal() * ProjectileMovement->InitialSpeed;
}

void AProjectileBase::OnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit)
{
    AActor* Causer = DamageCauser.Get();

    // 命中目标：造成点伤害
    if (OtherActor && OtherActor != Causer)
    {
        float FinalDamage = Damage;

        // 倍率模式：由发射者角色按统一伤害公式结算（攻击力×倍率×[暴击]×(1+增伤)×等级系数×0.9）
        if (bUseMultiplier)
        {
            if (ABattleCharacter* Shooter = Cast<ABattleCharacter>(Causer))
            {
                AMonsterBase* HitMonster = Cast<AMonsterBase>(OtherActor);
                bool bWasCrit = false;
                FinalDamage = Shooter->ComputeFinalDamage(Damage, HitMonster, bWasCrit);
                // 预设暴击标志：怪物 TakeDamage 按实际扣血量弹出（暴击/普通）伤害飘字
                if (HitMonster)
                {
                    HitMonster->SetPendingDamageNumberCrit(bWasCrit);
                }
            }
        }

        // ApplyPointDamage 返回目标 TakeDamage 的实际结算伤害
        const float ActualDamage = UGameplayStatics::ApplyPointDamage(
            OtherActor,
            FinalDamage,
            ProjectileMovement->Velocity.GetSafeNormal(),
            Hit,
            InstigatorCtrl.Get(),
            Causer,
            UDamageType::StaticClass());

        // 命中处于可弹刀前摇窗口的怪物 → 自动触发弹刀，打断其攻击（发射者即角色）
        if (AMonsterBase* HitMonster = Cast<AMonsterBase>(OtherActor))
        {
            if (ABattleCharacter* Shooter = Cast<ABattleCharacter>(Causer))
            {
                // 镜头振动仅由大招段命中触发（UpdateUltimateSegmentCombat），弹丸命中不再振动
                Shooter->TryParryOnHit(HitMonster);
            }
        }

        // 发射者为角色时：能量增伤窗口内按实际伤害吸血回血
        if (ABattleCharacter* Shooter = Cast<ABattleCharacter>(Causer))
        {
            Shooter->ApplyEnergyBuffLifesteal(ActualDamage);
        }

        UE_LOG(LogTemp, Warning, TEXT("Projectile hit %s for %.1f damage!"), *OtherActor->GetName(), Damage);
    }

    // 命中任意阻挡物后销毁
    Destroy();
}
