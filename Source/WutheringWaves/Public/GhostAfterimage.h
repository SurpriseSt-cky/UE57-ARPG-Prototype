#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/PoseableMeshComponent.h"
#include "GhostAfterimage.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * 蓝色残影：完美闪避时生成。生成瞬间对源网格做一次姿势快照（CopyPoseFromSkeletalComponent，
 * 之后不再更新——残影定格在"角色闪避过程中某一瞬间"的姿势，动作慢于角色即回溯），
 * 随后挂接到角色网格组件随其移动 → 残影始终贴身同位同高，不会因角色位移而被甩远。
 * 姿势回溯 + 位置贴身 → 角色身上叠映出"刚刚完成的动作"，即《鸣潮》完美闪避的观感。
 * 材质需为半透明（推荐 Unlit Translucent），支持两个可选参数：
 *   - 标量参数 "GhostAlpha"（0~1，淡出进度，未提供则等比缩放代替）
 *   - 向量参数 "GhostColor"（残影颜色）
 */
UCLASS()
class WUTHERINGWAVES_API AGhostAfterimage : public AActor
{
    GENERATED_BODY()

public:
    AGhostAfterimage();

    // 残影网格（PoseableMeshComponent：生成瞬间复制角色姿势快照并定格，无碰撞无阴影）
    UPROPERTY(VisibleAnywhere, Category = "Ghost")
    UPoseableMeshComponent* GhostMesh;

    // 初始化：复制源网格资产与缩放、定格姿势快照、应用残影材质与颜色
    // Lifetime = 残影存活时长（秒），期间逐渐淡出
    void InitGhost(USkeletalMeshComponent* SourceMesh, UMaterialInterface* GhostMaterial,
        const FLinearColor& GhostColor, float Lifetime);

    virtual void Tick(float DeltaTime) override;

protected:
    // 剩余存活时间 / 总时长（淡出比例用）
    float LifeRemaining = 1.0f;
    float TotalLife = 1.0f;

    // 材质实例（带 GhostAlpha/GhostColor 参数时驱动淡出与颜色）
    UPROPERTY()
    UMaterialInstanceDynamic* GhostMID = nullptr;
};
