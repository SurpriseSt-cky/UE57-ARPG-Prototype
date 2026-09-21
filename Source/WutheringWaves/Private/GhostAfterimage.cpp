#include "GhostAfterimage.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"

// 残影基础透明度：1.0 = 完全不透明；0.9 = 透明度降低 10%（初始值，淡出从该值线性降到 0）
namespace { constexpr float GHOST_BASE_ALPHA = 0.5f; }

AGhostAfterimage::AGhostAfterimage()
{
    PrimaryActorTick.bCanEverTick = true;

    GhostMesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("GhostMesh"));
    RootComponent = GhostMesh;

    // 纯视觉残影：无碰撞、无阴影、不生成重叠事件
    GhostMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GhostMesh->SetCastShadow(false);
    GhostMesh->SetGenerateOverlapEvents(false);
}

void AGhostAfterimage::InitGhost(USkeletalMeshComponent* SourceMesh, UMaterialInterface* GhostMaterial,
    const FLinearColor& GhostColor, float Lifetime)
{
    if (!SourceMesh)
        return;

    // 复制骨骼网格资产（同骨架；PoseableMeshComponent 用 SkinnedAsset API）
    if (USkeletalMesh* MeshAsset = SourceMesh->GetSkeletalMeshAsset())
    {
        GhostMesh->SetSkinnedAssetAndUpdate(MeshAsset);
    }

    // 姿势定格快照：一次性复制源网格当前姿势（Bone Space），之后不再更新。
    // 每个残影生成于闪避过程中的不同瞬间 → 各残影姿势互不相同且慢于角色当前动作，
    // 姿势回溯 → 角色身上叠映出"刚刚完成的动作"，即《鸣潮》完美闪避的观感。
    GhostMesh->CopyPoseFromSkeletalComponent(SourceMesh);

    // 挂接跟随：直接贴合源网格本地空间（SnapToTargetIncludingScale → 相对变换 = Identity），
    // 残影骨骼与角色骨骼 1:1 重叠 → 绝对贴身同位同高，随角色移动不甩远。
    // 注意：必须先 Attach 再设置相对变换。无父级时 SetRelativeLocation 等价于 SetWorldLocation，
    // 会把残影瞬移到世界原点，随后 KeepWorldTransform 保持该错误位置 → 残影钉在原点附近不贴身。
    AttachToComponent(SourceMesh, FAttachmentTransformRules::SnapToTargetIncludingScale);
    // 仅当需要"残影沿移动方向反向偏移"时在此设置相对位置（0 = 完全贴身）
    GhostMesh->SetRelativeLocation(FVector::ZeroVector);
    GhostMesh->SetRelativeRotation(FRotator::ZeroRotator);

    // 显式确保可见（防任何来源的隐藏状态污染残影）
    GhostMesh->SetVisibility(true, true);
    GhostMesh->SetHiddenInGame(false, true);
    // 放大边界盒：残影姿势为定格快照，防止视锥剔除误判导致"生成了却看不见"
    GhostMesh->SetBoundsScale(5.0f);

    // 应用半透明残影材质（所有材质槽统一替换），驱动颜色与淡出。
    // 直接基于材质资产创建 MID，不依赖网格现有材质槽数量，避免槽缺失导致材质未应用
    if (GhostMaterial)
    {
        GhostMID = UMaterialInstanceDynamic::Create(GhostMaterial, this);
        const int32 NumSlots = GhostMesh->GetNumMaterials();
        for (int32 SlotIdx = 0; SlotIdx < NumSlots; ++SlotIdx)
        {
            GhostMesh->SetMaterial(SlotIdx, GhostMID);
        }
        if (GhostMID)
        {
            GhostMID->SetVectorParameterValue(TEXT("GhostColor"), GhostColor);
            GhostMID->SetScalarParameterValue(TEXT("GhostAlpha"), GHOST_BASE_ALPHA);
        }
        UE_LOG(LogTemp, Warning, TEXT("Ghost mesh ready: Mesh=%s Material=%s MID=%s Slots=%d (pose SNAPSHOT)"),
            GhostMesh->GetSkinnedAsset() ? *GhostMesh->GetSkinnedAsset()->GetName() : TEXT("NULL"),
            GhostMaterial ? *GhostMaterial->GetName() : TEXT("NULL"),
            GhostMID ? TEXT("OK") : TEXT("NULL"), NumSlots);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Ghost material is NULL - ghost will be invisible!"));
    }

    TotalLife = FMath::Max(0.05f, Lifetime);
    LifeRemaining = TotalLife;

    // 诊断：验证残影是否贴身（世界位置偏差应接近 0；相对变换应为单位变换）
    const FVector WorldDelta = GhostMesh->GetComponentLocation() - SourceMesh->GetComponentLocation();
    UE_LOG(LogTemp, Warning, TEXT("Ghost fit check: relLoc=%s relRot=%s worldDelta=%s (target ~(0,0,0))"),
        *GhostMesh->GetRelativeLocation().ToString(),
        *GhostMesh->GetRelativeRotation().ToString(),
        *WorldDelta.ToString());
}

void AGhostAfterimage::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // 残影位置由 Attach 挂接跟随角色网格（始终贴身同位同高）；姿势为生成瞬间定格快照，
    // 不随动画更新 → 动作回溯（残影映出角色"刚刚完成的动作"），形成《鸣潮》式观感。
    // Tick 仅负责生命周期倒计时与淡出。

    LifeRemaining -= DeltaTime;

    // 存活时间耗尽 → 销毁残影
    if (LifeRemaining <= 0.0f)
    {
        Destroy();
        return;
    }

    // 淡出：从基础透明度（0.9）线性降到 0；材质未提供该参数时按比例整体缩放代替淡出
    const float Alpha = GHOST_BASE_ALPHA * FMath::Clamp(LifeRemaining / TotalLife, 0.0f, 1.0f);
    if (GhostMID)
    {
        GhostMID->SetScalarParameterValue(TEXT("GhostAlpha"), Alpha);
    }
    else
    {
        // 无材质实例：轻微下沉收缩，至少给出"正在消失"的视觉反馈
        const float Scale = 0.6f + 0.4f * Alpha;
        GhostMesh->SetRelativeScale3D(FVector(Scale, Scale, Scale));
    }
}
