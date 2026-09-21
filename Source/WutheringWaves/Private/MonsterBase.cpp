#include "MonsterBase.h"
#include "BattleCharacter.h"
#include "InventoryComponent.h"
#include "BossSting.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Components/CapsuleComponent.h"
#include "Components/WidgetComponent.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimInstance.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "TimerManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/DataTable.h"

AMonsterBase::AMonsterBase()
{
    PrimaryActorTick.bCanEverTick = true;
    // Boss 血条用世界坐标每帧锁定到摄像机前：把 Tick 放到帧末（相机已更新），
    // 保证用的是本帧相机变换 → 与"挂载到摄像机"同样零延迟、转动视角不抖
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    bUseControllerRotationYaw = false;

    // 摄像机防遮挡策略：怪物网格体/胶囊体对 Camera 通道设为 Ignore。
    // 【修复】此前网格体 Block Camera + 玩家弹簧臂 bDoCollisionTest 探针（ProbeSize=30）的组合，
    // 在怪物贴身攻击（接触判定时模型与角色重叠/擦身）时，探针在离弹簧臂枢轴极近处命中
    // 怪物模型 → 相机被一路拉回到枢轴（胶囊中心 = 腰部高度），表现为"受击时镜头卡在
    // 角色腰部"，既看不到完整角色也看不到怪物。
    // 现统一交给 UpdateCameraOcclusion 的 Fade（渐变半透明）处理怪物挡视野：
    // 镜头贴近怪物时怪物渐变透明，视野始终保持完整（ARPG 通用做法）。
    // BeginPlay 里会再次强制设置一次（防蓝图序列化值覆盖构造函数默认值）
    if (GetMesh())
    {
        GetMesh()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    }
    if (GetCapsuleComponent())
    {
        GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    }

    // ---- 小怪头顶血条：挂在怪物身上（胶囊顶部偏上），屏幕空间固定像素大小 ----
    HealthBarWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("HealthBarWidget"));
    HealthBarWidget->SetupAttachment(RootComponent);
    HealthBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
    HealthBarWidget->SetDrawSize(HealthBarDrawSize);
    HealthBarWidget->SetHiddenInGame(true); // 脱战不可视，进战斗再显示

    // ---- Boss 屏幕血条：组件始终挂在怪物自身，战斗中每帧用世界坐标锁定到玩家摄像机前（屏幕顶部居中）----
    BossBarWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("BossBarWidget"));
    BossBarWidget->SetupAttachment(RootComponent);
    BossBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
    BossBarWidget->SetDrawSize(BossBarDrawSize);
    BossBarWidget->SetHiddenInGame(true); // 脱战不可视

    // ---- 弹刀提示（怪物头顶）：由 C++ 全权控制可见性 ——
    // 显示条件 = 进入战斗（bInCombat）+ 攻击蒙太奇播放进入该段弹刀时间窗口（UpdateHitWindow 检测）。
    // 修复"怪物未触发战斗弹刀UI就出现"的 bug：可见性不再依赖蓝图接线/组件默认值，
    // 构造与 BeginPlay 均强制隐藏，脱战兜底隐藏
    ParryPromptUI = CreateDefaultSubobject<UWidgetComponent>(TEXT("ParryPromptUI"));
    ParryPromptUI->SetupAttachment(RootComponent);
    ParryPromptUI->SetWidgetSpace(EWidgetSpace::Screen);
    ParryPromptUI->SetDrawSize(ParryPromptDrawSize);
    ParryPromptUI->SetHiddenInGame(true); // 默认隐藏，进弹刀窗口才显示

    // ---- Boss 狂暴红光点光源：挂在网格体上随身体移动，狂暴完成时亮起（默认隐藏）----
    EnrageLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("EnrageLight"));
    EnrageLight->SetupAttachment(GetMesh());
    EnrageLight->SetVisibility(false);

    // 尝试加载弹刀提示UI类（蓝图里创建 WBP_ParryPrompt 后自动加载；也可在怪物蓝图 UI 分类手动指定）
    static ConstructorHelpers::FClassFinder<UUserWidget> ParryPromptClass(TEXT("Blueprint'/Game/UI/WBP_ParryPrompt.WBP_ParryPrompt_C'"));
    if (ParryPromptClass.Succeeded())
    {
        ParryPromptWidgetClass = ParryPromptClass.Class;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("WBP_ParryPrompt NOT found! Create it in /Content/UI (or set ParryPromptWidgetClass in the monster Blueprint)."));
    }

    // ---- 出伤预警提示（怪物头顶）：攻击蒙太奇播放到出伤时间前 DamageWarningLeadTime 秒时显示 ----
    DamageWarningUI = CreateDefaultSubobject<UWidgetComponent>(TEXT("DamageWarningUI"));
    DamageWarningUI->SetupAttachment(RootComponent);
    DamageWarningUI->SetWidgetSpace(EWidgetSpace::Screen);
    DamageWarningUI->SetDrawSize(DamageWarningDrawSize);
    DamageWarningUI->SetHiddenInGame(true); // 默认隐藏，进出伤预警窗口才显示

    // 尝试加载出伤预警UI类（蓝图里创建 WBP_DamageWarning 后自动加载；也可在怪物蓝图 UI 分类手动指定）
    static ConstructorHelpers::FClassFinder<UUserWidget> DamageWarningClass(TEXT("Blueprint'/Game/UI/WBP_DamageWarning.WBP_DamageWarning_C'"));
    if (DamageWarningClass.Succeeded())
    {
        DamageWarningWidgetClass = DamageWarningClass.Class;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("WBP_DamageWarning NOT found! Create it in /Content/UI (or set DamageWarningWidgetClass in the monster Blueprint)."));
    }

    // 尝试加载伤害飘字UI类（蓝图里创建 WBP_DamageNumber_Normal/_Crit 后自动加载；也可在怪物蓝图 UI|DamageNumber 分类手动指定）。
    // 未命中时不在构造里告警：等真正弹出飘字时一次性提示（避免每个怪物实例刷屏）
    static ConstructorHelpers::FClassFinder<UUserWidget> DmgNumNormalClass(TEXT("Blueprint'/Game/UI/WBP_DamageNumber_Normal.WBP_DamageNumber_Normal_C'"));
    if (DmgNumNormalClass.Succeeded())
    {
        DamageNumberNormalWidgetClass = DmgNumNormalClass.Class;
    }
    static ConstructorHelpers::FClassFinder<UUserWidget> DmgNumCritClass(TEXT("Blueprint'/Game/UI/WBP_DamageNumber_Crit.WBP_DamageNumber_Crit_C'"));
    if (DmgNumCritClass.Succeeded())
    {
        DamageNumberCritWidgetClass = DmgNumCritClass.Class;
    }

    // 尝试加载小怪血条UI类（蓝图里创建 WBP_MonsterHealthBar 后自动加载）
    static ConstructorHelpers::FClassFinder<UUserWidget> MinionBarClass(TEXT("Blueprint'/Game/UI/WBP_MonsterHealthBar.WBP_MonsterHealthBar_C'"));
    if (MinionBarClass.Succeeded())
    {
        HealthBarWidgetClass = MinionBarClass.Class;
    }
    else
    {
        HealthBarWidgetClass = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("WBP_MonsterHealthBar NOT found! Create it in /Content/UI (with a ProgressBar named MonsterHealthProgressBar)."));
    }

    // 尝试加载Boss血条UI类（蓝图里创建 WBP_BossHealthBar 后自动加载）
    static ConstructorHelpers::FClassFinder<UUserWidget> BossBarClass(TEXT("Blueprint'/Game/UI/WBP_BossHealthBar.WBP_BossHealthBar_C'"));
    if (BossBarClass.Succeeded())
    {
        BossBarWidgetClass = BossBarClass.Class;
    }
    else
    {
        BossBarWidgetClass = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("WBP_BossHealthBar NOT found! Create it in /Content/UI (with a ProgressBar named BossHealthProgressBar)."));
    }

    // 尝试加载毒刺投射物类（蓝图里创建 BP_BossSting 后自动加载；也可在怪物蓝图 Sting 分类手动指定）。
    // 未命中时 SpawnBossSting 会回落到 C++ 原生 ABossSting（默认圆锥模型），仍可正常发射
    static ConstructorHelpers::FClassFinder<ABossSting> BossStingClassFinder(TEXT("Blueprint'/Game/Blueprints/BP_BossSting.BP_BossSting_C'"));
    if (BossStingClassFinder.Succeeded())
    {
        BossStingClass = BossStingClassFinder.Class;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("BP_BossSting NOT found! Falling back to native ABossSting (default cone). Set BossStingClass in the monster Blueprint, or create BP_BossSting."));
    }
}

void AMonsterBase::BeginPlay()
{
    Super::BeginPlay();

    // 【修复】强制怪物网格体/胶囊体忽略 Camera 通道（运行时覆盖，防止蓝图资产里
    // 已序列化的旧 Block 值生效导致"受击时相机被拉到角色腰部"）。
    // 怪物挡视野统一由 UpdateCameraOcclusion 的 Fade 渐变半透明处理
    if (GetMesh())
    {
        GetMesh()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    }
    if (GetCapsuleComponent())
    {
        GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    }

    // 记录出生点（小怪巡逻/回家基准；Boss 原地驻守）
    SpawnLocation = GetActorLocation();
    SpawnRotation = GetActorRotation();
    CurrentHealth = MaxHealth;

    // 记录 Mesh 基准相对变换（Boss 狂暴技能根运动残留旋转清除时恢复用）
    if (GetMesh())
    {
        BaseMeshRelativeLocation = GetMesh()->GetRelativeLocation();
        BaseMeshRelativeRotation = GetMesh()->GetRelativeRotation();
    }

    // ---- 初始化小怪头顶血条（Boss 不使用头顶血条，强制隐藏）----
    if (Rank == EMonsterRank::Minion && HealthBarWidget && HealthBarWidgetClass)
    {
        HealthBarWidget->SetWidgetClass(HealthBarWidgetClass);
        HealthBarWidget->InitWidget();
        HealthBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
        HealthBarWidget->SetDrawSize(HealthBarDrawSize);

        // 头顶位置：胶囊半高 + 抬高偏移
        if (GetCapsuleComponent())
        {
            const float HeadZ = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + HealthBarHeadOffset;
            HealthBarWidget->SetRelativeLocation(FVector(0.0f, 0.0f, HeadZ));
        }
        HealthBarWidget->SetHiddenInGame(true);

        UE_LOG(LogTemp, Warning, TEXT("Monster Head HealthBar initialized!"));
    }
    else if (HealthBarWidget)
    {
        // Boss：头顶血条强制隐藏，任何情况下不显示
        HealthBarWidget->SetHiddenInGame(true);
    }

    // ---- 初始化Boss血条（保持隐藏，进战斗后每帧世界坐标锁定到摄像机前）----
    if (BossBarWidget && BossBarWidgetClass)
    {
        BossBarWidget->SetWidgetClass(BossBarWidgetClass);
        BossBarWidget->InitWidget();
        BossBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
        BossBarWidget->SetDrawSize(BossBarDrawSize);
        BossBarWidget->SetHiddenInGame(true);

        UE_LOG(LogTemp, Warning, TEXT("Boss Screen HealthBar initialized!"));
    }
    else if (!BossBarWidgetClass)
    {
        UE_LOG(LogTemp, Error, TEXT("Boss: WBP_BossHealthBar NOT loaded! Check /Content/UI/WBP_BossHealthBar exists."));
    }

    // ---- 初始化弹刀提示（保持隐藏：进战斗 + 蒙太奇进入弹刀时间窗口才由 C++ 显示）----
    if (ParryPromptUI)
    {
        if (ParryPromptWidgetClass)
        {
            ParryPromptUI->SetWidgetClass(ParryPromptWidgetClass);
            ParryPromptUI->InitWidget();
        }
        ParryPromptUI->SetWidgetSpace(EWidgetSpace::Screen);
        ParryPromptUI->SetDrawSize(ParryPromptDrawSize);

        // 头顶位置：胶囊半高 + 抬高偏移（比血条更高，避免与血条重叠）
        if (GetCapsuleComponent())
        {
            const float PromptZ = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + ParryPromptHeadOffset;
            ParryPromptUI->SetRelativeLocation(FVector(0.0f, 0.0f, PromptZ));
        }

        // 强制隐藏（覆盖蓝图资产里可能序列化的"可见"默认值——未进战斗绝不显示）
        ParryPromptUI->SetHiddenInGame(true);

        // 控件实例未创建（异常状态）→ 明确报错，便于排查
        if (!ParryPromptUI->GetWidget())
        {
            UE_LOG(LogTemp, Error, TEXT("Parry prompt widget INSTANCE is NULL after init (class=%s)! Widget will not render."),
                ParryPromptWidgetClass ? *ParryPromptWidgetClass->GetName() : TEXT("NONE"));
        }

        UE_LOG(LogTemp, Warning, TEXT("Parry prompt widget initialized (class=%s, hidden until parry window in combat)."),
            ParryPromptWidgetClass ? *ParryPromptWidgetClass->GetName() : TEXT("NONE"));
    }

    // ---- 初始化出伤预警提示（保持隐藏：进入出伤预警窗口才由 C++ 显示）----
    if (DamageWarningUI)
    {
        if (DamageWarningWidgetClass)
        {
            DamageWarningUI->SetWidgetClass(DamageWarningWidgetClass);
            DamageWarningUI->InitWidget();
        }
        DamageWarningUI->SetWidgetSpace(EWidgetSpace::Screen);
        DamageWarningUI->SetDrawSize(DamageWarningDrawSize);

        // 头顶位置：胶囊半高 + 抬高偏移（比弹刀提示更高，避免重叠）
        if (GetCapsuleComponent())
        {
            const float WarningZ = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + DamageWarningHeadOffset;
            DamageWarningUI->SetRelativeLocation(FVector(0.0f, 0.0f, WarningZ));
        }

        DamageWarningUI->SetHiddenInGame(true);

        UE_LOG(LogTemp, Warning, TEXT("Damage warning widget initialized (class=%s, hidden until damage warning window)."),
            DamageWarningWidgetClass ? *DamageWarningWidgetClass->GetName() : TEXT("NONE"));
    }

    // ---- 初始状态 ----
    if (Rank == EMonsterRank::Minion)
    {
        AIState = EMonsterAIState::Idle;
        PickNewPatrolTarget();
    }
    else
    {
        // Boss 脱战原地驻留，不巡逻
        AIState = EMonsterAIState::Idle;
    }

    UE_LOG(LogTemp, Warning, TEXT("Monster '%s' spawned! Rank=%s, Health=%.0f"),
        *GetName(), (Rank == EMonsterRank::Boss ? TEXT("Boss") : TEXT("Minion")), CurrentHealth);
}

void AMonsterBase::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // 摄像机贴近时的遮挡处理（默认渐变半透明，防止怪物挡住视野），死亡/冻结状态同样生效
    UpdateCameraOcclusion();

    // 视口尺寸变化检测：窗口拉伸/分辨率切换后强制刷新屏幕空间 UI（修复 resize 后控件消失）
    CheckViewportResize();

    // 伤害飘字寿命推进（上飘 + 末段淡出 + 到时销毁；死亡状态下也继续走完淡出）
    UpdateDamageNumbers(DeltaTime);

    if (AIState == EMonsterAIState::Dead)
        return;

    // ---- 毒刺命中后延迟瞬移：倒计时结束才瞬移至角色背后并开始背刺连击 ----
    if (BossStingTeleportDelayRemaining > 0.0f)
    {
        BossStingTeleportDelayRemaining -= DeltaTime;
        if (BossStingTeleportDelayRemaining <= 0.0f)
        {
            BossStingTeleportDelayRemaining = 0.0f;
            TeleportBehindPlayer();
            StartStingBackstabCombo();
            UE_LOG(LogTemp, Warning, TEXT("Boss '%s' sting teleport delay elapsed, teleporting behind and starting backstab combo."), *GetName());
        }
    }

    // 攻击冷却
    if (CurrentAttackCd > 0.0f)
    {
        CurrentAttackCd -= DeltaTime;
    }

    // 受击反应冷却递减
    if (HitReactionCooldownRemaining > 0.0f)
    {
        HitReactionCooldownRemaining -= DeltaTime;
        if (HitReactionCooldownRemaining < 0.0f)
        {
            HitReactionCooldownRemaining = 0.0f;
        }
    }

    // ---- 停滞（弹刀）：冻结 AI 移动与攻击 ----
    // StaggerRemaining>0 时：持续清零速度（防滑行）、跳过 UpdateAI（不追踪/不攻击/不播移动蒙太奇）
    if (StaggerRemaining > 0.0f)
    {
        StaggerRemaining -= DeltaTime;
        const bool bJustEndedStagger = (StaggerRemaining <= 0.0f);
        if (bJustEndedStagger)
        {
            StaggerRemaining = 0.0f;

            // 【修复"被弹刀后进行完被弹刀动作后出现一小段该动作"】硬直归零的这一刻，
            // 主动 Montage_Stop(弹刀蒙太奇, 0.0f) 立即干净停止 after_tandao，不留 BlendOut 残留。
            // 此前 StaggerRemaining 归零时 after_tandao 还在 BlendOut（混合回 Idle），
            // 这一小段混合输出看起来像"动作最后一小段被卡住/重播"。
            // 0.0f 混合时间 = 立即切到默认状态，无视觉残留。
            if (ParryReactionMontage && GetMesh() && GetMesh()->GetAnimInstance()
                && GetMesh()->GetAnimInstance()->Montage_IsPlaying(ParryReactionMontage))
            {
                GetMesh()->GetAnimInstance()->Montage_Stop(0.0f, ParryReactionMontage);
            }
        }
        StopHorizontalMovement();

        // 停滞时长已在 InterruptAttackByParry 按弹刀反应蒙太奇实际长度精确设定（与动画同时结束），
        // 此处不再轮询 Montage_IsPlaying 提前清零：Montage_Play 后的 BlendIn 混合期内
        // Montage_IsPlaying 可能短暂返回 false，若据此清零会导致弹刀动画刚起播就被"恢复"打断。
        // StaggerRemaining 自然递减到 0 时动画恰好播完，无缝衔接、无"站直僵硬"窗口。

        // 停滞仍在进行：刷新 Boss 血条位置后 return（冻结 AI，不播移动/待机蒙太奇）
        if (StaggerRemaining > 0.0f)
        {
            // 【修复】弹刀停滞期也必须刷新 Boss 屏幕血条位置：血条是世界坐标锁定屏幕顶部，
            // 此前提前 return 跳过了 UpdateBossBarPosition → 弹刀停滞的时长内相机/角色一移动，
            // 血条被冻结在世界空间旧位置，看起来"错位/飘走"（与狂暴期间同类问题）
            if (Rank == EMonsterRank::Boss && bInCombat)
            {
                UpdateBossBarPosition();
            }
            return;
        }
        // 停滞已结束（含弹刀反应蒙太奇提前播完）：不 return，落到下方正常帧逻辑，
        // 由移动/待机蒙太奇逻辑接管动画，衔接顺畅、无"站直僵住"窗口。
    }

    // ---- Boss 狂暴系统：回血阶段/狂暴技能期间不移动、不跑 AI、不播移动蒙太奇 ----
    // （UpdateEnrage 接管：回血计时/回满血/蒙太奇循环 + 段推进兜底与攻击窗口伤害结算；
    //   狂暴技能段的根运动位移仍会推动胶囊，与"不移动"不冲突——那是动画驱动的真实位移）
    if (bIsEnrageHealing || bIsEnrageSkillCasting)
    {
        UpdateEnrage(DeltaTime);

        // 【修复】狂暴期间也必须刷新 Boss 屏幕血条位置：血条是世界坐标锁定屏幕顶部，
        // 此前提前 return 跳过了 UpdateBossBarPosition → 回血的数秒内相机/角色一移动，
        // 血条被冻结在世界空间旧位置，看起来"位置异常/飘走"
        if (Rank == EMonsterRank::Boss && bInCombat)
        {
            UpdateBossBarPosition();
        }

        // 调试绘制狂暴技能作用范围（狂暴技能释放期间，蓝图 bDebugDrawEnrageRange 开关）
        if (bDebugDrawEnrageRange && bIsEnrageSkillCasting)
        {
            DrawEnrageRangeDebug();
        }
        return;
    }

    // ---- 旋转守卫：狂暴技能结束后 EnrageRotationGuardDuration 秒内每帧压平 Pitch/Roll ----
    // 蒙太奇 BlendOut 混合输出期（时长由蒙太奇资产决定）根运动旋转会以递减权重继续施加到 Actor，
    // 单次定时补清可能早于混合结束而失效；守卫期内逐帧压平可彻底清掉残留（Yaw 不受影响）
    if (EnrageRotationGuardRemaining > 0.0f)
    {
        EnrageRotationGuardRemaining -= DeltaTime;
        ResetEnrageMontageRotation();
    }

    // ---- 移动蒙太奇：按水平速度播放/停止（攻击蒙太奇播放期间不打断攻击动画）----
    {
        const FVector Velocity = GetCharacterMovement() ? GetCharacterMovement()->Velocity : FVector::ZeroVector;
        const bool bMovingNow = Velocity.SizeSquared2D() > 100.0f; // 10cm/s 以上视为移动中

        UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
        // 攻击蒙太奇正在播且移动蒙太奇未激活 → 不启动移动蒙太奇（避免覆盖攻击动画）
        const bool bAttackAnimPlaying = AnimInst && AnimInst->IsAnyMontagePlaying() && !bMoveMontageActive;
        if (!bAttackAnimPlaying)
        {
            UpdateMoveMontage(bMovingNow);
        }
    }

    // 状态机
    UpdateAI(DeltaTime);

    // Boss 脱战行为（待机循环/脱战寻找/回原点重置）—— 在 AI 状态机之后驱动，独立于进战逻辑
    UpdateBossIdleBehavior(DeltaTime);

    // 动画衔接空挡兜底：上一动作播完、当前静止、无任何蒙太奇在播时，刷新待机动画，
    // 消除受击/技能等瞬发动作结束后到 AI 重新接管之间的"站直僵住"空挡期。
    UpdateAnimFallback();

    // 调试绘制攻击判定范围（战斗中每帧刷新，配合 bDebugDrawAttackHit 调参用）
    if (bDebugDrawAttackHit && bInCombat)
    {
        DrawAttackHitDebug();
    }

    // Boss：头顶血条每帧强制隐藏（防止蓝图里 Hidden In Game 被误开）
    if (Rank == EMonsterRank::Boss && HealthBarWidget)
    {
        HealthBarWidget->SetHiddenInGame(true);
    }

    // Boss 战斗中：血条用世界坐标锁定屏幕顶部居中（本帧相机变换，零延迟）
    if (Rank == EMonsterRank::Boss && bInCombat)
    {
        UpdateBossBarPosition();
    }
}

