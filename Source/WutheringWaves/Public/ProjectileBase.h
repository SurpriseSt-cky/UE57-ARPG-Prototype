#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectileBase.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;

/**
 * 远程武器弹丸（手枪/音感仪用）：
 * - 球体碰撞 + 抛物线关闭的直线飞行
 * - 命中任意可阻挡目标造成点伤害后销毁
 * - 超时自动销毁
 * 武器蓝图在 ProjectileClass 里配置派生的 BP_Projectile
 */
UCLASS()
class WUTHERINGWAVES_API AProjectileBase : public AActor
{
    GENERATED_BODY()

public:
    AProjectileBase();

    // 初始化飞行方向与伤害来源（发射时由武器调用）
    UFUNCTION(BlueprintCallable, Category = "Projectile")
    void InitProjectile(const FVector& Direction, float DamageAmount, AActor* Causer, AController* InstigatorController);

    // 以「倍率」方式初始化弹丸（远程武器统一伤害公式用）：
    // 命中目标怪物时，由发射者角色按统一公式结算（攻击力×倍率×[暴击]×(1+增伤)×等级系数×0.9）。
    // 因为暴击判定与等级系数需命中目标后才可知，弹丸发射时不预结算伤害，命中时再结算
    void InitProjectileWithMultiplier(const FVector& Direction, float Multiplier, AActor* Causer, AController* InstigatorController);

protected:
    virtual void BeginPlay() override;

    UFUNCTION()
    void OnHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

    // ---- 组件 ----
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    USphereComponent* CollisionSphere;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UStaticMeshComponent* ProjectileMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UProjectileMovementComponent* ProjectileMovement;

    // ---- 数值 ----
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
    float Damage = 20.0f;

    // 是否使用「倍率模式」（true = 命中时由发射者按统一公式结算，Damage 字段存储倍率）
    bool bUseMultiplier = false;

    // 弹丸最长存活时间（秒），超时自动销毁
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
    float MaxLifeTime = 3.0f;

    // 伤害来源（发射者角色）
    TWeakObjectPtr<AActor> DamageCauser;

    // 伤害归属控制器
    TWeakObjectPtr<AController> InstigatorCtrl;
};