// ---- 状态机入口 ----
void AMonsterBase::UpdateAI(float DeltaTime)
{
    if (Rank == EMonsterRank::Minion)
    {
        UpdateMinionAI(DeltaTime);
    }
    else
    {
        UpdateBossAI(DeltaTime);
    }
}

// ---- 小怪 AI ----
void AMonsterBase::UpdateMinionAI(float DeltaTime)
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    const float Dist = GetPlayerDistance();

    switch (AIState)
    {
    case EMonsterAIState::Idle:
    case EMonsterAIState::Patrol:
    {
        // 角色出现在发现范围内 → 追踪并攻击
        if (Dist <= DetectRadius)
        {
            EnterCombat();
            break;
        }

        // 脱战计时清零（非战斗状态）
        OutOfCombatTimer = 0.0f;

        // 巡逻：停留等待 → 走向巡逻点 → 到达换新目标
        if (AIState == EMonsterAIState::Idle)
        {
            AIState = EMonsterAIState::Patrol;
        }

        if (PatrolWaitTimer > 0.0f)
        {
            PatrolWaitTimer -= DeltaTime;
            if (GetCharacterMovement())
            {
                // 停留时停下
                FVector V = GetCharacterMovement()->Velocity;
                V.X = 0.0f;
                V.Y = 0.0f;
                GetCharacterMovement()->Velocity = V;
            }
            break;
        }

        if (MoveTowards(PatrolTarget, PatrolSpeed, DeltaTime))
        {
            // 到达巡逻点：停留后换下一个目标
            PatrolWaitTimer = PatrolWaitTime;
            PickNewPatrolTarget();
        }
        break;
    }

    case EMonsterAIState::Chase:
    case EMonsterAIState::Attack:
    {
        // 脱战判定：角色距离超过 LeashRadius 持续 OutOfCombatDelay 秒 → 回家重置
        if (Dist > LeashRadius)
        {
            OutOfCombatTimer += DeltaTime;
            if (OutOfCombatTimer >= OutOfCombatDelay)
            {
                ReturnHome();
                break;
            }
        }
        else
        {
            OutOfCombatTimer = 0.0f;
        }

        // 攻击距离内 → 攻击；否则追踪（触发距离取 AttackRange 与 AttackHitRadius 较大值）
        if (Dist <= GetAttackTriggerDistance())
        {
            AIState = EMonsterAIState::Attack;
            FaceTarget(Player->GetActorLocation());
            // 进入攻击立即刹车：清掉追踪遗留速度，防止滑行贴脸挤进角色/摄像机
            StopHorizontalMovement();

            if (CurrentAttackCd <= 0.0f)
            {
                PerformAttack();
            }
        }
        else
        {
            AIState = EMonsterAIState::Chase;
            MoveTowards(Player->GetActorLocation(), ChaseSpeed, DeltaTime);
        }
        break;
    }

    case EMonsterAIState::Return:
    {
        // 回家途中角色再次进入发现范围 → 重新进入战斗
        if (Dist <= DetectRadius)
        {
            EnterCombat();
            break;
        }

        // 回到出生点 → 重置血量、隐藏血条、恢复巡逻
        if (MoveTowards(SpawnLocation, ChaseSpeed, DeltaTime))
        {
            ResetHealth();
            AIState = EMonsterAIState::Idle;
            PatrolWaitTimer = PatrolWaitTime;
            PickNewPatrolTarget();
            UE_LOG(LogTemp, Warning, TEXT("Minion returned home! Health reset to %.0f"), CurrentHealth);
        }
        break;
    }

    default:
        break;
    }
}

// ---- Boss AI ----
void AMonsterBase::UpdateBossAI(float DeltaTime)
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    // 狂暴回血/狂暴技能期间：不追踪、不攻击（Tick 已由 UpdateEnrage 接管，此处防御性兜底）
    if (bIsEnrageHealing || bIsEnrageSkillCasting)
        return;

    // 背刺连击期间：由连击定时器驱动逐段播放，跳过常规移动/攻击（防止 AI 抢占触发普通攻击）
    if (bBackstabComboActive)
    {
        StopHorizontalMovement();
        return;
    }

    const float Dist = GetPlayerDistance();

    if (!bInCombat)
    {
        // 脱战：原地驻留（不巡逻、不回家、不重置血量）
        // 角色进入以 Boss 为中心的触发战斗范围（BossCombatTriggerRadius，蓝图可调）→ 触发战斗，血条出现
        if (Dist <= BossCombatTriggerRadius)
        {
            EnterCombat();
        }
        return;
    }

    // 战斗中：角色超出"追击保持半径"（AggroRadius 与 DetectRadius 较大值）→ 脱战（原地驻留）。
    // 修复：角色越过 AggroRadius 但仍在 DetectRadius 内时 Boss 继续追击，
    // 不再因 AggroRadius 边界过小而提前脱战（此前 DetectRadius 对 Boss 完全不生效）
    if (Dist > GetBossChaseRadius())
    {
        LeaveCombat(false);
        return;
    }

    // ---- Boss 二阶段毒刺远程攻击：狂暴完成后，玩家在仇恨范围内但距离 > 触发距离 → 远程毒刺 ----
    // （毒刺阶段进行中时由 UpdateBossSting 接管，此处跳过常规移动/攻击）
    if (bBossStingStage)
    {
        UpdateBossSting(DeltaTime);
        if (Rank == EMonsterRank::Boss && bInCombat)
        {
            UpdateBossBarPosition();
        }
        return;
    }

    if (Dist <= GetAttackTriggerDistance())
    {
        AIState = EMonsterAIState::Attack;
        FaceTarget(Player->GetActorLocation());
        // 进入攻击立即刹车：清掉追踪遗留速度，防止滑行贴脸挤进角色/摄像机
        StopHorizontalMovement();

        if (CurrentAttackCd <= 0.0f)
        {
            PerformAttack();
        }
    }
    else
    {
        // 二阶段（狂暴完成）且玩家距离超过毒刺触发距离 → 尝试毒刺远程攻击
        if (bEnraged && bEnableBossSting)
        {
            TryTriggerBossSting(DeltaTime);
            // 毒刺触发后本帧不再追击（若已进入毒刺阶段则下一帧由 UpdateBossSting 接管）
            if (bBossStingStage)
            {
                return;
            }
        }

        AIState = EMonsterAIState::Chase;
        MoveTowards(Player->GetActorLocation(), ChaseSpeed, DeltaTime);
    }
}

// ---- 清零水平速度（保留垂直速度：重力/跳跃）----
void AMonsterBase::StopHorizontalMovement()
{
    if (GetCharacterMovement())
    {
        FVector V = GetCharacterMovement()->Velocity;
        V.X = 0.0f;
        V.Y = 0.0f;
        GetCharacterMovement()->Velocity = V;
    }
}

// ---- 移动辅助：直接驱动 CharacterMovement，无需 NavMesh ----
bool AMonsterBase::MoveTowards(const FVector& Target, float Speed, float DeltaTime)
{
    if (!GetCharacterMovement())
        return true;

    FVector Direction = Target - GetActorLocation();
    Direction.Z = 0.0f;

    // 已到达目标点（水平距离 < 50cm）
    if (Direction.SizeSquared() < 50.0f * 50.0f)
    {
        FVector V = GetCharacterMovement()->Velocity;
        V.X = 0.0f;
        V.Y = 0.0f;
        GetCharacterMovement()->Velocity = V;
        return true;
    }

    Direction.Normalize();

    // 水平速度朝目标，保留垂直速度（重力/跳跃）。
    // 起步/变速平滑：约 0.2s 趋近目标速度，与移动蒙太奇的混合时长匹配——
    // 身体从 0 平滑加速到 ChaseSpeed，脚步（按实际移速同步播放速率）同步加快，消除起步滑步
    FVector TargetVelocity = Direction * Speed;
    TargetVelocity.Z = GetCharacterMovement()->Velocity.Z;
    const FVector Smoothed = FMath::VInterpTo(GetCharacterMovement()->Velocity, TargetVelocity, DeltaTime, 12.0f);
    GetCharacterMovement()->Velocity = FVector(Smoothed.X, Smoothed.Y, TargetVelocity.Z);

    // 朝移动方向平滑转身
    FRotator TargetRotation = Direction.Rotation();
    TargetRotation.Pitch = 0.0f;
    TargetRotation.Roll = 0.0f;
    SetActorRotation(FMath::RInterpTo(GetActorRotation(), TargetRotation, DeltaTime, 10.0f));

    return false;
}

// ---- 朝向目标（攻击时面向角色）----
void AMonsterBase::FaceTarget(const FVector& Target)
{
    FVector Direction = Target - GetActorLocation();
    Direction.Z = 0.0f;
    if (Direction.SizeSquared() < 1.0f)
        return;

    FRotator TargetRotation = Direction.Rotation();
    TargetRotation.Pitch = 0.0f;
    TargetRotation.Roll = 0.0f;
    SetActorRotation(FMath::RInterpTo(GetActorRotation(), TargetRotation, GetWorld()->GetDeltaSeconds(), 10.0f));
}

// ---- 随机巡逻目标点（出生点周围 PatrolRadius 圆内）----
void AMonsterBase::PickNewPatrolTarget()
{
    const float Angle = FMath::FRandRange(0.0f, 2.0f * PI);
    // sqrt 使圆面积内均匀分布
    const float Radius = FMath::Sqrt(FMath::FRand()) * PatrolRadius;
    PatrolTarget = SpawnLocation + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0f);
}

// ---- 进入战斗：显示血条 ----
void AMonsterBase::EnterCombat()
{
    bInCombat = true;
    OutOfCombatTimer = 0.0f;
    AIState = EMonsterAIState::Chase;

    // 进战：停止 Boss 脱战行为（待机蒙太奇/寻找/回位）
    UpdateIdleMontage(false);
    OutOfCombatSearchTimer = 0.0f;
    SearchRemaining = 0.0f;
    if (bSearchMontageActive)
    {
        if (SearchMontage && GetMesh() && GetMesh()->GetAnimInstance())
        {
            GetMesh()->GetAnimInstance()->Montage_Stop(0.2f, SearchMontage);
        }
        bSearchMontageActive = false;
    }

    if (Rank == EMonsterRank::Minion)
    {
        // 小怪：显示头顶血条
        if (HealthBarWidget)
        {
            HealthBarWidget->SetHiddenInGame(false);
        }
        UE_LOG(LogTemp, Warning, TEXT("'%s' (Minion) enters combat! Player detected."), *GetName());
    }
    else
    {
        // Boss：血条挂到玩家摄像机，屏幕顶部居中
        AttachBossBarToCamera();
        UE_LOG(LogTemp, Warning, TEXT("'%s' (BOSS) enters combat! Boss bar shown."), *GetName());
    }

    UpdateHealthBar();
}

// ---- 离开战斗：隐藏血条（bResetHealth=true 时重置血量）----
void AMonsterBase::LeaveCombat(bool bResetHealth)
{
    bInCombat = false;
    AIState = EMonsterAIState::Idle;

    // 脱战立即收起弹刀提示（弹刀提示条件 = 战斗中 + 弹刀时间窗口，缺一不可）
    HideParryPrompt();

    // 停止移动
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->StopMovementImmediately();
    }

    if (Rank == EMonsterRank::Minion)
    {
        if (HealthBarWidget)
        {
            HealthBarWidget->SetHiddenInGame(true);
        }
    }
    else
    {
        DetachBossBar();
    }

    if (bResetHealth)
    {
        ResetHealth();
    }
}

// ---- 小怪脱战回家（隐藏血条，到家后重置血量）----
void AMonsterBase::ReturnHome()
{
    bInCombat = false;
    AIState = EMonsterAIState::Return;

    // 脱战回家：收起弹刀提示
    HideParryPrompt();

    if (HealthBarWidget)
    {
        HealthBarWidget->SetHiddenInGame(true);
    }

    UE_LOG(LogTemp, Warning, TEXT("Minion out of combat for %.1fs! Returning home."), OutOfCombatDelay);
}

// ---- 重置血量 ----
void AMonsterBase::ResetHealth()
{
    CurrentHealth = MaxHealth;
    UpdateHealthBar();

    // 掉落复位：本函数用于「怪物脱战回原点复活」，
    // 复活后应当能再次被打死并再次掉落 —— 否则第二次击杀会一无所获
    bDropsGranted = false;
}

// ---- 攻击角色（随机播放一段攻击，不与上一段重复；每段独立速度/范围/可弹刀；伤害延迟判定）----
void AMonsterBase::PerformAttack()
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    // 停滞期间不发起攻击（防御性兜底：冻结时 AI 计时已停，正常不会走到这里）
    if (bPerfectDodgeFrozen)
        return;

    // 攻击间隔从此刻开始计：每次从 [MinAttackInterval, MaxAttackInterval] 随机取值（默认 1-2.5s）；
    // 配置异常（Min>Max 或负数）回落到固定 AttackCooldown
    if (MinAttackInterval >= 0.0f && MaxAttackInterval >= MinAttackInterval)
    {
        CurrentAttackCd = FMath::FRandRange(MinAttackInterval, MaxAttackInterval);
    }
    else
    {
        CurrentAttackCd = AttackCooldown;
    }

    // 攻击时停止移动蒙太奇（让位给攻击蒙太奇）
    UpdateMoveMontage(false);

    // 进入攻击前摇窗口（直到伤害结算）：玩家在此窗口内闪避 = 完美闪避
    bIsTelegraphing = true;

    // 重置"本次攻击已命中玩家"标记：新一次攻击开始，完美闪避窗口重新开放
    bAttackHitPlayer = false;

    // 随机播放一段攻击（不与上一次重复），并刷新当前段运行时配置（速度/范围/可弹刀）
    const int32 MontageIndex = PlayRandomAttackMontage();
    if (MontageIndex != INDEX_NONE)
    {
        LastAttackMontageIndex = MontageIndex;
    }

    // 弹刀提示由 UpdateHitWindow 的时间轴检测驱动：
    // 蒙太奇播放进入 [ParryWindowStart, ParryWindowEnd] 区间时才触发 OnParryableTelegraph

    // 受击判定窗口：循环定时器按蒙太奇时间轴驱动（0.02s 检查播放位置是否进入判定区间）。
    // 有无蒙太奇行为一致；窗口起止由逐段 HitWindowStartTime/EndTime 或全局 AttackDamageDelay 回落决定
    StartHitWindow();
}

// ---- 受击判定窗口启动：攻击发起后循环检测（蒙太奇时间轴驱动）----
void AMonsterBase::StartHitWindow()
{
    bHitWindowRunning = true;
    bHitWindowOpen = false;
    bRangeHitDone = false;
    bParryWindowOpen = false;
    LastContactDamageTime = 0.0f;
    bPlayerWasInContact = false;
    AttackStartTime = GetWorld()->GetTimeSeconds();

    // 每 0.02s 检查一次蒙太奇播放位置是否进入判定区间
    GetWorldTimerManager().SetTimer(
        HitWindowTimerHandle,
        this, &AMonsterBase::UpdateHitWindow,
        0.02f, true);

    UE_LOG(LogTemp, Warning, TEXT("Monster hit-judgement window armed (montage %.2f~%.2fs, contact=%d, contactR=%.0f, dmgCooldown=%.2fs, parryWindow=%.2f~%.2f)."),
        CurrentHitWindowStart, CurrentHitWindowEnd, bCurrentUseContactHit ? 1 : 0, CurrentContactHitRadius, ContactHitDamageCooldown,
        CurrentParryWindowStart, CurrentParryWindowEnd);
}

// ---- 受击判定窗口循环检测：蒙太奇播放到 [Start, End] 区间内才执行受击判定 ----
void AMonsterBase::UpdateHitWindow()
{
    if (!bHitWindowRunning)
        return;

    // 攻击过程中死亡 → 立即收尾
    if (AIState == EMonsterAIState::Dead)
    {
        EndHitWindow(bAttackHitPlayer);
        return;
    }

    // ---- 取当前判定时间轴位置 ----
    UAnimInstance* AnimInst = (GetMesh()) ? GetMesh()->GetAnimInstance() : nullptr;
    float TimelinePos = 0.0f;

    if (CurrentAttackMontage)
    {
        // 有攻击蒙太奇：以蒙太奇时间轴为准（与蒙太奇编辑器显示一致，不受 PlayRate 换算影响）
        if (!AnimInst || !AnimInst->Montage_IsActive(CurrentAttackMontage))
        {
            // 蒙太奇已播完/被停（受击打断等）→ 判定窗口结束
            EndHitWindow(bAttackHitPlayer);
            return;
        }
        TimelinePos = AnimInst->Montage_GetPosition(CurrentAttackMontage);
    }
    else
    {
        // 无蒙太奇：用真实时间（秒）作窗口时间轴，行为与旧版延迟结算一致
        TimelinePos = GetWorld()->GetTimeSeconds() - AttackStartTime;
    }

    // ---- 弹刀时间窗口检测（独立于受击判定窗口，每 0.02s 检查蒙太奇播放位置）----
    // 进入 [ParryWindowStart, ParryWindowEnd] 区间 → 触发 OnParryableTelegraph（蓝图显示弹刀提示 UI）
    // 离开区间 → 触发 OnParryWindowEnd（蓝图隐藏弹刀提示）
    if (bCurrentAttackParryable)
    {
        const bool bShouldBeOpen =
            (TimelinePos >= CurrentParryWindowStart && TimelinePos <= CurrentParryWindowEnd);

        if (bShouldBeOpen && !bParryWindowOpen)
        {
            // 进入弹刀窗口
            bParryWindowOpen = true;
            bParryPromptActive = true;
            OnParryableTelegraph(LastAttackMontageIndex);
            // 显示头顶弹刀提示（内部有"战斗中"门控，脱战不显示）并重播入场动画
            ShowParryPrompt();
            UE_LOG(LogTemp, Warning, TEXT("Monster '%s' enters PARRY window (montage %.2f~%.2fs, now=%.2f)."),
                *GetName(), CurrentParryWindowStart, CurrentParryWindowEnd, TimelinePos);
        }
        else if (!bShouldBeOpen && bParryWindowOpen)
        {
            // 离开弹刀窗口
            bParryWindowOpen = false;
            if (bParryPromptActive)
            {
                bParryPromptActive = false;
                OnParryWindowEnd();
            }
            // 隐藏头顶弹刀提示
            HideParryPrompt();
            UE_LOG(LogTemp, Warning, TEXT("Monster '%s' exits PARRY window (now=%.2f)."), *GetName(), TimelinePos);
        }
    }

    // ---- 出伤预警检测：播放到出伤时间前 DamageWarningLeadTime 秒 → 显示"即将造成伤害"提示 ----
    // 预警期间（提示出现 → 出伤开始）玩家在攻击范围内闪避 = 完美闪避
    UpdateDamageWarning(TimelinePos, CurrentHitWindowStart);

    // 判定时间过线 → 窗口结束
    if (TimelinePos >= CurrentHitWindowEnd)
    {
        EndHitWindow(bAttackHitPlayer);
        return;
    }

    // 未到窗口起始 → 不判定（前摇中，可被完美闪避/弹刀）
    if (TimelinePos < CurrentHitWindowStart)
    {
        bHitWindowOpen = false;
        return;
    }

    // ---- 窗口内：执行受击判定（角色在攻击范围内 + 模型接触/范围命中）----
    bHitWindowOpen = true;

    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    if (bCurrentUseContactHit)
    {
        // 接触判定：贴身半径 + 多段伤害间隔 + 离开再进入即时结算
        const bool bInContact = IsPlayerInContactHitArea();
        const float Now = GetWorld()->GetTimeSeconds();

        if (bInContact)
        {
            // 玩家新进入接触半径（上一帧不在）→ 立即结算一次伤害
            if (!bPlayerWasInContact)
            {
                DealAttackDamageToPlayer();
                LastContactDamageTime = Now;
            }
            // 持续接触：按 ContactHitDamageCooldown 周期性结算
            else if (Now - LastContactDamageTime >= ContactHitDamageCooldown)
            {
                DealAttackDamageToPlayer();
                LastContactDamageTime = Now;
            }
        }
        bPlayerWasInContact = bInContact;
    }
    else
    {
        // 范围判定：窗口内首次进入攻击范围即结算一次（单次命中语义）
        if (!bRangeHitDone && IsPlayerInAttackHitArea())
        {
            DealAttackDamageToPlayer();
            bRangeHitDone = true;
        }
    }
}

// ---- 结束受击判定窗口（窗口到期/蒙太奇播完或被停/被打断/死亡）----
void AMonsterBase::EndHitWindow(bool bHit)
{
    bHitWindowRunning = false;
    bHitWindowOpen = false;
    bParryWindowOpen = false;
    bPlayerWasInContact = false;
    bCurrentAttackIsHeavy = false;

    // 收起出伤预警提示（攻击结束/被打断/落空都算预警结束）
    if (bDamageWarningActive)
    {
        bDamageWarningActive = false;
        OnDamageWarningEnd();
    }
    HideDamageWarning();

    GetWorldTimerManager().ClearTimer(HitWindowTimerHandle);

    // 前摇/判定窗口统一在此结束：关闭完美闪避与弹刀判定（优雅期从此刻起算）
    EndTelegraphWindow();

    UE_LOG(LogTemp, Warning, TEXT("Monster hit-judgement window ended (%s). window=%.2f~%.2fs."),
        bHit ? TEXT("hit") : TEXT("miss"), CurrentHitWindowStart, CurrentHitWindowEnd);
}

// ---- 当前攻击判定窗口的剩余真实秒数（完美闪避无敌时长计算用）----
float AMonsterBase::GetRemainingHitWindowSeconds() const
{
    if (!bHitWindowRunning)
        return 0.0f;

    UAnimInstance* AnimInst = (GetMesh()) ? GetMesh()->GetAnimInstance() : nullptr;

    if (CurrentAttackMontage)
    {
        if (!AnimInst || !AnimInst->Montage_IsActive(CurrentAttackMontage))
            return 0.0f; // 蒙太奇已结束

        const float TimelinePos = AnimInst->Montage_GetPosition(CurrentAttackMontage);
        const float RemainingMontageSec = FMath::Max(0.0f, CurrentHitWindowEnd - TimelinePos);
        // 蒙太奇秒 → 真实秒（PlayRate 越快真实耗时越短）
        return RemainingMontageSec / FMath::Max(0.05f, CurrentAttackPlayRate);
    }

    // 无蒙太奇：窗口时间轴即真实秒
    const float EndReal = AttackStartTime + CurrentHitWindowEnd;
    return FMath::Max(0.0f, EndReal - GetWorld()->GetTimeSeconds());
}

// ---- 实际对玩家造成攻击伤害（范围/接触两路共用）----
void AMonsterBase::DealAttackDamageToPlayer()
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    FVector Direction = (Player->GetActorLocation() - GetActorLocation()).GetSafeNormal();
    // 狂暴完成后攻击增伤（默认 x1.25）；狂暴技能段的伤害在 ApplyEnrageSkillDamage 单独结算
    const float Damage = AttackDamage * (bEnraged ? EnrageDamageMultiplier : 1.0f);
    // ApplyPointDamage 返回玩家 TakeDamage 的实际结算值：无敌状态格挡时返回 0
    const float ActualDamage = UGameplayStatics::ApplyPointDamage(Player, Damage, Direction, FHitResult(), GetController(), this, nullptr);

    // 本次攻击实际命中（未被无敌格挡）→ 关闭该次攻击的完美闪避资格：
    // 优雅期/接触窗口内再闪避只算普通闪避，杜绝"已扣血却弹完美闪避 UI"的时序问题
    if (ActualDamage > 0.0f)
    {
        bAttackHitPlayer = true;

        // 大幅度攻击命中：触发角色击飞蒙太奇（覆盖 TakeDamage 刚播的普通受击反应）。
        // 小幅度攻击：不额外处理，角色保持原设定的受击动画蒙太奇
        if (bCurrentAttackIsHeavy)
        {
            if (ABattleCharacter* PlayerChar = Cast<ABattleCharacter>(Player))
            {
                PlayerChar->PlayKnockbackReaction(this);
            }
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("Monster attacks player! Damage=%.1f (applied=%.1f)."), Damage, ActualDamage);
}

// ---- 结束攻击前摇窗口（伤害结算/落空/被弹刀打断/死亡时调用）----
void AMonsterBase::EndTelegraphWindow()
{
    // 记录前摇结束时间（完美闪避优雅期用）
    if (bIsTelegraphing)
    {
        TelegraphEndTime = GetWorld()->GetTimeSeconds();
    }
    bIsTelegraphing = false;

    // 弹刀提示正在显示 → 通知蓝图收起提示
    if (bParryPromptActive)
    {
        bParryPromptActive = false;
        OnParryWindowEnd();
    }

    // 兜底：确保头顶弹刀提示隐藏（所有攻击收尾路径均汇入此函数）
    HideParryPrompt();
}

// ---- 弹刀提示：显示（仅战斗中）+ 播放控件入场动画 ----
void AMonsterBase::ShowParryPrompt()
{
    if (!ParryPromptUI)
        return;

    // 战斗门控：弹刀提示显示条件 = 进入战斗 + 攻击蒙太奇进入弹刀时间窗口，缺一不可。
    // 脱战状态一律强制隐藏（修复"怪物未触发战斗弹刀UI就出现"的 bug）
    if (!bInCombat)
    {
        ParryPromptUI->SetHiddenInGame(true);
        return;
    }

    // 自愈：控件实例丢失（蓝图类损坏/初始化时序异常等）→ 立即重建，保证提示一定能渲染
    if (!ParryPromptUI->GetWidget() && ParryPromptWidgetClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("Parry prompt widget instance missing! Rebuilding from class..."));
        ParryPromptUI->SetWidgetClass(nullptr);
        ParryPromptUI->SetWidgetClass(ParryPromptWidgetClass);
        ParryPromptUI->InitWidget();
    }

    // 双保险：组件可见性 + 游戏中隐藏均强制打开
    ParryPromptUI->SetVisibility(true, true);
    ParryPromptUI->SetHiddenInGame(false);

    // 播放控件蓝图里做好的入场动画：每次进入弹刀窗口都从头重播一次
    if (UUserWidget* PromptWidget = ParryPromptUI->GetWidget())
    {
        if (UWidgetAnimation* PromptAnim = FindParryPromptAnimation(PromptWidget))
        {
            PromptWidget->PlayAnimation(PromptAnim);
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Parry prompt widget STILL null after rebuild attempt (class=%s)! Check WBP_ParryPrompt."),
            ParryPromptWidgetClass ? *ParryPromptWidgetClass->GetName() : TEXT("NONE"));
    }
}

// ---- 弹刀提示：隐藏 ----
void AMonsterBase::HideParryPrompt()
{
    if (ParryPromptUI)
    {
        ParryPromptUI->SetHiddenInGame(true);
    }
}

// ---- 出伤预警：更新状态（攻击时间轴位置 vs 出伤时间）----
void AMonsterBase::UpdateDamageWarning(float TimelinePos, float DamageStartTime)
{
    // 提前量 <= 0 = 关闭预警（同时预警式完美闪避窗口也不存在，回落旧完美闪避行为）
    const bool bShouldWarn =
        (DamageWarningLeadTime > 0.0f) &&
        (TimelinePos >= DamageStartTime - DamageWarningLeadTime) &&
        (TimelinePos < DamageStartTime);

    if (bShouldWarn && !bDamageWarningActive)
    {
        // 进入预警窗口：显示"即将造成伤害"提示
        bDamageWarningActive = true;
        OnDamageWarning();
        ShowDamageWarning();
        UE_LOG(LogTemp, Warning, TEXT("Monster '%s' DAMAGE WARNING! Hit at %.2fs, now=%.2fs (lead %.2fs). Dodge now = PERFECT DODGE."),
            *GetName(), DamageStartTime, TimelinePos, DamageWarningLeadTime);
    }
    else if (!bShouldWarn && bDamageWarningActive)
    {
        // 到达出伤时间（开始结算伤害）/ 窗口异常离开 → 收起提示
        bDamageWarningActive = false;
        OnDamageWarningEnd();
        HideDamageWarning();
        UE_LOG(LogTemp, Warning, TEXT("Monster '%s' damage warning ended (now=%.2fs, hit start=%.2fs)."),
            *GetName(), TimelinePos, DamageStartTime);
    }
}

// ---- 出伤预警：显示头顶提示（战斗中才显示）----
void AMonsterBase::ShowDamageWarning()
{
    if (!DamageWarningUI)
        return;

    // 脱战不显示（与弹刀提示同一门控）
    if (!bInCombat)
        return;

    // 控件实例未创建（蓝图类后配置等场景）→ 重建一次
    if (!DamageWarningUI->GetWidget() && DamageWarningWidgetClass)
    {
        DamageWarningUI->SetWidgetClass(nullptr);
        DamageWarningUI->SetWidgetClass(DamageWarningWidgetClass);
        DamageWarningUI->InitWidget();
    }

    DamageWarningUI->SetVisibility(true, true);
    DamageWarningUI->SetHiddenInGame(false);
}

// ---- 出伤预警：隐藏头顶提示 ----
void AMonsterBase::HideDamageWarning()
{
    if (DamageWarningUI)
    {
        DamageWarningUI->SetHiddenInGame(true);
    }
}

// ---- 在控件蓝图的动画列表中查找弹刀提示入场动画 ----
// ParryPromptAnimationName 留空 = 播放第一个动画；指定名称则按 MovieScene 名/对象名匹配，未命中回落第一个
UWidgetAnimation* AMonsterBase::FindParryPromptAnimation(UUserWidget* Widget) const
{
    if (!Widget)
        return nullptr;

    UWidgetBlueprintGeneratedClass* WidgetBPClass = Cast<UWidgetBlueprintGeneratedClass>(Widget->GetClass());
    if (!WidgetBPClass || WidgetBPClass->Animations.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("Parry prompt widget '%s' has NO animations! Add one in the Widget Blueprint timeline."),
            *Widget->GetName());
        return nullptr;
    }

    // 未指定动画名 → 播放第一个动画
    if (ParryPromptAnimationName.IsEmpty())
    {
        return WidgetBPClass->Animations[0].Get();
    }

    // 按名称查找（动画名 = Timeline 里的动画名）
    for (const TObjectPtr<UWidgetAnimation>& Anim : WidgetBPClass->Animations)
    {
        if (!Anim || !Anim->GetMovieScene())
            continue;

        if (Anim->GetMovieScene()->GetName() == ParryPromptAnimationName ||
            Anim->GetName() == ParryPromptAnimationName)
        {
            return Anim.Get();
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("Parry prompt animation '%s' NOT found in widget '%s'! Falling back to the first animation."),
        *ParryPromptAnimationName, *Widget->GetName());
    return WidgetBPClass->Animations[0].Get();
}

// ---- 完美闪避判定窗口：预警窗口模式 / 旧行为（前摇 + 接触窗口 + 优雅期）----
bool AMonsterBase::IsInPerfectDodgeWindow() const
{
    // 本次攻击已经实际命中过玩家（伤害未被无敌格挡）→ 不再允许完美闪避。
    // 修复时序问题：优雅期本意是覆盖"伤害结算瞬间"的前后误差，但伤害真的落到了
    // 玩家身上之后再闪避，观感上就是"完美闪避了却扣了血"——命中即关闭该次资格
    if (bAttackHitPlayer)
        return false;

    // ---- 声波攻击阶段：完美闪避 = 声波波前扫过玩家的容差窗口 ----
    // 狂暴技能声波模式下不再看普攻预警/前摇/优雅期，只认"波前距玩家 <= 容差+提前缓冲"
    // 的时机（窗口由 UpdateEnrageSoundWave 每帧维护）。两波之间无在飞波 → 返回 false
    if (bEnrageSoundWaveSkill && bEnrageSoundWaveStage)
    {
        return bEnrageWaveInFlight && bSoundWavePerfectWindow;
    }

    // ---- 毒刺远程攻击阶段：完美闪避 = 毒刺距玩家 > 阈值（还远，未贴身）----
    // 毒刺飞行中且距玩家足够远时，玩家闪避 = 完美闪避（毒刺随后穿过无敌玩家视为未命中）。
    // 毒刺一旦进入贴身距离（<= 阈值）则不再授予完美闪避——已来不及躲
    if (bBossStingFlying)
    {
        return IsBossStingPerfectDodgeWindow();
    }

    // ---- 新模式：完美闪避仅限出伤预警窗口（提示出现 → 出伤开始）----
    // 怪物攻击/狂暴技能播放到出伤时间前 DamageWarningLeadTime 秒时提示即将造成伤害，
    // 此期间玩家在攻击范围内闪避 = 完美闪避
    if (bPerfectDodgeOnlyInWarningWindow)
    {
        return bDamageWarningActive;
    }

    // ---- 旧行为：整个前摇 + 判定窗口 + 优雅期均可 ----
    // 前摇窗口内（攻击发起 → 判定窗口结束）
    if (bIsTelegraphing)
        return true;

    // 受击判定窗口检测进行中也算（窗口结束前闪避都有效）
    if (bHitWindowRunning || bHitWindowOpen)
        return true;

    // 优雅期：前摇结束后 PerfectDodgeGracePeriod 秒内仍可触发完美闪避
    if (PerfectDodgeGracePeriod > 0.0f && GetWorld())
    {
        const float TimeSinceEnd = GetWorld()->GetTimeSeconds() - TelegraphEndTime;
        return TimeSinceEnd < PerfectDodgeGracePeriod;
    }

    return false;
}

// ---- 弹刀打断当前攻击 ----
bool AMonsterBase::InterruptAttackByParry(AActor* Parrier)
{
    // 仅在弹刀时间窗口内（蒙太奇播放位置在 [ParryWindowStart, ParryWindowEnd] 内）生效
    if (!bParryWindowOpen || !bCurrentAttackParryable)
        return false;

    // Boss 狂暴阶段不可被弹刀打断（回血无敌 / 狂暴技能霸体）
    if (bIsEnrageHealing || bIsEnrageSkillCasting)
        return false;

    // 【修复"弹刀后僵硬站直"】停滞时长直接取弹刀反应蒙太奇的实际播放时长（含 PlayRate），
    // 而非固定的 ParryStaggerDuration。此前 ParryStaggerDuration（默认 2.0s）大于 after_tandao
    // 动画时长（约 1s）：动画播完混回 Idle（站直）后，StaggerRemaining 仍 >0 继续冻结 AI，
    // 导致怪物"站直一小会"才恢复行走。让停滞与动画同时结束即可无缝衔接、不僵硬。
    // 无弹刀反应蒙太奇时才回落到 ParryStaggerDuration（保持旧行为可配置）。
    const float ActualParryStagger = ParryReactionMontage
        ? ParryReactionMontage->GetPlayLength() / FMath::Max(0.05f, ParryReactionPlayRate)
        : ParryStaggerDuration;

    // 通用停滞：清攻击/接触定时器、结束前摇、停蒙太奇、停移动、设停滞时长
    ApplyStagger(ActualParryStagger);

    // 播放弹刀反应蒙太奇（攻击蒙太奇已被 Montage_Stop 停止，弹刀蒙太奇在硬直期间继续播放）
    if (ParryReactionMontage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Play(ParryReactionMontage, ParryReactionPlayRate);
    }

    // 通知蓝图（火花特效/音效/受击反馈）
    OnParried(Parrier);

    UE_LOG(LogTemp, Warning, TEXT("Monster '%s' attack PARRIED by '%s'! Staggered for %.2fs (parry reaction montage length %.2fs), montage played."),
        *GetName(), Parrier ? *Parrier->GetName() : TEXT("player"), ActualParryStagger,
        ParryReactionMontage ? ParryReactionMontage->GetPlayLength() : 0.0f);
    return true;
}

// ---- 通用停滞处理（弹刀用）----
// 清攻击与接触定时器、结束前摇、停蒙太奇、停移动、设置 StaggerRemaining 与 CurrentAttackCd。
// 不触发事件（由调用方按场景触发 OnParried/OnPerfectDodged）
void AMonsterBase::ApplyStagger(float Duration)
{
    // 取消尚未结算的伤害（含受击判定窗口）
    GetWorldTimerManager().ClearTimer(HitWindowTimerHandle);
    bHitWindowRunning = false;
    bHitWindowOpen = false;
    bParryWindowOpen = false;
    bPlayerWasInContact = false;

    // 结束前摇窗口（同时收起弹刀提示）
    EndTelegraphWindow();

    // 停止攻击蒙太奇（快速混出）
    if (GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Stop(0.15f);
    }

    // 【修复】同步重置移动/待机蒙太奇激活标志：Montage_Stop(0.15f) 停掉了所有蒙太奇
    // （含移动/待机），但标志若残留 true，恢复后 UpdateMoveMontage/UpdateIdleMontage 会
    // 误判"仍在播放"而漏播/错播，导致弹刀恢复瞬间动画衔接异常（站直僵住）。
    bMoveMontageActive = false;
    bIdleMontageActive = false;

    // 停止移动（清零速度，防止停滞期间滑行）
    StopHorizontalMovement();
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->StopMovementImmediately();
    }

    // 停滞期内无法再次攻击；StaggerRemaining 让 Tick 冻结 AI 移动
    StaggerRemaining = FMath::Max(StaggerRemaining, Duration);
    CurrentAttackCd = FMath::Max(CurrentAttackCd, Duration);
}

// ---- 完美闪避反馈：不打断怪物攻击，仅触发蓝图事件并返回建议无敌时长 ----
// 怪物的攻击动画继续播完、判定窗口继续运行；角色靠无敌帧规避伤害。
// 返回建议的角色无敌时长 = max(Duration, 该次攻击判定窗口剩余时间)：
// 从完美闪避触发直到该段攻击的受击判定时间结束，角色保持无敌
float AMonsterBase::StaggerByPerfectDodge(float Duration)
{
    if (AIState == EMonsterAIState::Dead)
        return 0.0f;

    // 取该次攻击判定窗口的剩余时间（攻击不取消，窗口仍在运行，直接取当前值）
    const float RemainingWindow = GetRemainingHitWindowSeconds();

    // 通知蓝图（时停/慢动作/特效/音效，强化完美闪避的实质反馈）
    OnPerfectDodged();

    // ---- 怪物整体停滞（可配置）：CustomTimeDilation=0 冻结动画/移动/AI 计时 ----
    // 蒙太奇暂停而非停止——动作不会被打断，恢复后从暂停处继续播放；
    // 判定窗口按蒙太奇时间轴驱动，动画冻结期间不会结算伤害
    if (bPerfectDodgeFreeze && PerfectDodgeFreezeDuration > 0.0f)
    {
        ApplyPerfectDodgeFreeze();
    }

    const float SuggestedInvincibility = FMath::Max(Duration, RemainingWindow);

    UE_LOG(LogTemp, Warning, TEXT("Monster '%s' perfect-dodged (attack NOT interrupted). Remaining hit window %.2fs -> suggest invincible %.2fs."),
        *GetName(), RemainingWindow, SuggestedInvincibility);

    return SuggestedInvincibility;
}

// ---- 完美闪避停滞：怪物时间冻结（动画/移动/AI 计时全部暂停，动作不打断）----
void AMonsterBase::ApplyPerfectDodgeFreeze()
{
    if (AIState == EMonsterAIState::Dead)
        return;

    // 首次进入冻结：记录原时间流速并归零（冻结期间再次完美闪避只重置计时）
    if (!bPerfectDodgeFrozen)
    {
        bPerfectDodgeFrozen = true;
        PreFreezeTimeDilation = CustomTimeDilation;
        CustomTimeDilation = 0.0f;
    }

    // 恢复计时必须用世界时间定时器：怪物自身 Tick 已被冻结（DeltaTime=0），无法推进任何计时
    GetWorldTimerManager().SetTimer(
        PerfectDodgeFreezeTimerHandle,
        this, &AMonsterBase::RestoreFromPerfectDodgeFreeze,
        FMath::Max(0.0f, PerfectDodgeFreezeDuration),
        false);
}

// ---- 解除完美闪避停滞：恢复时间流速，怪物从暂停处继续动作 ----
void AMonsterBase::RestoreFromPerfectDodgeFreeze()
{
    if (!bPerfectDodgeFrozen)
        return;

    bPerfectDodgeFrozen = false;
    CustomTimeDilation = PreFreezeTimeDilation;
    GetWorldTimerManager().ClearTimer(PerfectDodgeFreezeTimerHandle);
}

// ---- 大招停滞：冻结怪物时间流速（动画/移动/AI 计时全部暂停，动作不打断）----
// 与完美闪避停滞同理：CustomTimeDilation=0 让蒙太奇暂停而非停止，攻击不会被中断，
// 恢复后从暂停处继续。区别：无定时器、无固定时长，由角色大招 EndUltimate 主动恢复。
void AMonsterBase::ApplyUltimateFreeze()
{
    if (AIState == EMonsterAIState::Dead)
        return;

    // 首次进入冻结：记录原时间流速并归零（大招期间再次调用只刷新状态，不覆盖原流速）
    if (!bUltimateFrozen)
    {
        bUltimateFrozen = true;
        PreUltimateTimeDilation = CustomTimeDilation;
        CustomTimeDilation = 0.0f;
    }
}

// ---- 解除大招停滞：恢复时间流速，怪物从暂停处继续动作 ----
void AMonsterBase::RestoreFromUltimateFreeze()
{
    if (!bUltimateFrozen)
        return;

    bUltimateFrozen = false;
    CustomTimeDilation = PreUltimateTimeDilation;
}

// ---- 受击反应：随机播放一段受击蒙太奇 ----
void AMonsterBase::PlayHitReaction()
{
    // 死亡/停滞中不播放受击反应
    if (AIState == EMonsterAIState::Dead || StaggerRemaining > 0.0f)
        return;

    // Boss 狂暴阶段不播放受击反应：回血阶段无敌根本不会扣血（防御性兜底），
    // 狂暴技能霸体——可被攻击但不受击动画、不硬直、技能不中断
    if (bIsEnrageHealing || bIsEnrageSkillCasting)
        return;

    // 未配置受击蒙太奇 → 跳过
    if (HitReactionMontages.Num() == 0)
        return;

    // 冷却中 → 跳过（防止高频多段攻击刷屏受击动画）
    if (HitReactionCooldownRemaining > 0.0f)
        return;

    // 霸体模式（不打断攻击）：攻击进行中（前摇/判定窗口）不播放受击反应，攻击继续
    const bool bAttackActive = bIsTelegraphing || bHitWindowRunning;
    if (!bHitReactionInterruptsAttack && bAttackActive)
        return;

    // 打断模式：清除尚未结算的攻击（判定窗口定时器 + 前摇标志）
    if (bHitReactionInterruptsAttack && bAttackActive)
    {
        GetWorldTimerManager().ClearTimer(HitWindowTimerHandle);
        bHitWindowRunning = false;
        bHitWindowOpen = false;
        bParryWindowOpen = false;
        EndTelegraphWindow();
    }

    // 随机选一段受击蒙太奇
    const int32 Index = FMath::RandRange(0, HitReactionMontages.Num() - 1);
    UAnimMontage* Montage = HitReactionMontages[Index];
    if (Montage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Play(Montage, HitReactionPlayRate);

        // 【修复"受击后僵住站立"】受击蒙太奇（与移动/攻击同 Slot）会打断正在播的移动蒙太奇，
        // 但 bMoveMontageActive 标志残留 true。若受击后怪物仍在追击，下一帧 UpdateMoveMontage
        // 会因"速度尚未从 0 平滑加速到阈值"而走停止分支反复停移动蒙太奇，动画落到 Idle 站直。
        // 这里同步重置移动标志，让受击动画结束后 UpdateAnimFallback 能干净地按 Chase 意图补播 Walk。
        bMoveMontageActive = false;

        UE_LOG(LogTemp, Warning, TEXT("Monster '%s' plays hit reaction #%d (interrupt=%d)."),
            *GetName(), Index, bHitReactionInterruptsAttack ? 1 : 0);
    }

    // 进入受击反应冷却
    HitReactionCooldownRemaining = HitReactionCooldown;
}

// ---- 大范围区域判定：玩家是否落在攻击判定半径 + 扇形角度 + 高度容差内 ----
bool AMonsterBase::IsPlayerInAttackHitArea() const
{
    return IsPlayerInHitAreaWithRadius(CurrentHitRadius);
}

// ---- 模型接触判定：玩家是否落在当前段接触半径 + 扇形角度 + 高度容差内（贴身判定）----
bool AMonsterBase::IsPlayerInContactHitArea() const
{
    return IsPlayerInHitAreaWithRadius(CurrentContactHitRadius);
}

// ---- 通用命中区域判定（指定半径）：半径 + 扇形 + 高度容差 ----
bool AMonsterBase::IsPlayerInHitAreaWithRadius(float Radius) const
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return false;

    const FVector MonsterLoc = GetActorLocation();
    const FVector PlayerLoc = Player->GetActorLocation();
    const FVector ToPlayer = PlayerLoc - MonsterLoc;

    // ① 半径判定（水平距离，配合高度容差构成圆柱形判定区）
    const FVector ToPlayerXY(ToPlayer.X, ToPlayer.Y, 0.0f);
    if (ToPlayerXY.Size() > Radius)
        return false;

    // ② 高度容差判定：防止从头顶/脚下蹭到判定
    if (FMath::Abs(ToPlayer.Z) > AttackHitHeightTolerance)
        return false;

    // ③ 扇形判定（仅当角度 < 360 时生效）：玩家方向与怪物朝向的夹角需在半角内
    if (CurrentHitArcAngle < 360.0f - KINDA_SMALL_NUMBER)
    {
        const FVector FacingXY = FVector(GetActorForwardVector().X, GetActorForwardVector().Y, 0.0f).GetSafeNormal();
        const FVector ToPlayerXYDir = ToPlayerXY.GetSafeNormal();
        if (FacingXY.IsNearlyZero() || ToPlayerXYDir.IsNearlyZero())
            return false;

        const float HalfAngleRad = FMath::Clamp(CurrentHitArcAngle, 0.0f, 360.0f) * 0.5f * PI / 180.0f;
        if (FVector::DotProduct(FacingXY, ToPlayerXYDir) < FMath::Cos(HalfAngleRad))
            return false;
    }

    return true;
}

// ---- 调试绘制攻击判定范围（判定圆 + 扇形两条边界线）----
void AMonsterBase::DrawAttackHitDebug() const
{
    const UWorld* World = GetWorld();
    if (!World)
        return;

    const FVector Center = GetActorLocation();
    const FColor CircleColor = AttackHitArcAngle >= 360.0f ? FColor::Red : FColor::Orange;

    // 判定圆（水平地面）：YAxis/ZAxis 张成 X-Y 平面
    DrawDebugCircle(World, Center, AttackHitRadius, 48, CircleColor,
        false, -1.0f, 0, 2.0f, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f));

    // 模型接触判定：额外画当前段接触半径圆（蓝色，更小的贴身判定）
    if (bCurrentUseContactHit)
    {
        DrawDebugCircle(World, Center, CurrentContactHitRadius, 32, FColor::Blue,
            false, -1.0f, 0, 2.0f, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f));
    }

    // 扇形模式：画出怪物朝向与两条扇形边界线
    if (AttackHitArcAngle < 360.0f - KINDA_SMALL_NUMBER)
    {
        const float YawRad = FMath::Atan2(GetActorForwardVector().Y, GetActorForwardVector().X);
        const float HalfAngleRad = FMath::Clamp(AttackHitArcAngle, 0.0f, 360.0f) * 0.5f * PI / 180.0f;

        for (const float Offset : { -HalfAngleRad, HalfAngleRad })
        {
            const float Angle = YawRad + Offset;
            const FVector Edge = Center + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * AttackHitRadius;
            DrawDebugLine(World, Center, Edge, FColor::Yellow, false, -1.0f, 0, 2.0f);
        }
        // 朝向指示线
        DrawDebugLine(World, Center,
            Center + FVector(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.0f) * AttackHitRadius,
            FColor::Green, false, -1.0f, 0, 2.0f);
    }
}

// 调试绘制狂暴技能作用范围：EnrageSkillRadius 判定圆（紫色）+ 朝向线
void AMonsterBase::DrawEnrageRangeDebug() const
{
    const UWorld* World = GetWorld();
    if (!World || EnrageSkillRadius <= 0.0f)
        return;

    const FVector Center = GetActorLocation();

    // 判定圆（水平地面，紫色）：狂暴技能上半球范围的水平投影
    DrawDebugCircle(World, Center, EnrageSkillRadius, 64, FColor::Purple,
        false, -1.0f, 0, 2.0f, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f));

    // 朝向指示线（青色）：表明 Boss 当前朝向
    const FVector Forward = GetActorForwardVector();
    const FVector FlatForward = Forward.GetSafeNormal2D();
    DrawDebugLine(World, Center, Center + FlatForward * EnrageSkillRadius,
        FColor::Cyan, false, -1.0f, 0, 2.0f);
}

// ---- 随机播放一段攻击（不与上一次重复），返回索引；同时刷新当前段运行时配置 ----
int32 AMonsterBase::PlayRandomAttackMontage()
{
    UAnimInstance* AnimInst = (GetMesh()) ? GetMesh()->GetAnimInstance() : nullptr;

    // 默认回落：全局判定范围、全局速度、不可弹刀、不使用模型接触判定
    bCurrentAttackParryable = false;
    bCurrentUseContactHit = false;
    // 大幅度攻击默认回落旧版全局配置（逐段配置路径会被 Cfg.bHeavyAttack 覆盖）
    bCurrentAttackIsHeavy = bLegacyAttackIsHeavy;
    CurrentHitRadius = AttackHitRadius;
    CurrentHitArcAngle = AttackHitArcAngle;
    CurrentContactHitRadius = ContactHitRadius;
    CurrentContactHitWindow = ContactHitWindow;
    CurrentAttackMontage = nullptr;
    CurrentAttackPlayRate = FMath::Max(0.05f, AttackMontagePlayRate);
    float PlayRateToUse = AttackMontagePlayRate;

    // ---- 优先：逐段配置（AttackConfigs 非空时）----
    if (AttackConfigs.Num() > 0)
    {
        // 收集配置了蒙太奇的段索引
        TArray<int32> ValidIndices;
        for (int32 i = 0; i < AttackConfigs.Num(); ++i)
        {
            if (AttackConfigs[i].Montage)
            {
                ValidIndices.Add(i);
            }
        }
        if (ValidIndices.Num() == 0 || !AnimInst)
            return INDEX_NONE;

        // 随机选一段，不与上一次重复（仅一段时只能重复播它）
        int32 NewIndex = ValidIndices[0];
        if (ValidIndices.Num() > 1)
        {
            do
            {
                NewIndex = ValidIndices[FMath::RandRange(0, ValidIndices.Num() - 1)];
            } while (NewIndex == LastAttackMontageIndex);
        }

        // 刷新当前段运行时配置（逐段速度/范围/可弹刀/接触判定，<=0 的项回落到全局值）
        const FMonsterAttackConfig& Cfg = AttackConfigs[NewIndex];
        PlayRateToUse = FMath::Max(0.05f, Cfg.PlayRate);
        if (Cfg.HitRadius > 0.0f)
        {
            CurrentHitRadius = Cfg.HitRadius;
        }
        if (Cfg.HitArcAngle >= 0.0f)
        {
            CurrentHitArcAngle = Cfg.HitArcAngle;
        }
        bCurrentAttackParryable = Cfg.bParryable;
        bCurrentUseContactHit = Cfg.bUseContactHit;
        bCurrentAttackIsHeavy = Cfg.bHeavyAttack;
        if (Cfg.ContactHitRadius > 0.0f)
        {
            CurrentContactHitRadius = Cfg.ContactHitRadius;
        }
        if (Cfg.ContactHitWindow > 0.0f)
        {
            CurrentContactHitWindow = Cfg.ContactHitWindow;
        }

        // ---- 受击判定窗口（蒙太奇时间轴秒）----
        // 逐段 HitWindowStartTime/EndTime 配置；未配置时回落：
        //   起始 = AttackDamageDelay（真实秒 × PlayRate 换算成蒙太奇秒）
        //   结束 = 起始 + ContactHitWindow（接触段，同样换算）/ 起始 + 0.1（范围段，近似单点结算）
        CurrentAttackMontage = Cfg.Montage;
        CurrentAttackPlayRate = PlayRateToUse;
        CurrentHitWindowStart = (Cfg.HitWindowStartTime >= 0.0f)
            ? Cfg.HitWindowStartTime
            : FMath::Max(0.0f, AttackDamageDelay) * PlayRateToUse;
        const float FallbackDurationSec = bCurrentUseContactHit
            ? FMath::Max(0.05f, CurrentContactHitWindow)
            : 0.1f;
        const float FallbackEnd = CurrentHitWindowStart + FallbackDurationSec * PlayRateToUse;
        CurrentHitWindowEnd = (Cfg.HitWindowEndTime >= 0.0f)
            ? FMath::Max(Cfg.HitWindowEndTime, CurrentHitWindowStart)
            : FallbackEnd;

        // ---- 弹刀判定窗口（蒙太奇时间轴秒，仅 bParryable=true 段生效）----
        // 回落：起始=0（蒙太奇开头），结束=CurrentHitWindowStart（受击判定窗口起始，
        //   即弹刀窗口默认覆盖前摇/蓄力阶段）
        CurrentParryWindowStart = (Cfg.ParryWindowStartTime >= 0.0f)
            ? Cfg.ParryWindowStartTime
            : 0.0f;
        CurrentParryWindowEnd = (Cfg.ParryWindowEndTime >= 0.0f)
            ? Cfg.ParryWindowEndTime
            : CurrentHitWindowStart;

        AnimInst->Montage_Play(Cfg.Montage, PlayRateToUse);
        UE_LOG(LogTemp, Warning, TEXT("Monster plays attack config #%d (rate=%.2f, radius=%.0f, arc=%.0f, parryable=%d, contact=%d, contactR=%.0f, hitWindow=%.2f~%.2f, parryWindow=%.2f~%.2f)."),
            NewIndex, PlayRateToUse, CurrentHitRadius, CurrentHitArcAngle, bCurrentAttackParryable ? 1 : 0,
            bCurrentUseContactHit ? 1 : 0, CurrentContactHitRadius, CurrentHitWindowStart, CurrentHitWindowEnd,
            CurrentParryWindowStart, CurrentParryWindowEnd);
        return NewIndex;
    }

    // ---- 回落：旧版 AttackMontages + 全局速度/范围 ----
    TArray<UAnimMontage*> ValidMontages;
    ValidMontages.Reserve(AttackMontages.Num());
    for (UAnimMontage* M : AttackMontages)
    {
        if (M)
        {
            ValidMontages.Add(M);
        }
    }

    if (ValidMontages.Num() == 0 || !AnimInst)
        return INDEX_NONE;

    // 随机选一段，不与上一次播放的重复（仅一段时只能重复播它）
    int32 NewIndex = 0;
    if (ValidMontages.Num() > 1)
    {
        do
        {
            NewIndex = FMath::RandRange(0, ValidMontages.Num() - 1);
        } while (NewIndex == LastAttackMontageIndex);
    }

    // 旧版路径的判定窗口：全局 AttackDamageDelay 起始 + ContactHitWindow 时长（换算蒙太奇秒）
    CurrentAttackMontage = ValidMontages[NewIndex];
    CurrentAttackPlayRate = FMath::Max(0.05f, PlayRateToUse);
    CurrentHitWindowStart = FMath::Max(0.0f, AttackDamageDelay) * CurrentAttackPlayRate;
    CurrentHitWindowEnd = CurrentHitWindowStart + FMath::Max(0.05f, CurrentContactHitWindow) * CurrentAttackPlayRate;
    // 旧版路径不可弹刀（bCurrentAttackParryable=false），弹刀窗口设默认值无实际意义
    CurrentParryWindowStart = 0.0f;
    CurrentParryWindowEnd = CurrentHitWindowStart;

    AnimInst->Montage_Play(ValidMontages[NewIndex], PlayRateToUse);

    UE_LOG(LogTemp, Warning, TEXT("Monster plays attack montage #%d (%d available, rate=%.2f)."),
        NewIndex, ValidMontages.Num(), PlayRateToUse);
    return NewIndex;
}

// ---- 播放指定索引的攻击段（逐段配置路径，背刺连击用）----
// 复刻 PlayRandomAttackMontage 中"刷新当前段运行时配置 + 播放 + 启动判定窗口"的完整逻辑，
// 但播放的是指定索引段（不随机），供毒刺命中后的背刺连击依次触发不可弹刀攻击段
void AMonsterBase::PlaySpecificAttackConfig(int32 ConfigIndex)
{
    if (!AttackConfigs.IsValidIndex(ConfigIndex) || !AttackConfigs[ConfigIndex].Montage)
        return;

    UAnimInstance* AnimInst = (GetMesh()) ? GetMesh()->GetAnimInstance() : nullptr;
    if (!AnimInst)
        return;

    const FMonsterAttackConfig& Cfg = AttackConfigs[ConfigIndex];
    const float PlayRateToUse = FMath::Max(0.05f, Cfg.PlayRate);

    // 刷新当前段运行时配置（与 PlayRandomAttackMontage 完全一致）
    bCurrentAttackParryable = Cfg.bParryable;
    bCurrentUseContactHit = Cfg.bUseContactHit;
    bCurrentAttackIsHeavy = Cfg.bHeavyAttack;
    CurrentHitRadius = (Cfg.HitRadius > 0.0f) ? Cfg.HitRadius : AttackHitRadius;
    CurrentHitArcAngle = (Cfg.HitArcAngle >= 0.0f) ? Cfg.HitArcAngle : AttackHitArcAngle;
    CurrentContactHitRadius = (Cfg.ContactHitRadius > 0.0f) ? Cfg.ContactHitRadius : ContactHitRadius;
    CurrentContactHitWindow = (Cfg.ContactHitWindow > 0.0f) ? Cfg.ContactHitWindow : ContactHitWindow;

    // 受击判定窗口
    CurrentAttackMontage = Cfg.Montage;
    CurrentAttackPlayRate = PlayRateToUse;
    CurrentHitWindowStart = (Cfg.HitWindowStartTime >= 0.0f)
        ? Cfg.HitWindowStartTime
        : FMath::Max(0.0f, AttackDamageDelay) * PlayRateToUse;
    const float FallbackDurationSec = bCurrentUseContactHit ? FMath::Max(0.05f, CurrentContactHitWindow) : 0.1f;
    const float FallbackEnd = CurrentHitWindowStart + FallbackDurationSec * PlayRateToUse;
    CurrentHitWindowEnd = (Cfg.HitWindowEndTime >= 0.0f)
        ? FMath::Max(Cfg.HitWindowEndTime, CurrentHitWindowStart)
        : FallbackEnd;

    // 弹刀窗口
    CurrentParryWindowStart = (Cfg.ParryWindowStartTime >= 0.0f) ? Cfg.ParryWindowStartTime : 0.0f;
    CurrentParryWindowEnd = (Cfg.ParryWindowEndTime >= 0.0f) ? Cfg.ParryWindowEndTime : CurrentHitWindowStart;

    // 前摇窗口标志 + 播放蒙太奇
    bIsTelegraphing = true;
    bAttackHitPlayer = false;
    LastAttackMontageIndex = ConfigIndex;
    AnimInst->Montage_Play(Cfg.Montage, PlayRateToUse);

    // 启动受击判定窗口（每段带完整判定）
    StartHitWindow();

    UE_LOG(LogTemp, Warning, TEXT("Monster plays SPECIFIC attack config #%d (backstab, rate=%.2f)."), ConfigIndex, PlayRateToUse);
}

// ---- 播放指定索引的旧版攻击蒙太奇（背刺连击用，旧版 AttackMontages 路径）----
void AMonsterBase::PlaySpecificAttackMontage(int32 MontageIndex)
{
    if (!AttackMontages.IsValidIndex(MontageIndex) || !AttackMontages[MontageIndex])
        return;

    UAnimInstance* AnimInst = (GetMesh()) ? GetMesh()->GetAnimInstance() : nullptr;
    if (!AnimInst)
        return;

    UAnimMontage* Montage = AttackMontages[MontageIndex];
    const float PlayRateToUse = FMath::Max(0.05f, AttackMontagePlayRate);

    // 旧版路径：不可弹刀、不使用接触判定、全局范围
    bCurrentAttackParryable = false;
    bCurrentUseContactHit = false;
    bCurrentAttackIsHeavy = bLegacyAttackIsHeavy;
    CurrentHitRadius = AttackHitRadius;
    CurrentHitArcAngle = AttackHitArcAngle;
    CurrentContactHitRadius = ContactHitRadius;
    CurrentContactHitWindow = ContactHitWindow;

    CurrentAttackMontage = Montage;
    CurrentAttackPlayRate = PlayRateToUse;
    CurrentHitWindowStart = FMath::Max(0.0f, AttackDamageDelay) * PlayRateToUse;
    CurrentHitWindowEnd = CurrentHitWindowStart + FMath::Max(0.05f, CurrentContactHitWindow) * PlayRateToUse;
    CurrentParryWindowStart = 0.0f;
    CurrentParryWindowEnd = CurrentHitWindowStart;

    bIsTelegraphing = true;
    bAttackHitPlayer = false;
    LastAttackMontageIndex = MontageIndex;
    AnimInst->Montage_Play(Montage, PlayRateToUse);

    StartHitWindow();

    UE_LOG(LogTemp, Warning, TEXT("Monster plays SPECIFIC attack montage #%d (backstab, rate=%.2f)."), MontageIndex, PlayRateToUse);
}

// ---- 移动蒙太奇管理：移动时循环播放，停下/攻击/死亡时停止 ----
void AMonsterBase::UpdateMoveMontage(bool bIsMoving)
{
    UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
    if (!MoveMontage || !AnimInst)
        return;

    // 当前实际水平速度：脚步速率同步的基准（脚速 = 实际移速 → 无滑步）
    const float CurrentSpeed = GetCharacterMovement()
        ? GetCharacterMovement()->Velocity.Size2D()
        : 0.0f;

    // 播放速率 = 实际移速 / 动画原生速度（限制在合理区间防极端失真）
    const float NativeSpeed = GetMoveMontageNativeSpeed();
    const float PlayRate = FMath::Clamp(CurrentSpeed / NativeSpeed, 0.4f, 4.0f);

    if (bIsMoving && !bMoveMontageActive)
    {
        // 开始移动：播放移动蒙太奇（资产内需设置循环段才会循环），速率与当前移速同步
        AnimInst->Montage_Play(MoveMontage, PlayRate);
        bMoveMontageActive = true;
    }
    else if (bIsMoving && bMoveMontageActive)
    {
        // 蒙太奇若未配置循环段，播完一遍即停止：检测到已停止则立即重播，
        // 保证追击/移动全程持续触发移动动画（从起点一直播到贴脸攻击才停）
        if (!AnimInst->Montage_IsPlaying(MoveMontage))
        {
            AnimInst->Montage_Play(MoveMontage, PlayRate);
        }
        else
        {
            // 移动中移速变化（起步加速/受击减速等）：持续同步播放速率
            AnimInst->Montage_SetPlayRate(MoveMontage, PlayRate);
        }
    }
    else if (!bIsMoving && bMoveMontageActive)
    {
        // 停止移动：只停移动蒙太奇（不误停攻击/受击等其它蒙太奇），短淡出减少停止滑步
        AnimInst->Montage_Stop(0.15f, MoveMontage);
        bMoveMontageActive = false;
    }
}

// ---- 解析移动蒙太奇原生前进速度 ----
float AMonsterBase::GetMoveMontageNativeSpeed()
{
    // 显式配置优先
    if (MoveMontageNativeSpeed > 0.0f)
        return MoveMontageNativeSpeed;

    // 已缓存则直接返回
    if (CachedMoveMontageNativeSpeed > 0.0f)
        return CachedMoveMontageNativeSpeed;

    // 自动推导：取第一个 Slot 轨道首段动画序列在全序列上的根运动水平位移速率。
    // 市面动画多为「原地位移」制作后带根运动数据，此推导可得其制作时的真实前进速度
    float Derived = 0.0f;
    if (MoveMontage && MoveMontage->SlotAnimTracks.Num() > 0)
    {
        const FSlotAnimationTrack& Track = MoveMontage->SlotAnimTracks[0];
        if (Track.AnimTrack.AnimSegments.Num() > 0)
        {
            const FAnimSegment& Seg = Track.AnimTrack.AnimSegments[0];
            const UAnimSequence* Seq = Cast<UAnimSequence>(Seg.GetAnimReference().Get());
            const float AnimLength = Seq ? Seq->GetPlayLength() : 0.0f;
            if (AnimLength > 0.05f)
            {
                // UE5.7 新 API：ExtractRootMotionFromRange + FAnimExtractContext（返回该区间根运动累计位移）
                const FTransform RootMotion = Seq->ExtractRootMotionFromRange(
                    0.0, AnimLength, FAnimExtractContext(0.0, true));
                Derived = RootMotion.GetTranslation().Size2D() / AnimLength;
            }
        }
    }

    // In-Place 动画（无根运动数据）推导失败 → 回落通用跑步基准 300cm/s，可蓝图覆盖
    CachedMoveMontageNativeSpeed = (Derived >= 1.0f) ? Derived : 300.0f;
    return CachedMoveMontageNativeSpeed;
}

// ---- Boss 脱战行为（待机/寻找/回位）----
// 仅 Boss + bEnableBossIdleBehavior 生效。脱战期间：
//   1) 未进战 → 循环待机蒙太奇（IdleMontage）
//   2) 进战后脱战（角色超出追击范围）→ 计时 OutOfCombatSearchDelay
//   3) 延迟满 → 寻找阶段（原地播 SearchMontage SearchMontageDuration 秒）
//   4) 寻找结束 → 回原点（MoveTowards SpawnLocation），到达后重置血量/状态/狂暴回第一阶段
void AMonsterBase::UpdateBossIdleBehavior(float DeltaTime)
{
    if (Rank != EMonsterRank::Boss || !bEnableBossIdleBehavior)
        return;

    // 无玩家：回到/保持待机，不触发寻找回位（避免无玩家时反复空转回原点）
    APawn* Player = GetPlayerPawn();
    if (!Player)
    {
        UpdateIdleMontage(true);
        OutOfCombatSearchTimer = 0.0f;
        return;
    }

    // 死亡不处理（避免死后还回原点/播动画）
    if (AIState == EMonsterAIState::Dead)
        return;

    // 狂暴回血/技能期间：不干扰（UpdateEnrage 接管，脱战行为暂停）
    if (bIsEnrageHealing || bIsEnrageSkillCasting)
    {
        UpdateIdleMontage(false);
        return;
    }

    // ---- 阶段 A：寻找中（原地播寻找蒙太奇，计时结束后回原点）----
    if (SearchRemaining > 0.0f)
    {
        SearchRemaining -= DeltaTime;
        StopHorizontalMovement();

        // 寻找蒙太奇播完/未配置 → 直接进入回位；否则循环重播（短动画填满时长）
        if (SearchMontage && GetMesh() && GetMesh()->GetAnimInstance())
        {
            UAnimInstance* AnimInst = GetMesh()->GetAnimInstance();
            if (!AnimInst->Montage_IsPlaying(SearchMontage))
            {
                AnimInst->Montage_Play(SearchMontage);
            }
        }

        if (SearchRemaining <= 0.0f)
        {
            FinishSearching();
        }
        return;
    }

    // ---- 阶段 B：回位中（走回出生点）----
    if (AIState == EMonsterAIState::Return)
    {
        // 回家途中角色重新进入触发战斗范围 → 重新进战（中断回位）
        if (GetPlayerDistance() <= BossCombatTriggerRadius)
        {
            EnterCombat();
            return;
        }

        const float Speed = ReturnHomeSpeed > 0.0f ? ReturnHomeSpeed : ChaseSpeed;
        if (MoveTowards(SpawnLocation, Speed, DeltaTime))
        {
            ResetToHome();
        }
        return;
    }

    // ---- 阶段 C：战斗中 ----
    // Attack 距离内等冷却（AIState==Attack）→ 播待机蒙太奇兜底，避免攻击/技能/弹刀蒙太奇
    // 播完后动画落到 A-pose 僵直站立；Chase 追击中 → 停待机（移动蒙太奇由 UpdateMoveMontage
    // / UpdateAnimFallback 接管）。注意不能统一 UpdateIdleMontage(false)：那会与
    // UpdateAnimFallback 的 Attack 兜底形成每帧"停-播"抖动（待机蒙太奇永远卡在 BlendIn 起始帧）。
    if (bInCombat)
    {
        // 攻击/弹刀/受击等瞬发动作蒙太奇仍在播 → 不播待机蒙太奇，避免抢占 DefaultSlot 打断它们。
        // - 攻击蒙太奇：CurrentAttackMontage 仍在 Montage_IsActive（含 BlendOut）
        // - 弹刀硬直：StaggerRemaining > 0（after_tandao 播放期间冻结 AI，此时 AIState 仍为 Attack）
        UAnimInstance* CombatAnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
        const bool bAttackMontageActive = CombatAnimInst && CurrentAttackMontage
            && CombatAnimInst->Montage_IsActive(CurrentAttackMontage);
        const bool bTransientActionActive = bAttackMontageActive || (StaggerRemaining > 0.0f);

        const bool bShouldIdle = (AIState == EMonsterAIState::Attack) && !bTransientActionActive;
        UpdateIdleMontage(bShouldIdle);
        OutOfCombatSearchTimer = 0.0f;
        return;
    }

    // ---- 阶段 D：脱战（未进战或已脱战）----
    // 播放待机蒙太奇（循环）
    UpdateIdleMontage(true);

    // 角色超出追击范围 → 累计脱战延迟；延迟满 → 进入寻找阶段
    if (GetPlayerDistance() > GetBossChaseRadius())
    {
        OutOfCombatSearchTimer += DeltaTime;
        if (OutOfCombatSearchTimer >= OutOfCombatSearchDelay)
        {
            StartSearching();
        }
    }
    else
    {
        OutOfCombatSearchTimer = 0.0f;
    }
}

// ---- 播放/停止待机蒙太奇（脱战循环）----
void AMonsterBase::UpdateIdleMontage(bool bShouldPlay)
{
    UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
    if (!IdleMontage || !AnimInst)
    {
        bIdleMontageActive = false;
        return;
    }

    if (bShouldPlay && !bIdleMontageActive)
    {
        AnimInst->Montage_Play(IdleMontage);
        bIdleMontageActive = true;
    }
    else if (bShouldPlay && bIdleMontageActive)
    {
        // 蒙太奇未配置循环段，播完即停 → 重播保证持续待机
        if (!AnimInst->Montage_IsPlaying(IdleMontage))
        {
            AnimInst->Montage_Play(IdleMontage);
        }
    }
    else if (!bShouldPlay && bIdleMontageActive)
    {
        AnimInst->Montage_Stop(0.2f, IdleMontage);
        bIdleMontageActive = false;
    }
}

// ---- 动画衔接空挡兜底：消除"上一个动作播完 → 下一个动作接上"之间的站直僵住 ----
// 触发条件（全部满足才算空挡期）：
//   1) 非特殊状态：非死亡、非停滞(弹刀)、非狂暴回血/技能、非背刺连击、非毒刺阶段
//      （这些状态有专属动画/冻结逻辑，不可被兜底覆盖）
//   2) 当前没有任何蒙太奇在播放（IsAnyMontagePlaying == false）→ 上一个动作确实播完了
//   3) 未在移动（水平速度≈0）→ 移动蒙太奇此时不会主动接管
// 处理：空挡期按 AI 意图刷新动画（而非当前瞬时速度）——
//   - AI 正在追击（Chase）→ 立即补播移动蒙太奇（Walk），即使速度还在平滑加速未超阈值，
//     避免"先闪回 Idle 站直、再切 Walk"的突兀帧
//   - AI 原地驻留（Attack 距离内等冷却）→ 保持自然待机，不落到 A-pose
// 这样受击/技能/弹刀等瞬发动作播完后，动画能立即衔接到正确的下一状态，衔接流畅。
void AMonsterBase::UpdateAnimFallback()
{
    // 特殊状态直接跳过：这些状态有自己的动画管理，兜底会与其冲突
    if (AIState == EMonsterAIState::Dead) return;
    if (StaggerRemaining > 0.0f) return;               // 弹刀硬直：由弹刀蒙太奇接管
    if (bIsEnrageHealing || bIsEnrageSkillCasting) return; // 狂暴：由 UpdateEnrage 接管
    if (bBackstabComboActive) return;                  // 背刺连击：由连击定时器接管
    if (bBossStingStage) return;                       // 毒刺阶段：由 UpdateBossSting 接管

    UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
    if (!AnimInst) return;

    // 还有动画在播（含 BlendIn/BlendOut 混合期）→ 不算空挡，正常跳过
    if (AnimInst->IsAnyMontagePlaying()) return;

    // 正在移动（速度已超过阈值）→ 移动蒙太奇已在播或即将由 UpdateMoveMontage 接管，无需兜底
    const float Speed2D = GetCharacterMovement() ? GetCharacterMovement()->Velocity.Size2D() : 0.0f;
    if (Speed2D > 10.0f) return;

    // 到达这里 = 真正的"衔接空挡期"：上一个动作播完、当前静止、无任何动画在播。
    // 关键：按 AI 意图而非瞬时速度兜底，避免"速度平滑加速期(0~10cm/s)动画落到 Idle 站直"。

    // 追击中（Chase）→ 立即补播移动蒙太奇（Walk），即使速度还在加速未达阈值。
    // 注意：bMovingNow 用 10cm/s 阈值判断，平滑加速头几帧会误判"未移动"，这里主动接管。
    // Attack 距离内等冷却的空挡期由 UpdateBossIdleBehavior 播待机蒙太奇兜底（自然待机姿态）；
    // 脱战驻留由 UpdateBossIdleBehavior 的"阶段 D"自然处理。
    if (AIState == EMonsterAIState::Chase && MoveMontage && !bMoveMontageActive)
    {
        const float NativeSpeed = GetMoveMontageNativeSpeed();
        const float PlayRate = FMath::Clamp(FMath::Max(Speed2D, 1.0f) / NativeSpeed, 0.4f, 4.0f);
        AnimInst->Montage_Play(MoveMontage, PlayRate);
        bMoveMontageActive = true;
    }
}

// ---- 启动寻找阶段：停待机、原地播寻找蒙太奇、计时 ----
void AMonsterBase::StartSearching()
{
    UpdateIdleMontage(false);
    bSearchMontageActive = true;
    SearchRemaining = FMath::Max(0.0f, SearchMontageDuration);
    OutOfCombatSearchTimer = 0.0f;

    // 停下（回位前先原地寻找）
    StopHorizontalMovement();
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->StopMovementImmediately();
    }

    // 立即播放寻找蒙太奇（若配置）
    if (SearchMontage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Play(SearchMontage);
    }

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' out of combat %.1fs, searching for player (%.1fs)."),
        *GetName(), OutOfCombatSearchDelay, SearchRemaining);
}

// ---- 结束寻找阶段：停寻找蒙太奇、转回位状态 ----
void AMonsterBase::FinishSearching()
{
    SearchRemaining = 0.0f;
    if (bSearchMontageActive)
    {
        if (SearchMontage && GetMesh() && GetMesh()->GetAnimInstance())
        {
            GetMesh()->GetAnimInstance()->Montage_Stop(0.2f, SearchMontage);
        }
        bSearchMontageActive = false;
    }

    // 未配置寻找蒙太奇或时长为 0：直接重置（原地，不回走）
    if (!SearchMontage || SearchMontageDuration <= 0.0f)
    {
        ResetToHome();
        return;
    }

    // 转回位状态：走回出生点
    AIState = EMonsterAIState::Return;
    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' finished searching, returning home."), *GetName());
}

// ---- 回原点完成：重置血量、状态、狂暴标记回第一阶段 ----
void AMonsterBase::ResetToHome()
{
    // 停所有脱战相关蒙太奇
    UpdateIdleMontage(false);
    if (bSearchMontageActive)
    {
        if (SearchMontage && GetMesh() && GetMesh()->GetAnimInstance())
        {
            GetMesh()->GetAnimInstance()->Montage_Stop(0.2f, SearchMontage);
        }
        bSearchMontageActive = false;
    }
    SearchRemaining = 0.0f;
    OutOfCombatSearchTimer = 0.0f;

    // 停止移动
    StopHorizontalMovement();

    // 恢复出生朝向（只保留 Yaw，压平 Pitch/Roll 残留）
    SetActorRotation(FRotator(0.0f, SpawnRotation.Yaw, 0.0f));

    // 重置血量到满
    ResetHealth();

    // 状态回待机（脱战）
    bInCombat = false;
    AIState = EMonsterAIState::Idle;

    // ---- 狂暴状态重置回第一阶段 ----
    bEnraged = false;
    bEnrageTriggered = false;
    bIsEnrageHealing = false;
    bIsEnrageSkillCasting = false;
    SetEnrageGlow(false);

    // 隐藏 Boss 血条
    DetachBossBar();

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' returned home! State + health + enrage reset to phase 1."),
        *GetName());
}

// ---- 血条百分比更新 ----
void AMonsterBase::UpdateHealthBar()
{
    const float HealthPercent = MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;

    // 小怪头顶血条：刷新名为 MonsterHealthProgressBar 的进度条
    if (HealthBarWidget && HealthBarWidget->GetWidget())
    {
        if (UProgressBar* ProgressBar = Cast<UProgressBar>(HealthBarWidget->GetWidget()->GetWidgetFromName(TEXT("MonsterHealthProgressBar"))))
        {
            ProgressBar->SetPercent(HealthPercent);
        }
    }

    // Boss 屏幕血条：刷新名为 BossHealthProgressBar 的进度条
    if (BossBarWidget && BossBarWidget->GetWidget())
    {
        if (UProgressBar* ProgressBar = Cast<UProgressBar>(BossBarWidget->GetWidget()->GetWidgetFromName(TEXT("BossHealthProgressBar"))))
        {
            ProgressBar->SetPercent(HealthPercent);
        }

        // 同步 Boss 等级到 WBP_BossHealthBar 的 LV_num 文本
        if (UTextBlock* LevelText = Cast<UTextBlock>(BossBarWidget->GetWidget()->GetWidgetFromName(TEXT("LV_num"))))
        {
            LevelText->SetText(FText::AsNumber(MonsterLevel));
        }
    }
}

// ---- Boss 血条：进入战斗时显示（不做组件挂载，每帧世界坐标锁定到摄像机前）----
void AMonsterBase::AttachBossBarToCamera()
{
    APawn* Player = GetPlayerPawn();
    CachedPlayerCamera = Player ? Player->FindComponentByClass<UCameraComponent>() : nullptr;
    if (!CachedPlayerCamera)
    {
        UE_LOG(LogTemp, Error, TEXT("Boss bar: player camera not found!"));
        return;
    }

    if (!BossBarWidgetClass)
    {
        UE_LOG(LogTemp, Error, TEXT("Boss bar: WBP_BossHealthBar not loaded, bar will render empty!"));
    }

    BossBarWidget->SetHiddenInGame(false);

    UE_LOG(LogTemp, Warning, TEXT("Boss bar shown, locked to camera '%s' (owner '%s'), screen top-center."),
        *CachedPlayerCamera->GetName(), *GetNameSafe(CachedPlayerCamera->GetOwner()));

    UpdateBossBarPosition();
}

// ---- Boss 血条：脱离战斗（隐藏，位置无所谓）----
void AMonsterBase::DetachBossBar()
{
    if (!BossBarWidget)
        return;

    BossBarWidget->SetHiddenInGame(true);
    BossBarWidget->SetRelativeLocation(FVector::ZeroVector);
    CachedPlayerCamera = nullptr;
}

// ---- Boss 血条定位：世界坐标锁定屏幕顶部居中（FOV 解析计算，不挂载、不反投影，防抖动）----
void AMonsterBase::UpdateBossBarPosition()
{
    if (!BossBarWidget)
        return;

    // 摄像机失效 → 重新查找。失效判定不仅看「对象是否销毁」，还要看「所有者是否仍是当前操控的 Pawn」：
    // 切人（尤其技能中切人，旧角色留场放完技能自毁）时 Possess 已换到新 Pawn，但旧 Pawn 未销毁、
    // 其 FollowCamera 仍 IsValid → 仅靠 IsValid 会漏判，血条继续锁在旧角色的相机上导致错位/飘走。
    // 故需校验缓存的相机 Owner 是否仍是 GetPlayerPawn()（当前被 Possess 的 Pawn）。
    if (!IsValid(CachedPlayerCamera) || CachedPlayerCamera->GetOwner() != GetPlayerPawn())
    {
        if (APawn* Player = GetPlayerPawn())
        {
            CachedPlayerCamera = Player->FindComponentByClass<UCameraComponent>();
        }
        if (!CachedPlayerCamera)
            return;
    }

    APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
    if (!PC)
        return;

    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // 目标屏幕位置 = 屏幕顶部居中（上边距 + 半个条高）
    const float TargetScreenY = BossBarTopMargin + BossBarDrawSize.Y * 0.5f;

    // 解析计算摄像机局部偏移（与角色血条同一公式，防抖动）
    const float Dist = 300.0f; // 放置距离（屏幕空间下不影响绘制大小，只决定投影位置）
    const float HalfFovRad = FMath::DegreesToRadians(CachedPlayerCamera->FieldOfView * 0.5f);

    // 距摄像机 Dist 处，1 个屏幕像素对应的摄像机局部空间单位数（UE 的 FieldOfView 是水平 FOV）
    const float UnitsPerPixel = 2.0f * Dist * FMath::Tan(HalfFovRad) / static_cast<float>(ViewportX);

    // 目标点相对屏幕中心的像素偏移（屏幕 Y 向下为正）
    const float OffsetY = TargetScreenY - ViewportY * 0.5f;

    // 摄像机局部空间：X 前、Y 右、Z 上；屏幕 Y 向下为正 → 局部 Z 取负。水平偏移正值右移
    const FVector LocalOffset(Dist, BossBarHorizontalOffset * UnitsPerPixel, -OffsetY * UnitsPerPixel);

    // 局部偏移 → 世界坐标，直接设置血条世界位置。
    // 不做任何组件挂载（运行时跨 Actor 挂载在本项目实测会静默失败，导致血条留在怪物身上）：
    // 屏幕空间绘制只看世界位置，无论组件挂在谁身上，血条都锁定屏幕顶部居中。
    // 本怪物 Tick 在帧末（TG_PostUpdateWork）执行，用本帧相机变换 → 零延迟、转动视角不抖。
    const FVector WorldPos = CachedPlayerCamera->GetComponentLocation()
        + CachedPlayerCamera->GetComponentRotation().RotateVector(LocalOffset);
    BossBarWidget->SetWorldLocation(WorldPos);
}

// ---- 视口尺寸变化检测：resize 后强制刷新屏幕空间 UI 控件 ----
// UE 已知问题：Screen Space WidgetComponent 在视口尺寸变化（窗口拉伸/分辨率切换）后
// 其 Slate 窗口失效导致控件直接消失；销毁并重建控件即可恢复（位置由每帧定位逻辑自适应）
void AMonsterBase::CheckViewportResize()
{
    APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
    if (!PC)
        return;

    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // 首帧只记录基准尺寸，不触发刷新
    if (LastViewportSizeX == 0 && LastViewportSizeY == 0)
    {
        LastViewportSizeX = ViewportX;
        LastViewportSizeY = ViewportY;
        return;
    }

    if (ViewportX != LastViewportSizeX || ViewportY != LastViewportSizeY)
    {
        LastViewportSizeX = ViewportX;
        LastViewportSizeY = ViewportY;
        UE_LOG(LogTemp, Warning, TEXT("Monster '%s': viewport resized to %dx%d, refreshing screen-space UI."),
            *GetName(), ViewportX, ViewportY);

        // 刷新怪物全部屏幕空间控件：Boss 屏幕血条 + 头顶血条
        ForceRefreshScreenWidget(BossBarWidget);
        ForceRefreshScreenWidget(HealthBarWidget);
    }
}

// ---- 强制重建屏幕空间 WidgetComponent 的 Slate 控件（保留隐藏状态）----
void AMonsterBase::ForceRefreshScreenWidget(UWidgetComponent* WidgetComp)
{
    if (!WidgetComp)
        return;

    const bool bWasHidden = WidgetComp->bHiddenInGame;
    UUserWidget* Widget = WidgetComp->GetWidget();

    // 销毁失效的 Slate 控件 → 按当前视口重建（resize 后 Slate 窗口失效的标准修复手段）
    WidgetComp->SetWidget(nullptr);
    WidgetComp->SetWidget(Widget);

    // 重建不改变可见性，防御性写回
    WidgetComp->SetHiddenInGame(bWasHidden);
}

// ---- 受击 ----
// ---- 伤害飘字：弹出一条数字（暴击/非暴击分别用不同控件类）----
// 与头顶血条同机制：屏幕空间 WidgetComponent 挂在怪物身上。每次命中弹全新实例 →
// 新一段飘字不会覆盖上一段；水平随机散布避免完全重叠。
void AMonsterBase::ShowDamageNumber(float DamageAmount, bool bCrit)
{
    // 无效伤害（0/负数：无敌格挡、死亡早退等）不弹飘字
    if (DamageAmount <= 0.0f)
    {
        return;
    }

    // 选控件类：暴击优先用暴击类（未配置回落普通类），普通命中用普通类。
    // 两类都没配置 → 告警一次并放弃（不 fail-closed 打断伤害流程）
    UClass* WidgetClass = bCrit
        ? (DamageNumberCritWidgetClass ? DamageNumberCritWidgetClass.Get() : DamageNumberNormalWidgetClass.Get())
        : DamageNumberNormalWidgetClass.Get();
    if (!WidgetClass)
    {
        if (!bDamageNumberClassWarned)
        {
            bDamageNumberClassWarned = true;
            UE_LOG(LogTemp, Warning, TEXT("[DamageNumber] ★影响：怪物 %s 受击不显示伤害飘字（普通/暴击都不显示）。修法：在 /Content/UI 创建 WBP_DamageNumber_Normal 与 WBP_DamageNumber_Crit 后重启编辑器，或在怪物蓝图 UI|DamageNumber 分类手动指定控件类。"), *GetName());
        }
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // 创建控件实例。HitTestInvisible：飘字是纯装饰层，绝不拦截点击（铁律 7）
    UUserWidget* NewWidget = CreateWidget<UUserWidget>(World, WidgetClass);
    if (!NewWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("[DamageNumber] ★影响：本次伤害不显示飘字。控件实例创建失败（类=%s），检查 WBP 是否可实例化（根节点必须是画布容器）。"), *WidgetClass->GetName());
        return;
    }
    NewWidget->SetVisibility(ESlateVisibility::HitTestInvisible);

    // 填数字：约定 TextBlock 名 txt_damage_num（内部带前缀兜底，复制控件不会 miss）
    if (UTextBlock* DamageText = FindDamageNumberTextBlock(NewWidget))
    {
        DamageText->SetText(FText::AsNumber(FMath::RoundToInt32(DamageAmount)));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[DamageNumber] ★影响：飘字 %s 里找不到数字文本，只显示底图不出数字。修法：飘字控件里放一个名为 txt_damage_num 的 TextBlock。"), *WidgetClass->GetName());
    }

    // 每条飘字 = 一个独立的屏幕空间 WidgetComponent（弹出时设置一次尺寸/锚点，不每帧改）
    UWidgetComponent* NewComp = NewObject<UWidgetComponent>(this);
    NewComp->SetFlags(RF_Transient);
    NewComp->SetWidgetSpace(EWidgetSpace::Screen);
    NewComp->SetDrawSize(DamageNumberDrawSize);
    NewComp->SetWidget(NewWidget);
    NewComp->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

    // 位置：胶囊顶部 + 抬高偏移（比出伤预警更高），水平随机散布让连续飘字彼此错开
    FVector2D Jitter = FVector2D::ZeroVector;
    if (DamageNumberJitter > 0.0f)
    {
        Jitter = FMath::RandPointInCircle(DamageNumberJitter);
    }
    const float CapsuleHalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 90.0f;
    NewComp->SetRelativeLocation(FVector(Jitter.X, Jitter.Y, CapsuleHalfHeight + DamageNumberHeadOffset));
    NewComp->SetPivot(FVector2D(0.5f, 0.5f)); // 居中锚点，屏幕空间缩放/旋转不影响显示
    NewComp->RegisterComponent();

    // 登记寿命条目：Tick 里上飘 + 末段淡出 + 到时销毁
    FDamageNumberEntry Entry;
    Entry.WidgetComp = NewComp;
    Entry.WidgetInstance = NewWidget;
    Entry.Age = 0.0f;
    Entry.Lifetime = FMath::Max(DamageNumberLifetime, 0.1f);
    Entry.RiseSpeed = DamageNumberRiseSpeed;
    ActiveDamageNumbers.Add(Entry);
}

// ---- 在飘字控件里找数字 TextBlock ----
UTextBlock* AMonsterBase::FindDamageNumberTextBlock(UUserWidget* Widget) const
{
    if (!Widget || !Widget->WidgetTree)
    {
        return nullptr;
    }

    // 约定名优先：txt_damage_num
    if (UWidget* Found = Widget->WidgetTree->FindWidget(FName(TEXT("txt_damage_num"))))
    {
        if (UTextBlock* Text = Cast<UTextBlock>(Found))
        {
            return Text;
        }
    }

    // 兜底：前缀匹配第一命中（UMG 复制控件会自动加 _1/_2 序号后缀，精确名会 miss）
    UTextBlock* PrefixHit = nullptr;
    Widget->WidgetTree->ForEachWidget([&PrefixHit](UWidget* Child)
    {
        if (!PrefixHit && Child && Child->GetFName().ToString().StartsWith(TEXT("txt_damage_num")))
        {
            PrefixHit = Cast<UTextBlock>(Child);
        }
    });
    return PrefixHit;
}

// ---- 伤害飘字寿命推进：上飘 + 末段淡出 + 到时销毁 ----
void AMonsterBase::UpdateDamageNumbers(float DeltaTime)
{
    if (ActiveDamageNumbers.Num() == 0)
    {
        return;
    }

    const float FadeWindow = 0.25f; // 末段淡出时长（秒）

    for (int32 i = ActiveDamageNumbers.Num() - 1; i >= 0; --i)
    {
        FDamageNumberEntry& Entry = ActiveDamageNumbers[i];
        UWidgetComponent* Comp = Entry.WidgetComp.Get();
        UUserWidget* Widget = Entry.WidgetInstance.Get();

        // 组件已丢失（异常销毁）→ 直接清条目，不留死引用
        if (!Comp)
        {
            ActiveDamageNumbers.RemoveAt(i);
            continue;
        }

        Entry.Age += DeltaTime;

        // 上飘（相对位置每帧抬高，怪物移动时飘字跟着走）
        if (Entry.RiseSpeed > 0.0f)
        {
            Comp->AddRelativeLocation(FVector(0.0f, 0.0f, Entry.RiseSpeed * DeltaTime));
        }

        // 末段线性淡出
        if (Entry.Age >= Entry.Lifetime - FadeWindow && Widget)
        {
            const float Remain = FMath::Max(Entry.Lifetime - Entry.Age, 0.0f);
            Widget->SetRenderOpacity(FMath::Clamp(Remain / FadeWindow, 0.0f, 1.0f));
        }

        // 到时销毁：控件移出视口 + 组件销毁 + 清条目
        if (Entry.Age >= Entry.Lifetime)
        {
            if (Widget)
            {
                Widget->RemoveFromParent();
            }
            Comp->DestroyComponent();
            ActiveDamageNumbers.RemoveAt(i);
        }
    }
}

float AMonsterBase::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    // 消费角色侧预设的暴击标志（立即复位：无论本次是否有效命中，都不残留到下一次命中）
    const bool bWasCrit = bPendingDamageNumberCrit;
    bPendingDamageNumberCrit = false;

    if (AIState == EMonsterAIState::Dead)
        return 0.0f;

    // ---- Boss 狂暴：回血阶段无敌，不受到任何伤害 ----
    if (bIsEnrageHealing)
    {
        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' is INVULNERABLE while enrage-healing!"), *GetName());
        return 0.0f;
    }

    // ---- Boss 狂暴：技能施放期间可被攻击，但按当前段减伤比例结算 ----
    float DamageToApply = DamageAmount;
    if (bIsEnrageSkillCasting && EnrageSkillSegments.IsValidIndex(CurrentEnrageSegmentIndex))
    {
        const float Reduction = FMath::Clamp(EnrageSkillSegments[CurrentEnrageSegmentIndex].DamageReduction, 0.0f, 1.0f);
        DamageToApply = DamageAmount * (1.0f - Reduction);
        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' enrage skill damage reduction %.0f%%: %.1f -> %.1f."),
            *GetName(), Reduction * 100.0f, DamageAmount, DamageToApply);
    }

    const float ActualDamage = Super::TakeDamage(DamageToApply, DamageEvent, EventInstigator, DamageCauser);

    CurrentHealth = FMath::Clamp(CurrentHealth - ActualDamage, 0.0f, MaxHealth);
    UpdateHealthBar();

    // ---- 伤害飘字：按「实际扣血量」弹出（狂暴减伤等扣除后的真实数字），暴击/非暴击用不同控件类 ----
    if (ActualDamage > 0.0f)
    {
        ShowDamageNumber(ActualDamage, bWasCrit);
    }

    // 受击反击：未在战斗则立即进入战斗（即使玩家在发现范围外）
    if (!bInCombat)
    {
        EnterCombat();
    }
    else
    {
        OutOfCombatTimer = 0.0f;
    }

    UE_LOG(LogTemp, Warning, TEXT("Monster takes %.1f damage! Health: %.1f / %.1f"), ActualDamage, CurrentHealth, MaxHealth);

    if (CurrentHealth <= 0.0f)
    {
        Die();
    }
    else
    {
        // Boss 血量首次跌破阈值 → 触发狂暴回血阶段（只触发一次；随后 PlayHitReaction 因无敌被跳过）
        if (Rank == EMonsterRank::Boss)
        {
            TryTriggerEnrage();
        }

        // 存活：播放受击反应蒙太奇（按配置决定是否打断攻击；狂暴阶段内部自动跳过）
        PlayHitReaction();
    }

    return ActualDamage;
}

// ---- 死亡：播放死亡蒙太奇（可选），保留尸体到动画播完再销毁 ----
void AMonsterBase::Die()
{
    AIState = EMonsterAIState::Dead;
    CurrentHealth = 0.0f;

    // ---- 掉落物：击败即自动进背包 ----
    // ★ 放在最前：后面全是死亡演出（停蒙太奇/销毁计时/冻结姿势），
    //   掉落先结算，保证无论演出分支怎么走，物品都已经到手
    GrantDropsToPlayer();

    // ---- Boss 脱战行为收尾：停待机/寻找蒙太奇 ----
    UpdateIdleMontage(false);
    if (bSearchMontageActive)
    {
        if (SearchMontage && GetMesh() && GetMesh()->GetAnimInstance())
        {
            GetMesh()->GetAnimInstance()->Montage_Stop(0.2f, SearchMontage);
        }
        bSearchMontageActive = false;
    }
    SearchRemaining = 0.0f;

    // ---- Boss 狂暴状态收尾：停回血/技能流程与红光 ----
    bIsEnrageHealing = false;
    bIsEnrageSkillCasting = false;
    CurrentEnrageMontage = nullptr;
    CurrentEnrageSegmentIndex = -1;
    EnrageMontageSilentTime = 0.0f;
    // 声波攻击运行期状态复位（死亡即停波）
    bEnrageSoundWaveStage = false;
    bEnrageWaveInFlight = false;
    bEnrageWaveHitResolved = false;
    EnrageWaveAge = 0.0f;
    EnrageWaveCooldownRemaining = 0.0f;
    bSoundWavePerfectWindow = false;
    EnrageRotationGuardRemaining = 0.0f; // 死亡后停用旋转守卫，不干扰死亡蒙太奇姿势
    // 毒刺远程攻击运行期状态复位（死亡即停毒刺）
    bBossStingStage = false;
    bBossStingFlying = false;
    bBackstabComboActive = false;
    BackstabComboIndices.Reset();
    BackstabComboCursor = 0;
    if (CurrentSting.IsValid())
    {
        CurrentSting->Destroy();
    }
    CurrentSting = nullptr;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(EnrageRootRotationResetTimer);
    }
    SetEnrageGlow(false);

    // 停止未结算的攻击伤害与前摇窗口（含弹刀提示收起）
    GetWorldTimerManager().ClearTimer(HitWindowTimerHandle);
    bHitWindowRunning = false;
    bHitWindowOpen = false;
    bParryWindowOpen = false;
    bPlayerWasInContact = false;
    StaggerRemaining = 0.0f;
    EndTelegraphWindow();
    bMoveMontageActive = false;

    // 解除完美闪避停滞：恢复时间流速（否则死亡蒙太奇/销毁计时会被冻结）
    RestoreFromPerfectDodgeFreeze();

    // 解除大招停滞：恢复时间流速（怪物在大招停滞期间死亡时，同样要恢复才正常播死亡动画/销毁）
    RestoreFromUltimateFreeze();

    // 停止移动与碰撞（保留尸体模型）
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->StopMovementImmediately();
    }
    if (GetCapsuleComponent())
    {
        GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    // 隐藏血条
    if (HealthBarWidget)
    {
        HealthBarWidget->SetHiddenInGame(true);
    }
    if (Rank == EMonsterRank::Boss)
    {
        DetachBossBar();
    }

    // ---- 死亡蒙太奇 ----
    float DestroyDelay = DeathDestroyDelay;

    if (DeathMontage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        UAnimInstance* AnimInst = GetMesh()->GetAnimInstance();

        // 先停止当前所有蒙太奇（移动/攻击），再播放死亡蒙太奇
        AnimInst->Montage_Stop(0.0f);
        const float PlayLength = AnimInst->Montage_Play(DeathMontage, DeathMontagePlayRate);

        // 蒙太奇内容播完的瞬间直接销毁模型：
        // 不再取 max(DeathDestroyDelay, 时长+1s)——原先多出的缓冲时间正是怪物
        // 混合回 Idle“站起来再消失”的窗口。现在动画一结束模型立刻消失
        const float MontageDuration = DeathMontage->GetPlayLength() / FMath::Max(0.01f, DeathMontagePlayRate);
        DestroyDelay = FMath::Max(0.1f, MontageDuration);

        // 保险：内容结束前冻结骨骼动画，防止销毁前的最后几帧混合回 Idle
        if (PlayLength > 0.0f)
        {
            const float FreezeDelay = FMath::Max(0.0f, PlayLength - 0.05f);
            GetWorldTimerManager().SetTimer(
                DeathFreezeTimerHandle,
                this, &AMonsterBase::FreezeDeathPose,
                FreezeDelay, false);
        }

        UE_LOG(LogTemp, Warning, TEXT("Monster plays death montage! Duration=%.2fs, model destroyed right when montage ends."), MontageDuration);
    }
    else if (GetMesh() && GetMesh()->GetAnimInstance())
    {
        // 未配置死亡蒙太奇：停止所有动画（旧行为）
        GetMesh()->GetAnimInstance()->Montage_Stop(0.0f);
    }

    SetLifeSpan(DestroyDelay);
    UE_LOG(LogTemp, Warning, TEXT("Monster died! Body will be destroyed in %.1fs."), DestroyDelay);
}

// ---- 冻结死亡姿势：暂停骨骼动画更新，尸体停在倒地最后一帧 ----
void AMonsterBase::FreezeDeathPose()
{
    if (AIState != EMonsterAIState::Dead || !GetMesh())
        return;

    // bPauseAnims 暂停整个骨骼动画更新（含动画蓝图状态机与蒙太奇混合），
    // 网格体保持当前姿势（死亡动画最后一帧）直到 Actor 被销毁
    GetMesh()->bPauseAnims = true;

    UE_LOG(LogTemp, Warning, TEXT("Monster death pose frozen (anim paused)."));
}

// ==================== 掉落物（击败即自动进背包，无需拾取） ====================

// 编辑器里 Item Row Name 那一栏的下拉选项来源。
//
// 为什么直接 LoadObject 而不是问背包组件要：编辑器里选中怪物时它可能还没 BeginPlay，
// 拿不到 World、更拿不到玩家背包，只有资产加载这条路是任何时机都能用的。
TArray<FName> AMonsterBase::GetDropRowOptions() const
{
    TArray<FName> Options;

    const UDataTable* Table = LoadObject<UDataTable>(nullptr, *DropTablePath);
    if (!Table)
    {
        return Options;
    }

    Options = Table->GetRowNames();
    return Options;
}

// 本次生效的掉落清单：共享表优先（多只怪共用一套配置），否则用怪物自身的清单
const TArray<FMonsterDropEntry>& AMonsterBase::GetActiveDropList() const
{
    if (SharedDropTable && SharedDropTable->Drops.Num() > 0)
    {
        return SharedDropTable->Drops;
    }
    return DropItems;
}

void AMonsterBase::GrantDropsToPlayer()
{
    if (!bEnableDrops || bDropsGranted)
    {
        return;
    }

    const TArray<FMonsterDropEntry>& Drops = GetActiveDropList();

    // 没配掉落是合法状态（比如纯装饰怪），静默通过
    if (Drops.Num() == 0)
    {
        bDropsGranted = true;
        return;
    }

    // ★ 先置位再结算：即使下面中途失败（比如拿不到背包），也不会造成重复掉落
    bDropsGranted = true;

    // ---- 找玩家背包 ----
    ABattleCharacter* Player = Cast<ABattleCharacter>(GetPlayerPawn());
    UInventoryComponent* Inventory = Player ? Player->GetInventory() : nullptr;

    if (!Inventory)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop] %s 掉落了物品，但拿不到玩家背包，本次掉落丢失。原因：%s。"
                 "修法：确认玩家 Pawn 是 BP_PlayerCharacter（其上有 InventoryComponent）。"),
            *GetName(),
            Player ? TEXT("角色上没有 InventoryComponent") : TEXT("场景里找不到玩家 Pawn"));
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[Drop] %s 击败结算掉落，共 %d 条配置。"), *GetName(), Drops.Num());

    for (const FMonsterDropEntry& Drop : Drops)
    {
        if (Drop.ItemRowName.IsNone())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] %s 的掉落清单里有一条没填 Item Row Name，已跳过。"
                     "修法：怪物蓝图 Details → Monster|Drop → Drop Items，"
                     "在 Item Row Name 的下拉框里选一个掉落物。"),
                *GetName());
            continue;
        }

        // 概率判定：DropChance >= 1 时必定掉落（省掉一次随机）
        if (Drop.DropChance < 1.0f && FMath::FRand() > Drop.DropChance)
        {
            UE_LOG(LogTemp, Log, TEXT("[Drop]   行=%s 未通过概率判定（%.0f%%），本次不掉。"),
                *Drop.ItemRowName.ToString(), Drop.DropChance * 100.0f);
            continue;
        }

        // 数量取值：
        //   默认从数据表那一行读 tool_num —— 数量只维护一处，改表就全局生效；
        //   勾了 Override Count 才用怪物上配的范围随机（可做「2~4 个」这种浮动掉落）。
        int32 Count = 0;
        if (Drop.bOverrideCount)
        {
            // 先把上下限排好序再随机 —— 即使把 Min/Max 填反了也不会取到 0 或负数
            const int32 Lo = FMath::Max(1, FMath::Min(Drop.MinCount, Drop.MaxCount));
            const int32 Hi = FMath::Max(Lo, FMath::Max(Drop.MinCount, Drop.MaxCount));
            Count = (Lo == Hi) ? Lo : FMath::RandRange(Lo, Hi);
        }
        else
        {
            FBagItemEntry TableDef;
            // ★ 必须用 GetDropItemDefinition（只查 Data_diaoluo_boss 那一张表）。
            //   用 GetItemDefinition 的话查的是「两表按行名合并」的字典 ——
            //   两张表行名一撞车（都是 1、2、3…），读到的就是 Data_tool 那一行的 tool_num。
            if (Inventory->GetDropItemDefinition(Drop.ItemRowName, TableDef) && TableDef.ToolNum > 0)
            {
                Count = TableDef.ToolNum;
            }
            else
            {
                Count = 1;
                UE_LOG(LogTemp, Warning,
                    TEXT("[Drop]   行=%s 在数据表里的 tool_num 是 0 或读不到，本次按 1 个掉落。"
                         "修法：把 Data_diaoluo_boss 那一行的 tool_num 填成想要的数量，"
                         "或在掉落配置里勾上 Override Count 自己定数量。"),
                    *Drop.ItemRowName.ToString());
            }
        }

        FBagItemEntry ObtainedItem;
        // ★ 用 AddDropItemByRow（掉落表定义优先），不要用 AddItemByRow：
        //   后者是「Data_tool 优先」，行名撞车时会让提示显示 Data_tool 那件物品的名称和图标。
        const int32 Added = Inventory->AddDropItemByRow(Drop.ItemRowName, Count, ObtainedItem);

        if (Added <= 0)
        {
            // 这里不能再报一次「行名不存在」—— AddItemInternal 内部已经打了带修法的说明。
            // 但必须点明「所以这次屏幕上不会弹提示」：
            //   自从支持「不可堆叠物品一格一件 / 背包满拒收」之后，0 个入包成了**正常可能发生**的结果，
            //   而它的表现是「打死怪什么都没有」—— 不写清楚就会被当成提示 UI 坏了（现象完全一样）。
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] 行=%s 本次 0 个入包 → **屏幕上不会弹获得提示**（这是结果，不是提示 UI 的问题）。\n"
                     "      原因见上一条 [Bag] / [Drop] 日志，通常是这两种：\n"
                     "      ① [Bag] ★ 背包已满：格子被占满，本次被拒收（加大 Slot Count 或把该物品设为可堆叠）\n"
                     "      ② 行名在两张表里都不存在（怪物 Inspector 的 Item Row Name 用下拉框重选）"),
                *Drop.ItemRowName.ToString());
            continue;
        }

        if (Added < Count)
        {
            // 部分入包（背包快满时会发生）—— 提示上显示的是「实际拿到几个」，
            // 不说明的话会变成「我配了 3 个，怎么只提示 1 个」这类误报。
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] 行=%s 配置掉落 %d 个，实际只入包 %d 个 —— 提示会显示「×%d」。"
                     "差 %d 个是因为格子不够（见 [Bag] ★ 背包已满 那条日志）。"),
                *Drop.ItemRowName.ToString(), Count, Added, Added, Count - Added);
        }

        // 推送给 UI：WBP_HealthBar 实现 IItemPickupListener 后自动收到，无需蓝图连线
        Player->NotifyItemObtained(ObtainedItem, Added);
    }
}

// ---- Boss 狂暴系统：血量首次跌破阈值 → 无敌回血 → 多段狂暴技能 → 增伤+红光 ----

// 血量跌破阈值检测（TakeDamage 扣血后调用）
void AMonsterBase::TryTriggerEnrage()
{
    if (!bEnableEnrage || bEnrageTriggered)
        return;
    if (Rank != EMonsterRank::Boss || AIState == EMonsterAIState::Dead || CurrentHealth <= 0.0f)
        return;
    if (MaxHealth <= 0.0f || CurrentHealth / MaxHealth >= EnrageHealthPercent)
        return;

    TriggerEnrageHeal();
}

// 触发回血阶段：打断自身动作、停移动、播回血蒙太奇、进入无敌
void AMonsterBase::TriggerEnrageHeal()
{
    bEnrageTriggered = true;
    bIsEnrageHealing = true;
    EnrageHealTimeRemaining = FMath::Max(0.1f, EnrageHealDuration);
    EnrageHealStartHealth = CurrentHealth;

    // 立刻打断自身动作：取消未结算攻击（判定窗口/前摇/弹刀提示）、停攻击蒙太奇、停移动
    GetWorldTimerManager().ClearTimer(HitWindowTimerHandle);
    bHitWindowRunning = false;
    bHitWindowOpen = false;
    bParryWindowOpen = false;
    bPlayerWasInContact = false;
    StaggerRemaining = 0.0f;
    EndTelegraphWindow();

    // 收起出伤预警提示（死亡）
    if (bDamageWarningActive)
    {
        bDamageWarningActive = false;
        OnDamageWarningEnd();
    }
    HideDamageWarning();

    if (GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Stop(0.1f);
    }
    UpdateMoveMontage(false);
    StopHorizontalMovement();
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->StopMovementImmediately();
    }

    // 播放回血蒙太奇（短于阶段时长时由 UpdateEnrage 循环重播）
    if (EnrageHealMontage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Play(EnrageHealMontage, FMath::Max(0.05f, EnrageHealPlayRate));
    }

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' ENRAGE TRIGGERED! Health %.0f/%.0f below %.0f%%. Invulnerable healing for %.1fs."),
        *GetName(), CurrentHealth, MaxHealth, EnrageHealthPercent * 100.0f, EnrageHealTimeRemaining);
}

// 结束回血阶段：回满血、停蒙太奇、立刻朝玩家释放狂暴技能
void AMonsterBase::EndEnrageHeal()
{
    bIsEnrageHealing = false;
    EnrageHealTimeRemaining = 0.0f;

    // 停回血蒙太奇（快速混出）
    if (EnrageHealMontage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Stop(0.15f, EnrageHealMontage);
    }

    // 恢复至满血
    CurrentHealth = MaxHealth;
    UpdateHealthBar();

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' heal complete, health restored to %.0f. Enrage skill incoming!"), *GetName(), MaxHealth);

    // 立刻朝玩家释放狂暴技能
    StartEnrageSkill();
}

// 释放狂暴技能：朝向玩家并播放第一个有效段
void AMonsterBase::StartEnrageSkill()
{
    // 段根运动补丁（修复段间位置回跳，与角色技能/大招同方案）
    EnsureEnrageRootMotion();

    // 找第一个配置了蒙太奇的段（空段跳过）
    int32 FirstIndex = 0;
    while (EnrageSkillSegments.IsValidIndex(FirstIndex) && !EnrageSkillSegments[FirstIndex].Montage)
        ++FirstIndex;

    if (!EnrageSkillSegments.IsValidIndex(FirstIndex))
    {
        // 未配置任何段 → 直接进入狂暴状态（增伤 + 红光）
        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' has NO enrage skill segments configured, skipping to enraged state."), *GetName());
        EndEnrageSkill();
        return;
    }

    bIsEnrageSkillCasting = true;

    // 【落地矫正守卫复位】进入狂暴技能时清掉可能残留的旋转守卫值，确保第一段（空中/位移演出段）
    // 的根运动旋转表现完整保留，不被上一轮技能结束残留的守卫误压平。守卫只在段结束落地时（
    // OnEnrageSegmentMontageEnded）重新激活，覆盖段间 BlendOut 混合期。
    EnrageRotationGuardRemaining = 0.0f;

    // 立刻朝向玩家（朝玩家方向释放技能）
    if (APawn* Player = GetPlayerPawn())
    {
        FVector Dir = Player->GetActorLocation() - GetActorLocation();
        Dir.Z = 0.0f;
        if (Dir.SizeSquared() > 1.0f)
        {
            SetActorRotation(FRotator(0.0f, Dir.Rotation().Yaw, 0.0f));
        }
    }

    PlayEnrageSegment(FirstIndex);
}

// 播放指定狂暴技能段（先 Play 再绑结束回调，绑定才生效）
void AMonsterBase::PlayEnrageSegment(int32 Index)
{
    if (!EnrageSkillSegments.IsValidIndex(Index) || !EnrageSkillSegments[Index].Montage || !GetMesh())
    {
        EndEnrageSkill();
        return;
    }

    UAnimInstance* AnimInst = GetMesh()->GetAnimInstance();
    if (!AnimInst)
    {
        EndEnrageSkill();
        return;
    }

    const FBossEnrageSegment& Segment = EnrageSkillSegments[Index];
    CurrentEnrageSegmentIndex = Index;
    CurrentEnrageMontage = Segment.Montage;
    EnrageMontageSilentTime = 0.0f;
    EnrageLastHitTime = 0.0f;

    // 【修复段间倾斜】段衔接点（上一段结束、本段开始前）精准矫正一次倾斜：
    // 段蒙太奇的根运动旋转（动画根骨骼旋转副作用）在 BlendOut 混合输出期会以递减权重
    // 持续写到 Actor 的 Pitch/Roll，段与段衔接处此前无矫正 → 第一段结束后的残留倾斜被
    // 第二段继承并逐段叠加。这里只压平 Actor 的 Pitch/Roll（Yaw 保留，朝向不变），
    // 不碰位置与 Mesh 相对变换，因此根运动位移/朝向表现与相接流畅度完全不受影响。
    {
        const float Yaw = GetActorRotation().Yaw;
        SetActorRotation(FRotator(0.0f, Yaw, 0.0f));
    }

    AnimInst->Montage_Play(Segment.Montage, Segment.PlayRate);
    FOnMontageEnded EndDelegate;
    EndDelegate.BindUObject(this, &AMonsterBase::OnEnrageSegmentMontageEnded);
    AnimInst->Montage_SetEndDelegate(EndDelegate, Segment.Montage);

    UE_LOG(LogTemp, Warning, TEXT("Boss enrage skill segment %d/%d playing (Dmg=%.1f, Reduction=%.0f%%, Interval=%.2fs)."),
        Index + 1, EnrageSkillSegments.Num(),
        (Segment.DamagePerHit > 0.0f ? Segment.DamagePerHit : AttackDamage),
        Segment.DamageReduction * 100.0f, Segment.HitInterval);
}

// 狂暴技能段蒙太奇结束回调：正常播完或异常抢占（兜底）都推进下一段，最后一段 → 结束技能
void AMonsterBase::OnEnrageSegmentMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
    if (!bIsEnrageSkillCasting || Montage != CurrentEnrageMontage)
        return;

    // 狂暴技能不可被打断：即使蒙太奇被异常抢占（bInterrupted），也按"播完"推进，流程绝不中断

    // 【落地立即矫正】每段蒙太奇结束（如第一段空中动作落地）瞬间先压平一次倾斜，
    // 再启动短时旋转守卫：段结束后的 BlendOut 混合输出期（时长由蒙太奇资产决定，常见 0.25s+）
    // 根运动旋转会以递减权重持续写入 Actor 的 Pitch/Roll，单次压平可能被随后写入的残留覆盖。
    // 守卫在 UpdateEnrage 的 bIsEnrageSkillCasting 分支内逐帧压平（Yaw 保留），覆盖整个混合输出期，
    // 但不干扰下一段蒙太奇播放中的正常根运动旋转（守卫只持续 EnrageRotationGuardDuration 秒，
    // 且下一段 PlayEnrageSegment 会在播放前再压平一次，段本身的旋转表现完全保留）。
    ResetEnrageMontageRotation();
    EnrageRotationGuardRemaining = FMath::Max(0.3f, EnrageRotationGuardDuration);

    int32 NextIndex = CurrentEnrageSegmentIndex + 1;
    while (EnrageSkillSegments.IsValidIndex(NextIndex) && !EnrageSkillSegments[NextIndex].Montage)
        ++NextIndex;

    if (EnrageSkillSegments.IsValidIndex(NextIndex))
    {
        PlayEnrageSegment(NextIndex);
    }
    else
    {
        EndEnrageSkill();
    }
}

// 结束狂暴技能：停蒙太奇、清状态、进入狂暴（增伤 + 红光 + 清根运动残留旋转）
void AMonsterBase::EndEnrageSkill()
{
    // 停止当前段蒙太奇（正常播完时蒙太奇已结束，此处自然跳过）
    if (bIsEnrageSkillCasting && CurrentEnrageMontage && GetMesh())
    {
        if (UAnimInstance* AnimInst = GetMesh()->GetAnimInstance())
        {
            if (AnimInst->Montage_IsPlaying(CurrentEnrageMontage))
            {
                AnimInst->Montage_Stop(0.12f, CurrentEnrageMontage);
            }
        }
    }

    bIsEnrageSkillCasting = false;
    CurrentEnrageSegmentIndex = -1;
    CurrentEnrageMontage = nullptr;
    EnrageMontageSilentTime = 0.0f;
    EnrageLastHitTime = 0.0f;
    // 声波攻击运行期状态复位（技能结束即停波，保证下一轮狂暴重新计时）
    bEnrageSoundWaveStage = false;
    bEnrageWaveInFlight = false;
    bEnrageWaveHitResolved = false;
    EnrageWaveAge = 0.0f;
    EnrageWaveCooldownRemaining = 0.0f;
    bSoundWavePerfectWindow = false;

    // 收起出伤预警提示（狂暴技能结束）
    if (bDamageWarningActive)
    {
        bDamageWarningActive = false;
        OnDamageWarningEnd();
    }
    HideDamageWarning();

    // ---- 进入狂暴状态：后续攻击增伤 + 身上红光 ----
    if (!bEnraged)
    {
        bEnraged = true;
        SetEnrageGlow(bEnrageRedGlow);
        OnEnraged();
        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' ENRAGED! Subsequent attack damage x%.2f, red glow ON."), *GetName(), EnrageDamageMultiplier);
    }

    // 清除根运动残留旋转（修复技能结束后模型歪斜）：立即清一次 + 启动旋转守卫
    // 【根因】蒙太奇结束后的 BlendOut 混合输出期（时长由蒙太奇资产决定，常见 0.25s+）内，
    // 根运动旋转仍以递减权重持续施加到 Actor；旧的"0.16s 定时补一次"可能早于混合结束，
    // 残留随即又被写入 → 歪斜一直留到 Boss 移动/攻击才被朝向逻辑冲掉。
    // 守卫期内每帧压平 Pitch/Roll（Yaw 保留），无论混合输出多长都能清干净。
    ResetEnrageMontageRotation();
    EnrageRotationGuardRemaining = FMath::Max(0.3f, EnrageRotationGuardDuration);
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(EnrageRootRotationResetTimer);
    }

    // 狂暴技能结束后让 Boss 尽快恢复攻击节奏（攻击冷却压到最多 1s）
    CurrentAttackCd = FMath::Min(CurrentAttackCd, 1.0f);
}

// Tick 驱动：回血阶段计时/回血量/蒙太奇循环 + 狂暴技能段推进兜底与攻击窗口伤害结算
void AMonsterBase::UpdateEnrage(float DeltaTime)
{
    // ---- 回血阶段：无敌 + 从触发时血量线性恢复至满血 + 不移动 ----
    if (bIsEnrageHealing)
    {
        const float Duration = FMath::Max(0.1f, EnrageHealDuration);
        EnrageHealTimeRemaining -= DeltaTime;

        // 线性插值恢复：起始血量 → 满血
        const float Alpha = FMath::Clamp(1.0f - EnrageHealTimeRemaining / Duration, 0.0f, 1.0f);
        CurrentHealth = FMath::Clamp(FMath::Lerp(EnrageHealStartHealth, MaxHealth, Alpha), 0.0f, MaxHealth);
        UpdateHealthBar();

        // 保持静止（清零速度防滑行，保留重力）
        StopHorizontalMovement();

        // 回血蒙太奇短于阶段时长 → 播完循环重播
        if (EnrageHealMontage && GetMesh() && GetMesh()->GetAnimInstance())
        {
            UAnimInstance* AnimInst = GetMesh()->GetAnimInstance();
            if (!AnimInst->Montage_IsActive(EnrageHealMontage))
            {
                AnimInst->Montage_Play(EnrageHealMontage, FMath::Max(0.05f, EnrageHealPlayRate));
            }
        }

        if (EnrageHealTimeRemaining <= 0.0f)
        {
            EndEnrageHeal();
        }
        return;
    }

    // ---- 狂暴技能：段推进兜底 + 攻击窗口伤害结算 ----
    if (bIsEnrageSkillCasting)
    {
        // 不主动移动（清零速度输入防滑行；段根运动的动画位移仍正常推动角色）
        StopHorizontalMovement();

        // 【落地矫正守卫】段蒙太奇结束落地后（OnEnrageSegmentMontageEnded 里已置守卫），
        // 在下一段播放的 BlendOut 混合输出期内逐帧压平 Pitch/Roll（Yaw 保留），清掉残留倾斜。
        // 主 Tick 在 bIsEnrageSkillCasting 时提前 return 到不了下面的守卫逻辑，故在此分支内单独执行。
        // 这里只用「轻量 Actor 压平」（只清 Actor 的 Pitch/Roll、保留 Yaw，不碰 Mesh 相对变换），
        // 因为倾斜来源是根运动旋转写入 Actor 的 Pitch/Roll，而非 Mesh 相对旋转；不碰 Mesh 可避免
        // 守卫覆盖到下一段播放初期时干扰其骨骼/根运动表现。段本身位移/朝向完全保留。
        if (EnrageRotationGuardRemaining > 0.0f)
        {
            EnrageRotationGuardRemaining -= DeltaTime;
            const float GuardYaw = GetActorRotation().Yaw;
            SetActorRotation(FRotator(0.0f, GuardYaw, 0.0f));
        }

        if (!EnrageSkillSegments.IsValidIndex(CurrentEnrageSegmentIndex) || !CurrentEnrageMontage || !GetMesh())
        {
            EndEnrageSkill();
            return;
        }

        UAnimInstance* AnimInst = GetMesh()->GetAnimInstance();

        // 兜底：蒙太奇已停且迟迟未推进（结束回调丢失）→ 强制按播完推进，防止流程卡死
        if (!AnimInst || !AnimInst->Montage_IsPlaying(CurrentEnrageMontage))
        {
            EnrageMontageSilentTime += DeltaTime;
            if (EnrageMontageSilentTime > 0.3f)
            {
                UE_LOG(LogTemp, Warning, TEXT("Boss enrage montage end delegate lost, force advancing to next segment."));
                OnEnrageSegmentMontageEnded(CurrentEnrageMontage, false);
            }
            return;
        }
        EnrageMontageSilentTime = 0.0f;

        const FBossEnrageSegment& Segment = EnrageSkillSegments[CurrentEnrageSegmentIndex];

        // 有效攻击时间段（-1 回落：起始=0，结束=蒙太奇全长；时间轴秒与 PlayRate 无关）
        const float WindowStart = (Segment.AttackWindowStartTime >= 0.0f) ? Segment.AttackWindowStartTime : 0.0f;
        const float WindowEnd = (Segment.AttackWindowEndTime >= 0.0f) ? Segment.AttackWindowEndTime : CurrentEnrageMontage->GetPlayLength();

        const float MontagePos = AnimInst->Montage_GetPosition(CurrentEnrageMontage);

        // ---- 出伤预警：该段出伤时间前 DamageWarningLeadTime 秒提示（预警期间玩家闪避 = 完美闪避）----
        UpdateDamageWarning(MontagePos, WindowStart);

        // ---- 声波攻击模式：段蒙太奇播放期间周期性扩散声波（伤害/完美闪避由波前驱动，
        //     独立于段攻击窗口，整段演出都在"声波阶段"内；关闭则回退旧"窗口节流范围伤害"）----
        if (bEnrageSoundWaveSkill)
        {
            UpdateEnrageSoundWave(DeltaTime);
        }
        else if (MontagePos >= WindowStart && MontagePos <= WindowEnd)
        {
            ApplyEnrageSkillDamage(Segment);
        }
    }
}

// ---- 狂暴技能声波攻击：以自身为圆心周期性扩散声波圈 ----
// - 段蒙太奇播放期间每隔 EnrageSoundWaveInterval 秒发出一波；
// - 波前以 EnrageSoundWaveExpandSpeed 从圆心向外扩散，达 EnrageSoundWaveRadius 后本波结束；
// - 波前首次扫过玩家径向位置时结算一次伤害（ApplyPointDamage，可被无敌帧格挡）；
// - 波前距玩家径向距离 <= EnrageSoundWavePerfectDodgeMargin + ExpandSpeed×GraceSeconds 的
//   时机内玩家闪避 = 完美闪避（帧补偿保证 60fps 下不漏判、有稳定可操作的判定时长）
void AMonsterBase::UpdateEnrageSoundWave(float DeltaTime)
{
    if (!GetWorld() || AIState == EMonsterAIState::Dead)
        return;

    // 【段门控】声波只从 EnrageSoundWaveStartSegmentIndex 指定的段（默认第二段）起才触发。
    // 第一段若为空中/位移演出段，则整段不发波、不进声波阶段（也不授完美闪避窗口），
    // 保证第一段干净、声波集中在攻击段（第二段）释放，避免空中就发波导致难以闪避、手感突兀。
    if (CurrentEnrageSegmentIndex < EnrageSoundWaveStartSegmentIndex)
    {
        // 尚未到发声波段：清掉可能遗留的在飞波状态（理论上不会，防御性兜底）
        bEnrageWaveInFlight = false;
        bEnrageWaveHitResolved = false;
        bSoundWavePerfectWindow = false;
        EnrageWaveAge = 0.0f;
        return;
    }

    // 首次进入声波阶段：置阶段标志并让首波立即发射
    if (!bEnrageSoundWaveStage)
    {
        bEnrageSoundWaveStage = true;
        EnrageWaveCooldownRemaining = 0.0f;
    }

    // 波间冷却每帧递减（与当前波扩散并行计时 → 实际周期 = max(Interval, 波扩散完所需时长)）
    if (EnrageWaveCooldownRemaining > 0.0f)
    {
        EnrageWaveCooldownRemaining -= DeltaTime;
    }

    APawn* Player = GetPlayerPawn();
    const float PlayerDist = Player ? (Player->GetActorLocation() - GetActorLocation()).Size2D() : -1.0f;

    if (bEnrageWaveInFlight)
    {
        // 波前推进
        EnrageWaveAge += DeltaTime;
        const float WaveRadius = EnrageWaveAge * FMath::Max(1.0f, EnrageSoundWaveExpandSpeed);

        // ---- 完美闪避窗口：波前距玩家 <= 基础容差 + 提前缓冲换算距离 ----
        bSoundWavePerfectWindow = false;
        if (!bEnrageWaveHitResolved && Player && PlayerDist >= 0.0f && WaveRadius <= EnrageSoundWaveRadius)
        {
            const float LeadCm = EnrageSoundWaveExpandSpeed * FMath::Max(0.0f, EnrageSoundWaveGraceSeconds);
            const float Tolerance = EnrageSoundWavePerfectDodgeMargin + LeadCm;
            if (FMath::Abs(PlayerDist - WaveRadius) <= Tolerance)
            {
                bSoundWavePerfectWindow = true;
            }
        }

        // ---- 波前首次扫过玩家位置：结算一次伤害（每波仅一次）----
        if (!bEnrageWaveHitResolved && Player && PlayerDist >= 0.0f)
        {
            const FVector Offset = Player->GetActorLocation() - GetActorLocation();
            const bool bInRadius2D = PlayerDist <= EnrageSoundWaveRadius;
            const bool bInUpperHemisphere = Offset.Z >= -AttackHitHeightTolerance && Offset.Z <= EnrageSoundWaveRadius;
            if (bInRadius2D && bInUpperHemisphere && WaveRadius >= PlayerDist)
            {
                bEnrageWaveHitResolved = true;
                bSoundWavePerfectWindow = false; // 波已命中/被格挡 → 同一波不再授予完美闪避

                // 伤害：声波伤害优先，未配则回落当前段 DamagePerHit，再回落全局 AttackDamage
                const FBossEnrageSegment* Seg = EnrageSkillSegments.IsValidIndex(CurrentEnrageSegmentIndex)
                    ? &EnrageSkillSegments[CurrentEnrageSegmentIndex] : nullptr;
                float Damage = EnrageSoundWaveDamage;
                if (Damage <= 0.0f && Seg)
                {
                    Damage = Seg->DamagePerHit;
                }
                if (Damage <= 0.0f)
                {
                    Damage = AttackDamage;
                }

                if (Damage > 0.0f)
                {
                    const FVector Direction = Offset.GetSafeNormal();
                    const float Actual = UGameplayStatics::ApplyPointDamage(Player, Damage, Direction, FHitResult(), GetController(), this, nullptr);
                    UE_LOG(LogTemp, Warning, TEXT("Boss enrage SOUND WAVE hit player! Damage=%.1f (applied=%.1f)."), Damage, Actual);
                }
            }
        }

        // 波前扩散到最大半径 → 本波结束，可发下一波
        if (WaveRadius >= EnrageSoundWaveRadius)
        {
            bEnrageWaveInFlight = false;
            bEnrageWaveHitResolved = false;
            bSoundWavePerfectWindow = false;
        }
        return; // 波扩散期间不再发新波
    }

    // ---- 无在飞波：冷却结束即发出下一波 ----
    if (EnrageWaveCooldownRemaining <= 0.0f)
    {
        bEnrageWaveInFlight = true;
        EnrageWaveAge = 0.0f;
        bEnrageWaveHitResolved = false;
        bSoundWavePerfectWindow = false;
        EnrageWaveCooldownRemaining = FMath::Max(0.05f, EnrageSoundWaveInterval);

        // 通知蓝图播放该波扩散特效/音效（0基础特效制作入口：怪物蓝图实现此事件）
        OnEnrageSoundWaveEmitted(EnrageSoundWaveRadius, FMath::Max(1.0f, EnrageSoundWaveExpandSpeed));

        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' emits enrage SOUND WAVE (interval %.1fs, radius %.0fcm, expand %.0f cm/s)."),
            *GetName(), EnrageSoundWaveInterval, EnrageSoundWaveRadius, EnrageSoundWaveExpandSpeed);
    }
}

// 玩家是否在狂暴技能攻击范围内：以怪物为球心、半径 EnrageSkillRadius 的上半球体
bool AMonsterBase::IsPlayerInEnrageHemisphere() const
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return false;

    const FVector Offset = Player->GetActorLocation() - GetActorLocation();
    if (Offset.Size2D() > EnrageSkillRadius)
        return false;
    // 上半球：不低于怪物脚底过多（容差沿用攻击高度容差），不高于半球顶
    return Offset.Z >= -AttackHitHeightTolerance && Offset.Z <= EnrageSkillRadius;
}

// 对攻击范围内的玩家结算狂暴技能段伤害（HitInterval 节流）
void AMonsterBase::ApplyEnrageSkillDamage(const FBossEnrageSegment& Segment)
{
    if (!GetWorld() || AIState == EMonsterAIState::Dead)
        return;

    // 伤害：段配置优先（<=0 回落全局 AttackDamage）。
    // 狂暴增伤倍率只作用于"技能结束后"的后续攻击，狂暴技能本身不叠加
    const float Damage = (Segment.DamagePerHit > 0.0f) ? Segment.DamagePerHit : AttackDamage;
    if (Damage <= 0.0f)
        return;

    // 重复伤害节流：HitInterval>0 按间隔重复结算；0 = 该段内只结算一次
    const float Now = GetWorld()->GetTimeSeconds();
    if (Segment.HitInterval > 0.0f)
    {
        if (Now - EnrageLastHitTime < Segment.HitInterval)
            return;
    }
    else if (EnrageLastHitTime > 0.0f)
    {
        return;
    }

    if (!IsPlayerInEnrageHemisphere())
        return;

    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    EnrageLastHitTime = Now;

    const FVector Direction = (Player->GetActorLocation() - GetActorLocation()).GetSafeNormal();
    // ApplyPointDamage：玩家无敌帧期间返回 0（可被闪避规避）
    const float ActualDamage = UGameplayStatics::ApplyPointDamage(Player, Damage, Direction, FHitResult(), GetController(), this, nullptr);

    UE_LOG(LogTemp, Warning, TEXT("Boss enrage skill hits player! Damage=%.1f (applied=%.1f)."), Damage, ActualDamage);
}

// 运行时为狂暴技能段动画序列启用根运动（修复段间位置回跳，与角色段根运动补丁同方案）
void AMonsterBase::EnsureEnrageRootMotion()
{
    // 动画实例必须允许处理蒙太奇根运动（防御性纠正）
    if (UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
    {
        if (AnimInst->RootMotionMode == ERootMotionMode::IgnoreRootMotion ||
            AnimInst->RootMotionMode == ERootMotionMode::NoRootMotionExtraction)
        {
            AnimInst->RootMotionMode = ERootMotionMode::RootMotionFromMontagesOnly;
            UE_LOG(LogTemp, Warning, TEXT("[BossEnrageRootMotion] AnimInstance was ignoring root motion, switched to RootMotionFromMontagesOnly."));
        }
    }

    if (bEnrageRootMotionPatched)
        return;
    bEnrageRootMotionPatched = true;

    int32 PatchedCount = 0;
    for (int32 SegIdx = 0; SegIdx < EnrageSkillSegments.Num(); ++SegIdx)
    {
        UAnimMontage* Montage = EnrageSkillSegments[SegIdx].Montage;
        if (!Montage)
        {
            continue;
        }
        for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
        {
            for (const FAnimSegment& AnimSeg : Track.AnimTrack.AnimSegments)
            {
                UAnimSequence* Seq = Cast<UAnimSequence>(AnimSeg.GetAnimReference().Get());
                if (!Seq)
                {
                    continue;
                }
                if (!Seq->bEnableRootMotion)
                {
                    Seq->bEnableRootMotion = true;
                    ++PatchedCount;
                }

                // 诊断：确认根骨骼确实带位移数据（位移烤进姿势骨骼的动画无法靠根运动移动角色）
                const float SeqLength = Seq->GetPlayLength();
                if (SeqLength > 0.05f)
                {
                    const FTransform RootTravel = Seq->ExtractRootMotionFromRange(0.0, SeqLength, FAnimExtractContext(0.0, true));
                    const float TravelDist = RootTravel.GetTranslation().Size2D();
                    UE_LOG(LogTemp, Warning, TEXT("[BossEnrageRootMotion] Segment %d '%s': root travel %.1fcm%s"),
                        SegIdx, *Seq->GetName(), TravelDist,
                        TravelDist < 1.0f ? TEXT(" - NO root travel, movement baked into pose bones!") : TEXT(" - OK."));
                }
            }
        }
    }
    UE_LOG(LogTemp, Warning, TEXT("[BossEnrageRootMotion] Runtime root motion enabled on %d sequence(s)."), PatchedCount);
}

// 清除根运动残留旋转（修复狂暴技能结束后模型歪斜）：Actor 只保留 Yaw + Mesh 恢复基准相对变换
void AMonsterBase::ResetEnrageMontageRotation()
{
    const float Yaw = GetActorRotation().Yaw;
    SetActorRotation(FRotator(0.0f, Yaw, 0.0f));

    if (USkeletalMeshComponent* MeshComp = GetMesh())
    {
        MeshComp->SetRelativeLocationAndRotation(BaseMeshRelativeLocation, BaseMeshRelativeRotation);
    }
}

// 狂暴红光开关：点光源亮起/熄灭 + 尝试设置材质参数（参数不存在时为无副作用空操作）
void AMonsterBase::SetEnrageGlow(bool bEnabled)
{
    if (EnrageLight)
    {
        if (bEnabled)
        {
            EnrageLight->SetLightColor(EnrageGlowColor);
            EnrageLight->SetIntensity(FMath::Max(0.0f, EnrageGlowIntensity));
            EnrageLight->SetAttenuationRadius(500.0f);
        }
        EnrageLight->SetVisibility(bEnabled);
    }

    // 材质参数（可选增强）：怪物材质若提供标量参数 EnrageGlow（0~1）与向量参数 EnrageGlowColor，
    // 红光可直接烘到模型表面（自发光）；参数不存在时不产生任何副作用
    if (GetMesh())
    {
        GetMesh()->SetScalarParameterValueOnMaterials(TEXT("EnrageGlow"), bEnabled ? 1.0f : 0.0f);
        GetMesh()->SetVectorParameterValueOnMaterials(TEXT("EnrageGlowColor"),
            FVector(EnrageGlowColor.R, EnrageGlowColor.G, EnrageGlowColor.B));
    }
}

// ---- 摄像机贴近遮挡处理：怪物不挡玩家视野 ----
// SpringArm 的防穿模探针依赖网格体碰撞（Physics Asset 通常只覆盖躯干），挡不住攻击挥出的手臂。
// 两种模式（CameraOcclusionMode）：
//   Fade（默认）：按距离把材质参数 FadeOpacity 从 1 渐变到 MinFadeOpacity → 怪物半透明不挡视野。
//                需要怪物材质：Blend Mode=Translucent，且有名为 FadeOpacity 的标量参数连到 Opacity
//   Hide：距离内直接隐藏模型（含网格体上的子组件如武器），无需改材质
void AMonsterBase::UpdateCameraOcclusion()
{
    if (!GetMesh())
        return;

    // 查找/刷新玩家摄像机（小怪/Boss 通用；缓存失效时重新查找）。
    // 失效判定含「所有者是否仍是当前操控 Pawn」：切人（旧角色留场放技能）时旧相机仍 IsValid，
    // 但 Owner 已不是 GetPlayerPawn()，须重新查找，否则遮挡检测锁在旧相机上、距离算错。
    if (!IsValid(CachedPlayerCamera) || CachedPlayerCamera->GetOwner() != GetPlayerPawn())
    {
        if (APawn* Player = GetPlayerPawn())
        {
            CachedPlayerCamera = Player->FindComponentByClass<UCameraComponent>();
        }
        if (!CachedPlayerCamera)
            return;
    }

    const float CamDist = FVector::Dist(
        CachedPlayerCamera->GetComponentLocation(), GetActorLocation());

    // ---- Hide 模式：距离内直接隐藏 ----
    if (CameraOcclusionMode == EMonsterOcclusionMode::Hide)
    {
        const bool bShouldHide = CamDist < CameraHideDistance;

        // 仅在显隐状态变化时设置（避免每帧重复 SetHiddenInGame）
        if (bShouldHide != bMeshHiddenForCamera)
        {
            GetMesh()->SetHiddenInGame(bShouldHide);

            // 一并隐藏挂在网格体上的子组件（武器、特效等），避免裸武器悬浮在镜头前
            for (USceneComponent* Child : GetMesh()->GetAttachChildren())
            {
                if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Child))
                {
                    Prim->SetHiddenInGame(bShouldHide);
                }
            }

            bMeshHiddenForCamera = bShouldHide;

            UE_LOG(LogTemp, Warning, TEXT("Monster '%s' mesh %s (camera dist=%.0f, hide below %.0f)."),
                *GetName(), bShouldHide ? TEXT("HIDDEN") : TEXT("visible"), CamDist, CameraHideDistance);
        }
        return;
    }

    // ---- Fade 模式：距离映射到不透明度，平滑渐变半透明 ----
    // 渐变区间 [End, Start]：CamDist >= Start 完全不透明；CamDist <= End 达到 MinFadeOpacity
    const float Start = FMath::Max(CameraFadeStartDistance, CameraFadeEndDistance + 1.0f);
    const float End = FMath::Min(CameraFadeStartDistance, CameraFadeEndDistance);
    const float FadeAmount = FMath::Clamp((Start - CamDist) / (Start - End), 0.0f, 1.0f);
    const float NewOpacity = FMath::Lerp(1.0f, MinFadeOpacity, FadeAmount);

    // 不透明度变化才刷新材质参数（SetScalarParameterValueOnMaterials 会遍历所有材质元素）
    if (!FMath::IsNearlyEqual(NewOpacity, LastFadeOpacity, 0.005f))
    {
        // 写入所有材质的 FadeOpacity 参数（首次调用自动创建动态材质实例）
        GetMesh()->SetScalarParameterValueOnMaterials(TEXT("FadeOpacity"), NewOpacity);

        // 挂在网格体上的子组件（武器等）同步渐变
        for (USceneComponent* Child : GetMesh()->GetAttachChildren())
        {
            if (UMeshComponent* MeshChild = Cast<UMeshComponent>(Child))
            {
                MeshChild->SetScalarParameterValueOnMaterials(TEXT("FadeOpacity"), NewOpacity);
            }
        }

        LastFadeOpacity = NewOpacity;
    }
}

// ---- 玩家辅助 ----
APawn* AMonsterBase::GetPlayerPawn() const
{
    return UGameplayStatics::GetPlayerPawn(this, 0);
}

float AMonsterBase::GetPlayerDistance() const
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return TNumericLimits<float>::Max();

    return FVector::Dist(GetActorLocation(), Player->GetActorLocation());
}

// ==================== Boss 二阶段毒刺远程攻击 ====================

// 二阶段距离判定：满足条件则触发毒刺远程攻击（UpdateBossAI 的追击分支调用）
void AMonsterBase::TryTriggerBossSting(float DeltaTime)
{
    // 冷却递减
    if (BossStingCooldownRemaining > 0.0f)
    {
        BossStingCooldownRemaining -= DeltaTime;
    }

    if (bBossStingStage)
        return;
    if (!bEnraged || !bEnableBossSting)
        return;
    if (BossStingCooldownRemaining > 0.0f)
        return;

    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    const float Dist = GetPlayerDistance();

    // 玩家距离 <= 触发距离 → 不发射（还在近战范围）；玩家超出仇恨范围 → 不发射（脱战逻辑已处理）
    if (Dist <= BossStingTriggerDistance)
        return;
    if (Dist > GetBossChaseRadius())
        return;

    // 触发毒刺攻击
    SpawnBossSting();
}

// 发射毒刺：播放远程蒙太奇 + 生成毒刺投射物
void AMonsterBase::SpawnBossSting()
{
    if (!GetWorld())
        return;

    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    // 进入毒刺阶段
    bBossStingStage = true;
    bBossStingFlying = false;
    CurrentSting = nullptr;

    // 取消尚未结算的普通攻击（切换到远程攻击）：清判定窗口/前摇/弹刀提示
    GetWorldTimerManager().ClearTimer(HitWindowTimerHandle);
    bHitWindowRunning = false;
    bHitWindowOpen = false;
    bParryWindowOpen = false;
    bPlayerWasInContact = false;
    EndTelegraphWindow();
    if (GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Stop(0.1f);
    }
    UpdateMoveMontage(false);

    // 朝向玩家（远程攻击需面向玩家发射）
    FaceTarget(Player->GetActorLocation());

    // 停止追击（原地发射，可配置）
    if (bBossStingHoldStill)
    {
        StopHorizontalMovement();
        if (GetCharacterMovement())
        {
            GetCharacterMovement()->StopMovementImmediately();
        }
    }

    // 播放远程攻击蒙太奇（怪物释放技能本身，不接触判定）。留空则跳过动画直接发射
    if (BossStingMontage && GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetMesh()->GetAnimInstance()->Montage_Play(BossStingMontage, FMath::Max(0.05f, BossStingMontagePlayRate));

        // 蒙太奇播到「发射时刻」（取蒙太奇中点作为发射点，平衡前摇与手感的通用做法）后生成毒刺。
        // 用世界定时器在蒙太奇播放到一半时发射；若蒙太奇短则尽快发射
        const float MontageLength = BossStingMontage->GetPlayLength();
        const float EmitTime = FMath::Max(0.05f, (MontageLength * 0.5f) / FMath::Max(0.05f, BossStingMontagePlayRate));
        FTimerHandle EmitTimer;
        GetWorldTimerManager().SetTimer(EmitTimer, [this]()
        {
            EmitBossStingProjectile();
        }, EmitTime, false);
    }
    else
    {
        // 无蒙太奇：直接发射
        EmitBossStingProjectile();
    }

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' triggers STING remote attack (dist=%.0f > trigger %.0f)."),
        *GetName(), GetPlayerDistance(), BossStingTriggerDistance);
}

// 实际生成毒刺投射物（发射时刻调用）
void AMonsterBase::EmitBossStingProjectile()
{
    if (!GetWorld() || AIState == EMonsterAIState::Dead)
        return;

    APawn* Player = GetPlayerPawn();
    if (!Player)
    {
        // 无玩家：结束毒刺阶段
        bBossStingStage = false;
        bBossStingFlying = false;
        return;
    }

    // 发射方向：从怪物（胸口高度）朝玩家当前位置
    const FVector MuzzleLocation = GetActorLocation() + FVector(0.0f, 0.0f, GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 90.0f);
    const FVector AimLocation = Player->GetActorLocation();
    const FVector Direction = (AimLocation - MuzzleLocation).GetSafeNormal();
    if (Direction.IsNearlyZero())
    {
        bBossStingStage = false;
        bBossStingFlying = false;
        return;
    }

    // 生成毒刺（优先蓝图类，回落原生 ABossSting）
    TSubclassOf<ABossSting> StingClass = BossStingClass;
    if (!StingClass)
    {
        StingClass = ABossSting::StaticClass();
    }
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ABossSting* Sting = GetWorld()->SpawnActor<ABossSting>(StingClass, MuzzleLocation, Direction.Rotation(), SpawnParams);
    if (!Sting)
    {
        UE_LOG(LogTemp, Error, TEXT("Boss '%s' failed to spawn sting!"), *GetName());
        bBossStingStage = false;
        bBossStingFlying = false;
        return;
    }

    // 射程：直接使用蓝图配置的毒刺最大有效范围（0 = 不限距离，永不因射程自毁）
    const float MaxDist = BossStingMaxFlightDistance;

    // 攻击判定半径透传：从 Boss 蓝图配置同步到毒刺（半径越大 → 攻击判定范围越宽松、越易命中）
    // 在 InitSting 之前赋值，InitSting 内部会调用 ApplyHitDetectionRadius 应用到碰撞球
    Sting->HitDetectionRadius = BossStingHitDetectionRadius;

    // 初始化毒刺（伤害/速度/射程/完美闪避阈值 + 攻击判定半径）
    Sting->InitSting(Direction, BossStingDamage, BossStingSpeed, this, GetController(), MaxDist, BossStingPerfectDodgeDistanceThreshold);

    // 近身蹭闪范围透传：从 Boss 蓝图配置同步到毒刺（贴身边缘 <= 该值即可蹭闪完美闪避）
    Sting->CloseDodgeDistance = BossStingCloseDodgeDistance;

    CurrentSting = Sting;
    bBossStingFlying = true;

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' EMITS sting! Damage=%.1f, Speed=%.0f, MaxDist=%.0f, PerfectDodgeDist=%.0f."),
        *GetName(), BossStingDamage, BossStingSpeed, MaxDist, BossStingPerfectDodgeDistanceThreshold);
}

// 毒刺命中结果回调（ABossSting::Resolve 调用）
void AMonsterBase::OnBossStingResolved(bool bHitPlayer)
{
    // 毒刺已结算，结束飞行状态
    bBossStingFlying = false;
    CurrentSting = nullptr;
    bBossStingStage = false;

    // 进入发射冷却
    BossStingCooldownRemaining = FMath::Max(0.1f, BossStingCooldown);

    if (bHitPlayer)
    {
        // 命中玩家：延迟 BossStingTeleportDelay 秒后瞬移至角色背后 → 依次触发除可被弹刀外的技能攻击 → 恢复正常攻击频率
        // （延迟期间由 Tick 主循环递减 BossStingTeleportDelayRemaining，倒计时结束才真正瞬移）
        BossStingTeleportDelayRemaining = FMath::Max(0.0f, BossStingTeleportDelay);
        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' sting HIT player! Will teleport behind after %.2fs and start backstab combo."),
            *GetName(), BossStingTeleportDelayRemaining);
    }
    else
    {
        // 未命中：不瞬移，恢复正常攻击频率（毒刺未击中玩家则不触发瞬移）
        UE_LOG(LogTemp, Warning, TEXT("Boss '%s' sting MISSED! No teleport (attack rhythm unchanged)."), *GetName());
    }
}

// 瞬移到玩家背后（背刺起点）
void AMonsterBase::TeleportBehindPlayer()
{
    APawn* Player = GetPlayerPawn();
    if (!Player)
        return;

    // 玩家身后：玩家朝向的反方向偏移一段距离（背刺贴背）
    const FVector PlayerForward = Player->GetActorForwardVector();
    const FVector BackDir = -PlayerForward.GetSafeNormal2D();
    if (BackDir.IsNearlyZero())
    {
        return;
    }

    // 瞬移到玩家身后 150cm 处，保持玩家当前高度（Z 对齐玩家，防止瞬移进地）
    const float BehindDistance = 150.0f;
    FVector TargetLocation = Player->GetActorLocation() + BackDir * BehindDistance;
    TargetLocation.Z = Player->GetActorLocation().Z;

    // 直接瞬移（不 sweep，避免瞬移途中扫到碰撞被卡），随后清速度防止瞬移后残留滑行
    SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->Velocity = FVector::ZeroVector;
    }

    // 瞬移后朝向玩家（背刺前先面向玩家）
    const FRotator FacePlayer = (Player->GetActorLocation() - GetActorLocation()).Rotation();
    SetActorRotation(FRotator(0.0f, FacePlayer.Yaw, 0.0f));

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' TELEPORTED behind player."), *GetName());
}

// 命中后连击：依次播放所有「不可弹刀」的攻击段（每段带完整判定），播完恢复普通攻击频率
void AMonsterBase::StartStingBackstabCombo()
{
    // 收集所有不可弹刀（bParryable=false）且配置了蒙太奇的攻击段索引
    BackstabComboIndices.Reset();
    BackstabComboCursor = 0;

    // 优先逐段配置（AttackConfigs）
    if (AttackConfigs.Num() > 0)
    {
        for (int32 i = 0; i < AttackConfigs.Num(); ++i)
        {
            if (AttackConfigs[i].Montage && !AttackConfigs[i].bParryable)
            {
                BackstabComboIndices.Add(i);
            }
        }
    }
    // 旧版 AttackMontages：全部视为不可弹刀（旧路径本就不支持弹刀）
    else
    {
        for (int32 i = 0; i < AttackMontages.Num(); ++i)
        {
            if (AttackMontages[i])
            {
                BackstabComboIndices.Add(i);
            }
        }
    }

    if (BackstabComboIndices.Num() == 0)
    {
        // 无可连击段：直接恢复正常攻击频率
        bBackstabComboActive = false;
        CurrentAttackCd = 0.0f;
        return;
    }

    bBackstabComboActive = true;
    BackstabComboCursor = 0;

    // 立即播放第一段
    PlayBackstabComboSegment();
}

// 播放背刺连击当前段（依次播放不可弹刀攻击段）
void AMonsterBase::PlayBackstabComboSegment()
{
    if (!bBackstabComboActive || !BackstabComboIndices.IsValidIndex(BackstabComboCursor))
    {
        // 连击结束：恢复普通攻击频率
        bBackstabComboActive = false;
        CurrentAttackCd = 0.0f;
        return;
    }

    const int32 SegIndex = BackstabComboIndices[BackstabComboCursor];
    APawn* Player = GetPlayerPawn();
    if (!Player)
    {
        bBackstabComboActive = false;
        return;
    }

    // 朝向玩家并播放该段攻击（复用现有攻击段播放逻辑）
    FaceTarget(Player->GetActorLocation());

    if (AttackConfigs.Num() > 0)
    {
        // 逐段配置路径：直接播放指定段
        if (AttackConfigs.IsValidIndex(SegIndex) && AttackConfigs[SegIndex].Montage)
        {
            PlaySpecificAttackConfig(SegIndex);
        }
    }
    else if (AttackMontages.IsValidIndex(SegIndex) && AttackMontages[SegIndex])
    {
        // 旧版路径：播放指定蒙太奇
        PlaySpecificAttackMontage(SegIndex);
    }

    // 准备下一段
    ++BackstabComboCursor;

    // 当前段蒙太奇播完后触发下一段（用世界定时器按蒙太奇时长推进）
    float SegDuration = 0.5f;
    if (AttackConfigs.Num() > 0 && AttackConfigs.IsValidIndex(SegIndex) && AttackConfigs[SegIndex].Montage)
    {
        SegDuration = AttackConfigs[SegIndex].Montage->GetPlayLength() / FMath::Max(0.05f, AttackConfigs[SegIndex].PlayRate);
    }
    else if (AttackMontages.IsValidIndex(SegIndex) && AttackMontages[SegIndex])
    {
        SegDuration = AttackMontages[SegIndex]->GetPlayLength() / FMath::Max(0.05f, AttackMontagePlayRate);
    }

    FTimerHandle NextTimer;
    GetWorldTimerManager().SetTimer(NextTimer, [this]()
    {
        PlayBackstabComboSegment();
    }, FMath::Max(0.1f, SegDuration), false);

    UE_LOG(LogTemp, Warning, TEXT("Boss '%s' backstab combo segment %d/%d (index %d)."),
        *GetName(), BackstabComboCursor, BackstabComboIndices.Num(), SegIndex);
}

// 毒刺阶段 Tick 驱动（蒙太奇播放中朝向玩家；毒刺飞行距离完美闪避窗口维护）
void AMonsterBase::UpdateBossSting(float DeltaTime)
{
    if (!bBossStingStage)
        return;

    APawn* Player = GetPlayerPawn();

    // 毒刺飞行中：朝向玩家维持（可选）；完美闪避窗口由 IsBossStingPerfectDodgeWindow 查询 CurrentSting
    if (bBossStingFlying)
    {
        // 毒刺已销毁（命中/超射程）但回调未及时清状态 → 防御性兜底
        if (!CurrentSting.IsValid() || CurrentSting->IsResolved())
        {
            bBossStingFlying = false;
            bBossStingStage = false;
            CurrentSting = nullptr;
            return;
        }
    }

    // 无玩家：结束毒刺阶段
    if (!Player)
    {
        bBossStingStage = false;
        bBossStingFlying = false;
        CurrentSting = nullptr;
        return;
    }

    // 毒刺阶段保持朝向玩家（发射动画/飞行期间面向玩家，便于后续背刺/追击）
    if (bBossStingHoldStill)
    {
        FaceTarget(Player->GetActorLocation());
    }
}

// 当前在飞毒刺是否处于「可完美闪避」状态（毒刺距玩家 > 阈值）
bool AMonsterBase::IsBossStingPerfectDodgeWindow() const
{
    if (!bBossStingFlying)
        return false;
    if (!CurrentSting.IsValid())
        return false;
    return CurrentSting->CanPerfectDodge();
}
