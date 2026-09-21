#include "BattleCharacter.h"
#include "WeaponBase.h"
#include "CharaInfo.h"
#include "CharaTeamWidget.h"
#include "MonsterBase.h"
#include "AbilitySystemComponent.h"
#include "BattleAttributeSet.h"
#include "DamageGameplayEffect.h"
#include "UltimateAbility.h"
#include "InvincibilityGameplayEffect.h"
#include "DodgeAbility.h"
#include "PoisonGameplayEffect.h"
#include "DamageBoostGameplayEffect.h"
#include "WutheringWavesTags.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayAbilitySpec.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/WidgetComponent.h"
#include "Blueprint/UserWidget.h"
#include "ItemPickupTipsWidget.h"
#include "Components/ProgressBar.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "HAL/PlatformTime.h"
#include "Components/Button.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "InputActionValue.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "InputCoreTypes.h"
#include "Engine/InputDelegateBinding.h"
#include "Kismet/GameplayStatics.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "GhostAfterimage.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "InventoryComponent.h"
#include "Components/UniformGridPanel.h"
#include "Components/GridPanel.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "Components/PanelSlot.h"
#include "Components/SizeBox.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"

// ---- 武器蓝图自动发现用 ----
// 为什么用资产注册表而不是 UObject 的 GetDerivedClasses：后者只能看到「已经加载进内存」的类，
// 没打开过武器蓝图时那个列表是空的 → 会出现「刚重启编辑器，背包里的武器全装不上」这种玄学现象。
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

// ★ 每角色类「当前装备的武器」运行时表（绕开 CDO 继承链，见 BattleCharacter.h 里字段注释）。
//   装备是运行时状态，跨实例全局一份；开局自动装备仍走各角色类自己的 DefaultWeaponClass。
TMap<UClass*, TSubclassOf<AWeaponBase>> ABattleCharacter::ClassEquippedWeapon;

// ★ 武器实例占用表：key = "行名#实例号"，value = 装备它的角色类（唯一装备判据）。
TMap<FString, UClass*> ABattleCharacter::WeaponInstanceOwner;

// ★ 切人 CD 表：key = 编队行名，value = CD 结束的世界时间（秒）。
//   static：切人后操控权到了新实例身上，新实例必须能看到「旧实例刚设下的 CD」，
//   放实例字段会导致切一次 CD 表就丢一次（表现为 0.5s 内能无限来回切）。
TMap<FName, double> ABattleCharacter::SwitchCooldownUntilRow;

// ★ 切人装备保持表：角色类 → 上次装备的背包行名。
//   切出（旧实例销毁）时记录 + 释放占用；切回（新实例 BeginPlay）时按它恢复装备。
//   不记录的话：切走再切回，之前从背包装备的武器会丢（显示默认武器），
//   且占用表残留 → 那把武器永远显示「已装备」却没人用（死锁）。
TMap<UClass*, FName> ABattleCharacter::ClassEquippedBagRow;

// ★ 在场角色实例注册表：行名 → 当前在场存活的角色实例。
//   技能中切人（形态①）旧角色留场放技能不销毁，切回时若它仍存活，就归还操控权给它，
//   而不是再 spawn 一份（否则「原角色多出一个新的」）。spawn/初始角色注册，EndPlay 注销。
TMap<FName, ABattleCharacter*> ABattleCharacter::ActiveCharaInstances;

// 生成武器实例的唯一键（行名 + 实例号）。
static FString MakeWeaponInstanceKey(FName RowName, int32 InstanceNo)
{
    return FString::Printf(TEXT("%s#%d"), *RowName.ToString(), InstanceNo);
}

ABattleCharacter::ABattleCharacter()
{
    PrimaryActorTick.bCanEverTick = true;

    AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    AbilitySystemComponent->SetIsReplicated(true);
    AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

    // GAS 属性集：P0 地基——将生命/耐力/协奏/攻击/防御/Poise 迁移为 UAttributeSet
    AttributeSet = CreateDefaultSubobject<UBattleAttributeSet>(TEXT("AttributeSet"));

    // 背包数据组件（B 键背包系统的数据层）：默认读 /Game/UI/bag_sys/Data_tool
    InventoryComponent = CreateDefaultSubobject<UInventoryComponent>(TEXT("InventoryComponent"));

    // 默认伤害 GE（P1）：无蓝图覆盖时使用内置 UDamageGameplayEffect
    DamageEffectClass = UDamageGameplayEffect::StaticClass();

    // 默认无敌 GE（P3）：无蓝图覆盖时使用内置 UInvincibilityGameplayEffect
    InvincibilityEffectClass = UInvincibilityGameplayEffect::StaticClass();

    // 默认闪避 Ability（P3）：无蓝图覆盖时使用内置 UDodgeAbility
    DodgeAbilityClass = UDodgeAbility::StaticClass();

    // 默认中毒 GE（P4）：无蓝图覆盖时使用内置 UPoisonGameplayEffect
    PoisonEffectClass = UPoisonGameplayEffect::StaticClass();

    // 默认增伤 GE（P4）：无蓝图覆盖时使用内置 UDamageBoostGameplayEffect
    DamageBoostEffectClass = UDamageBoostGameplayEffect::StaticClass();

    // ---- 创建摄像机系统 ----
    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    CameraBoom->SetupAttachment(RootComponent);
    CameraBoom->TargetArmLength = 500.0f;
    CameraBoom->bUsePawnControlRotation = false;
    CameraBoom->bEnableCameraLag = true;
    // 拖尾速度 5 过慢，转身/移动时摄像机甩尾会扫过怪物模型内部（穿模观感）→ 提到 10（引擎默认值）
    CameraBoom->CameraLagSpeed = 10.0f;
    // 防穿模探测：沿摄像机臂做 ECC_Camera 碰撞检测，被挡住时自动把摄像机拉近。
    // 配合 MonsterBase 网格体阻挡 Camera 通道 → 怪物身体挡镜头时摄像机回拉，模型不再卡进镜头
    // 注意：若 BP_PlayerCharacter 里手动改过 CameraBoom 属性会以蓝图为准，请确认蓝图未关闭此项
    CameraBoom->bDoCollisionTest = true;
    // 探测球半径默认 12cm 太小，怪物肢体从探针旁擦过时漏检 → 加大到 30cm
    CameraBoom->ProbeSize = 30.0f;

    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
    FollowCamera->bUsePawnControlRotation = false;

    // ---- 【新增/警觉】让 Actor 朝向跟速度方向（上半身再在 ABP 中通过 Orientation Warping 朝目标）----
    // 不再让控制器/锁敌逻辑强行转 Actor，避免「整身转向」与「上半身朝向」打架
    bUseControllerRotationYaw = false;
    bUseControllerRotationPitch = false;
    bUseControllerRotationRoll = false;
    // ★ bOrientRotationToMovement 必须关闭：本项目移动方向由自研相机 CameraWorldYaw 决定，
    //   角色朝向由 UpdateMovement 的手动 SetActorRotation（最短路径）统一驱动。
    //   若开着 true，CharacterMovement 的 PhysicsRotation → ComputeOrientToMovementRotation
    //   会用 Acceleration 转向，而 Acceleration 来自蓝图 AddMovementInput（基于 GetControlRotation，
    //   但自研相机 bUsePawnControlRotation=false，ControlRotation 不跟随鼠标、恒为默认朝向）。
    //   相机朝 -x 时 CameraWorldYaw≈180° vs ControlRotation≈0°，两套转向方向相反 → 来回拉扯，
    //   表现为「角色不自觉转向某一边 + 速度降低」。关闭后转向由手动 SetActorRotation 独享。
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->RotationRate = FRotator(0.0f, 540.0f, 0.0f); // 转身速度 540 deg/s（CCW 风格）

    // 初始化摄像机角度
    CameraWorldYaw = 0.0f;
    CameraWorldPitch = -10.0f;

    // ---- 初始化跳跃 ----
    CurrentJumpCount = MaxJumpCount;
    GetCharacterMovement()->JumpZVelocity = JumpHeight;
    GetCharacterMovement()->AirControl = 0.8f;

    // ---- 【修复】实例化耐力条UI组件 ----
    StaminaBarWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("StaminaBarWidget"));
    // 绑定到摄像机：耐力条跟随画面而非角色
    StaminaBarWidget->SetupAttachment(FollowCamera);
    // 屏幕空间：DrawSize 为固定像素，摄像机缩放不影响大小
    StaminaBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
    StaminaBarWidget->SetDrawSize(StaminaBarDrawSize);
    StaminaBarWidget->SetHiddenInGame(true); // 默认隐藏，BeginPlay里再开启

    // 尝试加载耐力条UI类
    static ConstructorHelpers::FClassFinder<UUserWidget> StaminaBarClass(TEXT("Blueprint'/Game/UI/WBP_StaminaBar.WBP_StaminaBar_C'"));
    if (StaminaBarClass.Succeeded())
    {
        StaminaBarWidgetClass = StaminaBarClass.Class;
        UE_LOG(LogTemp, Warning, TEXT("StaminaBarWidgetClass loaded in constructor!"));
    }
    else
    {
        StaminaBarWidgetClass = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("StaminaBarWidgetClass NOT found in constructor!"));
    }

    // ---- 【新增】实例化血条UI组件 ----
    HealthBarWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("HealthBarWidget"));
    // 绑定到摄像机：血条跟随画面而非角色，视角旋转时与画面刚性锁定
    HealthBarWidget->SetupAttachment(FollowCamera);
    // 屏幕空间：DrawSize 为固定像素，摄像机缩放/视角旋转都不影响大小
    HealthBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
    HealthBarWidget->SetDrawSize(HealthBarDrawSize);

    // 尝试加载血条UI类（蓝图里创建 WBP_HealthBar 后自动加载）
    static ConstructorHelpers::FClassFinder<UUserWidget> HealthBarClass(TEXT("Blueprint'/Game/UI/WBP_HealthBar.WBP_HealthBar_C'"));
    if (HealthBarClass.Succeeded())
    {
        HealthBarWidgetClass = HealthBarClass.Class;
        UE_LOG(LogTemp, Warning, TEXT("HealthBarWidgetClass loaded in constructor!"));
    }
    else
    {
        HealthBarWidgetClass = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("HealthBarWidgetClass NOT found! Create WBP_HealthBar in Content/UI or set it in BP_PlayerCharacter."));
    }

    // 【重要】角色面板类（WBP_Character_imf）改为「首次打开面板时懒加载」。
    // 原因：ConstructorHelpers::FClassFinder 在构造函数里做同步 IO，对复杂 Widget 蓝图
    // 会在编辑器 CDO 构造阶段死锁（表现为编辑器卡在 72% 启动界面、主线程 CPU 0%）。
    // CharacterPanelClass 的真实赋值见 OpenCharacterPanel() 中的 EnsureCharacterPanelClassLoaded()。

    // 尝试加载中毒状态图标纹理（/Game/UI/tip_UI/state_UI/poisoning），可在角色蓝图覆盖
    static ConstructorHelpers::FObjectFinder<UTexture2D> PoisonIconFinder(TEXT("Texture2D'/Game/UI/tip_UI/state_UI/poisoning.poisoning'"));
    if (PoisonIconFinder.Succeeded())
    {
        PoisonIconTexture = PoisonIconFinder.Object;
        UE_LOG(LogTemp, Warning, TEXT("PoisonIconTexture loaded in constructor!"));
    }
    else
    {
        PoisonIconTexture = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("PoisonIconTexture NOT found! Set it in BP_PlayerCharacter (Health|Poison)."));
    }

    // 尝试加载大招增伤状态图标纹理（/Game/UI/tip_UI/state_UI/attack_up），可在角色蓝图覆盖
    static ConstructorHelpers::FObjectFinder<UTexture2D> UltimateBuffIconFinder(TEXT("Texture2D'/Game/UI/tip_UI/state_UI/attack_up.attack_up'"));
    if (UltimateBuffIconFinder.Succeeded())
    {
        UltimateBuffIconTexture = UltimateBuffIconFinder.Object;
        UE_LOG(LogTemp, Warning, TEXT("UltimateBuffIconTexture loaded in constructor!"));
    }
    else
    {
        UltimateBuffIconTexture = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("UltimateBuffIconTexture NOT found! Set it in BP_PlayerCharacter (Combat|Ultimate)."));
    }

    // 尝试加载吸血状态图标纹理（/Game/UI/tip_UI/state_UI/suck_blood），可在角色蓝图覆盖
    static ConstructorHelpers::FObjectFinder<UTexture2D> LifestealIconFinder(TEXT("Texture2D'/Game/UI/tip_UI/state_UI/suck_blood.suck_blood'"));
    if (LifestealIconFinder.Succeeded())
    {
        LifestealIconTexture = LifestealIconFinder.Object;
        UE_LOG(LogTemp, Warning, TEXT("LifestealIconTexture loaded in constructor!"));
    }
    else
    {
        LifestealIconTexture = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("LifestealIconTexture NOT found! Set it in BP_PlayerCharacter (Energy)."));
    }

    // 尝试加载护盾状态图标纹理（/Game/UI/tip_UI/state_UI/shield_up），可在角色蓝图覆盖
    static ConstructorHelpers::FObjectFinder<UTexture2D> ShieldIconFinder(TEXT("Texture2D'/Game/UI/tip_UI/state_UI/shield_up.shield_up'"));
    if (ShieldIconFinder.Succeeded())
    {
        ShieldIconTexture = ShieldIconFinder.Object;
        UE_LOG(LogTemp, Warning, TEXT("ShieldIconTexture loaded in constructor!"));
    }
    else
    {
        ShieldIconTexture = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("ShieldIconTexture NOT found! Set it in BP_PlayerCharacter (Health|Shield)."));
    }

    // ---- 受伤提示 UI：绑定相机（零偏移 → 永远位于屏幕中心，不受视野移动/缩放影响）----
    // 受到伤害时显示，HurtUIDisplayDuration 秒后隐藏直到下次受伤
    HurtUIWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("HurtUIWidget"));
    HurtUIWidget->SetupAttachment(FollowCamera);
    // 屏幕空间：固定像素尺寸，摄像机缩放不影响大小
    HurtUIWidget->SetWidgetSpace(EWidgetSpace::Screen);
    HurtUIWidget->SetDrawSize(HurtUIDrawSize);
    // 零偏移：控件与相机位置重合，投影永远落在屏幕中心，视角旋转/缩放均不改变其位置
    HurtUIWidget->SetRelativeLocation(FVector::ZeroVector);
    HurtUIWidget->SetHiddenInGame(true); // 默认隐藏，受伤时才显示

    // 尝试加载受伤提示UI类（蓝图里创建 WBP_HurtScreen 后自动加载——全屏闪红提示，
    // 通过 HurtUIWidget 实例绑定相机、锁屏幕中央；红闪内容做在 WBP_HurtScreen 里，
    // WBP_DamageWarning 是怪物头顶出伤预警用的，两者独立实例互不影响）
    static ConstructorHelpers::FClassFinder<UUserWidget> HurtUIClass(TEXT("Blueprint'/Game/UI/WBP_HurtScreen.WBP_HurtScreen_C'"));
    if (HurtUIClass.Succeeded())
    {
        HurtUIWidgetClass = HurtUIClass.Class;
        UE_LOG(LogTemp, Warning, TEXT("HurtUIWidgetClass loaded in constructor (WBP_HurtScreen)!"));
    }
    else
    {
        HurtUIWidgetClass = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("WBP_HurtScreen NOT found! Create it in /Content/UI (or set HurtUIWidgetClass in BP_PlayerCharacter)."));
    }

    // ---- 获得物品提示 UI：绑定相机（全屏画布：位置锁屏幕中心 + 绘制尺寸跟随视口）----
    // 「击败怪物 → 物品自动进背包 → 屏幕左侧弹出「物品名 ×N」」的提示列表做在这里。
    // 与受伤红屏同一套定位模式，但【默认可见】：它平时没有内容，自然什么都不显示，
    // 不需要按需显隐 —— 也就不会踩到「隐藏中的控件几何维度为 0」那类坑。
    ItemPickupTipWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("ItemPickupTipWidget"));
    ItemPickupTipWidget->SetupAttachment(FollowCamera);
    // 屏幕空间：固定像素尺寸，摄像机缩放/旋转不影响大小
    ItemPickupTipWidget->SetWidgetSpace(EWidgetSpace::Screen);
    ItemPickupTipWidget->SetDrawSize(ItemPickupTipDrawSize);
    // 零偏移：与相机位置重合，投影永远落在屏幕中心（实际尺寸每帧由 UpdateItemPickupTipPosition 同步）
    ItemPickupTipWidget->SetRelativeLocation(FVector::ZeroVector);

    // 控件类【不在构造函数里 FClassFinder 加载】—— 与 WBP_Character_imf 同一个坑：
    // 复杂 Widget 蓝图在 CDO 构造阶段做同步加载会死锁（编辑器卡在 72% 启动界面）。
    // 这里只创建组件，类在 BeginPlay 里按 ItemPickupTipWidgetPath 懒加载。

    // 尝试加载技能/大招图标UI类（蓝图里创建 WBP_Skill 后自动加载）
    static ConstructorHelpers::FClassFinder<UUserWidget> SkillIconClass(TEXT("Blueprint'/Game/UI/WBP_Skill.WBP_Skill_C'"));
    if (SkillIconClass.Succeeded())
    {
        SkillIconUIClass = SkillIconClass.Class;
        UE_LOG(LogTemp, Warning, TEXT("SkillIconUIClass loaded in constructor!"));
    }
    else
    {
        SkillIconUIClass = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("WBP_Skill NOT found! Create it in /Content/UI (or set SkillIconUIClass in BP_PlayerCharacter)."));
    }
}

UAbilitySystemComponent* ABattleCharacter::GetAbilitySystemComponent() const
{
    return AbilitySystemComponent;
}

void ABattleCharacter::BeginPlay()
{
    Super::BeginPlay();

    // ---- 记录 bOrientRotationToMovement 初值 ----
    // 供需要临时接管朝向的功能按初值恢复，否则会永久改掉蓝图配置的朝向行为。
    if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
    {
        bEnableOrientRotationToMovementDefault = MoveComp->bOrientRotationToMovement;
    }

    // ---- 普攻连段：解析段列表（段数 = ComboAttackSegments 元素个数）----
    // 放在最前面：它只依赖蓝图里填好的属性，不依赖武器/GAS/输入，
    // 早跑早出日志 —— 段数不对的时候第一眼就能看到，而不用等打怪才发现
    EnsureComboSegments();

    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            if (DefaultMappingContext)
            {
                Subsystem->AddMappingContext(DefaultMappingContext, 0);
            }
        }

        // 步行 Ctrl 输入兜底挂载：SetupPlayerInputComponent 阶段本地子系统若未就绪则在此补上（幂等）
        EnsureWalkToggleInput();

        // 背包 B 键输入兜底挂载（同样幂等）
        EnsureBagInput();
    }

    if (AbilitySystemComponent)
    {
        // ASC 绑定 Owner（角色自身）与 Avatar（角色自身），供属性复制与能力上下文使用
        AbilitySystemComponent->InitAbilityActorInfo(this, this);

        // 初始化 Attributes：把角色现有手写字段值写入 GAS 属性集
        // （P0 先做单向桥接，保证 GAS 属性与旧字段初值一致；后续逐阶段替换为 GameplayEffect 驱动）
        if (AttributeSet)
        {
            AttributeSet->InitMaxHealth(MaxHealth);
            AttributeSet->InitHealth(CurrentHealth);
            AttributeSet->InitMaxStamina(MaxStamina);
            AttributeSet->InitStamina(CurrentStamina);
            AttributeSet->InitMaxConcertoEnergy(MaxConcertoEnergy);
            AttributeSet->InitConcertoEnergy(ConcertoEnergy);
            // 攻击力：写入蓝图可配的 BaseAttack（技能倍率的伤害基数，GAS 唯一真源）
            AttributeSet->InitAttack(BaseAttack);
            // 暴击率/暴击伤害：写入蓝图可配的 BaseCritRate/BaseCritDamage（整数百分比，GAS 唯一真源）
            AttributeSet->InitCritRate(BaseCritRate);
            AttributeSet->InitCritDamage(BaseCritDamage);
        }

        // 授予大招 GameplayAbility（P2：外壳式封装样板）
        if (UltimateAbilityClass)
        {
            AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(
                UltimateAbilityClass, 1, INDEX_NONE, this));
            UE_LOG(LogTemp, Warning, TEXT("GAS: granted UltimateAbility."));
        }

        // 授予闪避 GameplayAbility（P3：外壳式封装）
        if (DodgeAbilityClass)
        {
            AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(
                DodgeAbilityClass, 1, INDEX_NONE, this));
            UE_LOG(LogTemp, Warning, TEXT("GAS: granted DodgeAbility."));
        }
    }

    // 记录基础移速（疾跑结束后恢复到该值）
    if (GetCharacterMovement())
    {
        BaseWalkSpeed = GetCharacterMovement()->MaxWalkSpeed;
    }

    // 计算耐力恢复速率
    if (MaxStamina > 0.0f && StaminaRegenTime > 0.0f)
    {
        StaminaRegenRate = MaxStamina / StaminaRegenTime;
        UE_LOG(LogTemp, Warning, TEXT("Stamina Regen Rate: %f per second"), StaminaRegenRate);    }

    // ---- 【新增】开局自动装备武器 ----
    if (DefaultWeaponClass)
    {
        EquipWeapon(DefaultWeaponClass);
    }

    // ---- 初始化耐力条UI ----
    if (StaminaBarWidget && StaminaBarWidgetClass)
    {
        // 将蓝图中设置的类赋值给组件
        StaminaBarWidget->SetWidgetClass(StaminaBarWidgetClass);
        StaminaBarWidget->InitWidget();

        // 【修改】强制使用屏幕空间绘制，固定像素尺寸
        // 屏幕空间Widget把组件的3D位置投影到屏幕上，以固定像素大小绘制：
        // - 大小恒定，不随摄像机缩放变化
        // - 位置由 UpdateStaminaBarPosition() 每帧设为屏幕中心+偏移，绑定摄像机视角
        StaminaBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
        StaminaBarWidget->SetDrawSize(StaminaBarDrawSize);

        // 【修改】定位到屏幕中心+偏移（绑定摄像机视角）
        UpdateStaminaBarPosition();

        // 【修改】初始隐藏耐力条，耐力变动后才显示
        LastStaminaValue = CurrentStamina;
        LastStaminaChangeTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
        SetStaminaBarVisible(false);

        UpdateStaminaBar();
        UE_LOG(LogTemp, Warning, TEXT("StaminaBarWidget initialized (Screen Space, fixed size, hidden until stamina changes)!"));
    }
    else
    {
        if (!StaminaBarWidget)
        {
            UE_LOG(LogTemp, Warning, TEXT("StaminaBarWidget is null!"));
        }
        else if (!StaminaBarWidgetClass)
            UE_LOG(LogTemp, Warning, TEXT("StaminaBarWidgetClass is not set in BP_PlayerCharacter!"));
    }

    // ---- 【新增】初始化血条UI ----
    CurrentHealth = MaxHealth;
    // GAS 化：同步 AttributeSet 的 Health 为满血（唯一真源）
    if (AttributeSet)
    {
        AttributeSet->InitHealth(MaxHealth);
        AttributeSet->InitMaxHealth(MaxHealth);
    }
    if (HealthBarWidget && HealthBarWidgetClass)
    {
        HealthBarWidget->SetWidgetClass(HealthBarWidgetClass);
        HealthBarWidget->InitWidget();

        // 强制屏幕空间绘制：固定像素尺寸，缩放/旋转不影响大小
        HealthBarWidget->SetWidgetSpace(EWidgetSpace::Screen);
        HealthBarWidget->SetDrawSize(HealthBarDrawSize);

        // 定位到屏幕底部居中（绑定摄像机视角）
        UpdateHealthBarPosition();

        UpdateHealthBar();
        // 能量进度条初始化为空（Passive_eng_1~4）
        UpdateEnergyBars();
        // 护盾进度条初始化为空（shield_bar）
        UpdateShieldBar();
        // 状态图标初始化为全隐藏（未获得任何状态时 state_01~04 均不可视）
        RefreshStatusIcons();
        UE_LOG(LogTemp, Warning, TEXT("HealthBarWidget initialized (Screen Space, bottom center)!"));
    }
    else
    {
        if (!HealthBarWidget)
        {
            UE_LOG(LogTemp, Warning, TEXT("HealthBarWidget is null!"));
        }
        else if (!HealthBarWidgetClass)
            UE_LOG(LogTemp, Warning, TEXT("HealthBarWidgetClass is not set! Create WBP_HealthBar in /Game/UI or set it in BP_PlayerCharacter."));
    }

    // 初始化摄像机到默认位置
    CameraWorldYaw = 0.0f;
    CameraWorldPitch = -10.0f;
    UpdateCameraRotation();

    // 记录玩家（蓝图）设定的相机臂长：缩放与推开偏移都基于该值
    if (CameraBoom)
    {
        UserCameraArmLength = CameraBoom->TargetArmLength;
    }

    // 记录 Mesh 基准相对变换：技能/大招结束后清除根运动旋转残留时恢复用
    if (USkeletalMeshComponent* MeshComp = GetMesh())
    {
        BaseMeshRelativeLocation = MeshComp->GetRelativeLocation();
        BaseMeshRelativeRotation = MeshComp->GetRelativeRotation();
    }

    // ---- 段根运动补丁：为技能/大招段动画序列启用根运动（修复段间位置回跳）----
    EnsureSegmentRootMotion();

    // ---- 初始化受伤提示 UI（保持隐藏，受伤时才显示）----
    if (HurtUIWidget)
    {
        if (HurtUIWidgetClass)
        {
            HurtUIWidget->SetWidgetClass(HurtUIWidgetClass);
            HurtUIWidget->InitWidget();
        }
        HurtUIWidget->SetWidgetSpace(EWidgetSpace::Screen);
        HurtUIWidget->SetDrawSize(HurtUIDrawSize);
        // 零偏移：与相机位置重合 → 投影永远落在屏幕中心，不受视野移动/缩放影响
        HurtUIWidget->SetRelativeLocation(FVector::ZeroVector);
        HurtUIWidget->SetHiddenInGame(true);

        UE_LOG(LogTemp, Warning, TEXT("Hurt UI widget initialized (class=%s, camera-locked center, hidden until damaged)."),
            HurtUIWidgetClass ? *HurtUIWidgetClass->GetName() : TEXT("NONE"));
    }

    // ---- 初始化获得物品提示 UI（全屏画布，常驻可见：没有内容时自然什么都不显示）----
    if (ItemPickupTipWidget)
    {
        // 控件类懒加载（构造函数里不加载 —— 见构造函数中的死锁说明）
        if (!ItemPickupTipWidgetClass && !ItemPickupTipWidgetPath.IsEmpty())
        {
            ItemPickupTipWidgetClass = LoadClass<UUserWidget>(nullptr, *ItemPickupTipWidgetPath);
        }

        // ---- ★ 自愈：这个组件的控件类必须是「列表容器」 ----
        // 它是一块 1920x1080 的画布，如果被指到「单条提示」控件（WBP_ItemTip，
        // 父类是 ItemTipWidget）上，那条提示的铺满式锚点会被拉伸到整屏，
        // 现象就是「一运行就有个大框占满屏幕」，而且它还会因为没人给它填数据而永远存在。
        // 单条提示永远是 WBP_ItemPickupTips 的 Tip Widget Class 来指定的，
        // 所以这里发现指错了就直接纠正回路径默认值。
        if (ItemPickupTipWidgetClass &&
            ItemPickupTipWidgetClass->IsChildOf(UItemTipWidget::StaticClass()))
        {
            // ★ 措辞顺序 2026-09-14 调整过，理由（这不是洁癖，是有实测依据的）：
            //   旧版第一句是「…而且它那套铺满式外观会被全屏画布拉伸到占满屏幕」——
            //   描述的是一个**被紧跟着的自愈逻辑阻止了、因而永远不会发生**的现象。
            //   实测：这段日志在启动日志里 10/10 次都出现（资产里确实填错、每次都被纠正），
            //   于是用户每次进 PIE 都读到「会占满屏幕」，然后去找一个根本不存在的大框。
            //   ⇒ 判据是「这条日志会让人做出什么动作？」——旧版会让人做出一个【不该做的动作】。
            //   现在第一句给结论（★ 影响：无 —— 已自动纠正），并且明说「不会发生」，
            //   最后才给「只为把资产改干净」的可选修法。
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] ★ 影响：无 —— 已自动纠正，这条只是提醒你把资产里那处改干净。\n"
                     "      资产里 Item Pickup Tip Widget Class 填的是「单条提示」控件（%s），"
                     "它不能当列表容器用（里面没有 VBox_ItemTips）。\n"
                     "      ★ 若不纠正会怎样：它那套铺满式外观会被全屏画布拉伸到占满屏幕 ——"
                     "但【不会发生】，因为下面已经把它改回路径默认值了。\n"
                     "      已自动改回路径默认值：%s\n"
                     "      实际生效的类请看紧随其后的那条 class= 日志（应为 WBP_ItemPickupTips_C）。\n"
                     "      修法（只为把资产改干净；不改也不影响运行）："
                     "打开 BP_PlayerCharacter → Class Defaults → UI → "
                     "Item Pickup Tip Widget Class → 清空（或改成 WBP_ItemPickupTips）；"
                     "单条提示是在 WBP_ItemPickupTips 的 Item|Tip → Tip Widget Class 里指定的。"),
                *ItemPickupTipWidgetClass->GetName(), *ItemPickupTipWidgetPath);

            ItemPickupTipWidgetClass = nullptr;
            if (!ItemPickupTipWidgetPath.IsEmpty())
            {
                ItemPickupTipWidgetClass = LoadClass<UUserWidget>(nullptr, *ItemPickupTipWidgetPath);
            }
        }

        if (ItemPickupTipWidgetClass)
        {
            ItemPickupTipWidget->SetWidgetClass(ItemPickupTipWidgetClass);
            ItemPickupTipWidget->InitWidget();
            ItemPickupTipWidget->SetWidgetSpace(EWidgetSpace::Screen);
            ItemPickupTipWidget->SetDrawSize(ItemPickupTipDrawSize);
            ItemPickupTipWidget->SetRelativeLocation(FVector::ZeroVector);
            UpdateItemPickupTipPosition();

            // 这个组件是一块「按视口尺寸撑开的空画布」，它本身不该显示任何东西 ——
            // 画面里出现的东西应该全部来自内部控件（VBox_ItemTips 里那些提示条）。
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] 获得物品提示组件已初始化：class=%s（应为列表容器 WBP_ItemPickupTips）"
                     " | DrawSize 跟随视口 | 位置锁屏幕中心。"),
                *ItemPickupTipWidgetClass->GetName());

            // ---- ★ 父类自检：这一步没做的话，整个提示功能是「哑」的 ----
            // 为什么必须在启动时就报：控件蓝图如果不继承 ItemPickupTipsWidget，
            // ABattleCharacter::NotifyItemObtained() 里的 Cast 会失败 ——
            // 掉落照常入包，但屏幕上永远不弹提示，而且**只有打死怪之后**才看得出来。
            // 这里提前把话说清楚，顺便给出唯一的修法。
            if (!ItemPickupTipWidgetClass->IsChildOf(UItemPickupTipsWidget::StaticClass()))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Drop] ★ 提示控件的父类没改：%s 没有继承 ItemPickupTipsWidget —— "
                         "掉落会正常入包，但屏幕上【永远不会】弹出获得提示。\n"
                         "      修法（两步，改完记得 Compile + Save）：\n"
                         "      1) 打开 WBP_ItemPickupTips → 右上角 Class Settings → "
                         "Parent Class → 搜 ItemPickupTipsWidget → 选中\n"
                         "      2) 同样把 WBP_ItemTip 的 Parent Class 改成 ItemTipWidget "
                         "（WBP_ItemPickupTips 的 Tip Widget Class 下拉框里才会出现 WBP_ItemTip）\n"
                         "      详细步骤见 output/drop_system/掉落系统制作步骤.md §3.1 / §3.2"),
                    *ItemPickupTipWidgetClass->GetName());
            }
        }
        else
        {
            // 这不算错误：还没做这个控件时掉落照样入包，只是不弹提示 —— 所以用 Log 而不是 Warning
            UE_LOG(LogTemp, Log,
                TEXT("[Drop] 未找到「获得物品提示」控件（%s）。掉落仍会正常进背包，只是不弹提示。"
                     "制作步骤见 output/drop_system/掉落系统制作步骤.md。"),
                *ItemPickupTipWidgetPath);
        }
    }

    // ---- 完美闪避残影材质：蓝图未指定时自动尝试加载 ----
    if (!PerfectDodgeGhostMaterial)
    {
        PerfectDodgeGhostMaterial = Cast<UMaterialInterface>(
            StaticLoadObject(UMaterialInterface::StaticClass(), this, TEXT("/Game/Effects/M_DodgeGhost.M_DodgeGhost")));
        if (!PerfectDodgeGhostMaterial)
        {
            PerfectDodgeGhostMaterial = Cast<UMaterialInterface>(
                StaticLoadObject(UMaterialInterface::StaticClass(), this, TEXT("/Game/M_DodgeGhost.M_DodgeGhost")));
        }
        if (!PerfectDodgeGhostMaterial)
        {
            UE_LOG(LogTemp, Warning, TEXT("M_DodgeGhost NOT found! Perfect dodge ghost effect disabled. ")
                TEXT("Create a translucent material at /Game/Effects/M_DodgeGhost (Unlit Translucent blue, ")
                TEXT("optional params: scalar GhostAlpha + vector GhostColor) or assign PerfectDodgeGhostMaterial in BP."));
        }
    }

    // ---- 初始化技能/大招图标 UI（常驻屏幕显示）----
    InitSkillIconUI();

    // ---- 切人系统收尾 ----
    // ★ 装备恢复（TryRestoreEquippedWeaponFromClaim）不在 BeginPlay 做：切人 spawn 的新角色
    //   BeginPlay 时背包数据还没从旧实例迁移过来（TransferPlayerStateTo 在 SpawnActor 之后），
    //   此时查背包必然查不到 → 会误判「那把武器已不可用」并把恢复记录清掉。
    //   恢复统一由切人流程在数据迁移完成后调用（ExecuteSwitchToRow 内）。
    //   右上角编队 HUD：每个上场角色建自己的，退役时随实例移除。
    //   切人 spawn 的新角色 BeginPlay 时还没被 Possess（Controller 空）→ 建不了，
    //   由切人流程在 Possess 之后补建（ExecuteSwitchToRow 里调 NewChar->BuildSwitchTeamHUD()）。
    if (Cast<APlayerController>(GetController()))
    {
        BuildSwitchTeamHUD();
    }

    // ---- ★ 在场角色实例注册表：初始角色（游戏开始 PlayerStart 生成、已 Possess）在此注册 ----
    // 切人 spawn 的新实例在 ExecuteSwitchToRow 里注册（那里已拿到 TargetRow 行名）；
    // 初始角色不走切人流程，只能在这里反查行名注册。注册表用于切人时判断「目标角色是否已
    // 有一份实例在场」——技能中切人旧角色留场放技能时，切回要归还操控权而非再 spawn 一份。
    {
        const FName MyRow = GetMyCharaRow();
        if (!MyRow.IsNone())
        {
            // 覆盖式 Add：同进程内若上次运行残留了同名行（理论上 EndPlay 已清，但防御覆盖）
            ActiveCharaInstances.Add(MyRow, this);
        }
    }
}

void ABattleCharacter::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // 更新冷却计时器
    if (CurrentAttackCooldown > 0.0f)
    {
        CurrentAttackCooldown -= DeltaTime;
    }
    if (CurrentSkillCooldown > 0.0f)
    {
        CurrentSkillCooldown -= DeltaTime;
    }
    if (CurrentUltimateCooldown > 0.0f)
    {
        CurrentUltimateCooldown -= DeltaTime;
    }
    if (CurrentEnergySkillCooldown > 0.0f)
    {
        CurrentEnergySkillCooldown -= DeltaTime;
    }

    // 中毒状态 Tick 驱动：递减剩余时间 + 按间隔结算持续毒伤
    UpdatePoison(DeltaTime);

    // 同步技能/大招冷却倒计时 UI（冷却中显示剩余秒数，未冷却隐藏）
    UpdateSkillIconCooldownUI();

    // 能量进度条帧驱动同步（格数未变时一次整型比较即返回，零开销；
    // 保证任何路径清空/增加能量后 UI 当帧刷新，不依赖事件调用）
    UpdateEnergyBars();

    // 完美闪避后的闪避锁定计时
    if (CurrentDodgeLockout > 0.0f)
    {
        CurrentDodgeLockout -= DeltaTime;
        if (CurrentDodgeLockout < 0.0f)
        {
            CurrentDodgeLockout = 0.0f;
        }
    }

    // 递减弹刀冷却
    if (CurrentParryCooldown > 0.0f)
    {
        CurrentParryCooldown -= DeltaTime;
        if (CurrentParryCooldown < 0.0f)
        {
            CurrentParryCooldown = 0.0f;
        }
    }

    // 弹刀镜头拉近保持时间递减（归零后由 UpdateCameraPushback 平滑恢复原距离）
    if (ParryCameraZoomTimeRemaining > 0.0f)
    {
        ParryCameraZoomTimeRemaining -= DeltaTime;
        if (ParryCameraZoomTimeRemaining < 0.0f)
        {
            ParryCameraZoomTimeRemaining = 0.0f;
        }
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

    // 受击硬直计时：与蒙太奇播放状态解耦，HitReactionDuration 时间到即解除操作锁定。
    // 站立阶段（bIsGettingUp）由下方独立分支驱动，这里必须排除——否则站立阶段每帧
    // 都会因 HitReactionTimeRemaining<=0 进入本分支并被 EndHitReaction 截停站立蒙太奇。
    // 条件含 bIsKnockback：击飞僵直开关（bKnockbackLockInput）关闭时 bIsHitReaction=false
    // 但击飞计时仍需推进，否则击飞状态永远无法流转到站立/解锁
    if ((bIsHitReaction || bIsKnockback) && !bIsGettingUp)
    {
        HitReactionTimeRemaining -= DeltaTime;
        if (HitReactionTimeRemaining <= 0.0f)
        {
            // 击飞硬直结束 → 若配置了站立蒙太奇，进入站立阶段（等待 GetupDelay + 播放站立）；
            // 否则（普通受击 / 未配置站立蒙太奇）直接解锁
            if (bIsKnockback && GetupMontage)
            {
                PlayGetupReaction();
            }
            else
            {
                EndHitReaction();
            }
        }
    }

    // 站立阶段计时：先等待 GetupDelay（蒙太奇尚未播放，角色保持躺地僵直），
    // 延迟结束后开始播放站立蒙太奇并按蒙太奇时长/播放速率重新计时，
    // 播完后解除操作锁定（Tick 计时驱动，不依赖蒙太奇结束回调）
    if (bIsGettingUp)
    {
        GetupTimeRemaining -= DeltaTime;
        if (GetupTimeRemaining <= 0.0f)
        {
            if (!bGetupMontageStarted)
            {
                // 击飞→站立自定义延迟结束：开始播放站立蒙太奇并重新计时
                bGetupMontageStarted = true;
                if (GetupMontage && GetMesh())
                {
                    if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
                    {
                        CurrentHitReactionMontage = GetupMontage;
                        AnimInstance->Montage_Play(GetupMontage, GetupMontagePlayRate);
                    }
                }
                // 站立时长 = 蒙太奇长度 / 播放速率（未配置蒙太奇时直接结束）
                GetupTimeRemaining = GetupMontage
                    ? GetupMontage->GetPlayLength() / FMath::Max(0.05f, GetupMontagePlayRate)
                    : 0.0f;
                if (GetupTimeRemaining <= 0.0f)
                {
                    EndHitReaction();
                }
            }
            else
            {
                EndHitReaction();
            }
        }
    }

    // 技能/大招施放期间：联动索敌机制，自动平滑转向范围内最近怪物
    UpdateSkillAim(DeltaTime);

    // 索敌锁定系统：维护唯一索敌目标（最近优先/更近者持续换锁）+ 刷新追踪 UI
    UpdateLockOn(DeltaTime);

    // 警觉朝向：每帧根据"锁定目标在范围内 + 不在 Sprint + 不在战斗动作中"刷新 YawOffset
    UpdateUpperBodyFacingTarget(DeltaTime);

    // 多段蒙太奇技能/大招：段推进兜底 + 攻击窗口内的接触伤害结算
    UpdateSkillSegmentCombat(DeltaTime);
    UpdateUltimateSegmentCombat(DeltaTime);
    UpdateEnergySkillCombat(DeltaTime);

    // ---- 普攻旋转守卫：普攻结束后的守卫窗口内，无任何蒙太奇播放时逐帧清除 ----
    // 根运动残留的 Pitch/Roll（覆盖 BlendOut 混合输出期；连段中下一蒙太奇已在播则跳过，
    // 避免打断进行中的根运动旋转）
    if (AttackRotationGuardRemaining > 0.0f)
    {
        AttackRotationGuardRemaining -= DeltaTime;
        UAnimInstance* GuardAnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
        if (!GuardAnimInst || !GuardAnimInst->IsAnyMontagePlaying())
        {
            ResetMontageRotation();
        }
    }

    // ---- 命中镜头振动：延迟倒计时 → 正弦衰减左右振动 → 归零复位 ----
    UpdateHitCameraShake(DeltaTime);

    // ---- 切人 HUD：CD 遮罩/秒数、当前操控高亮框每帧刷新 ----
    UpdateSwitchTeamHUD();

    // 技能/大招/能量技施放期间：武器持续显形（每帧刷新强制显形时长）
    if ((bIsSkillCasting || bIsUltimateCasting || bIsEnergySkillCasting) && CurrentWeapon)
    {
        CurrentWeapon->ShowForCombatAction(0.3f);
    }

    // ---- 普攻蒙太奇播放期间：武器持续显形（修复「最后一段武器隐形」）----
    // 近战武器仅靠 BeginAttack 起始瞬间刷新 LastAttackTime，长段（尤其收尾段）
    // 动画播放超过 IdleHideDelay(0.8s) 后会被闲置隐藏逻辑误判为「已闲置」而隐藏武器。
    // 非收尾段因连段衔接快、每段起始都重新 BeginAttack 刷新，故正常；收尾段动画较长
    // 且【后面没有下一段来刷新】→ 武器被提前隐藏。（段数可配之后，"收尾段"就是段列表的最后一段。）
    // 此处让普攻蒙太奇播放期间每帧刷新强制显形时长，与技能/大招同一机制，
    // 彻底消除「长段收尾武器消失」—— 且无论配几段都成立。
    if (!bIsSkillCasting && !bIsUltimateCasting && !bIsEnergySkillCasting
        && IsAttackMontagePlaying() && CurrentWeapon)
    {
        CurrentWeapon->ShowForCombatAction(0.3f);
    }

    // ---- 疾跑：持续消耗耐力 + 限时 + 受击打断 ----
    if (bIsSprinting)
    {
        CurrentStamina -= SprintStaminaDrainPerSecond * DeltaTime;
        SprintElapsedTime += DeltaTime;

        // 停止条件：耐力耗尽 / 超过最大持续时间 / 松开方向键停止移动
        if (CurrentStamina <= 0.0f || SprintElapsedTime >= SprintDuration || !bIsMovingInput)
        {
            CurrentStamina = FMath::Max(CurrentStamina, 0.0f);
            StopSprint();
        }
    }
    // 耐力恢复（疾跑中不回复）
    else if (CurrentStamina < MaxStamina)
    {
        CurrentStamina = FMath::Min(CurrentStamina + StaminaRegenRate * DeltaTime, MaxStamina);
    }

    // ---- 【新增】耐力条显隐检测 ----
    UpdateStaminaBarVisibility();

    // ---- 连击计时器更新 ----
    ComboTimer += DeltaTime;
    if (ComboTimer > ComboResetTime && CurrentComboStep > 0)
    {
        ResetCombo();
        UE_LOG(LogTemp, Warning, TEXT("Combo reset due to timeout!"));
    }

    // ---- 摄像机平滑回正 ----
    if (bIsResettingCamera && CameraBoom)
    {
        float YawDiff = FMath::FindDeltaAngleDegrees(CameraWorldYaw, TargetYaw);
        float PitchDiff = TargetPitch - CameraWorldPitch;

        if (FMath::Abs(YawDiff) < 0.5f && FMath::Abs(PitchDiff) < 0.5f)
        {
            CameraWorldYaw = TargetYaw;
            CameraWorldPitch = TargetPitch;
            bIsResettingCamera = false;
            UpdateCameraRotation();
            UE_LOG(LogTemp, Warning, TEXT("Camera reset complete!"));
        }
        else
        {
            float Speed = CameraResetSpeed * DeltaTime;
            CameraWorldYaw += YawDiff * FMath::Min(Speed, 1.0f);
            CameraWorldPitch += PitchDiff * FMath::Min(Speed, 1.0f);
            UpdateCameraRotation();
        }
    }

    // ---- 步行速度锁定：步行期间每帧强制 MaxWalkSpeed = 基础移速 × 倍率 ----
    // 保证任何时机（闪避/受击恢复/落地等之后）速度都不会被其他逻辑改掉，直到再次按 Ctrl 退出
    if (bWalking && GetCharacterMovement())
    {
        // 步行速度完全等于蓝图 WalkSpeed（自由调整），每帧锁定防止闪避/受击恢复等改掉
        GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;

        // 步行动画速率倍率：每帧同步，防止受击/闪避/起身等把 GlobalAnimRateScale 重置
        if (GetMesh())
        {
            GetMesh()->GlobalAnimRateScale = FMath::Max(0.01f, WalkAnimPlayRate);
        }
    }

    // 更新移动
    UpdateMovement(DeltaTime);

    // ---- 相机防贴脸推开检测（怪物贴太近时把摄像机往远处推，恢复后自动收回）----
    UpdateCameraPushback(DeltaTime);

    // 更新摄像机
    UpdateCameraRotation();

    // ---- 更新耐力条位置（绑定摄像机视角，屏幕中心+偏移）----
    UpdateStaminaBarPosition();

    // ---- 更新血条位置（绑定摄像机视角，屏幕底部居中）----
    UpdateHealthBarPosition();

    // ---- 更新受伤提示位置（绑定摄像机视角，全屏覆盖：锁屏幕中心 + 绘制尺寸跟随视口）----
    UpdateHurtUIPosition();

    // ---- 更新获得物品提示位置（同上一套：全屏画布，提示条目按控件内锚点定位）----
    UpdateItemPickupTipPosition();

    // ---- 更新耐力条 ----
    UpdateStaminaBar();

    // ---- 完美闪避飘字动画 ----
    // ---- Viewport resize detection: refresh screen-space UI widgets ----
    // (Known UE issue: Screen Space WidgetComponents vanish after viewport resize)
    CheckViewportResize();

    UpdatePerfectDodgeText(DeltaTime);

    // ---- 完美闪避蓝色残影：按间隔生成直到时长耗尽 ----
    UpdatePerfectDodgeGhosts(DeltaTime);
}

void ABattleCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        if (LightAttackAction)
        {
            EnhancedInput->BindAction(LightAttackAction, ETriggerEvent::Started, this, &ABattleCharacter::LightAttack);
        }
        if (DodgeAction)
        {
            EnhancedInput->BindAction(DodgeAction, ETriggerEvent::Started, this, &ABattleCharacter::Dodge);
        }
        if (SkillAction)
        {
            EnhancedInput->BindAction(SkillAction, ETriggerEvent::Started, this, &ABattleCharacter::Skill);
        }
        if (UltimateAction)
        {
            EnhancedInput->BindAction(UltimateAction, ETriggerEvent::Started, this, &ABattleCharacter::Ultimate);
        }
        if (EnergySkillAction)
        {
            EnhancedInput->BindAction(EnergySkillAction, ETriggerEvent::Started, this, &ABattleCharacter::EnergySkill);
        }
        if (JumpAction)
        {
            EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this, &ABattleCharacter::PerformJump);
        }
        if (ResetCameraAction)
        {
            EnhancedInput->BindAction(ResetCameraAction, ETriggerEvent::Started, this, &ABattleCharacter::SetCameraToBackView);
        }
        if (SprintAction)
        {
            // 点击触发疾跑（松开不结束，直到耐力耗尽或停止移动）
            EnhancedInput->BindAction(SprintAction, ETriggerEvent::Started, this, &ABattleCharacter::StartSprint);
        }
        if (CharacterPanelAction)
        {
            // ★ 角色面板打开后会 SetGamePaused(true)，游戏处于暂停态；若不设 bTriggerWhenPaused，
            //   暂停后 C 键的 Started 事件不再触发 → 面板打不开关不掉。与 B/L 键对齐，显式允许暂停时触发。
            //   （这也是「切到非 BP_PlayerCharacter 角色后关不掉」的兜底：Esc_butt 按钮在 WBP 蓝图里
            //   硬编码 Cast 到 BP_PlayerCharacter，切到 BP_Chara_magic 后 Cast 失败、CloseCharacterPanel 不调；
            //   而 C 键走 ToggleCharacterPanel 不依赖那个 Cast，只要能在暂停态触发就能关。）
            CharacterPanelAction->bTriggerWhenPaused = true;
            // C 键切换角色面板（打开/关闭）
            EnhancedInput->BindAction(CharacterPanelAction, ETriggerEvent::Started, this, &ABattleCharacter::ToggleCharacterPanel);
        }
        // 步行切换（Ctrl）：运行时创建输入动作并绑定；UInputTriggerPressed → Triggered 仅在按下沿触发一次
        if (EnsureWalkToggleInput() && WalkToggleAction)
        {
            EnhancedInput->BindAction(WalkToggleAction, ETriggerEvent::Triggered, this, &ABattleCharacter::ToggleWalkState);
        }

        // 背包（B）：运行时创建输入动作并绑定（带 bTriggerWhenPaused，暂停时仍可关闭背包）
        if (EnsureBagInput() && BagAction)
        {
            EnhancedInput->BindAction(BagAction, ETriggerEvent::Triggered, this, &ABattleCharacter::ToggleBag);
        }

        // 角色编队（L）：运行时创建输入动作并绑定（带 bTriggerWhenPaused，暂停时仍可关闭编队）
        if (EnsureCharaTeamInput() && CharaTeamAction)
        {
            EnhancedInput->BindAction(CharaTeamAction, ETriggerEvent::Triggered, this, &ABattleCharacter::ToggleCharaTeamUI);
        }

        // ---- 切人（1/2/3 键）：运行时创建 IA/IMC 并绑定（与背包 B 键同一套 Ensure 幂等模式）----
        // ★ 绑定放在 SetupPlayerInputComponent（Possess 时必调）而不是 BeginPlay：
        //   切人 spawn 的新角色 BeginPlay 时还没被 Possess（Controller 为空），当时绑不上；
        //   Possess 后这里会重跑 → 每个上场角色都自动带上 1/2/3 键。
        if (EnsureSwitchInput())
        {
            if (SwitchSlotAction1)
            {
                EnhancedInput->BindAction(SwitchSlotAction1, ETriggerEvent::Started, this, &ABattleCharacter::OnSwitchSlot1Pressed);
            }
            if (SwitchSlotAction2)
            {
                EnhancedInput->BindAction(SwitchSlotAction2, ETriggerEvent::Started, this, &ABattleCharacter::OnSwitchSlot2Pressed);
            }
            if (SwitchSlotAction3)
            {
                EnhancedInput->BindAction(SwitchSlotAction3, ETriggerEvent::Started, this, &ABattleCharacter::OnSwitchSlot3Pressed);
            }
        }
    }

    // ★ 显式置位：本次 SetupPlayerInputComponent 已执行完毕（无论上面走了几条绑定分支）。
    //   EnsurePlayerInputBound 以此标志判断「是否已绑」，而不是拿 InputComponent 非空去猜。
    bPlayerInputBound = true;
}

// ---- 角色面板（C 键）----
// 打开/关闭角色面板：切换开关。由 C 键输入调用。
void ABattleCharacter::ToggleCharacterPanel()
{
    if (bCharacterPanelOpen)
    {
        CloseCharacterPanel();
    }
    else
    {
        OpenCharacterPanel();
    }
}

// 打开角色面板：创建并显示 WBP_Character_imf + 暂停游戏 + 显示鼠标（UI Only 输入模式）。
// 暂停不会打断角色正在播放的动画蒙太奇——蒙太奇随角色 Tick 驱动，暂停时冻结在当前位置，
// 恢复后继续播放，符合「不会打断现有动作」的要求。
void ABattleCharacter::OpenCharacterPanel()
{
    if (bCharacterPanelOpen)
        return;

    // 与背包互斥：打开角色面板时先收起背包，避免两个全屏 UI 叠在一起
    if (bBagOpen)
    {
        // ★ 重入保护：CloseBag() 末尾有「从武器背包退出 → 自动回武器页」的逻辑，
        //   不拦一下它会在本函数内部再调一次 OpenCharacterPanel()：
        //   AddToViewport 被调两次，而且内层末尾的「默认选中角色页」会把武器页刷掉。
        //   加了标志后，CloseBag 里的自动回页会主动跳过，由外层（本次调用）开一次面板即可。
        bOpeningCharacterPanel = true;
        CloseBag();
        bOpeningCharacterPanel = false;
    }

    // 面板类懒加载：首次按 C 键时才加载 WBP_Character_imf。
    // 避免构造函数中的同步 IO —— 那是编辑器卡在 72% 启动界面的根因。
    if (!CharacterPanelClass)
    {
        CharacterPanelClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_Character_imf.WBP_Character_imf_C"));
        if (CharacterPanelClass)
        {
            UE_LOG(LogTemp, Warning, TEXT("CharacterPanelClass lazy-loaded: /Game/UI/WBP_Character_imf"));
        }
    }

    // 无面板类：仅提示，不进入面板状态
    if (!CharacterPanelClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("CharacterPanelClass is not set! Create WBP_Character_imf in /Game/UI or set it in BP_PlayerCharacter."));
        return;
    }

    // 创建面板控件（每次打开重建，关闭时销毁，保证数据最新）
    if (!CharacterPanelWidget)
    {
        CharacterPanelWidget = CreateWidget<UUserWidget>(GetWorld(), CharacterPanelClass);
    }

    if (CharacterPanelWidget)
    {
        CharacterPanelWidget->AddToViewport(100); // 最高层：角色面板覆盖所有 HUD
    }

    bCharacterPanelOpen = true;

    // 绑定 mod_butt 四个页签按钮，并默认选中「角色」页签（change_chara）
    BindCharacterPanelTabs();

    // 绑定武器页 arm_imfor 里的「打开武器背包」按钮
    // （面板每次打开都重建 Widget，所以每次都要重新绑 —— 与 BindCharacterPanelTabs 同理）
    BindArmPanelButtons();

    // 重建 chara_pitc_head 下的头像按钮：按「拥有的角色」动态生成（含选中框）。
    // 容器存在 → 清空 WBP 里硬编码的 chara_1/chara_2、换成动态按钮；
    // 容器不存在（WBP 被改动）→ 回退旧的 BindCharacterSlotButtons() 绑定硬编码按钮。
    RebuildCharaHeadList();

    // 恢复选中槽位：优先沿用【上次选中的角色】（CurrentCharaSlotIndex），
    // 否则默认选中第 0 个槽位。数据表有角色 / 旧数组有槽位才走槽位选择；都没有 → 显示自身（this）。
    //
    // ★ 为什么不能无条件 ApplyCharacterSlot(0)：
    //   关闭武器背包会自动「重新打开面板 → 回武器页」（CloseBag 末尾），若这里强制切回槽 0，
    //   玩家在武器背包里给「法师(BP_Chara_magic)」挑武器、关掉背包后，右侧选择会跳回
    //   槽 0 的 BP_PlayerCharacter —— 看着像「选择被重置了」。
    //   保留 CurrentCharaSlotIndex 即可让「打开武器背包 → 关闭 → 回面板」全程盯在同一个角色上。
    if (GetOwnedCharaRows().Num() > 0 || CharacterSlotClasses.Num() > 0)
    {
        const int32 RestoreSlot = (CurrentCharaSlotIndex >= 0) ? CurrentCharaSlotIndex : 0;
        ApplyCharacterSlot(RestoreSlot);
    }
    else
    {
        CurrentCharaSlotIndex = -1;
        UpdateCharacterPanelStats(); // 显示自身实时属性
    }

    SwitchCharacterPanelTab(static_cast<int32>(ECharacterPanelTab::Chara));

    // 暂停游戏（不会打断蒙太奇，恢复后继续）
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        UGameplayStatics::SetGamePaused(GetWorld(), true);

        // 显示并操纵鼠标：UI Only 输入模式（鼠标可点击面板按钮，键盘输入被屏蔽）
        PC->bShowMouseCursor = true;
        FInputModeUIOnly InputMode;
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PC->SetInputMode(InputMode);
    }

    UE_LOG(LogTemp, Warning, TEXT("Character panel opened (game paused, mouse shown)."));
}

// 关闭角色面板：恢复游戏 + 隐藏鼠标（恢复 Game 输入模式）。
// BlueprintCallable：供 WBP_Character_imf 的 Esc_butt 按钮 OnClicked 调用。
void ABattleCharacter::CloseCharacterPanel()
{
    if (!bCharacterPanelOpen)
        return;

    // 销毁面板控件
    if (CharacterPanelWidget)
    {
        CharacterPanelWidget->RemoveFromParent();
        CharacterPanelWidget = nullptr;
    }

    bCharacterPanelOpen = false;

    // 恢复游戏 + 隐藏鼠标（恢复 Game And UI 输入模式，保留键盘/鼠标操作）
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        UGameplayStatics::SetGamePaused(GetWorld(), false);

        PC->bShowMouseCursor = false;
        FInputModeGameOnly InputMode;
        PC->SetInputMode(InputMode);
    }

    UE_LOG(LogTemp, Warning, TEXT("Character panel closed (game resumed, mouse hidden)."));
}

// =====================================================================
// ---- 背包系统（B 键）----
// =====================================================================
// 与角色面板（C 键）同构：打开即暂停游戏 + 显示鼠标，关闭恢复。
// 区别：背包用 GameAndUI 输入模式（而非 UIOnly），配合 BagAction->bTriggerWhenPaused，
// 使 B 键在暂停状态下仍能关闭背包；鼠标依然可以自由操作 UI。

// 惰性创建 B 键输入：运行时自建 UInputAction + UInputMappingContext 并挂到本地子系统，
// 无需在编辑器创建 IA/IMC 资产或改动 IMC_Player。
bool ABattleCharacter::EnsureBagInput()
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return false;

    UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
    if (!Subsystem)
        return false;

    if (!BagAction)
    {
        BagAction = NewObject<UInputAction>(this);
        BagAction->ValueType = EInputActionValueType::Boolean;
        // 显式“按下”触发器：Triggered 只在按下沿触发一次（按住不会反复开关）
        BagAction->Triggers.Add(NewObject<UInputTriggerPressed>(BagAction));
        // 关键：背包打开后世界处于暂停状态，必须允许暂停时触发，否则 B 键关不掉背包
        BagAction->bTriggerWhenPaused = true;
    }

    if (!BagMappingContext)
    {
        BagMappingContext = NewObject<UInputMappingContext>(this);
        BagMappingContext->MapKey(BagAction, EKeys::B);
    }

    if (!bBagContextAdded)
    {
        // 优先级 2（高于步行 Ctrl 的 1，更高于是蓝图默认 0）：B 键未被其他动作占用
        Subsystem->AddMappingContext(BagMappingContext, 2);
        bBagContextAdded = true;
    }

    return true;
}

// 开关背包：打开?关闭
void ABattleCharacter::ToggleBag()
{
    if (bBagOpen)
    {
        CloseBag();
    }
    else
    {
        OpenBag();
    }
}

// 打开背包：创建 WBP_Bag → 生成全部格子 → 读表填充 → 暂停游戏 + 显示鼠标
void ABattleCharacter::OpenBag()
{
    if (bBagOpen)
        return;

    // 与角色面板互斥：打开背包时先收起角色面板，避免两个全屏 UI 叠在一起
    if (bCharacterPanelOpen)
    {
        CloseCharacterPanel();
    }

    // 控件类懒加载：首次按 B 才加载，避免构造函数里的同步 IO（那是编辑器卡 72% 的根因）
    if (!BagWidgetClass)
    {
        BagWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/bag_sys/WBP_Bag.WBP_Bag_C"));
    }
    if (!BagSlotWidgetClass)
    {
        BagSlotWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/bag_sys/WBP_Bag_Slot.WBP_Bag_Slot_C"));
    }

    if (!BagWidgetClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 找不到 WBP_Bag（预期 /Game/UI/bag_sys/WBP_Bag）。请先创建该控件，或在 BP_PlayerCharacter 的 Bag|UI 分类里手动指定。"));
        return;
    }

    if (!BagWidget)
    {
        BagWidget = CreateWidget<UUserWidget>(GetWorld(), BagWidgetClass);
    }
    if (!BagWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Bag] CreateWidget 失败（WBP_Bag）。"));
        return;
    }

    BagWidget->AddToViewport(120); // Z 序高于角色面板(100)

    bBagOpen = true;

    // 默认分类：-1 = 全部（想默认选中第一个分类改成 0 即可）
    CurrentBagKind = -1;
    SelectedBagIndex = -1;
    // 每次打开背包都默认选中第一个物品（具体落地在 RefreshBagInternal 里统一处理）
    bBagSelectFirstPending = true;

    // 尺寸自检状态：每次打开都重置，否则上一轮的结论会污染这一轮
    // （旧版的「已尝试修复」标志就是从不重置的 —— 一次误判会永久生效）
    BagGridSampleTries = 0;
    bBagGridStretchHandled = false;
    bBagGridCrossAxisFixed = false;

    // 顺序要求：先生成格子（会清空 BagActions），再绑定分类/关闭/使用按钮
    BuildBagGrid();
    BindBagButtons();
    RefreshBagInternal(true);

    // 暂停游戏（蒙太奇/怪物动画冻结在当前帧，恢复后继续，不会被打断）+ 显示鼠标
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        UGameplayStatics::SetGamePaused(GetWorld(), true);

        PC->bShowMouseCursor = true;
        // 暂停期间屏蔽移动/视角输入（UI 之外的点击不再驱动角色）
        PC->SetIgnoreMoveInput(true);
        PC->SetIgnoreLookInput(true);

        // GameAndUI：鼠标可自由点击 UI，同时保留键盘（B 键）用于关闭
        FInputModeGameAndUI InputMode;
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        InputMode.SetHideCursorDuringCapture(false);
        PC->SetInputMode(InputMode);
    }

    UE_LOG(LogTemp, Warning, TEXT("[Bag] 背包已打开（暂停游戏 + 显示鼠标）。"));
}

// 关闭背包：销毁控件 + 恢复游戏 + 隐藏鼠标。
// BlueprintCallable：供 WBP_Bag 的 btn_close 按钮 OnClicked 调用。
void ABattleCharacter::CloseBag()
{
    if (!bBagOpen)
        return;

    if (BagWidget)
    {
        BagWidget->RemoveFromParent();
        BagWidget = nullptr;
    }

    BagSlotWidgets.Reset();
    BagActions.Reset();
    CurrentBagItems.Reset();
    SelectedBagIndex = -1;
    bBagSelectFirstPending = false;

    bBagOpen = false;

    // ---- 武器背包的收尾：退出武器模式 + 自动回到角色面板的武器页 ----
    // 放在这里而不是 CloseWeaponBag 里，是为了让三条关闭路径（B 键 / btn_close / 武器页按钮）
    // 行为完全一致 —— 否则用 B 键关掉武器背包就不会回到武器页，玩家会以为「切换没生效」。
    const bool bWasWeaponMode = bBagInWeaponMode;
    bBagInWeaponMode = false;

    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        UGameplayStatics::SetGamePaused(GetWorld(), false);

        PC->SetIgnoreMoveInput(false);
        PC->SetIgnoreLookInput(false);
        PC->bShowMouseCursor = false;

        FInputModeGameOnly InputMode;
        PC->SetInputMode(InputMode);
    }

    UE_LOG(LogTemp, Warning, TEXT("[Bag] 背包已关闭（恢复游戏 + 隐藏鼠标）。"));

    // 从武器背包进来的 → 关掉后自动回到角色面板的武器页，让玩家立刻看到切换结果。
    // 这就是「切换武器 + 与面板同步」这条需求的闭环最后一环。
    if (bWasWeaponMode && bReopenPanelAfterWeaponBag && !bCharacterPanelOpen)
    {
        if (bOpeningCharacterPanel)
        {
            // 重入保护：此刻是 OpenCharacterPanel 自己在「先收起背包」，
            // 我们若在这里再开一次面板就会 AddToViewport 两次、页签被重置回角色页。
            // 由外层那一次调用把面板开好即可，这里只说明为什么跳过。
            UE_LOG(LogTemp, Log,
                TEXT("[Arm] 面板正在打开流程中 → 跳过「自动回武器页」（按 C 打开面板时默认停在角色页）。"));
        }
        else
        {
            OpenCharacterPanel();
            if (bCharacterPanelOpen)
            {
                SwitchCharacterPanelTab(static_cast<int32>(ECharacterPanelTab::Arm));
            }
            UE_LOG(LogTemp, Log, TEXT("[Arm] 已从武器背包返回角色面板的武器页。"));
        }
    }
}

// 重新读表并全量刷新（蓝图可调用）
void ABattleCharacter::RefreshBag()
{
    RefreshBagInternal(true);
}

// 切换当前分类（Kind < 0 = 全部）
void ABattleCharacter::SetBagKind(int32 Kind)
{
    CurrentBagKind = Kind;
    SelectedBagIndex = -1;
    // 切换分类后同样默认选中第一个物品（该分类没有物品时保持未选中）
    bBagSelectFirstPending = true;
    RefreshBagInternal(false);
    UE_LOG(LogTemp, Log, TEXT("[Bag] 切换分类：%d"), Kind);
}

UInventoryComponent* ABattleCharacter::GetInventory() const
{
    return InventoryComponent;
}

// ---- 获得物品通知：优先推给 C++ 提示控件，其次推给实现了 IItemPickupListener 的 UI ----
void ABattleCharacter::NotifyItemObtained(const FBagItemEntry& Item, int32 ObtainedCount)
{
    // 遍历挂在角色身上的所有 WidgetComponent。
    // 为什么是「遍历」而不是只找某一个组件：提示的载体由蓝图决定 —— 只要某个组件里
    // 装的控件是 ItemPickupTipsWidget 的子类（或实现了接口），就照样能收到，C++ 不用改。
    TArray<UWidgetComponent*> WidgetComps;
    GetComponents<UWidgetComponent>(WidgetComps);

    int32 DeliveredCount = 0;

    // 「名字像提示容器、但类不对」的控件名 —— 这是「父类忘了改」的典型特征。
    // 单独收集起来，好在下面的报错里给出准确方向，而不是让人去猜。
    TArray<FString> SuspectClassNames;

    for (UWidgetComponent* Comp : WidgetComps)
    {
        UUserWidget* Widget = Comp ? Comp->GetWidget() : nullptr;
        if (!Widget)
        {
            continue;
        }

        // 不是提示容器的控件一律跳过；但名字里带 ItemPickupTips 的记一笔
        if (!Widget->GetClass()->IsChildOf(UItemPickupTipsWidget::StaticClass())
            && !Widget->Implements<UItemPickupListener>())
        {
            if (Widget->GetClass()->GetName().Contains(TEXT("ItemPickupTips")))
            {
                SuspectClassNames.AddUnique(Widget->GetClass()->GetName());
            }
            continue;
        }

        // ---- ① 代码驱动（首选，推荐用法）----
        // 控件继承自 UItemPickupTipsWidget 时，提示条的创建 / 填数据 / 排队 / 计时移除
        // 全部在 C++ 里完成（见 ItemPickupTipsWidget.cpp）。蓝图侧只要摆好外观，
        // 不需要加接口、不需要连任何事件节点 —— 也就不会有「连错一根线就静默失效」的问题。
        if (UItemPickupTipsWidget* TipsWidget = Cast<UItemPickupTipsWidget>(Widget))
        {
            TipsWidget->AddTip(Item, ObtainedCount);
            ++DeliveredCount;
            continue;
        }

        // ---- ② 接口方式（兼容旧做法）----
        // 想在蓝图里自己控制列表时，仍可实现 IItemPickupListener。
        if (!Widget->Implements<UItemPickupListener>())
        {
            continue;
        }

        // ★ 必须确认该控件真的实现了事件函数，不能只加接口就调。
        // 原因：Execute_OnItemObtained 在找不到蓝图实现时会回落到 UHT 为接口自动生成的
        // _Implementation 默认实现上 —— 那个实现是给"直接调用事件函数"兜底的断言桩，
        // 不是用来跑业务逻辑的。也就是说「加了接口但忘了在事件图表里实现事件」会直接崩，
        // 所以这里先挡一道，并明确告诉用户缺哪一步。
        if (!Widget->FindFunction(TEXT("OnItemObtained")))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Drop] %s 实现了 ItemPickupListener 接口，但事件图表里没有实现 "
                     "On Item Obtained 事件，已跳过本次提示（物品已正常入包）。\n"
                     "      修法：打开该控件 → Event Graph → 右键搜「On Item Obtained」→ "
                     "添加该事件并把它接到 Create Widget / Add Child 那条链上。"),
                *Widget->GetClass()->GetName());
            continue;
        }

        IItemPickupListener::Execute_OnItemObtained(Widget, Item, ObtainedCount);
        ++DeliveredCount;
    }

    if (DeliveredCount > 0)
    {
        return;
    }

    // 一个载体都没有：物品已经正常入包，只是提示没地方显示。
    // 掉落可能连续触发，这里只完整说明一次，避免刷屏
    if (bItemPickupListenerReported)
    {
        return;
    }
    bItemPickupListenerReported = true;

    // 情况一：控件本身在，就是父类没改 —— 这是最容易被忽略、也最像「代码坏了」的一种
    if (SuspectClassNames.Num() > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Drop] ★ 找到了提示控件（%s），但它的父类不是 ItemPickupTipsWidget，"
                 "所以拿不到 AddTip —— 提示不会显示（物品已正常入包）。\n"
                 "      修法：打开该控件 → 右上角 Class Settings → Parent Class → "
                 "搜 ItemPickupTipsWidget → 选中 → Compile + Save。"),
            *FString::Join(SuspectClassNames, TEXT(" / ")));
        return;
    }

    // 情况二：整条提示 UI 还没建出来
    UE_LOG(LogTemp, Warning,
        TEXT("[Drop] 角色身上没有找到「获得物品提示」控件，提示不会显示"
             "（物品已正常入包，打开背包能查到）。\n"
             "      修法（三步）：\n"
             "      1) 新建控件蓝图 WBP_ItemPickupTips，路径 /Game/UI/，名字一字不差\n"
             "      2) 打开它 → Class Settings → Parent Class 改成 ItemPickupTipsWidget\n"
             "         并在里面放一个名叫 VBox_ItemTips 的 VerticalBox（锚点定位到屏幕左侧）\n"
             "      3) Class Defaults → Item|Tip → Tip Widget Class 选 WBP_ItemTip\n"
             "         详细点击级步骤见 output/drop_system/掉落系统制作步骤.md"));
}

// 背包 UI 全部动态按钮的统一入口
void ABattleCharacter::HandleBagAction(EBagActionType ActionType, int32 Index)
{
    switch (ActionType)
    {
    case EBagActionType::Close:
        CloseBag();
        break;

    case EBagActionType::Kind:
        SetBagKind(Index);
        break;

    case EBagActionType::Slot:
        // 点格子 → 选中并刷新（高亮 + 右侧详情）
        // 玩家手选优先：清掉「默认选中第一个」的待办，避免刚点完就被拉回第 1 格
        bBagSelectFirstPending = false;
        SelectedBagIndex = Index;
        RefreshBagInternal(false);
        break;

    case EBagActionType::Use:
        if (CurrentBagItems.IsValidIndex(SelectedBagIndex))
        {
            const FBagItemEntry& Item = CurrentBagItems[SelectedBagIndex];

            // 武器背包模式下，「使用」按钮的语义是【装备】。
            // 语义分开记日志：排查时能一眼看出这次是「装备」触发的还是「使用」触发的。
            if (bBagInWeaponMode && IsWeaponItem(Item))
            {
                EquipSelectedBagWeapon(TEXT("详情面板的按钮（btn_use）"));
            }
            else
            {
                UE_LOG(LogTemp, Log, TEXT("[Bag] 使用物品：%s x%d"), *Item.ToolName, Item.ToolNum);
                OnBagItemUsed(Item);
            }
        }
        break;

    case EBagActionType::Equip:
        // 显式「装备」按钮（btn_equip / btn_equip_weapon）。
        // 与 Use 分开是为了：① 蓝图里能单独接一条「装备」逻辑；
        //                  ② 排查日志里能区分玩家点的是哪个按钮。
        EquipSelectedBagWeapon(TEXT("专用的装备按钮（btn_equip）"));
        break;

    case EBagActionType::CharaHead:
        // 角色面板 chara_pitc_head 下的动态头像按钮（Index = 角色槽位序号）。
        // ApplyCharacterSlot 内部会刷新选中框（RefreshCharaHeadSelection）。
        ApplyCharacterSlot(Index);
        break;
    }

    // 武器背包模式：点格子就装备（★ 开关默认【关】，见 BattleCharacter.h 的
    // bEquipOnWeaponSlotClick —— 默认行为是「点格子只选中 → 点按钮才装」，对齐《鸣潮》；
    // 想让点一下就装，在角色蓝图的 Weapon|Switch 里把这个勾打上）。
    // 放在 switch 之后单独处理，是为了不打断上面 Slot 分支原有的「选中 + 刷新」——
    // 开关打开时：先选中（详情面板会跟着变），再装备，两个动作都要发生。
    // ★ 只在「这一格确实是武器」时才装备：玩家在武器模式下切到别的分类浏览时不会误装备。
    if (ActionType == EBagActionType::Slot
        && bBagInWeaponMode
        && bEquipOnWeaponSlotClick
        && CurrentBagItems.IsValidIndex(SelectedBagIndex))
    {
        // 走同一个入口（EquipSelectedBagWeapon）—— 三条装备路径共用一套校验与日志，
        // 免得「点格子能装、点按钮装不了」这种只有一条路才有的差异。
        EquipSelectedBagWeapon(TEXT("点格子即装备（bEquipOnWeaponSlotClick）"));
    }
}

// 在控件树里递归找「能放格子的容器」：优先 UniformGridPanel，其次普通 GridPanel。
// 目的：控件名写错 / 类型选错时也能自动救回来，而不是直接 0 个格子。
namespace BagGridFinder
{
    static UPanelWidget* FindGrid(UWidget* Root, bool& bOutIsUniform)
    {
        if (!Root)
        {
            return nullptr;
        }

        if (UUniformGridPanel* Uniform = Cast<UUniformGridPanel>(Root))
        {
            bOutIsUniform = true;
            return Uniform;
        }
        if (UGridPanel* Plain = Cast<UGridPanel>(Root))
        {
            bOutIsUniform = false;
            return Plain;
        }

        if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
        {
            const int32 ChildCount = Panel->GetChildrenCount();
            for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
            {
                bool bChildIsUniform = false;
                if (UPanelWidget* Found = FindGrid(Panel->GetChildAt(ChildIndex), bChildIsUniform))
                {
                    bOutIsUniform = bChildIsUniform;
                    return Found;
                }
            }
        }
        return nullptr;
    }
}

// 生成全部格子控件：数量 = SlotCount，空槽位同样存在（内容置空）
void ABattleCharacter::BuildBagGrid()
{
    BagSlotWidgets.Reset();
    BagActions.Reset();
    BagGridPanel = nullptr;
    bBagGridIsUniform = true;

    if (!BagWidget)
    {
        return;
    }

    // ---- 1) 先按约定名找 grid_slots ----
    UPanelWidget* GridPanel = nullptr;
    bool bIsUniform = true;

    if (UWidget* Named = BagWidget->GetWidgetFromName(TEXT("grid_slots")))
    {
        if (UUniformGridPanel* Uniform = Cast<UUniformGridPanel>(Named))
        {
            GridPanel = Uniform;
            bIsUniform = true;
        }
        else if (UGridPanel* Plain = Cast<UGridPanel>(Named))
        {
            GridPanel = Plain;
            bIsUniform = false;
            UE_LOG(LogTemp, Log, TEXT("[Bag] grid_slots 是普通 GridPanel，已按 GridPanel 的方式填充。"));
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] grid_slots 的类型是 %s，不是网格容器。将在控件树里自动搜索其它网格容器。"),
                *Named->GetClass()->GetName());
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] WBP_Bag 里没有名为 grid_slots 的控件（或它不是变量）。将在控件树里自动搜索网格容器。"));
    }

    // ---- 2) 没找到 → 全树搜一遍，把命名/类型写错的场景自动救回来 ----
    if (!GridPanel)
    {
        UWidget* RootWidget = BagWidget->WidgetTree ? BagWidget->WidgetTree->RootWidget : nullptr;
        bool bFoundIsUniform = false;
        if (UPanelWidget* Found = BagGridFinder::FindGrid(RootWidget, bFoundIsUniform))
        {
            GridPanel = Found;
            bIsUniform = bFoundIsUniform;
            UE_LOG(LogTemp, Log,
                TEXT("[Bag] 已自动改用控件树里的 %s（控件名 %s）作为格子容器。建议把它改名为 grid_slots。"),
                *Found->GetClass()->GetName(), *Found->GetName());
        }
    }

    if (!GridPanel)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[Bag] WBP_Bag 里没有 UniformGridPanel / GridPanel，无法生成格子。"
                 "请在设计器里加一个 Uniform Grid Panel，并命名为 grid_slots。"));
        return;
    }

    if (!BagSlotWidgetClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] BagSlotWidgetClass 未设置，无法生成格子。"
                 "请创建 WBP_Bag_Slot（/Game/UI/bag_sys），或在 BP_PlayerCharacter 的 Bag|UI 里挂上。"));
        return;
    }

    BagGridPanel = GridPanel;
    bBagGridIsUniform = bIsUniform;

    const int32 Total = InventoryComponent ? FMath::Max(1, InventoryComponent->SlotCount) : 40;
    const int32 Columns = InventoryComponent ? FMath::Max(1, InventoryComponent->GridColumns) : 8;
    const float CellSize = InventoryComponent ? FMath::Max(8.f, InventoryComponent->CellSize) : 90.f;

    GridPanel->ClearChildren();

    int32 FailedCount = 0;

    for (int32 Index = 0; Index < Total; ++Index)
    {
        UUserWidget* Slot = CreateWidget<UUserWidget>(GetWorld(), BagSlotWidgetClass);
        if (!Slot)
        {
            ++FailedCount;
            continue;
        }

        // ★ 关键：用 C++ 的 SizeBox 把格子尺寸「钉死」。
        //   UniformGridPanel 的格宽/格高完全由子控件的「期望尺寸」撑开 ——
        //   如果 WBP_Bag_Slot 的根节点没有固定尺寸（SizeBox 没设 Override / 根是 Canvas Panel），
        //   40 个格子会一起塌成 0×0，表现就是「背包打开了但一个格子都没有」。
        //   套一层 SizeBox 之后，无论 WBP_Bag_Slot 怎么搭，格子都有确定尺寸。
        USizeBox* CellBox = NewObject<USizeBox>(BagWidget);
        CellBox->SetWidthOverride(CellSize);
        CellBox->SetHeightOverride(CellSize);

        // AddChild 在「容器已经有子控件」时会返回 nullptr，此时格子会挂在控件树外 → 看不见。
        // 这种情况正常不会发生（SizeBox 每次都是新建的），但真发生时要退化为「不用兜底层」，别丢格子。
        UPanelSlot* CellContentSlot = CellBox->AddChild(Slot);
        UWidget* CellRoot = CellBox;
        if (!CellContentSlot)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] 第 %d 个格子的 SizeBox 兜底层挂载失败（AddChild 返回空），已退化为直接挂到网格上。"),
                Index);
            CellRoot = Slot;
        }

        if (bIsUniform)
        {
            CastChecked<UUniformGridPanel>(GridPanel)->AddChildToUniformGrid(CellRoot, Index / Columns, Index % Columns);
        }
        else
        {
            CastChecked<UGridPanel>(GridPanel)->AddChildToGrid(CellRoot, Index / Columns, Index % Columns);
        }

        BagSlotWidgets.Add(Slot);

        // 整格点击：btn_slot（UButton）为格子根节点
        if (UButton* SlotButton = Cast<UButton>(Slot->GetWidgetFromName(TEXT("btn_slot"))))
        {
            SlotButton->OnClicked.AddDynamic(MakeBagAction(EBagActionType::Slot, Index), &UBagWidgetAction::OnClicked);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[Bag] 生成格子 %d 个（%d 列，每格 %.0f×%.0f）。"),
        BagSlotWidgets.Num(), Columns, CellSize, CellSize);

    if (FailedCount > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 有 %d 个格子创建失败（CreateWidget 返回空）。请确认 Bag Slot Widget Class 指向 WBP_Bag_Slot。"),
            FailedCount);
    }

    LogBagGridDiagnostics();
}

// 尺寸自检：格子「有尺寸」和「看得见」是两件事。
// 这里用布局预演立刻拿到期望尺寸（不依赖帧循环，暂停状态下同样有效），
// 并把上层容器逐层打出来 —— 尺寸链断在哪一层一眼可见。
void ABattleCharacter::LogBagGridDiagnostics()
{
    if (!BagGridPanel)
    {
        return;
    }

    BagGridPanel->ForceLayoutPrepass();
    const FVector2D GridDesired = BagGridPanel->GetDesiredSize();

    FVector2D CellDesired = FVector2D::ZeroVector;
    for (UUserWidget* Slot : BagSlotWidgets)
    {
        if (Slot)
        {
            Slot->ForceLayoutPrepass();
            CellDesired = Slot->GetDesiredSize();
            break;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[Bag] 尺寸自检：格子期望尺寸 %.0f×%.0f，网格期望尺寸 %.0f×%.0f。"),
        CellDesired.X, CellDesired.Y, GridDesired.X, GridDesired.Y);

    // 往上 3 层容器，把每层期望尺寸也打出来；同时判定「网格是否真的在 scroll_slots 里」
    UWidget* Ancestor = BagGridPanel->GetParent();
    bool bInsideScrollSlots = false;
    FString Chain = BagGridPanel->GetName();
    for (int32 Depth = 0; Ancestor && Depth < 3; ++Depth)
    {
        Ancestor->ForceLayoutPrepass();
        const FVector2D AncestorDesired = Ancestor->GetDesiredSize();
        UE_LOG(LogTemp, Log, TEXT("[Bag]   上层[%d] %s（控件名 %s）期望尺寸 %.0f×%.0f"),
            Depth + 1, *Ancestor->GetClass()->GetName(), *Ancestor->GetName(),
            AncestorDesired.X, AncestorDesired.Y);

        Chain += FString::Printf(TEXT(" → %s"), *Ancestor->GetName());
        if (Ancestor->GetFName() == TEXT("scroll_slots") && Ancestor->IsA<UScrollBox>())
        {
            bInsideScrollSlots = true;
        }
        Ancestor = Ancestor->GetParent();
    }

    // 这一条是「格子超出预期范围」的核心事实：网格到底有没有被 ScrollBox 兜住
    UE_LOG(LogTemp, Log, TEXT("[Bag] 网格父链：%s"), *Chain);
    if (bInsideScrollSlots)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Bag] ✔ grid_slots 已在 scroll_slots 内：格子按内容大小排布，超出部分由 ScrollBox 滚动/裁剪。"));
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] ✘ grid_slots 不在 scroll_slots 内（父链见上一行）。"
                 "此时它的大小完全由自己的锚点决定：若锚点是「铺满」，格子就会按「分到的空间 ÷ 列数」"
                 "均分铺满整屏；若锚点是「左上/固定」，它就停在锚点位置、不会滚动。"
                 "推荐做法：把 grid_slots 拖进 scroll_slots 内部。"));
    }

    // 顺手报一下 scroll_slots 自身尺寸 —— 它就是「预期显示范围」的基准。
    // 如果它自己就是全屏的，那"格子超出范围"其实不是格子的错。
    if (BagWidget)
    {
        if (UWidget* ScrollSlots = BagWidget->GetWidgetFromName(TEXT("scroll_slots")))
        {
            ScrollSlots->ForceLayoutPrepass();
            const FVector2D ScrollDesired = ScrollSlots->GetDesiredSize();
            UE_LOG(LogTemp, Log,
                TEXT("[Bag] scroll_slots（预期显示范围）类型 %s，期望尺寸 %.0f×%.0f"
                     "（实际尺寸见下一帧的「容器实际尺寸」段）。"),
                *ScrollSlots->GetClass()->GetName(), ScrollDesired.X, ScrollDesired.Y);
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] WBP_Bag 里没有名为 scroll_slots 的 ScrollBox（或它不是变量）。"
                     "没有它也能跑，但格子就没有滚动/裁剪区域，需要靠 grid_slots 自己的锚点约束。"));
        }
    }

    // 分两种失败分别报：格子控件自身不会撑尺寸 / 网格没算出来
    const float CellSize = InventoryComponent ? InventoryComponent->CellSize : 90.f;

    if (CellDesired.X < 1.f || CellDesired.Y < 1.f)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] WBP_Bag_Slot 自身不会撑尺寸（期望尺寸 0×0），最常见的原因是根节点是 Canvas Panel。"
                 "C++ 已按 Cell Size=%.0f 兜底，格子仍会正常显示；"
                 "但建议把 WBP_Bag_Slot 的根节点换成 Size Box（Width/Height Override 都设 %.0f），内部布局才可控。"),
            CellSize, CellSize);
    }
    else if (GridDesired.X < 1.f || GridDesired.Y < 1.f)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 网格期望尺寸是 0：格子自身有尺寸，但网格没算出来。请检查 grid_slots 的列数与 Slot Padding。"));
    }

    // 下一帧再读一次「真正画出来的尺寸」——期望尺寸正常但显示尺寸是 0，说明上层容器没给它空间
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimerForNextTick(this, &ABattleCharacter::LogBagGridRealSize);
    }

    // 最后打一份 WBP_Bag_Slot 的结构快照（只打第 1 个格子，避免刷屏）
    LogBagSlotStructure();
}

// 结构快照：只对第 1 个格子打印「根节点 + 直接子控件」的类名 / 控件名 / 槽位类型 / 期望尺寸。
// 用途：格子仍有问题时，把这一段日志发出来就能定位到具体是哪个控件没尺寸、被放在什么容器里。
void ABattleCharacter::LogBagSlotStructure()
{
    if (BagSlotWidgets.Num() <= 0 || !BagSlotWidgets[0])
    {
        return;
    }

    UUserWidget* Slot = BagSlotWidgets[0];
    UWidget* RootWidget = Slot->WidgetTree ? Slot->WidgetTree->RootWidget : nullptr;
    if (!RootWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Bag] 结构快照：读不到 WBP_Bag_Slot 的根节点（WidgetTree 为空）。"));
        return;
    }

    RootWidget->ForceLayoutPrepass();
    const FVector2D RootDesired = RootWidget->GetDesiredSize();

    UE_LOG(LogTemp, Log,
        TEXT("[Bag] 结构快照（第 1 个格子）：WBP_Bag_Slot 根节点 = %s（控件名 %s），期望尺寸 %.0f×%.0f"),
        *RootWidget->GetClass()->GetName(), *RootWidget->GetName(), RootDesired.X, RootDesired.Y);

    UPanelWidget* RootPanel = Cast<UPanelWidget>(RootWidget);
    if (!RootPanel)
    {
        return;
    }

    const int32 ChildCount = RootPanel->GetChildrenCount();
    const int32 PrintCount = FMath::Min(ChildCount, 8);
    for (int32 ChildIndex = 0; ChildIndex < PrintCount; ++ChildIndex)
    {
        UWidget* Child = RootPanel->GetChildAt(ChildIndex);
        if (!Child)
        {
            continue;
        }

        Child->ForceLayoutPrepass();
        const FVector2D ChildDesired = Child->GetDesiredSize();
        UE_LOG(LogTemp, Log, TEXT("[Bag]   |- %s（控件名 %s）槽=%s 期望尺寸 %.0f×%.0f"),
            *Child->GetClass()->GetName(), *Child->GetName(),
            Child->Slot ? *Child->Slot->GetClass()->GetName() : TEXT("<无槽位>"),
            ChildDesired.X, ChildDesired.Y);
    }

    if (ChildCount > PrintCount)
    {
        UE_LOG(LogTemp, Log, TEXT("[Bag]   \\- …还有 %d 个直接子控件未列出"), ChildCount - PrintCount);
    }
}

// 尺寸自检第二步：读「真正画出来的尺寸」，并检测「网格被拉伸」。
//
// ⚠️ 关键教训（上一版在这里闯了祸）：
//    GetCachedGeometry() 在控件「还没被绘制过」时恒为 0×0，所以「读数是 0」根本不等于「布局坏了」。
//    上一版据此把上游容器改成「四边铺满」，结果网格被撑到全屏 ——
//    由 SUniformGridPanel::OnArrangeChildren 的公式「格子尺寸 = 分到的空间 ÷ 列数」，
//    40 个格子就按「全屏 ÷ 8 列」均分铺满整屏，把其他 UI 全挡住了。
//    → 因此现在：0 值只用于「重采样」，绝不用于「改动布局」。
void ABattleCharacter::LogBagGridRealSize()
{
    if (!bBagOpen || !BagGridPanel)
    {
        return;
    }

    BagGridPanel->ForceLayoutPrepass();

    const FVector2D GridDesired = BagGridPanel->GetDesiredSize();
    const FVector2D GridReal = BagGridPanel->GetCachedGeometry().GetLocalSize();

    FVector2D CellReal = FVector2D::ZeroVector;
    if (BagSlotWidgets.Num() > 0 && BagSlotWidgets[0])
    {
        CellReal = BagSlotWidgets[0]->GetCachedGeometry().GetLocalSize();
    }

    // 首帧还没画过 → 全部读数为 0。这不是故障，隔帧重采样（最多 3 次）后再下结论。
    const bool bNeverDrawnYet = (GridReal.X < 1.f && GridReal.Y < 1.f && CellReal.X < 1.f);
    if (bNeverDrawnYet && BagGridSampleTries < 3)
    {
        ++BagGridSampleTries;
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimerForNextTick(this, &ABattleCharacter::LogBagGridRealSize);
        }
        return;
    }

    UE_LOG(LogTemp, Log,
        TEXT("[Bag] 尺寸自检（实际显示）：格子 %.0f×%.0f，网格 %.0f×%.0f（网格期望 %.0f×%.0f，每格设定 %.0f）。"),
        CellReal.X, CellReal.Y, GridReal.X, GridReal.Y, GridDesired.X, GridDesired.Y,
        InventoryComponent ? InventoryComponent->CellSize : 90.f);

    // ---- 尺寸建议：直接算出「刚好放下全部格子」要多大，省得用户自己算 ----
    // 依据：格子占位 = CellSize（C++ 的 SizeBox 兜底值）+ grid_slots 的 SlotPadding。
    {
        const float CellConfig = InventoryComponent ? FMath::Max(8.f, InventoryComponent->CellSize) : 90.f;
        const int32 ColumnsCfg = InventoryComponent ? FMath::Max(1, InventoryComponent->GridColumns) : 8;
        const int32 SlotTotal = FMath::Max(1, BagSlotWidgets.Num());

        float PadX = 8.f;
        float PadY = 8.f;
        if (UUniformGridPanel* Uniform = Cast<UUniformGridPanel>(BagGridPanel))
        {
            const FMargin Padding = Uniform->GetSlotPadding();
            PadX = Padding.Left + Padding.Right;
            PadY = Padding.Top + Padding.Bottom;
        }

        const int32 Rows = FMath::DivideAndRoundUp(SlotTotal, ColumnsCfg);
        const float NeedWidth = ColumnsCfg * (CellConfig + PadX);
        const float NeedHeight = Rows * (CellConfig + PadY);

        UE_LOG(LogTemp, Log,
            TEXT("[Bag] 尺寸建议：%d 格 = %d 列 × %d 行，每格 %.0f（间距 %.0f×%.0f）"
                 "→ grid_slots 内容需要 %.0f×%.0f。\n"
                 "      请把 scroll_slots（预期显示范围）设成 %.0f×%.0f 左右，"
                 "或把它的锚点钉在你想放的那块区域，格子就会乖乖待在里面。"),
            SlotTotal, ColumnsCfg, Rows, CellConfig, PadX, PadY,
            NeedWidth, NeedHeight, NeedWidth + 16.f, NeedHeight + 16.f);

        // 横向/纵向分别判断，指出是哪个方向被摊开了
        const float Tolerance = 1.15f;
        if (GridReal.X > NeedWidth * Tolerance)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] 网格**横向**被摊开：实际宽 %.0f > 内容所需 %.0f。"
                     "ScrollBox 会把内容横向撑满，所以格子宽度 = 实际宽 ÷ %d = %.0f（不是设定的 %.0f）。"
                     "把 scroll_slots 的宽度收窄到 ≈ %.0f 即可。"),
                GridReal.X, NeedWidth, ColumnsCfg, GridReal.X / ColumnsCfg, CellConfig, NeedWidth + 16.f);

            // ---- 直接修掉它：把 ScrollBox 槽的横向对齐从 Fill 改成 Left ----
            //
            // 引擎依据（已核源码）：
            //   SScrollPanel::OnArrangeChildren → ArrangeChildrenInStack
            //     SlotSize.X = AllottedGeometry.GetLocalSize().X   ← 交叉轴取「整个 ScrollBox 的宽」
            //   AlignChild<Orient_Horizontal>(SlotSize.X, ...)
            //     HAlign_Fill → Size = (AllottedSize - margin)      ← 内容被撑满 → 格子跟着摊开
            //     HAlign_Left → Size = min(内容期望尺寸, 容器宽)     ← 内容只占自己需要的宽度
            //   且 bClampToParent = true → **永远不会超出容器**，所以这个改动是安全的：
            //   它只能把「摊满」收敛成「按内容宽度」，不可能把任何东西撑大。
            if (bAutoFitStretchedBagGrid && !bBagGridCrossAxisFixed)
            {
                if (UScrollBoxSlot* ScrollSlot = Cast<UScrollBoxSlot>(BagGridPanel->Slot))
                {
                    bBagGridCrossAxisFixed = true;
                    ScrollSlot->SetHorizontalAlignment(HAlign_Left);
                    ScrollSlot->SetVerticalAlignment(VAlign_Top);

                    UE_LOG(LogTemp, Warning,
                        TEXT("[Bag] 已把 grid_slots 在 ScrollBox 内的横向对齐由「Fill（撑满）」改为「Left（按内容宽度）」。"
                             "现在格子保持设定的 %.0f，整块宽约 %.0f，不再随 scroll_slots 变宽而摊开。"
                             "若你确实想让格子铺满整个 scroll_slots，请在设计器里把该槽的 Alignment 改回 Fill，"
                             "并关闭 Bag|UI → Auto Fit Stretched Bag Grid。"),
                        CellConfig, NeedWidth);
                }
            }
        }
        if (GridReal.Y > NeedHeight * Tolerance)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Bag] 网格**纵向**被摊开：实际高 %.0f > 内容所需 %.0f。把 scroll_slots 的高度收到 ≈ %.0f。"),
                GridReal.Y, NeedHeight, NeedHeight + 16.f);
        }
    }

    // ---- 容器实际尺寸逐层体检：找出「哪一层占了整屏」 ----
    // 这是上一轮修复留下的最后一个盲区：如果被撑成全屏的不是 grid_slots 自己，
    // 而是外层容器（例如 scroll_slots 本身就铺满全屏），那格子的"越界"其实是被动的。
    // 这里把每层的实际尺寸与整个背包控件对比，直接点名。
    const FVector2D WidgetReal = BagWidget ? BagWidget->GetCachedGeometry().GetLocalSize() : FVector2D::ZeroVector;
    const bool bWidgetSizeKnown = (WidgetReal.X > 1.f && WidgetReal.Y > 1.f);

    if (bWidgetSizeKnown)
    {
        UE_LOG(LogTemp, Log, TEXT("[Bag] 容器实际尺寸（整屏基准 = 背包控件 %.0f×%.0f）："), WidgetReal.X, WidgetReal.Y);

        UWidget* ChainWidget = BagGridPanel->GetParent();
        for (int32 Depth = 0; ChainWidget && Depth < 3; ++Depth)
        {
            const FVector2D LayerReal = ChainWidget->GetCachedGeometry().GetLocalSize();
            const bool bLayerFullScreen =
                (LayerReal.X >= WidgetReal.X * 0.95f && LayerReal.Y >= WidgetReal.Y * 0.95f);

            UE_LOG(LogTemp, Log, TEXT("[Bag]   上层[%d] %s（控件名 %s）实际 %.0f×%.0f%s"),
                Depth + 1, *ChainWidget->GetClass()->GetName(), *ChainWidget->GetName(),
                LayerReal.X, LayerReal.Y,
                bLayerFullScreen ? TEXT("  ← ★ 这一层占满了整屏") : TEXT(""));

            ChainWidget = ChainWidget->GetParent();
        }

        if (UWidget* ScrollSlots = BagWidget->GetWidgetFromName(TEXT("scroll_slots")))
        {
            const FVector2D ScrollReal = ScrollSlots->GetCachedGeometry().GetLocalSize();
            const bool bScrollFullScreen =
                (ScrollReal.X >= WidgetReal.X * 0.95f && ScrollReal.Y >= WidgetReal.Y * 0.95f);
            UE_LOG(LogTemp, Log, TEXT("[Bag]   scroll_slots 实际 %.0f×%.0f%s"),
                ScrollReal.X, ScrollReal.Y,
                bScrollFullScreen ? TEXT("  ← ★ 它自己就占满了整屏，格子当然会铺到整屏") : TEXT(""));
        }
    }

    // ---- 情形 A：网格「被拉伸」= 格子铺满屏幕的根因 ----
    const bool bStretched =
        (GridDesired.X > 1.f && GridReal.X > GridDesired.X * 1.15f) ||
        (GridDesired.Y > 1.f && GridReal.Y > GridDesired.Y * 1.15f);

    if (bStretched)
    {
        if (bBagGridStretchHandled)
        {
            return;
        }
        bBagGridStretchHandled = true;

        // 只有「网格自己就放在 Canvas Panel 里」时，拉伸才来自锚点，才由我们收敛。
        // 收敛动作 = AutoSize（网格只占内容大小），对齐 (0,0) 让它贴着锚点摆放。
        // 仅在「实测 > 期望」这种客观被拉伸的情况下动手 —— 此时格子必然已经铺到不该在的位置。
        if (bAutoFitStretchedBagGrid)
        {
            if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(BagGridPanel->Slot))
            {
                CanvasSlot->SetAutoSize(true);
                CanvasSlot->SetAlignment(FVector2D(0.f, 0.f));

                UE_LOG(LogTemp, Warning,
                    TEXT("[Bag] 检测到网格被拉伸（实测 %.0f×%.0f 远大于内容所需的 %.0f×%.0f）。"
                         "按引擎规则「格子尺寸 = 网格分到的空间 ÷ 列数」，这会让格子均分铺满整屏。"
                         "已把 grid_slots 临时改成按内容尺寸显示（AutoSize）。"
                         "请在设计器里彻底修好：选中 grid_slots → Details → Anchors 选「左上/固定」并把 Size To Content 打勾；"
                         "或者更推荐 —— 把 grid_slots 拖进 scroll_slots 内部，由 ScrollBox 划定显示范围。"),
                    GridReal.X, GridReal.Y, GridDesired.X, GridDesired.Y);
                return;
            }
        }

        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 网格被拉伸（实测 %.0f×%.0f 远大于内容所需的 %.0f×%.0f）：格子会按「分到的空间 ÷ 列数」均分，"
                 "所以会铺到整个容器的范围里。请让 grid_slots 只占内容大小：放进 scroll_slots 里，"
                 "或把它的锚点改成不铺满（Size To Content）。"),
            GridReal.X, GridReal.Y, GridDesired.X, GridDesired.Y);
        return;
    }

    // ---- 情形 B：网格根本没分到空间 ----
    if (GridReal.X < 1.f || GridReal.Y < 1.f)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 网格实际显示尺寸是 0（重采样 %d 次仍为 0）：格子已生成，但外层容器没给它空间。"
                 "请检查 scroll_slots（以及它的父容器）的尺寸与锚点 —— 让它占住你想显示的那块区域即可。"),
            BagGridSampleTries);
    }
}

// 绑定 btn_close / btn_use / kind_all / kind_0..kind_N
void ABattleCharacter::BindBagButtons()
{
    if (!BagWidget)
        return;

    if (UButton* CloseButton = Cast<UButton>(BagWidget->GetWidgetFromName(TEXT("btn_close"))))
    {
        CloseButton->OnClicked.AddDynamic(MakeBagAction(EBagActionType::Close, INDEX_NONE), &UBagWidgetAction::OnClicked);
    }

    if (UButton* UseButton = Cast<UButton>(BagWidget->GetWidgetFromName(TEXT("btn_use"))))
    {
        UseButton->OnClicked.AddDynamic(MakeBagAction(EBagActionType::Use, INDEX_NONE), &UBagWidgetAction::OnClicked);
    }

    // ---- 武器背包的「装备」按钮（可选控件）----
    // 为什么容了好几个名字：这是后加的按钮，各人 WBP 里的命名习惯不一样；
    // 而绑定失败在这里是【静默】的（找不到控件就不做事），所以多试几个名字比让用户
    // 去猜「我这名字对不对」划算。一个都没有也不影响功能 ——
    // 武器模式下详情面板上那个按钮（btn_use）的语义就是「装备」。
    static const TCHAR* const EquipButtonNames[] = {
        TEXT("btn_equip"),
        TEXT("btn_equip_weapon"),
        TEXT("btn_arm_equip"),
        TEXT("btn_equipment")
    };
    for (const TCHAR* EquipButtonName : EquipButtonNames)
    {
        if (UButton* EquipButton = Cast<UButton>(BagWidget->GetWidgetFromName(EquipButtonName)))
        {
            EquipButton->OnClicked.AddDynamic(
                MakeBagAction(EBagActionType::Equip, INDEX_NONE), &UBagWidgetAction::OnClicked);
            UE_LOG(LogTemp, Log,
                TEXT("[Arm] 「装备」按钮已绑定：%s → 点击后装备【当前选中】的那件武器。"), EquipButtonName);
            break;  // 一个就够，避免同一个按钮被 bind 多次（会触发多次装备）
        }
    }

    // 「全部」分类（可选控件）
    if (UButton* AllButton = Cast<UButton>(BagWidget->GetWidgetFromName(TEXT("kind_all"))))
    {
        AllButton->OnClicked.AddDynamic(MakeBagAction(EBagActionType::Kind, -1), &UBagWidgetAction::OnClicked);
    }

    // 各分类按钮 kind_0 .. kind_(KindCount-1)
    const int32 KindTotal = InventoryComponent ? InventoryComponent->KindCount : 6;
    for (int32 Kind = 0; Kind < KindTotal; ++Kind)
    {
        const FName ButtonName(*FString::Printf(TEXT("kind_%d"), Kind));
        if (UButton* KindButton = Cast<UButton>(BagWidget->GetWidgetFromName(ButtonName)))
        {
            KindButton->OnClicked.AddDynamic(MakeBagAction(EBagActionType::Kind, Kind), &UBagWidgetAction::OnClicked);
        }
    }
}

// 内部刷新：bReloadData = true 时先重新读表
void ABattleCharacter::RefreshBagInternal(bool bReloadData)
{
    if (!BagWidget)
        return;

    if (InventoryComponent && bReloadData)
    {
        InventoryComponent->ReloadItems();
    }

    CurrentBagItems = InventoryComponent
        ? InventoryComponent->GetItemsForKind(CurrentBagKind)
        : TArray<FBagItemEntry>();

    // ---- 武器模式：把「只是 kind_tool 碰巧等于武器」的东西剔出去 ----
    // 为什么要过滤：GetItemsForKind 只按 kind_tool 比数值，而 kind_tool 的默认值就是 0
    // （通常正是武器分类）→ 没填分类的掉落物会混进武器背包，还点了就能装。
    //
    // ★ 判据只用【数据】：kind_tool 是武器 且 行名属于 Data_tool（见 IsWeaponItem）。
    //   这里【不能】再要求「能解析出武器蓝图类」—— 那个要求会 fail-closed。
    //   实测（自动扫描返回 0 个候选 → 索引空）把它放在这里时，整页武器被清空，
    //   而用户能看到的只有「武器背包是空的」，完全联想不到「扫描断了」。
    //   「能不能装备」是另一个问题，放在装备路径上问（CanEquipWeaponItem）。
    //
    // 只在本模式且分类就是武器时才过滤：玩家在武器模式下切到别的分类浏览时不该被动手。
    if (bBagInWeaponMode && CurrentBagKind == WeaponKindTool)
    {
        const int32 BeforeFilter = CurrentBagItems.Num();
        CurrentBagItems.RemoveAll([this](const FBagItemEntry& Candidate)
        {
            return !IsWeaponItem(Candidate);
        });

        const int32 RemovedCount = BeforeFilter - CurrentBagItems.Num();
        if (RemovedCount > 0)
        {
            UE_LOG(LogTemp, Log,
                TEXT("[Arm] 武器背包过滤：kind_tool=%d 的 %d 件里，有 %d 件不算武器"
                     "（行名不属于背包物品表 Data_tool）→ 已剔除。"),
                WeaponKindTool, BeforeFilter, RemovedCount);
        }

        // ★ 安全阀：展示 / 列表路径【绝不允许】被过滤成空列表。
        //   过滤前有东西、过滤后一件不剩 —— 这必然是判据的输入有问题
        //   （数据表没读进来 / 行名集合没建起来），而不是「这些物品都不是武器」。
        //   宁可显示得不准，也不能让玩家面对一个空背包却拿不到任何解释：
        //   空背包会把「判据出错」伪装成「你没有武器」，这是最难自己查出来的一种现象。
        if (CurrentBagItems.Num() == 0 && BeforeFilter > 0 && InventoryComponent)
        {
            CurrentBagItems = InventoryComponent->GetItemsForKind(CurrentBagKind);
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] ⚠ 过滤把 %d 件全剔掉了 → 已【放弃本次过滤】并把它们放回列表。\n"
                     "      这几乎必然是判据的输入不对，不是这些物品真的都不是武器：\n"
                     "        ① 数据表没读进来（ToolTable 加载失败 / ReloadItems 提前返回）→ 看上面的 [Bag] 日志\n"
                     "        ② 行名集合没建起来（ToolTableRowNames 为空）→ 同上\n"
                     "      列表先照常显示；点装备时仍会逐件给出「为什么装不上」和该怎么修。"),
                BeforeFilter);
        }

        // ---- 职位过滤：只显示「与当前职位武器类别匹配」的武器 ----
        // 为什么在 IsWeaponItem 过滤【之后】再做：上面先把「不算武器」的剔掉，
        // 这里再把「算武器但类别不匹配职位」的剔掉。两层各自有明确语义与日志。
        //
        // ★ 与上面同一个 fail-closed 教训：职位过滤【不能】把列表清空就拉倒。
        //   枪手（占位）当前没有枪械武器，过滤后自然是空的 —— 这是「合法空」，
        //   必须和「判据出错导致的空」区分开：合法空要提示「这个职位还没有可用武器」，
        //   而不是回退放行（放行会让枪手装上一把剑，违反职位约束）。
        {
            const int32 BeforeJobFilter = CurrentBagItems.Num();
            // ★ 筛选职位：武器背包是从「角色面板武器页」进入的，玩家在给【面板选中的角色】
            //   挑武器，所以用选中角色的 JobClass（而非当前操控角色 this 的职位）。
            //   否则选中法师(BP_Chara_magic)却用 this(剑士 BP_PlayerCharacter)的职位过滤，
            //   会显示轻剑、看起来像「跳到了 BP_PlayerCharacter 的背包」。
            const EJobClass FilterJob = GetWeaponBagFilterJobClass();
            UE_LOG(LogTemp, Log,
                TEXT("[Job] 职位过滤入口：this=%s｜筛选职位=%d（this 职位=%d）｜武器 %d 件"),
                *GetClass()->GetName(), static_cast<int32>(FilterJob),
                static_cast<int32>(JobClass), BeforeJobFilter);
            CurrentBagItems.RemoveAll([this, FilterJob](const FBagItemEntry& Candidate)
            {
                // 只对「确实能解析出武器蓝图类」的条目做职位校验；
                // 解析不出类的（掉落物/未声明行）留给装备路径拦截，不在列表层误伤。
                const TSubclassOf<AWeaponBase> WClass = ResolveWeaponClassForItemRow(Candidate.RowName);
                if (!WClass)
                    return false; // 保留，装备时再判
                // 按【筛选职位】判，而不是 this 的职位（见上 FilterJob 说明）。
                const AWeaponBase* CDO = WClass->GetDefaultObject<AWeaponBase>();
                if (!CDO)
                    return false;
                return !FJobWeaponRules::CanJobUseCategory(FilterJob, CDO->GetWeaponCategory());
            });

            const int32 RemovedByJob = BeforeJobFilter - CurrentBagItems.Num();
            if (RemovedByJob > 0)
            {
                UE_LOG(LogTemp, Log,
                    TEXT("[Job] 武器背包按职位(%d，可用类别 %d)过滤：%d 件里剔除了 %d 件类别不匹配的武器。"),
                    static_cast<int32>(FilterJob),
                    static_cast<int32>(FJobWeaponRules::GetCategoryForJob(FilterJob)),
                    BeforeJobFilter, RemovedByJob);
            }

            // 职位过滤后为空：分两种情形给出不同提示（不静默）。
            if (CurrentBagItems.Num() == 0)
            {
                if (BeforeJobFilter > 0)
                {
                    UE_LOG(LogTemp, Log,
                        TEXT("[Job] 职位(%d)过滤后武器列表为空 —— 背包里的武器都不属于本职位可用类别。\n"
                             "      这通常不是错误：例如「枪手」暂未开发枪械武器，选枪手就是空的。\n"
                             "      想换武器：改 BP_PlayerCharacter → Job → Job Class 到有对应武器的职位。"),
                        static_cast<int32>(FilterJob));
                }
            }
        }
    }

    // ---- 同步：装备中的武器若已不在背包里（被 ClearRuntimeItems / 读档替换），自动卸下 ----
    // 不这么做会出现「武器页显示装备着某把武器，背包里却找不到它」的不一致，
    // 而且手上还拿着那把武器 —— 属于最难解释的一类 bug。
    // 只在「重新读表」这一路做（bReloadData），避免每次点格子都白查一遍。
    //
    // ★ bEquippedWeaponRowFromBag 这个条件不是可有可无的：
    //   装备入口不止「背包点格子」一条。BeginPlay 会用 DefaultWeaponClass 装开局武器，
    //   武器蓝图自己声明的 Bag Item Row 又会被回填进 EquippedWeaponRow ——
    //   若开局武器声明的行恰好不在背包里（它不是从背包发的），少了这个条件就会
    //   在「第一次打开背包」时把开局武器静默卸掉，还会连带清空角色蓝图的 Weapon Class。
    //   所以只对「玩家从背包点出来的」那把武器做这种一致性清理。
    if (bReloadData && bEquippedWeaponRowFromBag && InventoryComponent && !EquippedWeaponRow.IsNone())
    {
        FBagItemEntry EquippedCheck;
        if (!InventoryComponent->GetItemByRow(EquippedWeaponRow, EquippedCheck) || !EquippedCheck.bValid)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] 装备中的武器「%s」已不在背包里 → 自动卸下（保持武器页与背包一致）。"),
                *EquippedWeaponRow.ToString());
            // 注意：这里不能再传 bReloadData=true，否则递归。UnequipWeaponAndClearRow 内部
            // 只会用 false 刷新一次背包，而且清空 EquippedWeaponRow 后本分支不会再进。
            UnequipWeaponAndClearRow();
        }
    }

    if (SelectedBagIndex >= CurrentBagItems.Num())
    {
        SelectedBagIndex = -1;
    }

    // ---- 默认选中第一个物品 ----
    // 触发场景：按 B 打开背包、切换左侧分类（这两处会把待办标记置 true）。
    // ★ 必须放在逐格刷新【之前】：格子高亮是在下面的循环里按 SelectedBagIndex 画的，
    //   若放到循环之后，就会出现「右侧详情已经是第 1 个物品，格子高亮却还停在旧位置」。
    if (bBagSelectFirstPending)
    {
        // 无条件消费掉待办：即使开关是关的也要清，
        // 否则「关掉开关期间残留的标记」会在重新打开时突然生效
        bBagSelectFirstPending = false;

        // 开关关闭 → 选中项完全交给玩家点击决定；开启时也只在没有有效选中才自动选，
        // 避免把玩家/上一轮的选择覆盖掉
        if (bAutoSelectFirstBagItem && !CurrentBagItems.IsValidIndex(SelectedBagIndex))
        {
            // ★ 武器背包模式：优先直落【当前装备的那一格】。
            //   为什么：用户是「用眼睛」验证同步的 —— 身上装着那把刀，打开武器背包
            //   高亮却停在第 1 格，看起来就是「装备状态没同步过来」。
            //   ★ 用 GetEquippedWeaponDisplayRow()（以手上武器类为准），而不是裸的
            //     EquippedWeaponRow —— 后者只记行号，行号与实物脱钩时会高亮到别件东西上。
            //   找不到（没装备 / 那一行不在当前分类）→ 落回下面的「默认选中第 1 个」。
            int32 EquippedIndex = -1;
            const FName EquippedDisplayRow = bBagInWeaponMode ? GetEquippedWeaponDisplayRow() : NAME_None;
            if (!EquippedDisplayRow.IsNone())
            {
                for (int32 Index = 0; Index < CurrentBagItems.Num(); ++Index)
                {
                    if (CurrentBagItems[Index].RowName == EquippedDisplayRow)
                    {
                        EquippedIndex = Index;
                        break;
                    }
                }
            }

            SelectedBagIndex = (EquippedIndex >= 0)
                ? EquippedIndex
                : ((CurrentBagItems.Num() > 0) ? 0 : -1);

            if (EquippedIndex >= 0)
            {
                UE_LOG(LogTemp, Log,
                    TEXT("[Arm] 武器背包默认选中「当前装备」那一格：第 %d 格（行=%s / %s）"),
                    EquippedIndex, *EquippedDisplayRow.ToString(),
                    *CurrentBagItems[EquippedIndex].ToolName);
            }
            else if (SelectedBagIndex == 0)
            {
                UE_LOG(LogTemp, Log, TEXT("[Bag] 默认选中第 1 个物品：%s（品质 %d 星）"),
                    *CurrentBagItems[0].ToolName, CurrentBagItems[0].Star);
            }
            else
            {
                UE_LOG(LogTemp, Log, TEXT("[Bag] 当前分类没有物品，右侧详情留空。"));
            }
        }
    }

    // 物品数超过格子数时超出部分不会显示 —— 给个明确提示，避免「我的物品去哪了」
    if (CurrentBagItems.Num() > BagSlotWidgets.Num() && BagSlotWidgets.Num() > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Bag] 当前分类有 %d 件物品，但只生成了 %d 个格子，超出部分不显示。请加大 InventoryComponent 的 Slot Count（当前 %d）。"),
            CurrentBagItems.Num(), BagSlotWidgets.Num(),
            InventoryComponent ? InventoryComponent->SlotCount : 0);
    }

    for (int32 Index = 0; Index < BagSlotWidgets.Num(); ++Index)
    {
        const FBagItemEntry* Entry = CurrentBagItems.IsValidIndex(Index) ? &CurrentBagItems[Index] : nullptr;
        ApplyBagSlotVisual(Index, Entry);
    }

    UpdateBagCapacityText();
    UpdateBagKindHighlight();
    ShowBagItemDetail(CurrentBagItems.IsValidIndex(SelectedBagIndex) ? &CurrentBagItems[SelectedBagIndex] : nullptr);
}

// 刷新单格视觉。Entry 为 nullptr → 空槽（图标/数量隐藏，底色用 EmptySlotColor）
// 兜底：Image 没选 Brush 时，SetColorAndOpacity 只是「染色」而不是「换图」——
// 空刷子染完依然是空的，屏幕上什么都看不见（这是「格子没有物品时也要存在」最容易翻车的地方）。
// 这里检测到「没有可绘制资源」就装一个纯色笔刷，让后面的染色真正生效。
namespace BagVisualHelper
{
    static void EnsureSolidColorBrush(UImage* Image)
    {
        if (!Image)
        {
            return;
        }

        // 有贴图 / 图集资源 → 用户是故意用图的，完全不动它
        if (Image->GetBrush().GetResourceObject() != nullptr)
        {
            return;
        }

        // 没有资源 → 无论原来是空刷子还是纯色笔刷，换成纯白纯色笔刷都是安全的（白底 = 染色结果即目标色）
        Image->SetBrush(FSlateColorBrush(FLinearColor::White));
    }
}

void ABattleCharacter::ApplyBagSlotVisual(int32 SlotIndex, const FBagItemEntry* Entry)
{
    if (!BagSlotWidgets.IsValidIndex(SlotIndex))
        return;

    UUserWidget* Slot = BagSlotWidgets[SlotIndex];
    if (!Slot)
        return;

    UImage* IconImage = Cast<UImage>(Slot->GetWidgetFromName(TEXT("img_slot_icon")));
    UTextBlock* NumText = Cast<UTextBlock>(Slot->GetWidgetFromName(TEXT("txt_slot_num")));
    UTextBlock* StarText = Cast<UTextBlock>(Slot->GetWidgetFromName(TEXT("txt_slot_star")));
    UImage* BgImage = Cast<UImage>(Slot->GetWidgetFromName(TEXT("img_slot_bg")));
    UImage* SelectImage = Cast<UImage>(Slot->GetWidgetFromName(TEXT("img_slot_select")));
    UTextBlock* SlotNameText = Cast<UTextBlock>(Slot->GetWidgetFromName(TEXT("txt_slot_name")));

    const bool bHasItem = (Entry && Entry->bValid);

    // ---- 品质底色 / 空槽底色 ----
    if (BgImage)
    {
        // ★ 必须先确保有可绘制资源：img_slot_bg 是最容易漏选 Brush 的一个
        //   （手册也说了「漏了也能显示」，那就必须真的兜底，否则承诺与行为不符）
        BagVisualHelper::EnsureSolidColorBrush(BgImage);

        const FLinearColor BgColor = (bHasItem && InventoryComponent)
            ? InventoryComponent->GetStarColor(Entry->Star)
            : (InventoryComponent ? InventoryComponent->EmptySlotColor : FLinearColor(0.20f, 0.21f, 0.25f, 0.90f));
        BgImage->SetColorAndOpacity(BgColor);
    }

    // ---- 物品图标 ----
    if (IconImage)
    {
        if (bHasItem && Entry->ToolImage)
        {
            IconImage->SetBrushFromTexture(Entry->ToolImage, false);
            IconImage->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            IconImage->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    // ---- 数量角标（空槽隐藏）----
    // ★ 不可堆叠物品一格只有 1 个，再显示一个「1」角标既占地方又没有信息量 ——
    //   所以只在数量 > 1 时显示。这也让「可堆叠物品」和「一格一件」在视觉上可分。
    if (NumText)
    {
        if (bHasItem && Entry->ToolNum > 1)
        {
            NumText->SetText(FText::AsNumber(Entry->ToolNum));
            NumText->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            NumText->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    // ---- 星级文本 ----
    if (StarText)
    {
        if (bHasItem && InventoryComponent)
        {
            StarText->SetText(InventoryComponent->GetStarText(Entry->Star));
            StarText->SetColorAndOpacity(FSlateColor(InventoryComponent->GetStarColor(Entry->Star)));
            StarText->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            StarText->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    // ---- 格子内物品名（可选控件）----
    if (SlotNameText)
    {
        if (bHasItem)
        {
            SlotNameText->SetText(FText::FromString(Entry->ToolName));
            SlotNameText->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            SlotNameText->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    // ---- 「已装备」标记（只可能出现在武器那一格）----
    // 控件可选：WBP_Bag_Slot 里若没有 txt_slot_equipped，这里什么都不做（不报错）。
    // 为什么不改 img_slot_select：那是「当前选中」的通用高亮，被装备标记占用会让玩家
    // 分不清「我点中的」和「已装备的」—— 两者可以同时存在于两格上。
    if (UTextBlock* EquippedMark = Cast<UTextBlock>(Slot->GetWidgetFromName(TEXT("txt_slot_equipped"))))
    {
        // ★ 走统一判据，不再手写 `RowName == EquippedWeaponRow`：
        //   行号相等只是必要条件 —— 行号与手上那把刀脱钩时（声明回填 / 掉落物），
        //   背包会说「装备中」而角色拿的是另一把。详见 IsWeaponRowEquipped 的注释。
        //   传 InstanceNo：同类多把武器只标「真正装备的那一把」为装备中。
        const bool bIsEquipped = bHasItem && IsWeaponRowEquipped(Entry->RowName, Entry->InstanceNo);
        if (bIsEquipped)
        {
            EquippedMark->SetText(FText::FromString(TEXT("装备中")));
            EquippedMark->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            EquippedMark->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    // ---- 选中高亮 ----
    if (SelectImage)
    {
        BagVisualHelper::EnsureSolidColorBrush(SelectImage);
        SelectImage->SetColorAndOpacity(InventoryComponent
            ? InventoryComponent->SlotSelectColor
            : FLinearColor(0.90f, 0.75f, 0.38f, 0.55f));

        const bool bSelected = bHasItem && (SlotIndex == SelectedBagIndex);
        SelectImage->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
    }
}

// 刷新右侧详情面板（Entry 为 nullptr → 清空）
void ABattleCharacter::ShowBagItemDetail(const FBagItemEntry* Entry)
{
    if (!BagWidget)
        return;

    auto SetDetailText = [this](const TCHAR* WidgetName, const FString& Value)
    {
        if (UTextBlock* Text = Cast<UTextBlock>(BagWidget->GetWidgetFromName(WidgetName)))
        {
            Text->SetText(FText::FromString(Value));
        }
    };

    UImage* DetailIcon = Cast<UImage>(BagWidget->GetWidgetFromName(TEXT("img_detail_icon")));
    UImage* DetailStarBg = Cast<UImage>(BagWidget->GetWidgetFromName(TEXT("img_detail_star")));

    if (!Entry || !Entry->bValid)
    {
        SetDetailText(TEXT("txt_detail_name"), TEXT("-"));
        SetDetailText(TEXT("txt_detail_kind"), FString());
        SetDetailText(TEXT("txt_detail_star"), FString());
        SetDetailText(TEXT("txt_detail_num"), FString());
        SetDetailText(TEXT("txt_detail_stack"), FString());
        SetDetailText(TEXT("txt_detail_intro"), FString());

        if (DetailIcon)
        {
            DetailIcon->SetVisibility(ESlateVisibility::Hidden);
        }
        if (DetailStarBg)
        {
            DetailStarBg->SetVisibility(ESlateVisibility::Hidden);
        }
        return;
    }

    SetDetailText(TEXT("txt_detail_name"), Entry->ToolName.IsEmpty() ? Entry->ToolId.ToString() : Entry->ToolName);

    FString KindName;
    if (InventoryComponent)
    {
        const TArray<FText> KindNames = InventoryComponent->GetKindDisplayNames();
        if (KindNames.IsValidIndex(Entry->KindTool))
        {
            KindName = KindNames[Entry->KindTool].ToString();
        }
    }
    SetDetailText(TEXT("txt_detail_kind"), KindName);

    SetDetailText(TEXT("txt_detail_star"),
        InventoryComponent ? InventoryComponent->GetStarText(Entry->Star).ToString() : FString());

    // 数量：显示【该物品的总数】，而不是这一格里的数量。
    // 不可堆叠物品一格只放 1 个，玩家在详情面板里想看的是「我总共有几个」。
    const int32 TotalOwned = InventoryComponent
        ? InventoryComponent->GetTotalCountForItem(Entry->RowName)
        : Entry->ToolNum;
    const int32 OwnedSlots = InventoryComponent
        ? InventoryComponent->GetSlotCountForItem(Entry->RowName)
        : 1;

    SetDetailText(TEXT("txt_detail_num"), FString::Printf(TEXT("%d"), TotalOwned));

    // 堆叠说明：写「一格能放几个」这个结果，而不是原始的 max_duidie 字段 ——
    // 不可堆叠物品的 max_duidie 通常是 0，直接显示「上限 0」会让人以为是数据填错。
    if (InventoryComponent)
    {
        if (!Entry->bStackable)
        {
            SetDetailText(TEXT("txt_detail_stack"),
                FString::Printf(TEXT("不可堆叠 · 一格一件（占 %d 格）"), OwnedSlots));
        }
        else if (Entry->MaxStack <= 1)
        {
            SetDetailText(TEXT("txt_detail_stack"),
                TEXT("可堆叠 · 上限未填 → 视为无上限"));
        }
        else
        {
            SetDetailText(TEXT("txt_detail_stack"),
                FString::Printf(TEXT("可堆叠 · 上限 %d（占 %d 格）"),
                    InventoryComponent->GetEffectiveStackLimit(*Entry), OwnedSlots));
        }
    }
    else
    {
        SetDetailText(TEXT("txt_detail_stack"),
            Entry->bStackable ? TEXT("可堆叠") : TEXT("不可堆叠"));
    }

    // ---- 武器：在详情里明确写出「已装备 / 可装备」----
    // 为什么借用 txt_detail_stack 而不是新建控件：这个控件确定存在（不依赖你改名），
    // 而且它本来就是「这件物品的状态说明」，语义正好。
    if (IsWeaponItem(*Entry))
    {
        // 同样走统一判据。这里以前连 `!EquippedWeaponRow.IsNone()` 都没有 ——
        // 行号为空（没填 kind_tool 的掉落物、临时条目）时 `NAME_None == NAME_None` 成立，
        // 详情面板就会对着一个根本没装备的东西写「已装备」。
        FString State = IsWeaponRowEquipped(Entry->RowName, Entry->InstanceNo)
            ? TEXT("· ★ 已装备")
            : (bBagInWeaponMode
                ? (bEquipOnWeaponSlotClick ? TEXT("· 点一下即装备") : TEXT("· 点下面的按钮装备"))
                : TEXT("· 在角色面板的武器页里可装备"));

        // 同步关系在界面上也要看得见 —— 否则点上去只有一条日志，界面上毫无反应。
        // 三种情况分开说，因为修法完全不同：
        //   ① 没有武器类           → 去武器蓝图填 Bag Item Row（或角色上配 Weapon Class By Item Row）
        //   ② 有类、声明行也对     → 正常
        //   ③ 有类、但声明的是别的行 → 声明写歪了（以你点的这一行为准，但应改蓝图）
        if (const TSubclassOf<AWeaponBase> ResolvedClass = ResolveWeaponClassForItemRow(Entry->RowName))
        {
            // 同样先换算成行名再比：声明可能是 tool_id，
            // 不换算就会在界面上常驻一句「声明的是别的行」，而配置其实是对的。
            const FName DeclaredBagRow =
                ResolveBagKeyToInventoryRow(GetBagItemRowForWeaponClass(ResolvedClass));
            if (!DeclaredBagRow.IsNone() && DeclaredBagRow != Entry->RowName)
            {
                State += FString::Printf(TEXT("（⚠ %s 声明的是第『%s』行）"),
                    *ResolvedClass->GetName(), *DeclaredBagRow.ToString());
            }
        }
        else
        {
            State += TEXT("（⚠ 没有对应武器蓝图：在该武器蓝图里填 Bag Item Row）");
        }

        if (UTextBlock* StackText = Cast<UTextBlock>(BagWidget->GetWidgetFromName(TEXT("txt_detail_stack"))))
        {
            StackText->SetText(FText::FromString(
                FString::Printf(TEXT("%s %s"), *StackText->GetText().ToString(), *State)));
        }
    }

    SetDetailText(TEXT("txt_detail_intro"), Entry->ToolIntrodu);

    if (DetailIcon)
    {
        if (Entry->ToolImage)
        {
            DetailIcon->SetBrushFromTexture(Entry->ToolImage, false);
            DetailIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            DetailIcon->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    if (DetailStarBg && InventoryComponent)
    {
        BagVisualHelper::EnsureSolidColorBrush(DetailStarBg);
        DetailStarBg->SetColorAndOpacity(InventoryComponent->GetStarColor(Entry->Star));
        DetailStarBg->SetVisibility(ESlateVisibility::HitTestInvisible);
    }
}

// 顶栏：分类名 + 「拥有数 / 上限」
void ABattleCharacter::UpdateBagCapacityText()
{
    if (!BagWidget || !InventoryComponent)
        return;

    if (UTextBlock* CapacityText = Cast<UTextBlock>(BagWidget->GetWidgetFromName(TEXT("txt_capacity"))))
    {
        const int32 Owned = InventoryComponent->GetOwnedCountForKind(CurrentBagKind);
        const int32 Capacity = InventoryComponent->GetCapacityForKind(CurrentBagKind);
        CapacityText->SetText(FText::FromString(FString::Printf(TEXT("%d/%d"), Owned, Capacity)));
    }

    if (UTextBlock* TitleText = Cast<UTextBlock>(BagWidget->GetWidgetFromName(TEXT("txt_kind_title"))))
    {
        const TArray<FText> KindNames = InventoryComponent->GetKindDisplayNames();
        if (CurrentBagKind < 0)
        {
            TitleText->SetText(FText::FromString(TEXT("全部")));
        }
        else if (KindNames.IsValidIndex(CurrentBagKind))
        {
            TitleText->SetText(KindNames[CurrentBagKind]);
        }
    }
}

// 左侧分类按钮：写入分类名 + 选中高亮
void ABattleCharacter::UpdateBagKindHighlight()
{
    if (!BagWidget || !InventoryComponent)
        return;

    const TArray<FText> KindNames = InventoryComponent->GetKindDisplayNames();

    auto StyleKindButton = [this](const TCHAR* ButtonName, const TCHAR* TextName, bool bSelected, const FText& Label)
    {
        if (UButton* Button = Cast<UButton>(BagWidget->GetWidgetFromName(ButtonName)))
        {
            Button->SetBackgroundColor(bSelected
                ? InventoryComponent->KindSelectedColor
                : InventoryComponent->KindNormalColor);
        }
        if (TextName)
        {
            if (UTextBlock* LabelText = Cast<UTextBlock>(BagWidget->GetWidgetFromName(TextName)))
            {
                LabelText->SetText(Label);
            }
        }
    };

    // 「全部」（可选控件）
    StyleKindButton(TEXT("kind_all"), TEXT("kind_txt_all"), CurrentBagKind < 0, FText::FromString(TEXT("全部")));

    for (int32 Kind = 0; Kind < InventoryComponent->KindCount; ++Kind)
    {
        const FString ButtonName = FString::Printf(TEXT("kind_%d"), Kind);
        const FString TextName = FString::Printf(TEXT("kind_txt_%d"), Kind);
        StyleKindButton(*ButtonName, *TextName, CurrentBagKind == Kind,
            KindNames.IsValidIndex(Kind) ? KindNames[Kind] : FText::GetEmpty());
    }
}

// 创建并登记一个动态按钮回调载体（保证被 GC 引用，随背包一起销毁）
UBagWidgetAction* ABattleCharacter::MakeBagAction(EBagActionType ActionType, int32 Index)
{
    UBagWidgetAction* Action = NewObject<UBagWidgetAction>(this);
    Action->ActionType = ActionType;
    Action->Index = Index;
    Action->Owner = this;
    BagActions.Add(Action);
    return Action;
}

// ---- 角色面板属性填充（核心）----
// 用指定角色（Source）的属性填充 WBP_Character_imf 的控件：
//   chara_name               → 角色游戏名称（CharacterName）
//   pict_chara               → 角色图像（CharacterPortrait，UTexture2D）
//   something_show           → 背景/头像展示图（同步 CharacterPortrait）
//   level_show_num           → 角色等级（CharacterLevel）
//   attack_num               → 总攻击值（GetTotalAttackPower = 基础 + 武器 + 装备）
//   health_num               → 最大血量（MaxHealth）
//   critical_hit_num         → 暴击率（GetCritRate，整数百分比，如 5 = 5%）
//   critical_hit_damage_num  → 暴击伤害（GetCritDamage，整数百分比，如 150 = 150%）
// Source 为 nullptr 时填默认空值：数值显示 0（暴击率/伤害显示 0%）、头像为空、名称为 "Null"。
// 数值统一取整显示；文本控件不存在时静默跳过（不阻塞面板打开）。
void ABattleCharacter::ApplyCharacterPanelFrom(ABattleCharacter* Source)
{
    if (!CharacterPanelWidget)
        return;

    UUserWidget* Widget = CharacterPanelWidget;
    if (!Widget)
        return;

    // Source 为空 → 默认空值（未绑定角色的显示）
    const FString NameStr = Source ? Source->CharacterName : TEXT("Null");
    UTexture2D* Portrait = Source ? Source->CharacterPortrait : nullptr;
    const int32 Level = Source ? Source->CharacterLevel : 0;
    const float TotalAtk = Source ? Source->GetTotalAttackPower() : 0.0f;
    const float MaxHP = Source ? Source->MaxHealth : 0.0f;
    const float CritRate = Source ? Source->GetCritRate() : 0.0f;
    const float CritDamage = Source ? Source->GetCritDamage() : 0.0f;

    // 角色游戏名称（未绑定 → "Null"）
    if (UTextBlock* NameText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("chara_name"))))
    {
        NameText->SetText(FText::FromString(NameStr));
    }

    // 角色图像（头像/立绘；未绑定 → 空）
    if (UImage* PortraitImg = Cast<UImage>(Widget->GetWidgetFromName(TEXT("pict_chara"))))
    {
        PortraitImg->SetBrushFromTexture(Portrait);
    }

    // something_show 展示图：同步头像（未绑定 → 空）
    if (UImage* ShowImage = Cast<UImage>(Widget->GetWidgetFromName(TEXT("something_show"))))
    {
        ShowImage->SetBrushFromTexture(Portrait);
    }

    // 角色等级（未绑定 → 0）
    if (UTextBlock* LevelText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("level_show_num"))))
    {
        LevelText->SetText(FText::AsNumber(Level));
    }

    // 总攻击值（基础攻击力 + 武器攻击力 + 装备总攻击力；未绑定 → 0）
    if (UTextBlock* AttackText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("attack_num"))))
    {
        AttackText->SetText(FText::AsNumber(FMath::RoundToInt(TotalAtk)));
    }

    // 最大血量（未绑定 → 0）
    if (UTextBlock* HealthText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("health_num"))))
    {
        HealthText->SetText(FText::AsNumber(FMath::RoundToInt(MaxHP)));
    }

    // 暴击率（整数百分比，显示带 % 号，如 "5%"；未绑定 → "0%"）
    if (UTextBlock* CritRateText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("critical_hit_num"))))
    {
        CritRateText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(CritRate))));
    }

    // 暴击伤害（整数百分比，显示带 % 号，如 "150%"；未绑定 → "0%"）
    if (UTextBlock* CritDamageText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("critical_hit_damage_num"))))
    {
        CritDamageText->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(CritDamage))));
    }

    UE_LOG(LogTemp, Log, TEXT("Character panel stats applied: Name=%s, Level=%d, Atk=%.0f, MaxHP=%.0f, CritRate=%.0f%%, CritDamage=%.0f%%"),
        *NameStr, Level, TotalAtk, MaxHP, CritRate, CritDamage);
}

// 打开角色面板时，把当前角色自身（this）的实时属性写入面板。
// 保留此入口以兼容原有调用（OpenCharacterPanel 中默认显示自身）。
void ABattleCharacter::UpdateCharacterPanelStats()
{
    ApplyCharacterPanelFrom(this);
}

// ---- 角色槽位选择（chara_pitc_head 内 chara_1、chara_2...）----

// 应用第 SlotIndex 个角色槽位（0 → chara_1，1 → chara_2 ...）：
// 取 CharacterSlotClasses[SlotIndex] 对应角色的属性填充面板；槽位越界或类为空（未绑定）→ 填默认空值。
//
// 【实时 vs 默认】若槽位类就是当前角色自身的类（this->IsA(SlotClass)），则用 this 实例——
// 这样能读到运行时实时属性：AttributeSet 里的暴击率/暴击伤害（BeginPlay 从 BaseCritRate/BaseCritDamage 写入）、
// 以及已装备武器的攻击力加成（GetWeaponAttackPower）。否则（其他角色类）读其 CDO 默认属性。
// 解析「当前正在看的角色」实例（数据表优先，表无效回退旧数组）：
//  -1 → nullptr；选中类 == this 的类 → this；否则 → 该类 CDO。
//  ApplyCharacterSlot / RefreshArmPanel 共用这一份解析，避免两处写同一套「表 vs 旧数组」逻辑漂移。
ABattleCharacter* ABattleCharacter::ResolveSelectedCharaSource()
{
    const int32 SlotIndex = CurrentCharaSlotIndex;
    if (SlotIndex < 0)
    {
        return nullptr;
    }

    // ① 数据表优先
    if (EnsureCharaInfoTable())
    {
        const TArray<FName> Rows = GetOwnedCharaRows();
        if (Rows.IsValidIndex(SlotIndex))
        {
            const FCharaInfoEntry* Info = FindCharaInfo(Rows[SlotIndex]);
            if (Info && Info->CharaClass)
            {
                return IsA(Info->CharaClass) ? this : Info->CharaClass->GetDefaultObject<ABattleCharacter>();
            }
        }
        return nullptr;
    }

    // ② 回退旧数组
    if (!CharacterSlotClasses.IsValidIndex(SlotIndex))
    {
        return nullptr;
    }
    TSubclassOf<ABattleCharacter> SlotClass = CharacterSlotClasses[SlotIndex];
    if (!SlotClass)
    {
        return nullptr;
    }
    return IsA(SlotClass) ? this : SlotClass->GetDefaultObject<ABattleCharacter>();
}

EJobClass ABattleCharacter::GetWeaponBagFilterJobClass()
{
    // 只有「从角色面板武器页打开武器背包」时，职位筛选才有意义；此时玩家是在给
    // 【面板选中的角色】挑武器，筛选用选中角色的职位，而不是当前操控角色 this 的职位。
    //
    // 为什么不能用 this->JobClass：角色面板允许「选中查看其他角色」（CurrentCharaSlotIndex
    // 指向别人），此时 this 仍是当前操控角色（如剑士 BP_PlayerCharacter），用 this 的职位
    // 筛选就会显示轻剑，看起来像「跳到了 BP_PlayerCharacter 的背包」——正是本轮 bug。
    const ABattleCharacter* Selected = ResolveSelectedCharaSource();
    if (Selected && Selected != this)
    {
        return Selected->JobClass;
    }
    return JobClass;
}

void ABattleCharacter::ApplyCharacterSlot(int32 SlotIndex)
{
    // 记录「当前正在看哪个槽位」—— RefreshCharacterPanelStats 靠它做重算。
    // 放在最前面（早于下面所有提前 return）：槽位越界 / 未绑定类也要记住，
    // 这样重算时能还原出同样的「显示空值」，而不是退回显示当前角色的实时属性。
    CurrentCharaSlotIndex = SlotIndex;

    ABattleCharacter* Source = ResolveSelectedCharaSource();
    if (!Source)
    {
        // 槽位越界 / 未绑定类 / 表里不存在 → 未绑定角色 → 空值（0 / 0% / 空图 / "Null"）
        ApplyCharacterPanelFrom(nullptr);
        RefreshCharaHeadSelection();
        UE_LOG(LogTemp, Log, TEXT("[CharaHead] 槽位 %d 无绑定角色（表里不存在或未填角色类）→ 显示空值。"), SlotIndex);
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("Character slot %d -> %s."), SlotIndex,
        Source == this ? TEXT("live instance (this)") : TEXT("class CDO"));

    ApplyCharacterPanelFrom(Source);
    RefreshCharaHeadSelection();

    // ★ 切换角色后，还要刷新【当前激活的页签】内容，而不是只刷新角色页。
    //   否则「先点 mod_butt 切到武器页 → 再点头像换角色」时，武器页还停在旧角色的武器上。
    //   ApplyCharacterPanelFrom 已经覆盖了「角色页」（Chara）的属性，
    //   但武器页（Arm）的 RefreshArmPanel 只在 SwitchCharacterPanelTab 切页时被调一次，
    //   切角色不会触发它 —— 这里按当前页签补刷，保证任何页签下换角色内容都跟着变。
    if (CurrentPanelTab == ECharacterPanelTab::Arm)
    {
        RefreshArmPanel();
    }
    else if (CurrentPanelTab == ECharacterPanelTab::Chara)
    {
        // 角色页属性已由上面的 ApplyCharacterPanelFrom 写好了，无需重复；
        // 这里留分支仅为将来 Yiqi/Mingzuo 页签接入时照此补刷，逻辑对称。
    }
}

// 绑定 chara_pitc_head 内角色头像按钮（chara_1、chara_2...）的 OnClicked。
// 与 BindCharacterPanelTabs 同理：OnClicked 无参委托，AddDynamic 需字面量函数名，
// 因此每个槽位按钮独立 if 分支 + 专用 UFUNCTION 回调。
void ABattleCharacter::BindCharacterSlotButtons()
{
    if (!CharacterPanelWidget)
        return;

    UUserWidget* Widget = CharacterPanelWidget;

    // chara_1 → 槽位 0
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("chara_1"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnCharaSlot1Clicked);
    }

    // chara_2 → 槽位 1
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("chara_2"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnCharaSlot2Clicked);
    }

    // chara_3 → 槽位 2
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("chara_3"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnCharaSlot3Clicked);
    }

    // chara_4 → 槽位 3
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("chara_4"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnCharaSlot4Clicked);
    }

    UE_LOG(LogTemp, Log, TEXT("Character slot buttons bound."));
}

// 各角色槽位按钮回调：转发到 ApplyCharacterSlot（槽位索引 = 按钮编号 - 1）。
void ABattleCharacter::OnCharaSlot1Clicked() { ApplyCharacterSlot(0); }
void ABattleCharacter::OnCharaSlot2Clicked() { ApplyCharacterSlot(1); }
void ABattleCharacter::OnCharaSlot3Clicked() { ApplyCharacterSlot(2); }
void ABattleCharacter::OnCharaSlot4Clicked() { ApplyCharacterSlot(3); }

// =====================================================================
// ---- 角色信息表（Data_chara_imfor）----
// =====================================================================

UDataTable* ABattleCharacter::EnsureCharaInfoTable()
{
    if (CharaInfoTable)
        return CharaInfoTable;

    // 懒加载约定路径（与背包 / 角色面板同一个防卡顿模式：不在构造函数里加载）
    CharaInfoTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/UI/chara_imf/Data_chara_imfor.Data_chara_imfor"));
    if (!CharaInfoTable)
    {
        // 不当报错刷屏：表还没建是正常状态（回退 CharacterSlotClasses 旧行为），只打一次
        static bool bWarnedOnce = false;
        if (!bWarnedOnce)
        {
            bWarnedOnce = true;
            UE_LOG(LogTemp, Log,
                TEXT("[CharaHead] 未找到 /Game/UI/chara_imf/Data_chara_imfor（角色信息表）。\n"
                     "      角色面板头像暂用旧的 CharacterSlotClasses 数组；\n"
                     "      建表后（行结构 CharaInfoEntry，每行填角色蓝图类+头像）两处 UI 自动切到表驱动。"));
        }
    }
    return CharaInfoTable;
}

const FCharaInfoEntry* ABattleCharacter::FindCharaInfo(FName RowName) const
{
    if (!CharaInfoTable || RowName.IsNone())
        return nullptr;
    return CharaInfoTable->FindRow<FCharaInfoEntry>(RowName, TEXT("FindCharaInfo"), /*bWarnIfRowMissing*/ false);
}

bool ABattleCharacter::GetCharaInfoByRow(FName RowName, FCharaInfoEntry& OutInfo) const
{
    const FCharaInfoEntry* Info = FindCharaInfo(RowName);
    if (!Info)
        return false;
    OutInfo = *Info;
    return true;
}

TArray<FName> ABattleCharacter::GetOwnedCharaRows() const
{
    TArray<FName> Rows;

    // ① 表有效：行名来自表（按 OwnedCharaRows 过滤；空 = 全部拥有）
    //    const 函数里调懒加载不合适 —— 表没加载时这里返回空，调用方先用 EnsureCharaInfoTable() 预热。
    if (CharaInfoTable)
    {
        // 全表行名
        TArray<FName> AllRows = CharaInfoTable->GetRowNames();
        if (OwnedCharaRows.Num() == 0)
        {
            Rows = AllRows;
        }
        else
        {
            for (const FName& Owned : OwnedCharaRows)
            {
                // ★ 去重：拥有列表里同一行填了两次 → 快编网格出两个相同头像格子，
                //   两个都选会把同一角色写进队伍两位（2026-09-16 实测 slot_1/slot_2 同为 10002）。
                if (Rows.Contains(Owned))
                {
                    continue;
                }
                if (AllRows.Contains(Owned))
                {
                    Rows.Add(Owned);
                }
                else
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[CharaHead] Owned Chara Rows 里的『%s』在 Data_chara_imfor 中不存在 → 已跳过（检查行名拼写）。"),
                        *Owned.ToString());
                }
            }
        }
        return Rows;
    }

    // ② 表无效：回退旧数组 —— 行名用 NAME_None 占位（数量与旧槽位一致，保持旧行为可用）
    for (int32 i = 0; i < CharacterSlotClasses.Num(); ++i)
    {
        Rows.Add(NAME_None);
    }
    return Rows;
}

// ---- 动态头像格子（chara_pitc_head 自动添加 + 选中框；与背包 WBP_Bag_Slot 同一套「独立格子 + 填充」模式）----
TSubclassOf<UUserWidget> ABattleCharacter::GetCharaHeadSlotClass()
{
    // 懒加载约定路径（与 RebuildCharaHeadList 同一份；可在蓝图里 CharaHeadSlotClass 显式覆盖）
    if (!CharaHeadSlotClass)
    {
        CharaHeadSlotClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_CharaHead_Slot.WBP_CharaHead_Slot_C"));
    }
    return CharaHeadSlotClass;
}

void ABattleCharacter::RebuildCharaHeadList()
{
    if (!CharacterPanelWidget)
        return;

    // 面板每次打开都重建 Widget → 旧格子/旧载体/旧选中框全部过期，一律清空重来
    CharaHeadSlotWidgets.Reset();
    CharaHeadActions.Reset();

    UPanelWidget* Panel = Cast<UPanelWidget>(CharacterPanelWidget->GetWidgetFromName(TEXT("chara_pitc_head")));
    if (!Panel)
    {
        // WBP 里找不到 chara_pitc_head（被改名/删除）→ 回退旧的硬编码按钮绑定
        UE_LOG(LogTemp, Warning,
            TEXT("[CharaHead] WBP_Character_imf 里没找到 chara_pitc_head 容器 → 回退旧的 chara_1~4 硬编码按钮。\n"
                 "      想用「自动添加头像」：打开 WBP_Character_imf，确认存在名为 chara_pitc_head 的容器控件。"));
        BindCharacterSlotButtons();
        return;
    }

    // 头像格子类懒加载（约定路径 /Game/UI/WBP_CharaHead_Slot，同 WBP_Bag_Slot 的懒加载模式）
    if (!CharaHeadSlotClass)
    {
        CharaHeadSlotClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_CharaHead_Slot.WBP_CharaHead_Slot_C"));
    }
    if (!CharaHeadSlotClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[CharaHead] CharaHeadSlotClass 未设置，无法生成头像格子。\n"
                 "      请创建 WBP_CharaHead_Slot（/Game/UI），或在 BP_PlayerCharacter 的 UI 分类里挂上。\n"
                 "      格子内部约定控件名：img_head_icon（头像图）、img_select_frame（金色选中框）、btn_head（点击按钮）。"));
        return;
    }

    // 清空 WBP 里硬编码的旧头像按钮（chara_1/chara_2...）—— 它们连着的蓝图图表事件不受影响
    while (Panel->GetChildrenCount() > 0)
    {
        Panel->RemoveChildAt(0);
    }

    // 拥有的角色列表（表驱动；表无效时行名为 NAME_None 占位、数量 = 旧数组）
    UDataTable* Table = EnsureCharaInfoTable();
    const TArray<FName> Rows = GetOwnedCharaRows();
    if (Rows.Num() == 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[CharaHead] 没有任何角色可显示：Data_chara_imfor 为空（或未建表且 CharacterSlotClasses 为空）。\n"
                 "      修法：建表 /Game/UI/chara_imf/Data_chara_imfor（行结构 CharaInfoEntry）并每行填一个角色；\n"
                 "            或者仍用旧方式：BP_PlayerCharacter → Character Slot Classes 填角色蓝图类。"));
        return;
    }

    UCanvasPanel* Canvas = Cast<UCanvasPanel>(Panel);

    int32 FailedCount = 0;
    for (int32 i = 0; i < Rows.Num(); ++i)
    {
        // ★ 与背包格子同构：每个头像格子用 CharaHeadSlotClass 独立 CreateWidget，再套一层
        //   USizeBox 把尺寸钉死（防格子根节点是 Canvas Panel 时塌成 0×0）。
        UUserWidget* Slot = CreateWidget<UUserWidget>(GetWorld(), CharaHeadSlotClass);
        if (!Slot)
        {
            ++FailedCount;
            continue;
        }

        USizeBox* CellBox = NewObject<USizeBox>(CharacterPanelWidget);
        CellBox->SetWidthOverride(90.f);
        CellBox->SetHeightOverride(90.f);

        UPanelSlot* CellContentSlot = CellBox->AddChild(Slot);
        UWidget* CellRoot = CellBox;
        if (!CellContentSlot)
        {
            // AddChild 返回空（正常不会发生）→ 退化为直接挂格子，别丢
            UE_LOG(LogTemp, Warning,
                TEXT("[CharaHead] 第 %d 个头像格子的 SizeBox 兜底层挂载失败（AddChild 返回空），已退化为直接挂到容器。"), i);
            CellRoot = Slot;
        }

        // 放进容器：Canvas → 逐个竖排定位；其他容器 → 交给容器自己的布局
        if (Canvas)
        {
            if (UCanvasPanelSlot* CSlot = Canvas->AddChildToCanvas(CellRoot))
            {
                // 90×90 格子 + 10px 间隔（步进 100 = 90 格子 + 10 间隔）
                CSlot->SetPosition(FVector2D(6.f, 6.f + i * 100.f));
                CSlot->SetSize(FVector2D(90.f, 90.f));
            }
        }
        else
        {
            Panel->AddChild(CellRoot);
        }

        CharaHeadSlotWidgets.Add(Slot);

        // 整格点击：head_butt（格子内部按钮，WBP_CharaHead_Slot 里的实际名字）；
        //   兼容旧约定 btn_head；都找不到就绑格子根节点（若它本身是 Button）。
        UButton* SlotButton = Cast<UButton>(Slot->GetWidgetFromName(TEXT("head_butt")));
        if (!SlotButton)
        {
            SlotButton = Cast<UButton>(Slot->GetWidgetFromName(TEXT("btn_head")));
        }
        if (!SlotButton)
        {
            SlotButton = Cast<UButton>(Slot->GetRootWidget());
        }
        if (SlotButton)
        {
            // 点击：载体转发（OnClicked 无参委托 → Index 走载体，与背包格子同一个模式）
            UBagWidgetAction* Action = MakeBagAction(EBagActionType::CharaHead, i);
            SlotButton->OnClicked.AddDynamic(Action, &UBagWidgetAction::OnClicked);
            CharaHeadActions.Add(Action);
        }
    }

    // 逐格填充头像（选中态统一由 RefreshCharaHeadSelection 打，这里先填头像图 + 等级）
    for (int32 i = 0; i < CharaHeadSlotWidgets.Num(); ++i)
    {
        UTexture2D* HeadTex = nullptr;
        int32 Level = 0;
        if (Table && Rows.IsValidIndex(i) && !Rows[i].IsNone())
        {
            if (const FCharaInfoEntry* Info = FindCharaInfo(Rows[i]))
            {
                HeadTex = Info->HeadIcon.Get();
                // 等级从角色蓝图 CDO 取（GetCharacterLevelValue 是 protected 字段的 getter）
                if (Info->CharaClass)
                {
                    if (const ABattleCharacter* CDO = Info->CharaClass->GetDefaultObject<ABattleCharacter>())
                    {
                        Level = CDO->GetCharacterLevelValue();
                    }
                }
            }
        }
        else if (CharacterSlotClasses.IsValidIndex(i) && CharacterSlotClasses[i])
        {
            if (const ABattleCharacter* CDO = CharacterSlotClasses[i]->GetDefaultObject<ABattleCharacter>())
            {
                HeadTex = CDO->CharacterPortrait;
                Level = CDO->GetCharacterLevelValue();
            }
        }
        ApplyCharaHeadSlotVisual(i, HeadTex, false, Level);
    }

    if (FailedCount > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[CharaHead] 有 %d 个头像格子创建失败（CreateWidget 返回空）。请确认 CharaHeadSlotClass 指向 WBP_CharaHead_Slot。"), FailedCount);
    }

    UE_LOG(LogTemp, Log, TEXT("[CharaHead] 已按拥有的角色生成 %d 个头像格子。"), CharaHeadSlotWidgets.Num());
}

// 填充第 SlotIndex 个头像格子：头像图 + 选中框 + 等级（按约定控件名取，与背包 ApplyBagSlotVisual 同一模式）。
// ★ 强制控件在其父槽里铺满（与 CharaTeamWidget.cpp 的 ForceFillWithinParent 同款）。
//   WBP_CharaHead_Slot 的子控件是 CanvasPanelSlot 绝对定位（WBP 设计时固定尺寸），
//   格子外套的 SizeBox 改尺寸后子控件不会自动跟随 —— 框/头像/按钮必须铺满格子是
//   功能语义（框住整个格子、覆盖整个可点区域），由 C++ 保证，等级文本位置不动。
namespace
{
	void ForceFillWithinParent(UWidget* W)
	{
		if (!W || !W->Slot)
		{
			return;
		}
		if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(W->Slot))
		{
			Canvas->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
			Canvas->SetOffsets(FMargin(0.f));
		}
		else if (UOverlaySlot* Overlay = Cast<UOverlaySlot>(W->Slot))
		{
			Overlay->SetHorizontalAlignment(HAlign_Fill);
			Overlay->SetVerticalAlignment(VAlign_Fill);
		}
	}
}

void ABattleCharacter::ApplyCharaHeadSlotVisual(int32 SlotIndex, UTexture2D* HeadTex, bool bSelected, int32 Level)
{
    if (!CharaHeadSlotWidgets.IsValidIndex(SlotIndex))
        return;

    UUserWidget* Slot = CharaHeadSlotWidgets[SlotIndex];
    if (!Slot)
        return;

    // ---- 头像图（img_head_icon）----
    if (UImage* HeadImg = Cast<UImage>(Slot->GetWidgetFromName(TEXT("img_head_icon"))))
    {
        // ★ 头像铺满格子（防格子尺寸变更后头像/框脱节）
        ForceFillWithinParent(HeadImg);
        if (HeadTex)
        {
            HeadImg->SetBrushFromTexture(HeadTex, false);
            HeadImg->SetVisibility(ESlateVisibility::HitTestInvisible);
            HeadImg->SetColorAndOpacity(FLinearColor::White);
        }
        else
        {
            // 没配头像：给一个可辨识的占位色（避免「格子看不见」被当成功能没生效）
            HeadImg->SetVisibility(ESlateVisibility::HitTestInvisible);
            HeadImg->SetColorAndOpacity(FLinearColor(0.25f, 0.30f, 0.40f, 1.f));
        }
    }

    // ---- 等级（tx_level_chara）----
    if (UTextBlock* LevelText = Cast<UTextBlock>(Slot->GetWidgetFromName(TEXT("tx_level_chara"))))
    {
        LevelText->SetText(Level > 0
            ? FText::FromString(FString::Printf(TEXT("Lv.%d"), Level))
            : FText::GetEmpty());
    }

    // ---- 选中框（img_select_frame）：bSelected 显示，否则隐藏 ----
    if (UImage* FrameImg = Cast<UImage>(Slot->GetWidgetFromName(TEXT("img_select_frame"))))
    {
        // ★ 选中框铺满格子（「框比头像小」根因修复：WBP 固定尺寸不跟随 SizeBox 缩放）
        ForceFillWithinParent(FrameImg);
        FrameImg->SetVisibility(bSelected ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
    }
}

void ABattleCharacter::RefreshCharaHeadSelection()
{
    for (int32 i = 0; i < CharaHeadSlotWidgets.Num(); ++i)
    {
        if (UUserWidget* Slot = CharaHeadSlotWidgets[i].Get())
        {
            if (UImage* FrameImg = Cast<UImage>(Slot->GetWidgetFromName(TEXT("img_select_frame"))))
            {
                FrameImg->SetVisibility(i == CurrentCharaSlotIndex ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
            }
        }
    }
}

// =====================================================================
// ---- 角色编队（L 键；纯 C++ 构建界面）----
// =====================================================================

void ABattleCharacter::EnsureTeamsArray()
{
    // 默认 8 支队伍（不够自动补，配多了不裁 —— 配了就别丢）
    if (Teams.Num() < 8)
    {
        Teams.SetNum(8);
    }
}

const TArray<FName>& ABattleCharacter::GetTeamMembers(int32 TeamIndex)
{
    EnsureTeamsArray();
    TeamIndex = FMath::Clamp(TeamIndex, 0, Teams.Num() - 1);
    // 惰性补齐到 3 个（None = 空）—— 上层读的时候不用再判长度
    while (Teams[TeamIndex].MemberRows.Num() < 3)
    {
        Teams[TeamIndex].MemberRows.Add(NAME_None);
    }
    return Teams[TeamIndex].MemberRows;
}

void ABattleCharacter::SetTeamMembers(int32 TeamIndex, const TArray<FName>& NewMembers)
{
    EnsureTeamsArray();
    TeamIndex = FMath::Clamp(TeamIndex, 0, Teams.Num() - 1);

    TArray<FName>& Members = Teams[TeamIndex].MemberRows;
    Members.SetNum(3);

    // 逐位覆盖：第 i 位有新选择 → 覆盖；没有 → 保持原样（「已选择就覆盖，为空就填充」）
    for (int32 i = 0; i < 3; ++i)
    {
        if (NewMembers.IsValidIndex(i) && !NewMembers[i].IsNone())
        {
            Members[i] = NewMembers[i];
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[Team] 队伍 %d 已更新：[%s | %s | %s]（未选到的位保持原样）"),
        TeamIndex + 1,
        *Members[0].ToString(), *Members[1].ToString(), *Members[2].ToString());
}

void ABattleCharacter::OverwriteTeamMembers(int32 TeamIndex, const TArray<FName>& NewMembers)
{
    EnsureTeamsArray();
    TeamIndex = FMath::Clamp(TeamIndex, 0, Teams.Num() - 1);

    TArray<FName>& Members = Teams[TeamIndex].MemberRows;
    Members.SetNum(3);

    // 整体替换：本轮选了 N 个（0<=N<=3）就写 N 个，其余位清空为 None。
    // ★ 关键差异：不再「未选到的位保持原样」—— 重新编队是新的一轮，选得少就该让多余位空掉，
    //   而不是残留上一次的旧选择。
    for (int32 i = 0; i < 3; ++i)
    {
        Members[i] = (NewMembers.IsValidIndex(i) && !NewMembers[i].IsNone()) ? NewMembers[i] : NAME_None;
    }

    UE_LOG(LogTemp, Log, TEXT("[Team] 队伍 %d 已整体覆盖：[%s | %s | %s]（未选到的位已清空）"),
        TeamIndex + 1,
        *Members[0].ToString(), *Members[1].ToString(), *Members[2].ToString());
}

FName ABattleCharacter::ResolveDefaultMemberRow() const
{
    // 玩家自己（this 的类）在 Data_chara_imfor 里对应的行名。
    if (!CharaInfoTable)
    {
        return NAME_None;
    }
    const TArray<FName> Rows = CharaInfoTable->GetRowNames();
    for (const FName& Row : Rows)
    {
        if (const FCharaInfoEntry* Info = FindCharaInfo(Row))
        {
            if (Info->CharaClass && Info->CharaClass == GetClass())
            {
                return Row;
            }
        }
    }
    return NAME_None;
}

bool ABattleCharacter::EnsureCharaTeamInput()
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return false;

    UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
    if (!Subsystem)
        return false;

    if (!CharaTeamAction)
    {
        CharaTeamAction = NewObject<UInputAction>(this);
        CharaTeamAction->ValueType = EInputActionValueType::Boolean;
        CharaTeamAction->Triggers.Add(NewObject<UInputTriggerPressed>(CharaTeamAction));
        // 编队打开后游戏暂停，必须允许暂停时触发，否则 L 关不掉编队
        CharaTeamAction->bTriggerWhenPaused = true;
    }

    if (!CharaTeamMappingContext)
    {
        CharaTeamMappingContext = NewObject<UInputMappingContext>(this);
        CharaTeamMappingContext->MapKey(CharaTeamAction, EKeys::L);
    }

    if (!bCharaTeamContextAdded)
    {
        Subsystem->AddMappingContext(CharaTeamMappingContext, 2);
        bCharaTeamContextAdded = true;
    }

    return true;
}

void ABattleCharacter::ToggleCharaTeamUI()
{
    if (bCharaTeamOpen)
    {
        CloseCharaTeamUI();
    }
    else
    {
        OpenCharaTeamUI();
    }
}

void ABattleCharacter::OpenCharaTeamUI()
{
    if (bCharaTeamOpen)
        return;

    // 与背包 / 角色面板互斥（三个全屏 UI 不叠放）
    if (bBagOpen)
    {
        CloseBag();
    }
    if (bCharacterPanelOpen)
    {
        CloseCharacterPanel();
    }

    if (!CharaTeamWidget)
    {
        // ★ 加载 WBP 用 LoadClass<UUserWidget>（与 OpenCharacterPanel 一致）：
        //   即使 WBP 父类设错（仍是 UserWidget 而非 CharaTeamWidget）也能加载到，
        //   避免 LoadClass<UCharaTeamWidget> 因父类不匹配返回 null 而完全拿不到资产。
        //   加载后 Cast<UCharaTeamWidget> 判父类：
        //     - Cast 成功（WBP 父类 = CharaTeamWidget）→ 用 WBP 设计树（BindDesignedWidgets 生效）；
        //     - Cast 失败（WBP 父类 = UserWidget）→ 回退纯 C++ 构建，并打日志提醒改父类。
        TSubclassOf<UUserWidget> LoadedClass = nullptr;
        if (!CharaTeamWidgetClass)
        {
            LoadedClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_CharaTeam.WBP_CharaTeam_C"));
        }

        if (CharaTeamWidgetClass)
        {
            // 蓝图里显式配了 CharaTeamWidgetClass → 优先用（它必须是 UCharaTeamWidget 子类）
            CharaTeamWidget = CreateWidget<UCharaTeamWidget>(GetWorld(), CharaTeamWidgetClass);
        }
        else if (LoadedClass)
        {
            // 懒加载 WBP → 先创建再 Cast 判父类
            if (UUserWidget* LoadedWidget = CreateWidget<UUserWidget>(GetWorld(), LoadedClass))
            {
                if (UCharaTeamWidget* AsTeam = Cast<UCharaTeamWidget>(LoadedWidget))
                {
                    CharaTeamWidget = AsTeam;
                }
                else
                {
                    // WBP 父类设错（是 UserWidget 不是 CharaTeamWidget）：丢弃它，回退纯 C++。
                    LoadedWidget->RemoveFromParent();
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Team] WBP_CharaTeam 的父类不是 CharaTeamWidget（仍是 UserWidget）！"
                             "请在编辑器里把它的 Parent Class 改成 CharaTeamWidget，否则只能走纯 C++ 构建界面。"));
                }
            }
        }

        // 仍然没有（没建 WBP / 父类错误）→ 纯 C++ 类回退
        if (!CharaTeamWidget)
        {
            CharaTeamWidget = CreateWidget<UCharaTeamWidget>(GetWorld(), UCharaTeamWidget::StaticClass());
        }
    }
    if (!CharaTeamWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Team] 编队界面创建失败！"));
        return;
    }

    CharaTeamWidget->OwnerCharacter = this;

    // ★ 预热角色信息表：RefreshAll / RefreshQuickEdit 里的 GetOwnedCharaRows()/GetTeamMembers()
    //   都依赖 Data_chara_imfor 已加载；懒加载未触发时返回空 → slot 全空、快编网格一格都没有。
    EnsureCharaInfoTable();

    CharaTeamWidget->AddToViewport(101);
    bCharaTeamOpen = true;

    // 暂停游戏 + 唤起鼠标（与角色面板一致的 UIOnly 模式）
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        UGameplayStatics::SetGamePaused(GetWorld(), true);
        PC->bShowMouseCursor = true;
        FInputModeUIOnly InputMode;
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PC->SetInputMode(InputMode);
    }

    UE_LOG(LogTemp, Warning, TEXT("[Team] 角色编队已打开（队伍 %d，游戏暂停，鼠标已唤起）。"), CurrentTeamIndex + 1);
}

void ABattleCharacter::CloseCharaTeamUI()
{
    if (!bCharaTeamOpen)
        return;

    if (CharaTeamWidget)
    {
        CharaTeamWidget->RemoveFromParent();
        CharaTeamWidget = nullptr;
    }
    bCharaTeamOpen = false;

    // 恢复游戏 + 收回鼠标（与关闭角色面板一致）
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        UGameplayStatics::SetGamePaused(GetWorld(), false);
        PC->bShowMouseCursor = false;
        FInputModeGameOnly InputMode;
        PC->SetInputMode(InputMode);
    }

    UE_LOG(LogTemp, Log, TEXT("[Team] 角色编队已关闭。"));
}


// ---- 角色面板 Tab 切换（mod_butt 四个按钮）----

// 绑定 mod_butt 内四个页签按钮的 OnClicked 到 C++ 处理函数。
// 面板每次打开都重建 Widget（CloseCharacterPanel 里 CharacterPanelWidget = nullptr），
// 因此绑定必须在 OpenCharacterPanel 的 AddToViewport 之后、且每次打开时都重新执行。
// OnClicked 是无参数委托，无法直接传 TabIndex，因此每个按钮绑定到各自的专用 UFUNCTION 回调。
//
// 【重要】AddDynamic 是宏，参数中的函数名必须直接以字面量传入（不是函数指针变量），
// 宏内部用 #Fn 字符串化函数名去反射系统查找 UFUNCTION。用 lambda 包装函数指针再传入
// 会导致宏展开后 #Fn 是形参名 "Fn"，反射找不到对应 UFUNCTION，断言崩溃：
//   "Fn does not look like a member function" [Delegate.h:474]
// 因此这里必须用 4 个独立的 if 分支，每行直接写成员函数名。
void ABattleCharacter::BindCharacterPanelTabs()
{
    if (!CharacterPanelWidget)
        return;

    UUserWidget* Widget = CharacterPanelWidget;

    // change_chara —— 角色面板展示
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("change_chara"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnTabCharaClicked);
    }

    // change_arm —— 角色武器展示
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("change_arm"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnTabArmClicked);
    }

    // change_yiqi —— 角色装备展示
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("change_yiqi"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnTabYiqiClicked);
    }

    // change_mingzuo —— 命座展示
    if (UButton* Btn = Cast<UButton>(Widget->GetWidgetFromName(TEXT("change_mingzuo"))))
    {
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnTabMingzuoClicked);
    }

    UE_LOG(LogTemp, Log, TEXT("Character panel tab buttons bound."));
}

// 四个页签按钮回调：转发到 SwitchCharacterPanelTab。
void ABattleCharacter::OnTabCharaClicked()   { SwitchCharacterPanelTab(static_cast<int32>(ECharacterPanelTab::Chara)); }
void ABattleCharacter::OnTabArmClicked()     { SwitchCharacterPanelTab(static_cast<int32>(ECharacterPanelTab::Arm)); }
void ABattleCharacter::OnTabYiqiClicked()    { SwitchCharacterPanelTab(static_cast<int32>(ECharacterPanelTab::Yiqi)); }
void ABattleCharacter::OnTabMingzuoClicked() { SwitchCharacterPanelTab(static_cast<int32>(ECharacterPanelTab::Mingzuo)); }

// 切换到指定页签：设置对应组件可见、隐藏其余组件。
// TabIndex 与 ECharacterPanelTab 枚举一致：0=Chara, 1=Arm, 2=Yiqi, 3=Mingzuo。
// 注意：something_show 展示图由 ApplyCharacterPanelFrom 统一控制（显示角色头像），
// 本函数不再设置，避免覆盖头像。
void ABattleCharacter::SwitchCharacterPanelTab(int32 TabIndex)
{
    if (!CharacterPanelWidget)
        return;

    CurrentPanelTab = static_cast<ECharacterPanelTab>(FMath::Clamp(TabIndex, 0, 3));

    UUserWidget* Widget = CharacterPanelWidget;

    // 1) 可见性切换：只显示当前页签对应组件，隐藏其余。
    //    当前仅「角色」页签（chara_imfor / chara_pitc_head）已建好组件，
    //    其余三页签（arm/yiqi/mingzuo）组件尚未添加，GetWidgetFromName 返回 null 时静默跳过。
    auto SetVisible = [&](const TCHAR* Name, bool bVisible)
    {
        if (UWidget* W = Widget->GetWidgetFromName(Name))
        {
            W->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
        }
    };

    // 角色页签组件：chara_imfor 随「角色」页签切换；chara_pitc_head 始终可见（不随 Tab 切换）。
    SetVisible(TEXT("chara_imfor"), CurrentPanelTab == ECharacterPanelTab::Chara);
    SetVisible(TEXT("chara_pitc_head"), true); // 固定可见：仅受 change_chara 控制的 chara_imfor 切换，不影响它

    // 武器页签组件（arm_imfor）：本次接上 —— 之前这里是预留注释。
    // 说明：imf_ui/arm_show_ui 那些是 Texture2D（展示图），由设计器里的 Image 引用；
    // 这里只负责「这一页要不要显示」，以及显示时把当前武器的信息填进去。
    SetVisible(TEXT("arm_imfor"), CurrentPanelTab == ECharacterPanelTab::Arm);

    // 装备/命座页签组件（组件名待后续添加后在这里补充）
    // 预留：SetVisible(TEXT("yiqi_imfor"), CurrentPanelTab == ECharacterPanelTab::Yiqi); 等

    // 切到武器页时刷新武器信息（当前装备的武器名/品质/类型/攻击力）
    if (CurrentPanelTab == ECharacterPanelTab::Arm)
    {
        RefreshArmPanel();
    }
    // 切回角色页时重算属性 —— 换过武器的话 attack_num 已经变了
    // （总攻击 = 基础 + 武器 + 装备），不重算就是「武器页换了刀，角色页攻击力还是旧的」。
    else if (CurrentPanelTab == ECharacterPanelTab::Chara)
    {
        RefreshCharacterPanelStats();
    }

    UE_LOG(LogTemp, Log, TEXT("Character panel tab switched to %d."), TabIndex);
}

// ==================================================================
// ---- 武器页（arm_imfor）+ 武器背包：切换角色武器类 ----
// ==================================================================
// 这一块对应需求里的三句话，三条主线各就各位：
//   「arm_imfor 进行角色武器类切换」 → RefreshArmPanel / EquipWeaponFromItemRow
//   「通过按钮打开武器背包」         → BindArmPanelButtons / OnOpenWeaponBagClicked / OpenWeaponBag
//   「同时和背包同步」               → EquippedWeaponRow（唯一依据）
//                                      + ApplyBagSlotVisual 打「装备中」标记
//                                      + ShowBagItemDetail 写状态
//                                      + RefreshBagInternal 自动卸下失效武器
//                                      + CloseBag 后自动回武器页
//
// 关键事实（读资产/代码得到，不是猜的）：
//   · E_kind_tool 显示名（按值序）：0=武器 1=声骸 2=消耗品 3=材料 4=箱匣 5=特殊 → 武器类 = 0
//   · imf_ui/arm_show_ui、inpress_ui/arm_show_inpress 都是 Texture2D（展示图），不是控件
//   · 已有武器蓝图：/Game/characters/arms/sword、/Game/characters/arms/test_knife（都继承 AWeaponBase）

bool ABattleCharacter::IsWeaponItem(const FBagItemEntry& Item) const
{
    // ① 数据判据：这一格的种类是武器。
    if (!Item.bValid || Item.KindTool != WeaponKindTool)
    {
        return false;
    }

    // ② 来源判据：这一行必须属于【背包物品表 Data_tool】。
    //
    // ★ 这一条替代了原来的「必须能解析出武器蓝图类」。为什么换掉 —— 那道闸门会 fail-closed，
    //   而它 fail 的后果是把整个武器背包清空。运行时日志（2026-09-13）把整条链钉死了：
    //
    //     自动扫描 0 个候选 → 索引空 → 每一行都解析不出武器蓝图
    //       → 本函数对【所有】物品返回 false
    //       → RefreshBagInternal 的过滤把 2 件武器全剔掉 → 「武器背包只有空格子」
    //       → 而同一个背包切到「全部」就正常（那条路不过滤）
    //       → 「装备」按钮点下去也无效（列表是空的，没有选中项）
    //       → 「已装备」标记也不出现（IsWeaponRowEquipped 当时同样卡在解析上）
    //
    //   教训：**展示 / 列表路径不许 fail-closed**。
    //   「扫描断了、声明没填」= 我们【不知道】它是不是武器，不等于【它必定不是武器】。
    //   把「不知道」当成「不是」，就把一个配置问题放大成了整个功能不可用。
    //
    //   「它是不是一把【能装备的】武器」这个问题仍然要问 —— 但要在装备路径上问
    //   （CanEquipWeaponItem / EquipWeaponFromItemRow），那才问对了地方：
    //   那里能给出「该填哪个字段」的修法，而列表这里只能把东西藏起来。
    if (InventoryComponent && !InventoryComponent->IsToolTableRow(Item.RowName))
    {
        return false;
    }

    return true;
}

bool ABattleCharacter::CanEquipWeaponItem(const FBagItemEntry& Item) const
{
    // 先过「是不是背包里的一件武器」这一关（数据判据 + 来源判据）。
    if (!IsWeaponItem(Item))
    {
        return false;
    }

    // 再单独问「能不能装备」：必须有对应的武器蓝图类，否则没有可 Spawn 的东西。
    // 这一步放在装备路径上（而不是列表过滤里）是刻意的 ——
    // 它失败时调用方要能给出「打开哪个蓝图、填哪个字段」的修法，而不是把这一格藏掉。
    if (!bRequireWeaponClassForWeaponItem)
    {
        // ★ 即便不要求武器蓝图类，职位约束仍然必须生效 —— 「法师不能装剑」是职位规则，
        //   与「有没有武器蓝图」是两回事。这个开关只放行「没蓝图」，不放行「类别不符」。
        const TSubclassOf<AWeaponBase> WClass = ResolveWeaponClassForItemRow(Item.RowName);
        return WClass ? CanEquipWeaponClassForJob(WClass) : true;
    }

    const TSubclassOf<AWeaponBase> WClass = ResolveWeaponClassForItemRow(Item.RowName);
    if (!WClass)
    {
        return false;
    }

    // ★ 职位约束（「法师不能装非法器」的最终闸门）：能解析出武器类，还要类别与职位匹配。
    //   为什么放这里而不是只靠 EquipWeapon 里的拦截：装备路径有好几条（点格子 / 详情按钮 /
    //   btn_equip / 蓝图直接调 EquipWeapon），都汇到 EquipSelectedBagWeapon / EquipWeaponFromItemRow，
    //   而这两条在真正 Spawn 前都会问 CanEquipWeaponItem —— 在这里拦住，
    //   就能在「还没卸下旧武器」时给出明确的职位不匹配提示，且列表过滤与装备拦截口径一致。
    return CanEquipWeaponClassForJob(WClass);
}

bool ABattleCharacter::IsWeaponRowEquipped(FName RowName, int32 InstanceNo) const
{
    // ① 行号为空 → 绝不可能是「装备中的武器」。
    //    这一条以前在部分调用点漏掉了，而 `NAME_None == NAME_None` 是 true ——
    //    于是「行号没填的物品」会被显示成已装备。
    if (RowName.IsNone())
    {
        return false;
    }

    // ①' 行号必须指向【背包物品表（Data_tool）】里的一格。
    //     两套行名互不相干：Data_tool 用 `1`~`5`，掉落表用 `100`/`101`。
    //     拿着掉落表的行名去背包里找「装备中的那一格」，找到的会是那件掉落物 ——
    //     本轮实测就是这个：test_knife 的 Bag Item Row 填了 100（掉落表的行），
    //     于是掉落物那一格被标成「装备中」。
    if (InventoryComponent && !InventoryComponent->IsToolTableRow(RowName))
    {
        return false;
    }

    // ①'' 实例号校验（唯一装备）：传了具体实例号时，必须是「那一把」才算装备中。
    //     同名（同 RowName）的不可堆叠武器会占多格，光按行名判断会把所有同名格子都标成
    //     「装备中」。传 -1（默认）表示不区分实例（兼容旧调用，如「是否已装备这一行」）。
    if (InstanceNo >= 0 && EquippedWeaponInstanceNo != InstanceNo)
    {
        return false;
    }

    // ② 能解析出武器类 → 以【实际装备的武器 Actor】为准 —— 这才是「装备中」三个字真正的含义。
    //    行号相等只是必要条件：EquippedWeaponRow 会被武器蓝图声明回填、也会被别的
    //    装备入口改写，行号与手上那把刀脱钩时，背包说「装备中」而角色拿着别的武器。
    const TSubclassOf<AWeaponBase> RowClass = ResolveWeaponClassForItemRow(RowName);
    if (CurrentWeapon && RowClass)
    {
        return CurrentWeapon->GetClass() == RowClass.Get();
    }

    // ③ 解析链断掉（或还没有武器 Actor）→ 回退到「记录行 + 展示行一致」。
    //
    //    ★ 为什么不能在这里直接 `return false`（原来就是那么写的）：
    //      那会让「已装备」标记完全依赖解析链。实测（自动扫描返回 0 个候选时）：
    //      角色明明装备着 sword、武器页也显示得好好的，背包那一格却不显示「装备中」——
    //      用户看到的现象是「装备状态根本没同步」，而真相是扫描断了，两件事被耦合在一起。
    //      配置没配好是配置的问题，不该顺手把「装备状态」这个事实也一起弄丢。
    //
    //    为什么仍然安全（不会把掉落物标成「装备中」）：
    //      · ①' 已经要求行名属于 Data_tool，而掉落物的行（100/101）不属于它；
    //      · 这里还要求「记录的行」与「展示行」一致 —— 展示行来自手上那把武器的
    //        自声明（不经过索引），两者都指向这一行才采信。
    if (EquippedWeaponRow != RowName)
    {
        return false;
    }
    const FName DisplayRow = GetEquippedWeaponDisplayRow();
    return DisplayRow.IsNone() || DisplayRow == RowName;
}

FName ABattleCharacter::GetEquippedWeaponDisplayRow() const
{
    // 手上那把武器的类自己声明的行 —— 前提是这一行确实属于【背包物品表】。
    // ★ 为什么必须核这一条（本轮 bug 的直接原因）：
    //   武器蓝图里的 Bag Item Row 是手填的 FName，而两套行名都是数字
    //   （Data_tool：1~5；掉落表：100/101）—— 填串了不会有任何编译/加载报错。
    //   实测 test_knife 就填成了 `100`（掉落表的行）：
    //   于是「当前装备的武器 → 背包行」这一步落到掉落物头上，
    //   背包把掉落物标成「装备中」，武器页显示它的名字、图标、品质。
    if (CurrentWeapon)
    {
        const FName DeclaredRaw = GetBagItemRowForWeaponClass(CurrentWeapon->GetClass());
        const FName Declared = ResolveBagKeyToInventoryRow(DeclaredRaw);
        if (!Declared.IsNone())
        {
            if (!InventoryComponent || InventoryComponent->IsToolTableRow(Declared))
            {
                return Declared;
            }

            // 声明指向了别的表 → 报清楚，并且【不采信】它。
            // 不报的话，用户只会看到武器页显示了一个武器类名（而不是物品名），
            // 完全联想不到「是武器蓝图里那一个字段填错了表」。
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] ⚠ 武器蓝图 %s 声明的背包行『%s』不属于背包物品表（Data_tool）→ 已忽略该声明。\n"
                     "      两套行名是各自独立的：Data_tool 用 1、2、3…，掉落表 Data_diaoluo_boss 用 100、101…。\n"
                     "      后果（本轮修掉的现象）：这一行会落到掉落物头上 —— 背包把掉落物标成「装备中」，\n"
                     "      武器页显示掉落物的名字与图标，看着就像「武器信息与 Weapon Class 不符」。\n"
                     "      修法：打开 %s → Class Defaults → Weapon|Bag → Bag Item Row，\n"
                     "            改成 Data_tool 里【武器行】的行名（本工程的武器行是『1』）。\n"
                     "      若那件掉落物确实要做成武器：另建一个武器蓝图并声明它自己的行名，\n"
                     "            不要借用已有武器的声明。"),
                *CurrentWeapon->GetClass()->GetName(), *Declared.ToString(),
                *CurrentWeapon->GetClass()->GetName());
        }

        // ② 类没声明行（或声明无效）→ 记录只有「它解析出的类正是手上这把」时才可信。
        //    （否则会出现：手上拿着 sword，背包却高亮掉落物那一格。）
        //
        //    ★ 「解析不出类」不能当成「不一致」：解析链断掉时（扫描返回 0 个候选）
        //      所有行都解析不出类，这时一旦判成不一致，武器页就会显示成
        //      「类名（不在背包中）」—— 手里那把明明就在背包里。无法验证 ≠ 验证失败。
        if (!EquippedWeaponRow.IsNone()
            && (!InventoryComponent || InventoryComponent->IsToolTableRow(EquippedWeaponRow)))
        {
            const TSubclassOf<AWeaponBase> RecordedClass = ResolveWeaponClassForItemRow(EquippedWeaponRow);
            if (!RecordedClass || RecordedClass.Get() == CurrentWeapon->GetClass())
            {
                return EquippedWeaponRow;
            }
        }

        // 手上确实有武器，但它在背包里没有对应的一格（开局自带、或声明填错了）
        // → 返回空，调用方就该显示成「没有对应格子」，而不是硬指到别的格子上去。
        return NAME_None;
    }

    // ③ 没有武器 Actor → 保持旧行为（开局前/卸下后）
    return EquippedWeaponRow;
}

void ABattleCharacter::LogWeaponItemFilterReport() const
{
    // 目的：让「这件物品明明是武器，为什么不在列表里 / 为什么装不上」变成一条能自己看懂的日志。
    // 触发时机是打开武器背包之后。
    //
    // ★ 本函数修这一轮时变了形：原来只维护一份「解析不出武器蓝图」的名单，
    //   而且把它当成「已从武器背包排除」的依据 —— 那个排除逻辑正是把整页清空的元凶。
    //   现在列表只按【数据】过滤（IsWeaponItem），于是「显示在列表里」与「能装备」
    //   变成两件事，名单也必须分成两份报，否则用户会遇到「东西在背包里、点装备却没反应」。
    if (!InventoryComponent)
    {
        return;
    }

    TArray<FString> Equippable;      // 有武器蓝图 → 能装备
    TArray<FString> NoBlueprint;     // 在列表里，但解析不出武器蓝图 → 装不上
    TArray<FString> NotInToolTable;  // 行名不属于 Data_tool → 会被列表过滤掉（掉落物走这支）

    for (const FBagItemEntry& E : InventoryComponent->GetItemsForKind(WeaponKindTool))
    {
        const FString Label = FString::Printf(TEXT("%s（行=%s）"),
            E.ToolName.IsEmpty() ? TEXT("<无名>") : *E.ToolName, *E.RowName.ToString());

        if (!InventoryComponent->IsToolTableRow(E.RowName))
        {
            NotInToolTable.Add(Label);
        }
        else if (ResolveWeaponClassForItemRow(E.RowName))
        {
            Equippable.Add(Label);
        }
        else
        {
            NoBlueprint.Add(Label);
        }
    }

    UE_LOG(LogTemp, Log,
        TEXT("[Arm] 武器判定自检：kind_tool=%d 的物品 %d 件 → 能装备 %d 件｜在列表里但装不上 %d 件｜不属背包表被剔除 %d 件。"),
        WeaponKindTool,
        Equippable.Num() + NoBlueprint.Num() + NotInToolTable.Num(),
        Equippable.Num(), NoBlueprint.Num(), NotInToolTable.Num());

    if (NoBlueprint.Num() > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ⚠ 这 %d 件东西【会显示在武器背包里，但点装备会被拒绝】—— 找不到对应的武器蓝图：\n"
                 "        %s\n"
                 "      为什么会这样：它们的 kind_tool 是武器分类(%d)，而注意「从没填过 kind_tool」也等于 %d，\n"
                 "      但没有任何武器蓝图声明过它们那一行。三种处理（按你的意图选一个）：\n"
                 "        ① 它确实【要当武器】→ 在 /Game/characters/arms/ 下给它做一个武器蓝图，\n"
                 "           并在该蓝图 Class Defaults → Weapon|Bag → Bag Item Row 填它的行名。\n"
                 "        ② 它本来就【不该是武器】→ 打开它所在的数据表，把那一行的 kind_tool 填成真正的分类\n"
                 "           （掉落物的表是 Data_diaoluo_boss）。\n"
                 "        ③ 或者用 Weapon|Switch → Weapon Class By Item Row 显式配一条 行名 → 武器类。"),
            NoBlueprint.Num(), *FString::Join(NoBlueprint, TEXT("\n        ")),
            WeaponKindTool, WeaponKindTool);
    }

    if (NotInToolTable.Num() > 0)
    {
        // ★ 措辞为什么这么写（2026-09-14 调整）：
        //   ① 【结论前置】原来开头是「修法：它们不该是武器 → 把那一行的 kind_tool 改成真正的分类」——
        //      那是在替用户做设计决定，而它未必对（本工程那件叫「教学棒球棍」，名字本身就是武器）。
        //      现在第一句就给结论「影响：无」—— 这类物品走的正是
        //      「行名不属于 Data_tool → 被列表剔除」这条正确路径，用户本来不需要做任何事。
        //      不写这句，用户会为一条正常状态白查一轮（他最可能做的动作就是去把 kind_tool 改掉）。
        //   ② 【口径要和姊妹告警一致】上面 NoBlueprint 那条给了三个选项（含「确实要当武器就建蓝图」），
        //      这一条原来只给一个定论 —— 两条姊妹告警互相矛盾，读者按哪条都可能走错。
        //      现在两条都给同一组选项，只是入口不同（一条是「在列表里但装不上」，一条是「已被剔除」）。
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ⚠ 这 %d 件东西的 kind_tool 是武器分类，但行名不属于背包物品表（Data_tool）\n"
                 "      → 已从武器背包列表里剔除（它们是掉落物表 Data_diaoluo_boss 的行）。\n"
                 "      ★ 影响：无 —— 它们不会出现在武器背包里，也不会被点装备。这是正确结果，不用处理。\n"
                 "        名单：%s\n"
                 "      只有当你【希望它出现在武器背包里】时才需要动它，两种做法（按你的意图选一个）：\n"
                 "        ① 它确实【要当武器】→ 在 /Game/characters/arms/ 下给它做一个武器蓝图，并在该蓝图\n"
                 "           Class Defaults → Weapon|Bag → Bag Item Row 填【它自己那一行的行名】。\n"
                 "        ② 它本来就【不该是武器】→ 打开 Data_diaoluo_boss，把那一行的 kind_tool 填成真正的\n"
                 "           分类（材料(3) / 消耗品(2) / 特殊(5)…；同表的「龙鳞」填的就是 材料(3)）。\n"
                 "      ⚠ 别只为了消掉这条提示去改 kind_tool —— 若它本来就该是武器，改了只会更偏。"),
            NotInToolTable.Num(), *FString::Join(NotInToolTable, TEXT("\n        ")));
    }
}

TSubclassOf<AWeaponBase> ABattleCharacter::ResolveWeaponClassForItemRow(FName RowName) const
{
    if (RowName.IsNone())
    {
        return nullptr;
    }

    // 1) 显式映射：BP_PlayerCharacter → Weapon|Switch → Weapon Class By Item Row
    if (const TSubclassOf<AWeaponBase>* Found = WeaponClassByItemRow.Find(RowName))
    {
        if (*Found)
        {
            return *Found;
        }
    }

    // 2) 武器蓝图【自声明】：蓝图里填了 Bag Item Row / Bag Item Tool Id → 直接命中。
    //    这是日常走的那条路：做新武器时顺手在武器蓝图里填一行，角色侧什么都不用配。
    EnsureWeaponRowIndex();
    if (const TSubclassOf<AWeaponBase>* ByDeclaration = WeaponRowToClassIndex.Find(RowName))
    {
        if (*ByDeclaration)
        {
            // 只提示一次：本函数会被「刷新详情面板」反复调到（判断这件武器能不能装备）。
            if (!bWeaponDeclaredHintLogged)
            {
                bWeaponDeclaredHintLogged = true;
                UE_LOG(LogTemp, Log,
                    TEXT("[Arm] 按武器蓝图自声明找到武器类：行『%s』 → %s\n"
                         "      （声明位置：该武器蓝图 → Class Defaults → Weapon|Bag → Bag Item Row）"),
                    *RowName.ToString(), *(*ByDeclaration)->GetName());
            }
            return *ByDeclaration;
        }
    }

    // 3) 约定兜底：/Game/characters/arms/<行名 或 tool_id>
    //    为什么留兜底：本工程的武器蓝图（sword / test_knife）本来就按武器名放在这个目录下，
    //    这样「TMap 还没配」时也能先跑通；失败只是返回 nullptr，由调用方给出明确修法。
    if (!InventoryComponent)
    {
        return nullptr;
    }

    FBagItemEntry Def;
    if (!InventoryComponent->GetItemDefinition(RowName, Def))
    {
        return nullptr;
    }

    // 2b) 武器蓝图可能是用 tool_id 声明的（而不是行名）→ 拿这件物品的 tool_id 再查一次索引。
    //     放在这里而不是上面，是因为要先用行名换出物品定义才知道 tool_id 是多少。
    if (!Def.ToolId.IsNone())
    {
        if (const TSubclassOf<AWeaponBase>* ByToolId = WeaponRowToClassIndex.Find(Def.ToolId))
        {
            if (*ByToolId)
            {
                return *ByToolId;
            }
        }
    }

    TArray<FString> Candidates;
    Candidates.Add(RowName.ToString());
    if (!Def.ToolId.IsNone())
    {
        Candidates.Add(Def.ToolId.ToString());
    }
    // ★ 第 3 个候选：物品名。本工程（以及大多数人的做法）武器蓝图的资产名 == 物品名，
    //   所以「行名是随手起的数字、声明又没填」时，这一条是唯一能自动接上的路。
    //   例：Data_tool 行『6』的物品名是 sword，武器蓝图就是 /Game/characters/arms/sword。
    if (!Def.ToolName.IsEmpty())
    {
        Candidates.Add(Def.ToolName);
    }

    TArray<FString> TriedPaths;
    for (const FString& Candidate : Candidates)
    {
        if (Candidate.IsEmpty())
        {
            continue;
        }
        const FString Path = FString::Printf(TEXT("/Game/characters/arms/%s.%s_C"), *Candidate, *Candidate);
        TriedPaths.Add(Path);
        if (UClass* Loaded = LoadClass<AWeaponBase>(nullptr, *Path))
        {
            // 只提示一次：本函数会被「刷新详情面板」反复调到（判断这件武器能不能装备），
            // 不 gate 的话每点一下格子就刷一条日志。
            if (!bWeaponPathHintLogged)
            {
                bWeaponPathHintLogged = true;
                UE_LOG(LogTemp, Log,
                    TEXT("[Arm] 按约定路径找到武器蓝图：%s\n"
                         "      建议去该武器蓝图里把 Bag Item Row 填上，或去 Weapon Class By Item Row 显式配一条 ——\n"
                         "      约定路径在资产改名后会失联，那时的现象就是「本来能装，改了个名字就装不上了」。"),
                    *Path);
            }
            return Loaded;
        }
    }

    // ★ 一次性「逐条路径结果」追踪。
    //   为什么值得单独留一条：本函数失败是【静默】返回 nullptr 的，而它过去会连锁成
    //   「整个武器背包被清空」这种大现象；只有把「试了哪几条、各自什么结果」摆出来，
    //   才能一眼分出是「索引是空的」还是「路径名不对」。
    //   只报一次 —— 本函数会被每格渲染反复调到，不 gate 的话开一次背包能刷几百行。
    if (!bWeaponResolveFailLogged)
    {
        bWeaponResolveFailLogged = true;
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ★ 解析不出「行『%s』（%s）」对应的武器蓝图类。已试过的路径与结果：\n"
                 "        ① 显式映射 Weapon Class By Item Row：未命中（当前配了 %d 条）\n"
                 "        ② 武器蓝图自声明 Bag Item Row / Bag Item Tool Id：未命中（索引共 %d 条）\n"
                 "        ③ 约定路径：%s → 都加载不到\n"
                 "      影响面：只是这一行【不能装备】；它仍然会正常显示在武器背包列表里。"),
            *RowName.ToString(),
            Def.ToolName.IsEmpty() ? TEXT("<无名>") : *Def.ToolName,
            WeaponClassByItemRow.Num(), WeaponRowToClassIndex.Num(),
            TriedPaths.Num() > 0 ? *FString::Join(TriedPaths, TEXT("、")) : TEXT("（没有可用候选名）"));
    }

    return nullptr;
}

// ==================================================================
// ---- 武器蓝图 ↔ 背包数据：索引 / 反查 / 同步 ----
// ==================================================================

// 从指定目录里收集所有「继承自 AWeaponBase 的蓝图类」。
// 用资产注册表查（不是逐个已知路径 LoadObject）→ 你新加一把武器、丢进 arms 目录，
// 不用改任何配置就能被发现。
namespace WeaponBlueprintScanner
{
    // 返回【真正加进 Out 的武器蓝图类个数】（不是注册表返回的资产数）。
    // 为什么要返回值而不是让调用方做减法：去重之后「总数 - 手工数」会失真，
    // 而这条数字正是「自动发现到底有没有在工作」的判据。
    static int32 Collect(const TArray<FString>& SearchPaths, TArray<TSubclassOf<AWeaponBase>>& Out)
    {
        int32 AddedCount = 0;
        // 「扫到了几个资产」与「几个通过父类判定」要分开数 —— 见函数尾部日志的说明。
        int32 PassedParentCheck = 0;
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        // 资产注册表可能还没扫完盘（刚打开工程的前几秒）→ 主动等一次。
        // 不等的话现象是「第一次打开武器背包是空的，第二次进来就好了」——
        // 而玩家只会记得「有时候是空的」，最难查的一类 bug。
        if (AssetRegistry.IsLoadingAssets())
        {
            AssetRegistry.WaitForCompletion();
        }

        TArray<FString> Paths = SearchPaths;
        if (Paths.Num() == 0)
        {
            // 默认目录：本工程武器蓝图（sword / test_knife）所在处
            Paths.Add(TEXT("/Game/characters/arms"));
        }

        FARFilter Filter;
        Filter.bRecursivePaths = true;
        // ★★ 不再用 ClassPaths 按父类筛 —— 这条判据【实测会返回 0 个】。
        //    为什么：资产注册表给【蓝图资产】标的类是 /Script/Engine.Blueprint，
        //    不是它生成的那个类；父类 AWeaponBase 即便有 bRecursiveClasses 也走不到，
        //    于是「目录里明明有两把武器蓝图」却一个都筛不出来。
        //    这个失败的破坏力被放大过：它让「行名 → 武器蓝图类」索引变成空的，
        //    而当时的 IsWeaponItem 又硬性要求「能解析出武器蓝图」→
        //    整页武器背包被清空（运行时日志：kind_tool=0 的 2 件里，有 2 件找不到武器蓝图）。
        //    现在的做法：只按【目录】筛，父类判定交给下面逐个 LoadObject 后的 IsChildOf ——
        //    那个判据是权威的，不依赖注册表的标签语义（扫到什么、判定是什么，都打进日志）。
        for (const FString& Path : Paths)
        {
            if (!Path.IsEmpty())
            {
                Filter.PackagePaths.Add(FName(*Path));
            }
        }

        TArray<FAssetData> FoundAssets;
        AssetRegistry.GetAssets(Filter, FoundAssets);

        // 一个资产都没扫到 → 目录写错，或者资产注册表还没刷。
        // 注意：这句话说的是「目录里没有资产」，不是「目录里的资产都不是武器蓝图」——
        // 后者由函数尾部那条日志的「扫到 N 个 / 通过 0 个」区分，两种故障的修法不同。
        if (FoundAssets.Num() == 0)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] ⚠ 自动扫描在下面的目录里一个【资产】都没找到：%s\n"
                     "      修法（三选一）：\n"
                     "        ① 确认武器蓝图真的在这些目录下（Package 路径以 /Game/ 开头，不含 Content/）\n"
                     "        ② 改 Weapon|Bag → Weapon Blueprint Search Paths 填正确目录\n"
                     "        ③ 或直接在 Weapon|Bag → Weapon Class Candidates 里手工登记武器蓝图类\n"
                     "      另外：编辑器刚打开时资产注册表可能还没扫完，本函数已等待刷新，一般不用管。"),
                *FString::Join(Paths, TEXT(" / ")));
        }

        for (const FAssetData& Asset : FoundAssets)
        {
            UClass* LoadedClass = nullptr;

            // 蓝图资产上标着它生成的类（GeneratedClass 标签）→ 直接读，最省事：
            // 拿到的是 /Game/characters/arms/sword.sword_C 这种类路径。
            FString GeneratedClassPath;
            if (Asset.GetTagValue(TEXT("GeneratedClass"), GeneratedClassPath))
            {
                LoadedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath);
            }

            // 标签读不到（老资产 / 不是蓝图）→ 回落到把资产本体当蓝图读
            if (!LoadedClass)
            {
                if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset()))
                {
                    LoadedClass = Blueprint->GeneratedClass;
                }
            }

            // ★ 这里才是「是不是武器蓝图」的权威判据：拿加载出来的类问引擎。
            //   上面的注册表筛选只是缩小范围用的，绝不能拿它当结论 ——
            //   它一旦判错，错的是「一个候选都没有」，而后果是整个武器背包被清空。
            if (LoadedClass && LoadedClass->IsChildOf(AWeaponBase::StaticClass()))
            {
                ++PassedParentCheck;
                const int32 Before = Out.Num();
                Out.AddUnique(TSubclassOf<AWeaponBase>(LoadedClass));
                if (Out.Num() > Before)
                {
                    ++AddedCount;
                }
            }
        }

        // 「扫到几个资产 / 几个通过父类判定」必须都报 —— 这两个数分开才能区分：
        //   · 资产 0 个 → 目录写错 / 注册表没扫
        //   · 资产 N 个但通过 0 个 → 目录里有东西，但都不是继承 AWeaponBase 的蓝图
        // 合成一个数的话，这两种完全不同的故障长得一模一样。
        UE_LOG(LogTemp, Log,
            TEXT("[Arm] 自动扫描：目录 %s → 扫到 %d 个资产，其中继承 AWeaponBase 的有 %d 个（新加入候选 %d 个）。"),
            *FString::Join(Paths, TEXT(" / ")), FoundAssets.Num(), PassedParentCheck, AddedCount);

        return AddedCount;
    }
}

FName ABattleCharacter::GetBagItemRowForWeaponClass(TSubclassOf<AWeaponBase> WeaponClass) const
{
    if (!WeaponClass)
    {
        return NAME_None;
    }

    // 读【蓝图默认值】而不是场景里那个武器 Actor：声明是写在蓝图上的一份配置，
    // 任何实例读到的都是同一个答案。
    const AWeaponBase* Def = WeaponClass->GetDefaultObject<AWeaponBase>();
    if (!Def)
    {
        return NAME_None;
    }

    // 行名优先，没填行名才用 tool_id —— 与建索引时的优先级保持一致
    return Def->GetBagItemRow().IsNone() ? Def->GetBagItemToolId() : Def->GetBagItemRow();
}

FName ABattleCharacter::ResolveBagKeyToInventoryRow(FName Key) const
{
    if (Key.IsNone() || !InventoryComponent)
    {
        return Key;
    }

    // 1) Key 本身就是「拥有的行名」→ 直接用。绝大多数情况走这条，零额外开销。
    //    用 GetItemByRow（会检查是否拥有）而不是 GetItemDefinition：
    //    我们要的是「背包里真有这一格」，只在表里存在但没拥有的行没有格子可高亮。
    FBagItemEntry Probe;
    if (InventoryComponent->GetItemByRow(Key, Probe))
    {
        return Key;
    }

    // 2) 当成 tool_id 找对应行。为什么必须有这一步：Bag Item Tool Id 这条声明路是允许的
    //    （行名可能是随手起的数字），但「装备中」标记是按行名查表格的。
    for (const FBagItemEntry& Owned : InventoryComponent->GetItemsForKind(-1))
    {
        if (!Owned.RowName.IsNone() && Owned.ToolId == Key)
        {
            return Owned.RowName;
        }
    }

    // 3) 都找不到 → 原样返回。此时 EquippedWeaponRow 会是一个「不存在的行名」，
    //    而「那一行不在背包里」的日志本来就带修法提示，不会静默。
    return Key;
}

void ABattleCharacter::EnsureWeaponRowIndex() const
{
    if (!bWeaponRowIndexBuilt)
    {
        // 索引是「首次解析时懒建一次」的缓存（mutable），不是角色状态 → 解析函数可以是 const，
        // 蓝图的纯节点调用不会因此变成有副作用的节点。
        const_cast<ABattleCharacter*>(this)->RebuildWeaponRowIndex();
    }
}

void ABattleCharacter::RebuildWeaponRowIndex()
{
    // 重入保护：扫描过程会加载资产，加载中可能触发别的刷新又绕回来 →
    // 没有它的话索引会被清空两次，第二次建到一半的状态可能被别人看到。
    if (bWeaponRowIndexBuilding)
    {
        return;
    }
    TGuardValue<bool> BuildingGuard(bWeaponRowIndexBuilding, true);

    WeaponRowToClassIndex.Reset();
    bWeaponRowIndexBuilt = true;

    // ---- 候选集 = 手工登记的 + 自动扫描的 + 兜底种子 ----
    TArray<TSubclassOf<AWeaponBase>> Candidates = WeaponClassCandidates;
    const int32 ManualCount = Candidates.Num();

    // ★ 兜底种子：角色蓝图里配的那把 + 手上正拿着的那把。
    //   为什么必须有：自动扫描和手工登记【同时】失灵时（本轮实测就是这个），
    //   候选集会整个是空的 → 连「当前装备的武器」都解析不出自己属于哪一行 ——
    //   而那是最荒谬的一种失败：武器页明明正显示着它。
    //   这两个是蓝图里的硬引用，不依赖资产注册表、也不依赖任何声明。
    if (DefaultWeaponClass)
    {
        Candidates.AddUnique(DefaultWeaponClass);
    }
    if (CurrentWeapon)
    {
        Candidates.AddUnique(CurrentWeapon->GetClass());
    }
    const int32 SeededCount = Candidates.Num() - ManualCount;

    // 自动扫描：把「扫到了几个」明确报出来。
    // 为什么值得单独一个变量：这是「自动发现到底有没有在工作」的唯一判据 ——
    // 用户填完 Bag Item Row 却装不上时，第一件事就是看这个数是不是 0。
    int32 ScannedCount = 0;
    if (bAutoScanWeaponBlueprints)
    {
        ScannedCount = WeaponBlueprintScanner::Collect(WeaponBlueprintSearchPaths, Candidates);
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Arm] 自动扫描已关闭（bAutoScanWeaponBlueprints=false）→ 只用 Weapon Class Candidates 里的 %d 个。"),
            ManualCount);
    }

    // ---- 读每个蓝图的声明，建索引 ----
    TArray<FString> NoDeclaration;  // 没声明归属的 → 它的背包格点了会装不上，要列出来
    TArray<FString> Conflicts;      // 同一行被多个蓝图声明 → 先到先得，但要提醒

    for (const TSubclassOf<AWeaponBase>& Candidate : Candidates)
    {
        if (!Candidate)
        {
            continue;
        }

        const AWeaponBase* Def = Candidate->GetDefaultObject<AWeaponBase>();
        if (!Def)
        {
            continue;
        }

        // 两个字段都建索引：这样用行名查得到、用 tool_id 也查得到
        bool bDeclared = false;
        const FName Keys[2] = { Def->GetBagItemRow(), Def->GetBagItemToolId() };
        for (const FName& Key : Keys)
        {
            if (Key.IsNone())
            {
                continue;
            }
            bDeclared = true;

            if (const TSubclassOf<AWeaponBase>* Existing = WeaponRowToClassIndex.Find(Key))
            {
                Conflicts.Add(FString::Printf(TEXT("『%s』被 %s 和 %s 同时声明"),
                    *Key.ToString(),
                    *(*Existing)->GetName(),
                    *Candidate->GetName()));
                continue;
            }
            WeaponRowToClassIndex.Add(Key, Candidate);
        }

        if (!bDeclared)
        {
            NoDeclaration.Add(Candidate->GetName());
        }
    }

    // ---- 报告 ----
    const FString PathText = WeaponBlueprintSearchPaths.Num() > 0
        ? FString::Join(WeaponBlueprintSearchPaths, TEXT(" / "))
        : FString(TEXT("/Game/characters/arms（默认）"));

    UE_LOG(LogTemp, Log,
        TEXT("[Arm] 武器蓝图索引已重建：候选 %d 个（手工登记 %d + 自动扫描 %d + 兜底种子 %d）→ 成功绑定 %d 条。\n"
             "      扫描目录：%s"),
        Candidates.Num(), ManualCount, ScannedCount, SeededCount,
        WeaponRowToClassIndex.Num(), *PathText);

    // 候选集整个是空的 → 自动发现这条线断了。必须一次把三条修法给全，
    // 否则用户的现象只是「武器蓝图里填了 Bag Item Row，还是装不上」，无从下手。
    if (Candidates.Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ★ 一个候选武器蓝图都没有 → 自动发现这条线断了，武器背包里点哪一格都装不上。\n"
                 "      修法（三选一）：\n"
                 "        ① 确认武器蓝图在 %s 下（注意是 Package 路径，不含 Content/）\n"
                 "        ② 在 BP_PlayerCharacter → Weapon|Bag → Weapon Class Candidates 里手工登记\n"
                 "           （手工登记会建立硬引用 → 打包后也能用）\n"
                 "        ③ 或者干脆把武器蓝图命名成「数据表行名」，走 /Game/characters/arms/<行名> 兜底"),
            *PathText);
    }

    for (const TPair<FName, TSubclassOf<AWeaponBase>>& Pair : WeaponRowToClassIndex)
    {
        UE_LOG(LogTemp, Log, TEXT("[Arm]   行名/ID『%s』 → %s"),
            *Pair.Key.ToString(),
            Pair.Value ? *Pair.Value->GetName() : TEXT("?"));
    }

    if (NoDeclaration.Num() > 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Arm]   这些武器蓝图没声明「我属于背包哪一行」→ 它们对应的背包格点了会装不上：%s\n"
                 "      修法：打开该武器蓝图 → Class Defaults → Weapon|Bag → Bag Item Row，"
                 "填 Data_tool 里的行名（表左侧那一列）。"),
            *FString::Join(NoDeclaration, TEXT(" / ")));
    }
    if (Conflicts.Num() > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ⚠ 有 %d 处行名被多个武器蓝图同时声明（实际用的是先到的那一个）：%s\n"
                 "      修法：把它们的 Bag Item Row 改成各自对应的行；"
                 "或想指定用哪个，就在角色的 Weapon Class By Item Row 里显式配一条（优先级更高）。"),
            Conflicts.Num(), *FString::Join(Conflicts, TEXT("；")));
    }
}

bool ABattleCharacter::SyncDefaultWeaponClass(TSubclassOf<AWeaponBase> WeaponClass)
{
    if (!bSyncDefaultWeaponClass)
    {
        return false;
    }

    // ★ 装备状态写【每角色类的运行时表】，不再写 CDO。
    //
    // 为什么不再写 CDO（这是「串号」bug 的直接根源）：
    //   以前这里把武器写进 this 所属类的 CDO。但 UE 蓝图 CDO 有继承语义 ——
    //   BP_Chara_magic 继承自 BP_PlayerCharacter 时，写父类 CDO 的 DefaultWeaponClass，
    //   会被子类 CDO 继承过去。结果「给 BP_PlayerCharacter 切武器，BP_Chara_magic 的
    //   角色面板也跟着变」（两个角色显示同一把武器）。
    //
    //   装备是【运行时状态】，本来就该和「蓝图默认配置」分开存：
    //   · DefaultWeaponClass          = 开局默认装备（编辑器里配，BeginPlay 用）
    //   · ClassEquippedWeapon[类]     = 运行时切换后的装备（本表）
    //   两者各归其位，运行时切武器只碰本表，不再污染任何 CDO。
    const TSubclassOf<AWeaponBase> Previous = DefaultWeaponClass;
    DefaultWeaponClass = WeaponClass;

    if (UClass* SelfClass = GetClass())
    {
        if (WeaponClass)
        {
            ClassEquippedWeapon.Add(SelfClass, WeaponClass);
        }
        else
        {
            ClassEquippedWeapon.Remove(SelfClass);
        }
    }

    UE_LOG(LogTemp, Log,
        TEXT("[Arm] 已同步角色 %s 的装备武器：%s → %s（按角色类隔离，不影响其他角色）。"),
        *GetClass()->GetName(),
        Previous ? *Previous->GetName() : TEXT("(空)"),
        WeaponClass ? *WeaponClass->GetName() : TEXT("(空)"));

    return true;
}

bool ABattleCharacter::EquipSelectedBagWeapon(const FString& TriggerName)
{
    if (!CurrentBagItems.IsValidIndex(SelectedBagIndex))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] 收到装备请求，但当前【没有选中任何格子】→ 什么也没发生（触发=%s）。\n"
                 "      修法：先在背包里点一下要装备的那件武器（选中后右侧详情会显示它）。"),
            *TriggerName);
        return false;
    }

    const FBagItemEntry& Item = CurrentBagItems[SelectedBagIndex];

    // 已经装着它 → 明确回一句。不回的话玩家会以为「按钮坏了 / 没生效」。
    // 传 InstanceNo：同名多把时，只有「同一把」才叫「已经装着」。
    if (IsWeaponRowEquipped(Item.RowName, Item.InstanceNo))
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Arm] 「%s」（行=%s，实例#%d）已经是当前装备的武器，无需重复装备（触发=%s）。"),
            *Item.ToolName, *Item.RowName.ToString(), Item.InstanceNo, *TriggerName);
        return false;
    }

    // ★ 装备前的校验分两层，而且【判定位置】本身是这一轮修出来的：
    //   · 能不能进武器背包列表 → IsWeaponItem（只看数据：kind_tool + 行名属于 Data_tool）
    //   · 能不能真正换上       → CanEquipWeaponItem（再要求有武器蓝图类）
    //   上一轮把第二层写进了第一层，于是「解析链断了」=「所有物品都不是武器」=
    //   整页武器被清空。现在它只影响「装不上」，且拒绝时会给出该改哪个字段的修法。
    if (!CanEquipWeaponItem(Item))
    {
        if (!IsWeaponItem(Item))
        {
            // 连「算作背包里的一件武器」都不成立 → 两种子原因，修法完全不同。
            // 混成一条「kind_tool 不对」的话，第二类用户会照着一个本来就对的字段反复改。
            if (Item.KindTool != WeaponKindTool)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Arm] 「%s」（行=%s，kind_tool=%d）不是可装备的武器 → 已忽略本次装备请求（触发=%s）。\n"
                         "      原因：它的 kind_tool 不是武器分类（武器分类 = %d）。\n"
                         "      修法：打开它所在的数据表，把那一行的 kind_tool 改成 %d（武器）；\n"
                         "            如果它本来就是武器，把 Weapon|Switch → Weapon Kind Tool 改成 %d。"),
                    *Item.ToolName, *Item.RowName.ToString(), Item.KindTool, *TriggerName,
                    WeaponKindTool, WeaponKindTool, Item.KindTool);
            }
            else
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Arm] 「%s」（行=%s）的 kind_tool 是武器分类(%d)，但它的行名【不属于背包物品表 Data_tool】\n"
                         "      → 不算背包里的武器，已忽略本次装备请求（触发=%s）。\n"
                         "      为什么会有这种情况：kind_tool 的默认值是 0（正好等于武器分类），\n"
                         "      所以「从没填过分类」的掉落物会自动满足「是武器」—— 打 boss 掉的那件就是典型。\n"
                         "      修法（二选一）：\n"
                         "        ① 它不该是武器 → 打开它所在的表（掉落物是 Data_diaoluo_boss），\n"
                         "           把那一行的 kind_tool 改成真正的分类。\n"
                         "        ② 它确实要做成武器 → 在 /Game/characters/arms/ 下建武器蓝图，\n"
                         "           并在其 Weapon|Bag → Bag Item Row 填『%s』，同时把它加进 Data_tool。"),
                    *Item.ToolName, *Item.RowName.ToString(), WeaponKindTool, *TriggerName,
                    *Item.RowName.ToString());
            }
        }
        else
        {
            // 在武器背包列表里能看见、也确实是一件武器，但装备被拒。可能两种原因，分别给修法：
            //   ① 职位不匹配（武器类别 ≠ 当前职位可用类别）
            //   ② 找不到对应的武器蓝图类（解析链断 / 没填声明）
            const TSubclassOf<AWeaponBase> ResolvedClass = ResolveWeaponClassForItemRow(Item.RowName);
            if (ResolvedClass && !CanEquipWeaponClassForJob(ResolvedClass))
            {
                // ★ 职位不匹配：这是「法师不能装剑」的直接闸门。
                const AWeaponBase* WCDO = ResolvedClass->GetDefaultObject<AWeaponBase>();
                UE_LOG(LogTemp, Warning,
                    TEXT("[Job] 「%s」（行=%s）不能装备：其武器类别(%d) 与当前职位(%d，可用类别 %d) 不匹配（触发=%s）。\n"
                         "      职位对应关系：剑士→轻剑、法师→法器、枪手→枪械。\n"
                         "      修法（二选一）：\n"
                         "        ① 改角色职位：BP_PlayerCharacter → Job → Job Class 改成与武器匹配的职位\n"
                         "        ② 改武器类别：打开 %s → Class Defaults → Weapon → Weapon Category 改成职位对应的类别"),
                    *Item.ToolName, *Item.RowName.ToString(),
                    WCDO ? static_cast<int32>(WCDO->GetWeaponCategory()) : -1,
                    static_cast<int32>(JobClass),
                    static_cast<int32>(FJobWeaponRules::GetCategoryForJob(JobClass)),
                    *TriggerName,
                    *ResolvedClass->GetName());
            }
            else
            {
                // 找不到对应的武器蓝图类。
                UE_LOG(LogTemp, Warning,
                    TEXT("[Arm] 「%s」（行=%s）是武器，但【找不到对应的武器蓝图类】→ 无法换上它（触发=%s）。\n"
                         "      注意：它仍然会显示在武器背包里 —— 列表只看 kind_tool 与数据表归属，\n"
                         "      不因为「配置没配好」就把东西藏起来（上一轮那个「武器背包只有空格子」的 bug 就是这么来的）。\n"
                         "      修法（三选一）：\n"
                         "        ① 让武器蓝图自己声明：打开武器蓝图 → Class Defaults → Weapon|Bag →\n"
                         "           Bag Item Row，填『%s』（本工程的两把武器都该填 Data_tool 里的武器行名）。\n"
                         "        ② 在角色上显式配：BP_PlayerCharacter → Weapon|Switch → Weapon Class By Item Row，\n"
                         "           加一条 Key=『%s』，Value=对应的武器蓝图类。\n"
                         "        ③ 或把武器蓝图放到 /Game/characters/arms/ 下，并让资产名 == 行名（或 tool_id）。\n"
                         "      ★ 若上面三条你都已经做对了还是这样 → 看日志里的这两行：\n"
                         "        「自动扫描：目录 … → 扫到 N 个资产，其中继承 AWeaponBase 的有 M 个」\n"
                         "        「武器蓝图索引已重建：候选 … → 成功绑定 K 条」，K=0 就是解析链断了。"),
                    *Item.ToolName, *Item.RowName.ToString(), *TriggerName,
                    *Item.RowName.ToString(), *Item.RowName.ToString());
            }
        }
        return false;
    }

    UE_LOG(LogTemp, Log,
        TEXT("[Arm] 装备请求：%s（行=%s，实例#%d，触发=%s）"),
        *Item.ToolName, *Item.RowName.ToString(), Item.InstanceNo, *TriggerName);

    return EquipWeaponFromItemRow(Item.RowName, Item.InstanceNo);
}

bool ABattleCharacter::EquipWeaponFromItemRow(FName RowName, int32 InstanceNo)
{
    if (RowName.IsNone())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Arm] 装备失败：行名为空。"));
        return false;
    }

    // 先确认它确实是「武器」类物品。
    // 为什么要校验：武器背包模式下玩家可以切到别的分类浏览，那些格子不该被装备。
    FBagItemEntry Item;
    if (InventoryComponent && InventoryComponent->GetItemByRow(RowName, Item) && !IsWeaponItem(Item))
    {
        // ★ 两种失败原因必须分开说 —— 修法完全不同。
        //   混成一条「kind_tool 不对」的话，第二类用户会照着一个正确的字段反复改。
        if (Item.KindTool != WeaponKindTool)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] 「%s」（行=%s）不是武器：它的 kind_tool=%d，而武器分类是 %d → 已忽略装备请求。\n"
                     "      修法：打开它所在的数据表，把那一行的 kind_tool 改成 %d（武器）；\n"
                     "      或者：如果它本来就是武器，把 Weapon Kind Tool 改成 %d。"),
                *Item.ToolName, *RowName.ToString(), Item.KindTool, WeaponKindTool,
                WeaponKindTool, Item.KindTool);
        }
        else
        {
            // kind_tool 是武器，但行名不属于背包物品表 → 它是【别的表】的行（掉落物）。
            // 说明（本轮）：这条分支以前写的是「找不到对应的武器蓝图」，那是旧判据的说法 ——
            // 「有没有武器蓝图」已经拆到 CanEquipWeaponItem 去了，这里只可能是归属问题。
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] 「%s」（行=%s）的 kind_tool 是武器分类(%d)，但它的行名【不属于背包物品表 Data_tool】\n"
                     "      → 不算背包里的武器，已忽略装备请求。\n"
                     "      为什么会有这种情况：kind_tool 的默认值是 0（正好等于武器分类），\n"
                     "      所以「从没填过分类」的掉落物会自动满足「是武器」—— 打 boss 掉的那件就是典型。\n"
                     "      修法（二选一）：\n"
                     "        ① 它不该是武器 → 打开它所在的表（掉落物是 Data_diaoluo_boss），\n"
                     "           把那一行的 kind_tool 改成真正的分类。\n"
                     "        ② 它确实要做成武器 → 在 /Game/characters/arms/ 下建武器蓝图，\n"
                     "           在其 Weapon|Bag → Bag Item Row 填『%s』，并把它加进 Data_tool。"),
                *Item.ToolName, *RowName.ToString(), WeaponKindTool, *RowName.ToString());
        }
        return false;
    }

    const TSubclassOf<AWeaponBase> WeaponClass = ResolveWeaponClassForItemRow(RowName);
    if (!WeaponClass)
    {
        // 不只说「失败」—— 把两种修法和当前可选项一次列清，省掉来回试
        TArray<FString> RowNames;
        if (InventoryComponent)
        {
            for (const FBagItemEntry& E : InventoryComponent->GetItemsForKind(WeaponKindTool))
            {
                if (!E.RowName.IsNone())
                {
                    RowNames.AddUnique(E.RowName.ToString());
                }
            }
        }

        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ★ 找不到「%s」对应的武器蓝图类，无法切换武器。\n"
                 "      当前「行名 → 武器蓝图类」索引里有 %d 条 —— 「0 条」说明解析链断了（不是你没配）。\n"
                 "      修法（三选一）：\n"
                 "        ① 让武器蓝图自己声明：打开该武器蓝图 → Class Defaults → Weapon|Bag → Bag Item Row，填『%s』\n"
                 "        ② 在角色上显式配：BP_PlayerCharacter → Class Defaults → Weapon|Switch → Weapon Class By Item Row，\n"
                 "           加一条 Key=『%s』，Value=武器蓝图类（如 sword / test_knife）\n"
                 "        ③ 或把武器蓝图放到 /Game/characters/arms/ 下，并让资产名 == 行名 / tool_id / 物品名\n"
                 "      当前背包里的武器行名：%s\n"
                 "      ★ 要是索引是 0 条 → 看日志里这两行（本函数上方的解析过程已经把它们打出来了）：\n"
                 "        「自动扫描：目录 … → 扫到 N 个资产，其中继承 AWeaponBase 的有 M 个」\n"
                 "        「武器蓝图索引已重建：候选 … → 成功绑定 K 条」，K=0 说明一条声明都没读到。"),
            *RowName.ToString(), WeaponRowToClassIndex.Num(),
            *RowName.ToString(), *RowName.ToString(),
            RowNames.Num() > 0 ? *FString::Join(RowNames, TEXT(" / ")) : TEXT("（一件都没有）"));
        return false;
    }

    // ---- ★ 唯一装备校验：这把武器实例（行名#实例号）不能被【其他角色类】占用 ----
    // 「同类不同把的武器只能被一个角色装备」：两把 sword（同 RowName，InstanceNo 0/1）是两把
    // 独立的武器，可以分别给两个角色；但同一把（如 1#0）不能同时挂在两个角色身上。
    {
        const FString InstanceKey = MakeWeaponInstanceKey(RowName, InstanceNo);
        if (UClass* const* ExistingOwner = WeaponInstanceOwner.Find(InstanceKey))
        {
            if (*ExistingOwner && *ExistingOwner != GetClass())
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Weapon] 无法装备「%s」（实例 %s）：这把武器已经被角色 %s 装备，同类不同把的武器同一把只能被一个角色使用。\n"
                         "      想给当前角色用：先到那个角色身上卸下这把武器，或换一把（同名武器的另一把）。"),
                    *RowName.ToString(), *InstanceKey,
                    *(*ExistingOwner)->GetName());
                return false;
            }
        }
    }

    // ★ 换装时先记住旧武器的实例键：EquipWeapon 内部会回填 EquippedWeaponRow（覆盖旧值），
    //   所以必须在调用前先把旧行名 + 旧实例号存下来，装备成功后再用它释放旧占用。
    const FName PreviousEquippedRow = EquippedWeaponRow;
    const int32 PreviousEquippedInstance = EquippedWeaponInstanceNo;

    // EquipWeapon 内部会先把旧武器 Unequip + Destroy，再 Spawn 新武器并 OnEquipped（吸附到手部插槽）
    if (!EquipWeapon(WeaponClass))
    {
        return false;
    }

    // ★ 换装成功 → 释放旧武器的实例占用（EquipWeapon 的 UnequipWeapon 不碰占用表）。
    if (!PreviousEquippedRow.IsNone())
    {
        WeaponInstanceOwner.Remove(MakeWeaponInstanceKey(PreviousEquippedRow, PreviousEquippedInstance));
    }

    // ★ 装备成功后登记占用：这把实例归当前角色类。
    WeaponInstanceOwner.Add(MakeWeaponInstanceKey(RowName, InstanceNo), GetClass());

    // ---- 核对「玩家点的这一行」与「武器蓝图自己声明的行」----
    // 不一致不影响本次操作（以你点的这一行为准），但说明蓝图的声明写歪了。
    // 现在提示出来，免得下次从别的入口装备时又装到别的行上、还得从头查。
    const FName DeclaredRow = GetBagItemRowForWeaponClass(WeaponClass);
    // ★ 比较的是【换算之后的真实行名】：声明可能是 tool_id，直接拿它和行名比必然不等 →
    //   会误报「声明写歪了」。误报的代价不只是多一条日志，而是用户会去改一份本来就对的配置。
    const FName DeclaredBagRow = ResolveBagKeyToInventoryRow(DeclaredRow);
    if (!DeclaredBagRow.IsNone() && DeclaredBagRow != RowName)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ⚠ 武器蓝图 %s 声明自己属于背包行『%s』%s，但你点的是『%s』。\n"
                 "      本次以你点的『%s』为准。想纠正：打开 %s → Class Defaults → Weapon|Bag → "
                 "Bag Item Row 改成『%s』。"),
            *WeaponClass->GetName(), *DeclaredBagRow.ToString(),
            DeclaredBagRow != DeclaredRow
                ? *FString::Printf(TEXT("（蓝图里写的是 tool_id『%s』）"), *DeclaredRow.ToString())
                : TEXT(""),
            *RowName.ToString(), *RowName.ToString(),
            *WeaponClass->GetName(), *RowName.ToString());
    }

    EquippedWeaponRow = RowName;
    EquippedWeaponInstanceNo = InstanceNo;
    // 记住「这次是从背包点出来的」→ 只有这类装备才参与「那一格没了就自动卸下」
    bEquippedWeaponRowFromBag = true;

    // ---- 同步角色蓝图的 Weapon Class ----
    // 需求原文：「装备后会同步更换角色蓝图中的 Weapon Class」。
    SyncDefaultWeaponClass(WeaponClass);

    // 日志读【实际装备上的那把武器】的数值，而不是数据表 ——
    // 类别/攻击力是 AWeaponBase 上的属性，读它才能证明「类真的换了」。
    UE_LOG(LogTemp, Log,
        TEXT("[Arm] 已切换武器：行=%s → 类=%s（武器类别=%d，攻击力=%.0f）"),
        *RowName.ToString(), *WeaponClass->GetName(),
        CurrentWeapon ? static_cast<int32>(CurrentWeapon->GetWeaponCategory()) : -1,
        CurrentWeapon ? CurrentWeapon->GetAttackPower() : 0.0f);

    RefreshArmPanel();

    // 面板上的 attack_num 显示的是总攻击（基础 + 武器 + 装备）—— 换武器后它必须跟着变，
    // 否则玩家切回角色页会看到旧攻击力，以为「换了刀没生效」。
    RefreshCharacterPanelStats();

    // 背包开着（武器模式）→ 立刻刷新，让「装备中」标记跟上（「与背包同步」的即时部分）
    if (bBagOpen)
    {
        RefreshBagInternal(false);
    }
    return true;
}

void ABattleCharacter::UnequipWeaponAndClearRow()
{
    const FName PreviousRow = EquippedWeaponRow;

    // ★ 释放武器实例占用：卸下后这把武器可以被别的角色装备了。
    if (!PreviousRow.IsNone())
    {
        WeaponInstanceOwner.Remove(MakeWeaponInstanceKey(PreviousRow, EquippedWeaponInstanceNo));
    }

    UnequipWeapon();
    EquippedWeaponRow = NAME_None;
    EquippedWeaponInstanceNo = 0;
    bEquippedWeaponRowFromBag = false;

    // 卸下 → 角色蓝图的 Weapon Class 也同步成空手。
    // 不这么做的话，下次开局自动装备又会装回那把刀，与「我现在空手」不一致。
    // 不想要这个行为：把 bSyncDefaultWeaponClass 设成 false。
    SyncDefaultWeaponClass(nullptr);

    UE_LOG(LogTemp, Log, TEXT("[Arm] 已卸下武器（原先装备的行=%s）。"), *PreviousRow.ToString());

    RefreshArmPanel();
    // 卸下会掉掉武器攻击力 → 面板 attack_num 同样要重算（理由见装备那边）
    RefreshCharacterPanelStats();

    if (bBagOpen)
    {
        RefreshBagInternal(false);
    }
}

void ABattleCharacter::OpenWeaponBag()
{
    // 已经开着背包时【不关掉再开】：那条路会多走一次「要不要回武器页」的收尾逻辑，
    // 直接在现有背包上锁到武器分类即可。
    if (!bBagOpen)
    {
        OpenBag();
    }

    if (!bBagOpen)
    {
        return; // OpenBag 已经写过失败原因（找不到 WBP_Bag / CreateWidget 失败）
    }

    bBagInWeaponMode = true;

    // 锁到「武器」分类 —— 这是「武器背包」与普通背包的差别之一（另一个是点格子=装备）
    SetBagKind(WeaponKindTool);

    UE_LOG(LogTemp, Warning,
        TEXT("[Arm] 已打开武器背包：分类=%d（武器）｜点格子就装备=%s｜共 %d 件武器可选。"),
        WeaponKindTool,
        bEquipOnWeaponSlotClick ? TEXT("开") : TEXT("关"),
        CurrentBagItems.Num());

    // 被 kind_tool 算成武器、却找不到武器蓝图的那几件，一次性列出来。
    // 不报的话玩家只会看到「武器背包里东西不对」，而这几乎必然是数据/蓝图没配好，不是坏了。
    LogWeaponItemFilterReport();

    // ---- 每次打开武器背包都体检一次 ----
    //
    // ★ 这里原来是「只在 CurrentBagItems.Num() == 0 时才打」。
    //   为什么改成【无条件】：那个条件本身依赖「过滤 / 解析是否正确」——
    //   而最需要日志的情形恰恰是「列表非空、但内容不对」（东西都在，却点不动装备）。
    //   上一轮用户的三个现象就是这个形状：列表被清空那次有日志，可一旦列表里还剩东西，
    //   体检就一条都不打了。凡是「值不值得打日志」靠另一个可能出错的东西判断，就会这样漏。
    {
        EnsureWeaponRowIndex();

        UE_LOG(LogTemp, Log,
            TEXT("[Arm] 武器背包体检：索引 %d 条｜Data_tool 行名 %d 个｜列表 %d 件｜手上武器 %s｜装备记录行=%s"),
            WeaponRowToClassIndex.Num(),
            InventoryComponent ? InventoryComponent->ToolTableRowNames.Num() : 0,
            CurrentBagItems.Num(),
            CurrentWeapon ? *CurrentWeapon->GetClass()->GetName() : TEXT("<无>"),
            EquippedWeaponRow.IsNone() ? TEXT("<无>") : *EquippedWeaponRow.ToString());

        for (const TPair<FName, TSubclassOf<AWeaponBase>>& Pair : WeaponRowToClassIndex)
        {
            UE_LOG(LogTemp, Log, TEXT("[Arm]   索引：行名/ID『%s』 → %s"),
                *Pair.Key.ToString(), Pair.Value ? *Pair.Value->GetName() : TEXT("?"));
        }

        if (WeaponRowToClassIndex.Num() == 0)
        {
            // 索引空 = 解析链断了。它现在【不会】再清空武器背包（列表只看数据），
            // 但仍然会让「装备」点不动、「已装备」只能靠记录兜底 —— 必须报出来。
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] ★ 索引是空的 → 任何一行都解析不出武器蓝图类。后果：\n"
                     "        · 武器背包列表照常显示（列表只按 kind_tool + 数据表归属判定）；\n"
                     "        · 但点「装备」会被拒绝，且「已装备」标记只能靠装备记录兜底。\n"
                     "      定位：看日志里紧邻上面的这两行\n"
                     "        「自动扫描：目录 … → 扫到 N 个资产，其中继承 AWeaponBase 的有 M 个」\n"
                     "        「武器蓝图索引已重建：候选 … → 成功绑定 K 条」\n"
                     "      修法（三选一）：\n"
                     "        ① BP_PlayerCharacter → Weapon|Switch → Weapon Class Candidates 里手工登记武器蓝图类\n"
                     "        ② 给每个武器蓝图填 Class Defaults → Weapon|Bag → Bag Item Row\n"
                     "        ③ 确认 Weapon|Switch → Weapon Blueprint Search Paths 指向 /Game/characters/arms"));
        }
        else if (CurrentBagItems.Num() == 0)
        {
            // 索引有货、列表却空 → 根因在数据表这一侧（kind_tool 没填成武器 / 行名不在 Data_tool）。
            // 两者都要报 —— 只报一头的话，用户改完表还是空的，会以为代码坏了。
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] ★ 武器分类里一件物品都没有（kind_tool=%d）→ 武器背包会是空的。修法（二选一）：\n"
                     "        ① 把数据表里武器那些行的 kind_tool 改成 %d\n"
                     "           （E_kind_tool 的显示名按值序：0=武器 1=声骸 2=消耗品 3=材料 4=箱匣 5=特殊）\n"
                     "        ② 若武器其实被归到了别的分类，把 Weapon|Switch → Weapon Kind Tool 改成那个值"),
                WeaponKindTool, WeaponKindTool);

            if (InventoryComponent)
            {
                const TArray<FBagItemEntry> AllOwned = InventoryComponent->GetItemsForKind(-1);
                UE_LOG(LogTemp, Warning,
                    TEXT("[Arm]   当前背包里共 %d 条物品（行名 / 名称 / kind_tool / 是否算武器）："),
                    AllOwned.Num());
                for (const FBagItemEntry& Owned : AllOwned)
                {
                    UE_LOG(LogTemp, Warning, TEXT("[Arm]     · %s / %s / kind_tool=%d / %s"),
                        *Owned.RowName.ToString(),
                        *Owned.ToolName,
                        Owned.KindTool,
                        IsWeaponItem(Owned) ? TEXT("是") : TEXT("否"));
                }
                if (AllOwned.Num() == 0)
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Arm]     背包本体也是空的 → 先确认 Data_tool 里有物品、且 tool_num 填了数量。"));
                }
            }
        }
    }
}

void ABattleCharacter::CloseWeaponBag()
{
    // 语义入口。真正的收尾（恢复游戏 + 自动回武器页）在 CloseBag 里，
    // 这样「B 键关 / btn_close 关 / 这里关」三条路行为完全一致。
    CloseBag();
}

void ABattleCharacter::OnOpenWeaponBagClicked()
{
    UE_LOG(LogTemp, Log, TEXT("[Arm] 武器页按钮被点击 → 打开武器背包。"));
    OpenWeaponBag();
}

UWidget* ABattleCharacter::FindArmPanelWidget(const TCHAR* WidgetName) const
{
    if (!CharacterPanelWidget || !WidgetName)
    {
        return nullptr;
    }

    // 直接在整个面板里按名字找，而不是限定在 arm_imfor 子树里 ——
    // 你把控件和 arm_imfor 平级放（或放进别的容器）也能命中，少一层「必须放在哪」的约束。
    return CharacterPanelWidget->GetWidgetFromName(WidgetName);
}

namespace ArmPanelDump
{
    // 递归打印「名字 (类名)」：控件名能不能对上 BindWidget / GetWidgetFromName，看这个最快
    static void Walk(UWidget* W, int32 Depth)
    {
        if (!W || Depth > 8)
        {
            return;
        }

        FString Pad;
        for (int32 i = 0; i < Depth; ++i)
        {
            Pad += TEXT("    ");
        }
        UE_LOG(LogTemp, Log, TEXT("[Arm]   %s%s (%s)"), *Pad, *W->GetName(), *W->GetClass()->GetName());

        if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
        {
            const int32 ChildCount = Panel->GetChildrenCount();
            for (int32 i = 0; i < ChildCount; ++i)
            {
                Walk(Panel->GetChildAt(i), Depth + 1);
            }
        }
        else if (UContentWidget* Content = Cast<UContentWidget>(W))
        {
            Walk(Content->GetContent(), Depth + 1);
        }
    }
}

void ABattleCharacter::LogArmPanelStructure()
{
    if (!CharacterPanelWidget)
    {
        return;
    }

    UWidget* ArmRoot = CharacterPanelWidget->GetWidgetFromName(TEXT("arm_imfor"));
    if (!ArmRoot)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ★ 面板里没有名为 arm_imfor 的控件 → 武器页无法定位（切到武器页时也不会显示任何东西）。\n"
                 "      修法：在 WBP_Character_imf 里把武器页那一块的容器命名为 arm_imfor"
                 "（类型建议 Canvas Panel / Border，方便整页显隐）。\n"
                 "      下面是面板顶层的实际控件，挑一个改成 arm_imfor 即可："));
        if (UPanelWidget* RootPanel = Cast<UPanelWidget>(CharacterPanelWidget->GetRootWidget()))
        {
            const int32 N = RootPanel->GetChildrenCount();
            for (int32 i = 0; i < N; ++i)
            {
                if (UWidget* C = RootPanel->GetChildAt(i))
                {
                    UE_LOG(LogTemp, Log, TEXT("[Arm]   顶层[%d] %s (%s)"), i, *C->GetName(), *C->GetClass()->GetName());
                }
            }
        }
        return;
    }

    UE_LOG(LogTemp, Log, TEXT("[Arm] ===== arm_imfor 控件树快照（%s / %s）====="),
        *ArmRoot->GetName(), *ArmRoot->GetClass()->GetName());

    // 父链：控件「跑到屏幕外 / 看不见」时，第一件要看的事就是它挂在哪一层
    FString Chain;
    for (UWidget* P = ArmRoot->GetParent(); P; P = P->GetParent())
    {
        Chain += FString::Printf(TEXT(" ← %s(%s)"), *P->GetName(), *P->GetClass()->GetName());
    }
    UE_LOG(LogTemp, Log, TEXT("[Arm]   父链：%s%s"), *ArmRoot->GetName(),
        Chain.IsEmpty() ? TEXT("（无父控件 → 它就是根，注意它的尺寸是不是撑满了）") : *Chain);

    ArmPanelDump::Walk(ArmRoot, 0);

    UE_LOG(LogTemp, Log,
        TEXT("[Arm]   想显示武器信息，就按这些约定名放控件（缺哪个都不影响切换武器，只是那一项不显示）：\n"
             "        btn_open_arm_bag / btn_arm_bag / btn_arm_butt / arm_bag_butt / btn_weapon_bag / arm_butt  → Button（打开武器背包）\n"
             "        img_arm_icon   → Image（武器图标）      txt_arm_name   → TextBlock（武器名）\n"
             "        txt_arm_star   → TextBlock（品质 5★）  txt_arm_type   → TextBlock（武器类型）\n"
             "        txt_arm_attack → TextBlock（攻击力）    txt_arm_intro  → TextBlock（简介）\n"
             "        （可选）背包格子里的 WBP_Bag_Slot 加一个 TextBlock 命名 txt_slot_equipped → 给已装备那格标「装备中」"));
    UE_LOG(LogTemp, Log, TEXT("[Arm] ===== 快照结束 ====="));
}

void ABattleCharacter::BindArmPanelButtons()
{
    bArmPanelButtonsBound = false;

    if (!CharacterPanelWidget)
    {
        return;
    }

    // 「打开武器背包」按钮：按候选名依次找。
    // 候选取多个，是因为这个按钮是你在设计器里新建的、名字只有你知道；
    // 全都没命中时不是静默跳过，而是把「实际有哪些控件」打出来（见 LogArmPanelStructure）。
    static const TCHAR* const Candidates[] = {
        TEXT("btn_open_arm_bag"),
        TEXT("btn_arm_bag"),
        TEXT("btn_arm_butt"),
        TEXT("arm_bag_butt"),
        TEXT("btn_weapon_bag"),
        TEXT("arm_butt"),
    };

    UWidget* Found = nullptr;
    FString FoundName;

    for (const TCHAR* Candidate : Candidates)
    {
        if (UWidget* W = CharacterPanelWidget->GetWidgetFromName(Candidate))
        {
            Found = W;
            FoundName = Candidate;
            break;
        }
    }

    if (UButton* Btn = Cast<UButton>(Found))
    {
        // AddDynamic 是宏，函数名必须直接以字面量传入（见 BindCharacterPanelTabs 的详细说明）。
        // 面板每次打开都重建 Widget → RemoveAll 先清掉旧绑定，避免同一实例被绑多次。
        Btn->OnClicked.RemoveAll(this);
        Btn->OnClicked.AddDynamic(this, &ABattleCharacter::OnOpenWeaponBagClicked);
        bArmPanelButtonsBound = true;
        UE_LOG(LogTemp, Log, TEXT("[Arm] 武器页按钮已绑定：%s → 打开武器背包。"), *FoundName);
    }
    else if (Found)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ★ 找到名为 %s 的控件，但它的类型是 %s，不是 Button → 点了没反应。\n"
                 "      修法：把它换成 Button（或外面套一个 Button），或另建一个 Button 命名为 btn_open_arm_bag。"),
            *FoundName, *Found->GetClass()->GetName());
        LogArmPanelStructure();
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ★ 武器页里没有找到「打开武器背包」按钮（找过：btn_open_arm_bag / btn_arm_bag / "
                 "btn_arm_butt / arm_bag_butt / btn_weapon_bag / arm_butt）。\n"
                 "      功能本身不受影响 —— 蓝图里也可以直接调 Open Weapon Bag 节点，或用 B 键打开背包。\n"
                 "      想让按钮生效：在 WBP_Character_imf 的 arm_imfor 里加一个 Button，"
                 "命名为上面任意一个（推荐 btn_open_arm_bag）。"));
        LogArmPanelStructure();
    }
}

void ABattleCharacter::RefreshArmPanel()
{
    // 面板没开（例如玩家已关掉面板、只开着武器背包）→ 刷新只是"顺带"，无事可做。
    // 这里不报错：面板下次打开会重新算一遍。
    if (!CharacterPanelWidget)
    {
        return;
    }

    // ★ 选中「其他角色」（非当前操控角色 this）时，武器页同步显示该角色的默认武器，
    //   而不是 this 手上的武器 —— 否则切到头像选了别人，武器页还显示自己的刀，看着像串号。
    //   this 自己（或没选角色）走下面原来的「实际装备」逻辑不变。
    if (const ABattleCharacter* Selected = ResolveSelectedCharaSource())
    {
        if (Selected != this)
        {
            RefreshArmPanelForCharacter(Selected);
            return;
        }
    }

    // ---- 组装要显示的内容 ----
    FString NameStr = TEXT("未装备武器");
    FString StarStr;
    FString TypeStr;
    FString AttackStr;
    FString IntroStr;
    UTexture2D* IconTex = nullptr;
    FLinearColor StarColor = FLinearColor::White;

    // ---- 先清洗装备记录：它可能被一个「填错表」的武器蓝图声明写成了别的表的行名 ----
    // 不清的话，每次刷新都要重新识别一遍，而且这个错误的行号会一直挂在角色身上
    // （背包那边会因为「不属于背包表」而永远不显示它，看起来像装备状态丢了）。
    if (!EquippedWeaponRow.IsNone() && InventoryComponent
        && !InventoryComponent->IsToolTableRow(EquippedWeaponRow))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] 装备记录『%s』不属于背包物品表（Data_tool）→ 已清除该记录。\n"
                 "      它多半是被某个武器蓝图里填错表的 Bag Item Row 写进来的：\n"
                 "      两套行名各自独立（Data_tool 用 1、2、3…；掉落表用 100、101…），填串了不会有任何报错。\n"
                 "      清除后武器页会改显示武器类名 —— 那是正确状态（这把武器在背包里没有对应的一格）。"),
            *EquippedWeaponRow.ToString());
        EquippedWeaponRow = NAME_None;
        bEquippedWeaponRowFromBag = false;
    }

    // ★ 展示信息以【实际装备的武器类】为准，而不是裸的背包行号。
    //   为什么（本轮 bug 的第二个现象）：EquippedWeaponRow 只是「记录」，会被武器蓝图
    //   声明回填、也会被别的装备入口改写；真正生效的是手上的武器 Actor。
    //   两者脱钩时武器页就会显示成另一件东西 —— 就是「武器信息与角色蓝图的 Weapon Class 不符」。
    const FName DisplayRow = GetEquippedWeaponDisplayRow();
    const TSubclassOf<AWeaponBase> EquippedClass = CurrentWeapon ? CurrentWeapon->GetClass() : nullptr;

    // ---- 记录纠偏：记录的背包行 ≠ 实际装着的那把 → 报一次并把记录修正过来 ----
    // 不修正的话，背包里的「装备中」会一直落在错误的那一格上（而且每次刷新都错）。
    if (EquippedClass && !DisplayRow.IsNone() && EquippedWeaponRow != DisplayRow
        && ResolveWeaponClassForItemRow(EquippedWeaponRow) != EquippedClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] ⚠ 装备记录与实际武器不一致，已自动纠正：\n"
                 "        记录的行=『%s』（解析出 %s）｜实际装备的类=%s（对应的行=『%s』）\n"
                 "      这类不一致的常见来路：① 武器蓝图声明的 Bag Item Row 与玩家点的那一行不同；\n"
                 "      ② 那一格是掉落物/别类物品（kind_tool 取默认值 0 被当成武器）。\n"
                 "      现在以【实际装备的类】为准，背包高亮与武器页都跟随它。"),
            *EquippedWeaponRow.ToString(),
            ResolveWeaponClassForItemRow(EquippedWeaponRow)
                ? *ResolveWeaponClassForItemRow(EquippedWeaponRow)->GetName() : TEXT("(解析不出武器类)"),
            *EquippedClass->GetName(), *DisplayRow.ToString());
        EquippedWeaponRow = DisplayRow;
    }

    FBagItemEntry EquippedItem;
    const bool bHasEquipped = !DisplayRow.IsNone()
        && InventoryComponent
        && InventoryComponent->GetItemByRow(DisplayRow, EquippedItem);

    if (bHasEquipped)
    {
        NameStr = EquippedItem.ToolName.IsEmpty() ? DisplayRow.ToString() : EquippedItem.ToolName;
        IntroStr = EquippedItem.ToolIntrodu;
        IconTex = EquippedItem.ToolImage;
        if (InventoryComponent)
        {
            StarStr = InventoryComponent->GetStarText(EquippedItem.Star).ToString();
            StarColor = InventoryComponent->GetStarColor(EquippedItem.Star);
        }
    }
    else if (EquippedClass)
    {
        // 手上确实拿着武器，但它在背包里没有对应的一格（典型：开局自带的 Default Weapon Class）。
        // 前两份信息（名称/图标）没有表数据可查 —— 那就把武器类名显示出来，
        // 而不是让面板写着「未装备武器」而人手上拿着刀。
        NameStr = FString::Printf(TEXT("%s（不在背包中：来自角色蓝图的 Weapon Class）"),
            *EquippedClass->GetName());
    }

    // 类别 / 攻击力从【实际装备上的武器 Actor】读，而不是从数据表 ——
    // 这两项是 AWeaponBase 上的属性（Enemy 表里根本没有这两列），
    // 而且切换武器类时变化的就是它们，读 Actor 才能证明「类真的换了」。
    if (CurrentWeapon)
    {
        // 武器类别（三分类：轻剑/法器/枪械）—— 鸣潮五类已移除，类型统一按类别显示。
        static const TCHAR* const CategoryNames[] = {
            TEXT("轻剑"), TEXT("法器"), TEXT("枪械")
        };
        const int32 CatIndex = static_cast<int32>(CurrentWeapon->GetWeaponCategory());
        TypeStr = CategoryNames[(CatIndex >= 0 && CatIndex < 3) ? CatIndex : 0];
        AttackStr = FString::Printf(TEXT("%.0f"), CurrentWeapon->GetAttackPower());
    }

    // ---- 写进控件（按约定名；缺哪个就跳过那一项，不影响功能）----
    auto SetArmText = [this](const TCHAR* WidgetName, const FString& Value)
    {
        if (UTextBlock* Text = Cast<UTextBlock>(FindArmPanelWidget(WidgetName)))
        {
            Text->SetText(FText::FromString(Value));
        }
    };

    SetArmText(TEXT("txt_arm_name"), NameStr);
    SetArmText(TEXT("txt_arm_star"), StarStr);
    SetArmText(TEXT("txt_arm_type"), TypeStr);
    SetArmText(TEXT("txt_arm_attack"), AttackStr);
    SetArmText(TEXT("txt_arm_intro"), IntroStr);

    if (UTextBlock* StarText = Cast<UTextBlock>(FindArmPanelWidget(TEXT("txt_arm_star"))))
    {
        StarText->SetColorAndOpacity(FSlateColor(StarColor));
    }

    if (UImage* Icon = Cast<UImage>(FindArmPanelWidget(TEXT("img_arm_icon"))))
    {
        if (IconTex)
        {
            Icon->SetBrushFromTexture(IconTex, false);
            Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            // 未装备 / 该物品在表里没填图标 → 隐藏，别留一个空白框让人以为是坏了
            Icon->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    const FString StarPart = StarStr.IsEmpty() ? FString() : FString::Printf(TEXT(" %s"), *StarStr);
    const FString TypePart = TypeStr.IsEmpty() ? FString() : FString::Printf(TEXT(" 类型=%s"), *TypeStr);
    const FString AtkPart = AttackStr.IsEmpty()
        ? FString(TEXT("（未装备任何武器 → 武器处为空，属正常）"))
        : FString::Printf(TEXT(" 攻击力=%s"), *AttackStr);

    UE_LOG(LogTemp, Log, TEXT("[Arm] 武器页已刷新：%s%s%s%s"), *NameStr, *StarPart, *TypePart, *AtkPart);

    // ---- 控件名一个都没对上时，一次性把真实结构打出来（只打一次，避免每次切页刷屏）----
    if (!bArmPanelStructureLogged)
    {
        const bool bAnyConventionWidget = FindArmPanelWidget(TEXT("txt_arm_name")) != nullptr
            || FindArmPanelWidget(TEXT("img_arm_icon")) != nullptr
            || FindArmPanelWidget(TEXT("txt_arm_star")) != nullptr;

        if (!bAnyConventionWidget)
        {
            bArmPanelStructureLogged = true;
            UE_LOG(LogTemp, Warning,
                TEXT("[Arm] arm_imfor 里没有任何「约定名」控件（txt_arm_name / img_arm_icon / txt_arm_star / "
                     "txt_arm_type / txt_arm_attack / txt_arm_intro）→ 武器信息没有地方显示。\n"
                     "      功能不受影响（武器照样能切换、能同步），只是面板上不显示信息。\n"
                     "      想显示：在 arm_imfor 里加 TextBlock / Image 并按上面的名字命名即可。"));
            LogArmPanelStructure();
        }
    }
}

// 用指定角色（其他角色 CDO）的默认武器信息填武器页。
// 选中其他角色时，武器页显示「该角色蓝图里配的 Default Weapon Class」的默认属性，
// 而不是 this 手上实装的那把 —— 否则切到头像选了别人，武器页还显示自己的刀。
void ABattleCharacter::RefreshArmPanelForCharacter(const ABattleCharacter* Selected)
{
    if (!Selected)
    {
        return;
    }

    FString NameStr = TEXT("未装备武器");
    FString StarStr;
    FString TypeStr;
    FString AttackStr;
    FString IntroStr;
    UTexture2D* IconTex = nullptr;
    FLinearColor StarColor = FLinearColor::White;

    // 该角色类【当前装备的武器】：优先查每角色类运行时表（绕开 CDO 继承），
    // 查不到才回退该角色类 CDO 的 DefaultWeaponClass（开局默认）。
    //
    // ★ 为什么要先查表（「串号」bug 的修复）：直接读 Selected->DefaultWeaponClass 会拿到
    //   CDO 值 —— 而 BP_Chara_magic 继承 BP_PlayerCharacter 时，子类 CDO 会继承父类 CDO 的
    //   DefaultWeaponClass，于是「给 BP_PlayerCharacter 切武器，BP_Chara_magic 面板也跟着变」。
    //   装备状态已在 ClassEquippedWeapon 按角色类隔离存，这里必须优先读它。
    TSubclassOf<AWeaponBase> WeaponClass = nullptr;
    if (const TSubclassOf<AWeaponBase>* Equipped = ClassEquippedWeapon.Find(Selected->GetClass()))
    {
        WeaponClass = *Equipped;
    }
    if (!WeaponClass)
    {
        WeaponClass = Selected->DefaultWeaponClass;
    }
    const AWeaponBase* WeaponCDO = WeaponClass ? WeaponClass->GetDefaultObject<AWeaponBase>() : nullptr;

    if (WeaponClass && WeaponCDO)
    {
        // 类别 / 攻击力：从武器 CDO 读（AWeaponBase 上的属性，表里没有这两列）
        static const TCHAR* const CategoryNames[] = {
            TEXT("轻剑"), TEXT("法器"), TEXT("枪械")
        };
        const int32 CatIndex = static_cast<int32>(WeaponCDO->GetWeaponCategory());
        TypeStr = CategoryNames[(CatIndex >= 0 && CatIndex < 3) ? CatIndex : 0];
        AttackStr = FString::Printf(TEXT("%.0f"), WeaponCDO->GetAttackPower());

        // 名称/图标/品质：能对上背包表才显示（对不上就只显示类名，与 this 路径的「不在背包中」一致）
        const FName BagRow = WeaponCDO->GetBagItemRow();

        FBagItemEntry EquippedItem;
        if (!BagRow.IsNone() && InventoryComponent
            && InventoryComponent->GetItemByRow(BagRow, EquippedItem))
        {
            NameStr = EquippedItem.ToolName.IsEmpty() ? BagRow.ToString() : EquippedItem.ToolName;
            IntroStr = EquippedItem.ToolIntrodu;
            IconTex = EquippedItem.ToolImage;
            StarStr = InventoryComponent->GetStarText(EquippedItem.Star).ToString();
            StarColor = InventoryComponent->GetStarColor(EquippedItem.Star);
        }
        else
        {
            NameStr = FString::Printf(TEXT("%s（角色 %s 的默认武器）"),
                *WeaponClass->GetName(), *Selected->GetCharacterDisplayName());
        }
    }

    // ---- 写进控件（复用同一套约定名写入逻辑）----
    auto SetArmText = [this](const TCHAR* WidgetName, const FString& Value)
    {
        if (UTextBlock* Text = Cast<UTextBlock>(FindArmPanelWidget(WidgetName)))
        {
            Text->SetText(FText::FromString(Value));
        }
    };

    SetArmText(TEXT("txt_arm_name"), NameStr);
    SetArmText(TEXT("txt_arm_star"), StarStr);
    SetArmText(TEXT("txt_arm_type"), TypeStr);
    SetArmText(TEXT("txt_arm_attack"), AttackStr);
    SetArmText(TEXT("txt_arm_intro"), IntroStr);

    if (UTextBlock* StarText = Cast<UTextBlock>(FindArmPanelWidget(TEXT("txt_arm_star"))))
    {
        StarText->SetColorAndOpacity(FSlateColor(StarColor));
    }

    if (UImage* Icon = Cast<UImage>(FindArmPanelWidget(TEXT("img_arm_icon"))))
    {
        if (IconTex)
        {
            Icon->SetBrushFromTexture(IconTex, false);
            Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
        else
        {
            Icon->SetVisibility(ESlateVisibility::Hidden);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[Arm] 武器页已同步选中角色『%s』的默认武器：%s（类型=%s 攻击力=%s）"),
        *Selected->GetCharacterDisplayName(), *NameStr, *TypeStr, *AttackStr);
}

void ABattleCharacter::RefreshCharacterPanelStats()
{
    // 面板没开 → 无事可做（面板每次打开都会从头填一遍属性，不会漏）
    if (!CharacterPanelWidget)
    {
        return;
    }

    // 这条日志是「换武器 → attack_num 变了」的**唯一证据**，所以两个分支各打一条：
    //   ① 有它 → 说明重算逻辑真的被调到了（没打到 = 调用点没接上，不是数值算错）
    //   ② 括号里的「其中武器 X」→ 一眼看出攻击力里武器贡献了多少
    //      现象「换了刀攻击力没变」时：X=0 是武器蓝图没配攻击力；X≠0 是你看到的是别的角色槽位。
    const FString WeaponRowText =
        EquippedWeaponRow.IsNone() ? FString(TEXT("(未装备)")) : EquippedWeaponRow.ToString();

    if (CurrentCharaSlotIndex >= 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] 角色面板属性已重算：按槽位 %d｜攻击力=%.0f（其中武器 %.0f）｜当前武器行=%s。"),
            CurrentCharaSlotIndex, GetTotalAttackPower(), GetWeaponAttackPower(), *WeaponRowText);
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Arm] 角色面板属性已重算：按当前角色｜攻击力=%.0f（其中武器 %.0f）｜当前武器行=%s。"),
            GetTotalAttackPower(), GetWeaponAttackPower(), *WeaponRowText);
    }

    if (CurrentCharaSlotIndex >= 0)
    {
        // 走 ApplyCharacterSlot：它自己会处理「槽位越界 / 未绑定类 → 显示空值」，
        // 所以重算结果与当初选那个槽位时完全一致，不会把「空值」变成「当前角色属性」。
        ApplyCharacterSlot(CurrentCharaSlotIndex);
    }
    else
    {
        // 没选过槽位（或没配 CharacterSlotClasses）→ 显示当前角色自身的实时属性。
        // 这条路才是「换武器 → 攻击力变化」最直观的体现：GetTotalAttackPower()
        // 里就含着 CurrentWeapon->GetAttackPower()。
        ApplyCharacterPanelFrom(this);
    }
}

// ---- 攻击系统 ----
// ---- 攻击索敌：以角色为中心 AttackAimRadius 范围内有存活怪物时，转向最近的怪物 ----
void ABattleCharacter::AimAttackAtNearestMonster()
{
    if (!GetWorld())
        return;

    const FVector PlayerLoc = GetActorLocation();

    // ---- 索敌优先：存在锁定目标（存活且在 LockOnRadius 内）→ 倾向朝锁定目标 ----
    // 警觉态启用（bEnableUpperBodyFacingTarget=true）：不再整体旋转 Actor，让 Actor 跟速度方向走；
    // 目标朝向在 ABP 中通过 Orientation Warping 节点把上半身旋到目标（"警觉"效果）。
    // 警觉态关闭：回退原行为——整体 Actor 转向锁定目标（保持旧手感）。
    if (AMonsterBase* Locked = GetValidLockedTarget())
    {
        FVector ToLocked = Locked->GetActorLocation() - PlayerLoc;
        ToLocked.Z = 0.0f;
        if (!ToLocked.IsNearlyZero())
        {
            FRotator LockRot = ToLocked.Rotation();
            LockRot.Pitch = 0.0f;
            LockRot.Roll = 0.0f;
            if (bEnableUpperBodyFacingTarget)
            {
                // 警觉态：不转 Actor，仅刷新 UpperBodyYawOffset 给 ABP 使用（攻击起手瞬时对齐，无平滑）
                if (!ToLocked.IsNearlyZero())
                {
                    const float TargetYawDeg = FRotator::NormalizeAxis(ToLocked.Rotation().Yaw);
                    const float ActorYawDeg = FRotator::NormalizeAxis(GetActorRotation().Yaw);
                    UpperBodyYawOffset = FRotator::NormalizeAxis(TargetYawDeg - ActorYawDeg);
                    UpperBodyYawWeight = FMath::Min(1.0f, UpperBodyYawWeight + 0.5f);
                }
            }
            else
            {
                // 回退行为：整体转向目标
                SetActorRotation(LockRot);
            }
        }
        return;
    }

    // 开启索敌但当前无锁定目标 → 不强行转向（等索敌系统自动锁上范围内最近的怪物）
    if (bEnableLockOn)
        return;

    // ---- 旧行为（索敌关闭时保留）：AttackAimRadius 内最近的存活怪物瞬时转向 ----
    TArray<AActor*> Monsters;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

    const float RadiusSq = AttackAimRadius * AttackAimRadius;

    AMonsterBase* Nearest = nullptr;
    float NearestDistSq = RadiusSq;
    for (AActor* Actor : Monsters)
    {
        AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster || Monster->IsDead())
            continue;

        const float DistSq = FVector::DistSquared(PlayerLoc, Monster->GetActorLocation());
        if (DistSq <= NearestDistSq)
        {
            NearestDistSq = DistSq;
            Nearest = Monster;
        }
    }

    if (!Nearest)
        return;

    FVector ToTarget = Nearest->GetActorLocation() - PlayerLoc;
    ToTarget.Z = 0.0f;
    if (ToTarget.IsNearlyZero())
        return;

    FRotator TargetRotation = ToTarget.Rotation();
    TargetRotation.Pitch = 0.0f;
    TargetRotation.Roll = 0.0f;
    if (bEnableUpperBodyFacingTarget)
    {
        // 警觉态：不转 Actor，仅刷新 UpperBodyYawOffset 给 ABP 使用（攻击起手瞬时对齐，无平滑）
        if (!ToTarget.IsNearlyZero())
        {
            const float TargetYawDeg = FRotator::NormalizeAxis(ToTarget.Rotation().Yaw);
            const float ActorYawDeg = FRotator::NormalizeAxis(GetActorRotation().Yaw);
            UpperBodyYawOffset = FRotator::NormalizeAxis(TargetYawDeg - ActorYawDeg);
            UpperBodyYawWeight = FMath::Min(1.0f, UpperBodyYawWeight + 0.5f);
        }
    }
    else
    {
        // 回退行为：整体转向目标
        SetActorRotation(TargetRotation);
    }
}

// ---- 切人自动普攻门控：附近是否存在「已进入战斗」的存活怪物 ----
// 判据：AttackAimRadius 范围内存在 bInCombat=true 的存活怪物（仇恨已建立）。
// 切人自动普攻仅在返回 true 时触发，避免脱战/未引起仇恨时空挥。
bool ABattleCharacter::HasMonsterInCombat() const
{
    if (!GetWorld())
        return false;

    const FVector PlayerLoc = GetActorLocation();
    const float RadiusSq = AttackAimRadius * AttackAimRadius;

    TArray<AActor*> Monsters;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

    for (AActor* Actor : Monsters)
    {
        const AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster || Monster->IsDead())
            continue;
        // 只认「已进入战斗」的怪物：仇恨已建立才算有效目标
        if (!Monster->IsInCombat())
            continue;
        // 距离门控：仅 AttackAimRadius 内的怪物算「附近」（与自动普攻的索敌半径一致）
        const float DistSq = FVector::DistSquared(PlayerLoc, Monster->GetActorLocation());
        if (DistSq <= RadiusSq)
            return true;
    }

    return false;
}

// ---- 技能索敌：指定半径内寻找最近存活怪物并平滑转向（RInterpTo，只转 Yaw）----
// 锁定目标存在且在指定半径内 → 优先朝向锁定目标（技能/大招施放期间持续追踪的基准）
bool ABattleCharacter::AimAtNearestMonsterInRange(float Radius, float DeltaTime, float InterpSpeed)
{
    if (!GetWorld() || Radius <= 0.0f)
        return false;

    const FVector PlayerLoc = GetActorLocation();
    const float RadiusSq = Radius * Radius;

    AMonsterBase* Preferred = nullptr;

    // 锁定目标（存活 + 在指定半径内）优先作为转向基准
    if (AMonsterBase* Locked = LockedTarget.Get())
    {
        if (!Locked->IsDead() &&
            FVector::DistSquared(PlayerLoc, Locked->GetActorLocation()) <= RadiusSq)
        {
            Preferred = Locked;
        }
    }

    // 无锁定目标/锁定目标不在半径内 → 找半径内最近的存活怪物
    if (!Preferred)
    {
        TArray<AActor*> Monsters;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

        float NearestDistSq = RadiusSq;
        for (AActor* Actor : Monsters)
        {
            AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
            if (!Monster || Monster->IsDead())
                continue;

            const float DistSq = FVector::DistSquared(PlayerLoc, Monster->GetActorLocation());
            if (DistSq <= NearestDistSq)
            {
                NearestDistSq = DistSq;
                Preferred = Monster;
            }
        }
    }

    if (!Preferred)
        return false;

    FVector ToTarget = Preferred->GetActorLocation() - PlayerLoc;
    ToTarget.Z = 0.0f;
    if (ToTarget.IsNearlyZero())
        return true;

    FRotator TargetRotation = ToTarget.Rotation();
    TargetRotation.Pitch = 0.0f;
    TargetRotation.Roll = 0.0f;

    // 平滑转向：只保留 Yaw，同时清除可能的 Pitch/Roll 残留
    const FRotator CurrentFlat(0.0f, GetActorRotation().Yaw, 0.0f);
    if (bEnableUpperBodyFacingTarget)
    {
        // 警觉态：不转 Actor，仅刷新 UpperBodyYawOffset（目标方向供 ABP Warping 使用）
        const FVector ToTargetFlat = ToTarget;
        SetUpperBodyYawOffsetToFaceTarget(ToTargetFlat, DeltaTime);
        return true;
    }
    const FRotator NewRotation = FMath::RInterpTo(CurrentFlat, TargetRotation, DeltaTime, InterpSpeed);
    SetActorRotation(FRotator(0.0f, NewRotation.Yaw, 0.0f));
    return true;
}

// ---- Tick：技能/大招施放期间持续自动索敌转向 ----
void ABattleCharacter::UpdateSkillAim(float DeltaTime)
{
    if (!bSkillAutoAim)
        return;

    if (bIsSkillCasting || bIsUltimateCasting)
    {
        AimAtNearestMonsterInRange(SkillAimRadius, DeltaTime, SkillAimInterpSpeed);
    }
}

// ---- 索敌锁定系统 ----
// 返回当前有效索敌目标：已锁定、存活、且在 LockOnRadius（水平距离）内；否则返回 nullptr
AMonsterBase* ABattleCharacter::GetValidLockedTarget() const
{
    AMonsterBase* Target = LockedTarget.Get();
    if (!Target || Target->IsDead() || !GetWorld())
        return nullptr;

    const FVector Delta = Target->GetActorLocation() - GetActorLocation();
    if (Delta.SizeSquared2D() > LockOnRadius * LockOnRadius)
        return nullptr;

    return Target;
}

// Tick 维护索敌目标：
// - 无目标 → 自动锁定范围内最近的存活怪物（单目标，有且只有一个）
// - 有目标但死亡/出范围 → 立即更换为范围内最近怪物
// - 某怪物比当前目标更近且持续保持更近 LockOnRetargetTime 秒 → 更换为新目标
// 同时每帧刷新索敌追踪 UI（目标位置随动 + 离屏/背后隐藏）
void ABattleCharacter::UpdateLockOn(float DeltaTime)
{
    if (!GetWorld())
        return;

    if (!bEnableLockOn)
    {
        LockedTarget = nullptr;
        CloserCandidate = nullptr;
        CloserCandidateTime = 0.0f;
        HideLockOnIndicator();
        return;
    }

    // 追踪 UI 每帧刷新（与怪物列表扫描节流解耦，保证指示器随动平滑）
    UpdateLockOnIndicator();

    LockOnScanTimer -= DeltaTime;
    if (LockOnScanTimer > 0.0f)
        return;
    LockOnScanTimer = LockOnScanInterval;

    // ---- 收集范围内存活怪物并找最近 ----
    TArray<AActor*> Monsters;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

    const FVector PlayerLoc = GetActorLocation();
    const float LockRadiusSq = LockOnRadius * LockOnRadius;

    AMonsterBase* Nearest = nullptr;
    float NearestDistSq = LockRadiusSq;
    for (AActor* Actor : Monsters)
    {
        AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster || Monster->IsDead())
            continue;

        const float DistSq = (Monster->GetActorLocation() - PlayerLoc).SizeSquared2D();
        if (DistSq <= NearestDistSq)
        {
            NearestDistSq = DistSq;
            Nearest = Monster;
        }
    }

    AMonsterBase* Current = LockedTarget.Get();
    const bool bCurrentValid = Current && !Current->IsDead() &&
        (Current->GetActorLocation() - PlayerLoc).SizeSquared2D() <= LockRadiusSq;

    // 无目标 / 当前目标失效（死亡或出范围）→ 立即锁定范围内最近怪物
    if (!bCurrentValid)
    {
        LockedTarget = Nearest;
        CloserCandidate = nullptr;
        CloserCandidateTime = 0.0f;
        return;
    }

    // 有有效目标：存在比它更近的存活怪物 → 开始/累计换锁计时（同一候选需一直保持更近）
    if (Nearest && Nearest != Current)
    {
        const float CurrentDistSq = (Current->GetActorLocation() - PlayerLoc).SizeSquared2D();
        if (NearestDistSq < CurrentDistSq)
        {
            if (CloserCandidate.Get() != Nearest)
            {
                CloserCandidate = Nearest;
                CloserCandidateTime = 0.0f;
            }
            else
            {
                // 每次扫描累加一个扫描间隔，近似"持续更近"的真实时长
                CloserCandidateTime += LockOnScanInterval;
                if (CloserCandidateTime >= LockOnRetargetTime)
                {
                    UE_LOG(LogTemp, Warning, TEXT("LockOn: switched to closer monster '%s' (%.1fs closer than previous target)."),
                        *Nearest->GetName(), LockOnRetargetTime);
                    LockedTarget = Nearest;
                    CloserCandidate = nullptr;
                    CloserCandidateTime = 0.0f;
                }
            }
            return;
        }
    }

    // 当前目标就是最近（或没有更近候选）→ 清空换锁累计
    CloserCandidate = nullptr;
    CloserCandidateTime = 0.0f;
}

// 蓝图只读查询：当前索敌目标
AMonsterBase* ABattleCharacter::GetLockedTargetActor() const
{
    return GetValidLockedTarget();
}

// 蓝图只读查询：警觉朝向 Yaw 差（ABP Orientation Warping 的 Goal Yaw Offset 输入）
float ABattleCharacter::GetUpperBodyYawOffset() const
{
    return UpperBodyYawOffset;
}

// 蓝图只读查询：警觉激活权重（ABP 可在警觉朝向 vs Actor 朝向间做 Lerp）
float ABattleCharacter::GetUpperBodyYawWeight() const
{
    return UpperBodyYawWeight;
}

// 懒创建索敌追踪 Widget：加视口、中心对齐、默认隐藏（后续每帧 SetPositionInViewport 定位）
void ABattleCharacter::CreateLockOnIndicatorWidget()
{
    if (LockOnIndicatorWidget || !LockOnIndicatorClass || !GetWorld())
        return;

    LockOnIndicatorWidget = CreateWidget<UUserWidget>(GetWorld(), LockOnIndicatorClass);
    if (LockOnIndicatorWidget)
    {
        LockOnIndicatorWidget->AddToViewport(50); // 高层：追踪指示器常驻最上
        LockOnIndicatorWidget->SetAlignmentInViewport(FVector2D(0.5f, 0.5f));
        LockOnIndicatorWidget->SetVisibility(ESlateVisibility::Hidden);
        UE_LOG(LogTemp, Warning, TEXT("LockOn indicator widget created (%s)."), *LockOnIndicatorClass->GetName());
    }
}

// 隐藏索敌提示（无目标/离屏/未配置时调用；不销毁，锁定后直接复用同一实例）
void ABattleCharacter::HideLockOnIndicator()
{
    if (LockOnIndicatorWidget && bLockOnIndicatorVisible)
    {
        LockOnIndicatorWidget->SetVisibility(ESlateVisibility::Hidden);
        bLockOnIndicatorVisible = false;
    }
}

// 每帧刷新索敌追踪 UI：
// - 追踪点取目标怪物碰撞胶囊的几何中心（≈ 模型中心，自动适配大/小体型）；
//   目标无胶囊组件时兜底退回脚底上方 LockOnIndicatorHeight 处
// - 追踪点投影到屏幕（屏幕空间定位 → 不随镜头缩放/距离变化）
// - 投影点在相机背后或移出视口矩形 → 隐藏（怪物未出现在视野内不显示）
// - 未配置 LockOnIndicatorClass 则永远不显示
void ABattleCharacter::UpdateLockOnIndicator()
{
    AMonsterBase* Target = GetValidLockedTarget();
    if (!Target || !LockOnIndicatorClass)
    {
        HideLockOnIndicator();
        return;
    }

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
    {
        HideLockOnIndicator();
        return;
    }

    CreateLockOnIndicatorWidget();
    if (!LockOnIndicatorWidget)
        return;

    // 怪物中心：胶囊组件原点即胶囊几何中心（ACharacter 的 Capsule 原点 = 脚底 + 半高），
    // 对任意体型的怪自动取到身体中心，避免标记浮在头顶/陷进脚下
    FVector WorldPos = Target->GetActorLocation();
    if (UCapsuleComponent* MonsterCapsule = Target->GetCapsuleComponent())
    {
        WorldPos = MonsterCapsule->GetComponentLocation();
    }
    else
    {
        WorldPos.Z += LockOnIndicatorHeight;
    }

    // 相机背后的点投影失败 → 目标不在视野内，隐藏
    FVector2D ScreenPos(0.0f, 0.0f);
    if (!PC->ProjectWorldLocationToScreen(WorldPos, ScreenPos, false))
    {
        HideLockOnIndicator();
        return;
    }

    // 目标移出屏幕矩形（含小边距）→ 隐藏
    int32 ViewW = 0, ViewH = 0;
    PC->GetViewportSize(ViewW, ViewH);
    const bool bOnScreen = ScreenPos.X >= -8.0f && ScreenPos.X <= (float)ViewW + 8.0f &&
                           ScreenPos.Y >= -8.0f && ScreenPos.Y <= (float)ViewH + 8.0f;
    if (!bOnScreen)
    {
        HideLockOnIndicator();
        return;
    }

    // 屏幕空间定位：指示器像素尺寸恒定，不受镜头缩放/目标距离影响
    LockOnIndicatorWidget->SetAlignmentInViewport(FVector2D(0.5f, 0.5f));
    LockOnIndicatorWidget->SetPositionInViewport(ScreenPos, true);
    if (!bLockOnIndicatorVisible)
    {
        LockOnIndicatorWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
        bLockOnIndicatorVisible = true;
    }
}

// ---- 警觉朝向：按条件计算权重 + 平滑 YawOffset ----
// 触发条件（全部满足才进入警觉）：
//  1) bEnableUpperBodyFacingTarget = true
//  2) 锁定目标存活 + 水平距离 ≤ AlertFacingRadius
//  3) 不在 Sprint（bExitAlertOnSprint = true 时）；或在 Sprint 中允许警觉 = 不退出
//  4) 不在空中（避免半空中身体乱转）
// 进入 → 权重平滑到 1；退出 → 权重平滑到 0
// YawOffset = (目标方向 - Actor 朝向)的 Yaw 差，ABP 用作 Warping Goal
void ABattleCharacter::UpdateUpperBodyFacingTarget(float DeltaTime)
{
    if (!bEnableUpperBodyFacingTarget)
    {
        // 总开关关闭：权重持续归零（不旋转上半身）
        UpperBodyYawWeight = FMath::FInterpTo(UpperBodyYawWeight, 0.0f, DeltaTime, AlertFacingInterpSpeed);
        if (UpperBodyYawWeight < KINDA_SMALL_NUMBER)
        {
            UpperBodyYawWeight = 0.0f;
            UpperBodyYawOffset = 0.0f;
        }
        return;
    }

    // ---- 计算"目标警觉权重"（0/1 目标值）----
    float DesiredWeight = 0.0f;
    AMonsterBase* Locked = GetValidLockedTarget();
    if (Locked && GetCharacterMovement() && GetCharacterMovement()->IsMovingOnGround())
    {
        const FVector ToTarget = Locked->GetActorLocation() - GetActorLocation();
        const float DistSq2D = ToTarget.SizeSquared2D();
        if (DistSq2D <= AlertFacingRadius * AlertFacingRadius)
        {
            if (!(bExitAlertOnSprint && bIsSprinting))
            {
                DesiredWeight = 1.0f;
            }
        }
    }

    // ---- 平滑权重 ----
    UpperBodyYawWeight = FMath::FInterpTo(UpperBodyYawWeight, DesiredWeight, DeltaTime, AlertFacingInterpSpeed);
    if (FMath::IsNearlyZero(UpperBodyYawWeight))
    {
        UpperBodyYawWeight = 0.0f;
        UpperBodyYawOffset = 0.0f;
        return;
    }

    // ---- 权重 > 0：刷新 YawOffset（目标相对 Actor 的 Yaw 差）----
    if (Locked)
    {
        const FVector ToTargetFlat = Locked->GetActorLocation() - GetActorLocation();
        SetUpperBodyYawOffsetToFaceTarget(ToTargetFlat, DeltaTime);
    }
    else
    {
        // 目标中途丢失 → 逐渐回零
        UpperBodyYawOffset = FMath::FInterpTo(UpperBodyYawOffset, 0.0f, DeltaTime, AlertFacingInterpSpeed);
        if (FMath::Abs(UpperBodyYawOffset) < 0.5f)
        {
            UpperBodyYawOffset = 0.0f;
        }
    }
}

// ---- 工具：把 ToTargetFlat 转成相对 Actor 的 Yaw 差，平滑写入 UpperBodyYawOffset ----
// 入参 ToTargetFlat 必须为水平向量（Z 已清零）。最终 UpperBodyYawOffset 单位为度
void ABattleCharacter::SetUpperBodyYawOffsetToFaceTarget(const FVector& ToTargetFlat, float DeltaTime)
{
    FVector Flat = ToTargetFlat;
    Flat.Z = 0.0f;
    if (Flat.IsNearlyZero())
    {
        UpperBodyYawOffset = FMath::FInterpTo(UpperBodyYawOffset, 0.0f, DeltaTime, AlertFacingInterpSpeed);
        return;
    }

    // 目标 Yaw（世界空间）vs Actor Yaw（世界空间） → 相对 Yaw 差
    const float TargetYawDeg = FRotator::NormalizeAxis(Flat.Rotation().Yaw);
    const float ActorYawDeg = FRotator::NormalizeAxis(GetActorRotation().Yaw);
    const float DeltaYaw = FRotator::NormalizeAxis(TargetYawDeg - ActorYawDeg);

    // 平滑（避免硬切）。注意 RInterpTo 不走最短路径，这里用手动 Lerp + NormalizeAxis 走最短路径
    float NewOffset = UpperBodyYawOffset;
    const float MaxStep = AlertFacingInterpSpeed * DeltaTime;
    const float Diff = FRotator::NormalizeAxis(DeltaYaw - UpperBodyYawOffset);
    if (FMath::Abs(Diff) <= MaxStep)
    {
        NewOffset = DeltaYaw;
    }
    else
    {
        NewOffset = FRotator::NormalizeAxis(UpperBodyYawOffset + FMath::Sign(Diff) * MaxStep);
    }
    UpperBodyYawOffset = FRotator::NormalizeAxis(NewOffset);
}

void ABattleCharacter::LightAttack()
{
    if (!CanAttack())
    {
        UE_LOG(LogTemp, Warning, TEXT("LightAttack: Cannot attack right now!"));
        return;
    }

    // ---- 跳跃期间普攻 → 下落攻击（落地结算伤害）----
    // 条件：在空中 + 脚部离地高度达到触发门槛。
    // 门槛 FallAttackTriggerHeight <= 0 时自动取"实际可达最大跳跃高度"（按物理推导，含一段跳+二段跳）
    if (GetCharacterMovement() && !GetCharacterMovement()->IsMovingOnGround())
    {
        // 有效触发门槛：手动值 > 0 用手动值，否则自动取物理推导的最大跳跃高度
        const float EffectiveTriggerHeight = (FallAttackTriggerHeight > 0.0f)
            ? FallAttackTriggerHeight
            : GetMaxJumpHeight();

        // 从脚部（胶囊底部）向下长射线检测地面，计算真实离地高度
        float HeightAboveGround = 0.0f;
        FHitResult FloorHit;
        const float CapsuleHalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 90.0f;
        const FVector TraceStart = GetActorLocation() - FVector(0.0f, 0.0f, CapsuleHalfHeight);
        const FVector TraceEnd = TraceStart - FVector(0.0f, 0.0f, 10000.0f);
        FCollisionQueryParams QueryParams;
        QueryParams.AddIgnoredActor(this);
        if (GetWorld()->LineTraceSingleByChannel(FloorHit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
        {
            HeightAboveGround = TraceStart.Z - FloorHit.ImpactPoint.Z;
        }
        else
        {
            // 射线未命中 → 高度足够（高于检测范围）
            HeightAboveGround = EffectiveTriggerHeight + 1.0f;
        }

        // 带 200cm 容差的 >= 判定：跳跃到最高点（恰好等于最大跳跃高度）也能触发，
        // 避免顶点零点几帧严格大于判定失效
        if (HeightAboveGround >= EffectiveTriggerHeight - 200.0f)
        {
            UE_LOG(LogTemp, Warning, TEXT("Fall Attack: Height %.0f >= Trigger %.0f, triggering!"),
                HeightAboveGround, EffectiveTriggerHeight);
            PerformFallAttack();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Fall Attack: Height %.0f < Trigger %.0f, too low!"),
                HeightAboveGround, EffectiveTriggerHeight);
        }
        return;
    }

    // 连击逻辑：检查是否超时
    if (ComboTimer > ComboResetTime && CurrentComboStep > 0)
    {
        ResetCombo();
        UE_LOG(LogTemp, Warning, TEXT("Combo reset due to timeout!"));
    }

    // 执行当前段的攻击
    PerformComboAttack(CurrentComboStep);
}

// ---- 下落攻击：空中普攻触发（播放蒙太奇 + 加速下坠，伤害落地结算）----
void ABattleCharacter::PerformFallAttack()
{
    // ---- 攻击索敌：下落攻击出招瞬间同样转向最近的怪物 ----
    AimAttackAtNearestMonster();

    bIsFallAttacking = true;
    bIsAttacking = true;
    CurrentAttackCooldown = AttackCooldown;

    // 下落攻击不打断连击段数记忆，落地后可继续连招
    ResetCombo();

    // 播放下落攻击蒙太奇
    if (FallAttackMontage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            AnimInstance->Montage_Play(FallAttackMontage);
            UE_LOG(LogTemp, Warning, TEXT("Fall Attack: Playing FallAttackMontage!"));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Fall Attack triggered! (FallAttackMontage not set in BP_PlayerCharacter)"));
    }

    // 快速下坠扑向地面
    if (GetCharacterMovement())
    {
        FVector V = GetCharacterMovement()->Velocity;
        V.Z = -FMath::Abs(FallAttackDiveSpeed);
        GetCharacterMovement()->Velocity = V;
    }
}

// ---- 下落攻击落地冲击：对范围内所有怪物造成伤害 ----
void ABattleCharacter::ApplyFallAttackImpact()
{
    if (!GetWorld())
        return;

    TArray<AActor*> Monsters;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

    int32 HitCount = 0;
    for (AActor* Actor : Monsters)
    {
        AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster)
            continue;

        if (FVector::Dist(Monster->GetActorLocation(), GetActorLocation()) <= FallAttackImpactRadius)
        {
            FVector Direction = (Monster->GetActorLocation() - GetActorLocation()).GetSafeNormal();
            // 下落攻击伤害 = 统一伤害公式（攻击力×下落攻击倍率×[暴击]×(1+增伤)×等级系数×0.9），返回值为实际结算伤害
            bool bWasCrit = false;
            const float FinalDamage = ComputeFinalDamage(FallAttackDamage, Monster, bWasCrit);
            // 预设暴击标志：怪物 TakeDamage 按实际扣血量弹出（暴击/普通）伤害飘字
            Monster->SetPendingDamageNumberCrit(bWasCrit);
            const float ActualDamage = UGameplayStatics::ApplyPointDamage(Monster, FinalDamage, Direction, FHitResult(), GetController(), this, nullptr);
            // 增伤窗口内按实际伤害吸血回血
            ApplyEnergyBuffLifesteal(ActualDamage);

            // 命中处于可弹刀前摇窗口的怪物 → 自动触发弹刀，打断其攻击
            TryParryOnHit(Monster);
            ++HitCount;
        }
    }

    // 镜头振动改为仅弹刀触发（TryParryOnHit 弹刀成功分支），普通命中不再振动

    UE_LOG(LogTemp, Warning, TEXT("Fall Attack impact! Radius=%.0f, Damage=%.1f, Hit %d monster(s)."),
        FallAttackImpactRadius, FallAttackDamage, HitCount);
}

// ==================== 普攻连段段列表（段数 = ComboAttackSegments 元素个数） ====================

// ---- 启动时解析段列表：同步段数 → 打一条「现在到底有几段」的结论日志 ----
void ABattleCharacter::EnsureComboSegments()
{
    // ---- ① 段数 = 数组长度 ----
    // 下限 1：段数为 0 会让下面的 `% MaxComboStep` 变成除零（直接崩），必须兜住。
    MaxComboStep = FMath::Max(1, ComboAttackSegments.Num());

    // 段数变少时把游标夹回合法区间，否则会一直停在越界段号上、取不到蒙太奇
    CurrentComboStep = FMath::Clamp(CurrentComboStep, 0, MaxComboStep - 1);

    // ---- ② 结论日志：逐段列出来，而不是只说一个数字 ----
    // ★ 为什么要逐段列：段数可配之后，「我明明加了 5 段，怎么只打 4 段」这类问题光看一个数字
    //   判断不出来 —— 可能是数组只有 4 个元素，也可能是第 5 个元素没填蒙太奇。
    //   把「有值 / 空段」逐条打出来，两种情况一眼可辨，不用再回来查一轮。
    UE_LOG(LogTemp, Warning,
        TEXT("[Combo] 普攻段数 = %d（= Combo Attack Segments 元素个数），连段重置时间 = %.2fs"),
        MaxComboStep, ComboResetTime);

    for (int32 Step = 0; Step < ComboAttackSegments.Num(); ++Step)
    {
        const FAttackComboSegment& Segment = ComboAttackSegments[Step];
        const FString MontageName = Segment.Montage
            ? Segment.Montage->GetName()
            : FString(TEXT("（空 —— 这一段不会有动画）"));
        const FString WindowText = (Segment.CancelWindowTime < 0.0f)
            ? FString(TEXT("立即可打断"))
            : FString::Printf(TEXT("%.2fs"), Segment.CancelWindowTime);

        UE_LOG(LogTemp, Warning, TEXT("[Combo]   第 %d 段：%s   可打断窗口 = %s"),
            Step + 1, *MontageName, *WindowText);
    }

    if (ComboAttackSegments.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[Combo] ★ Combo Attack Segments 是空的 → 普攻不会有任何动画。\n"
                 "        修法：BP_PlayerCharacter → Details → Combo → Combo Attack Segments\n"
                 "              → 点 + 加元素，每个元素填一个普攻蒙太奇（第 1 个元素 = 第 1 段）。"));
    }
}

int32 ABattleCharacter::GetComboSegmentCount() const
{
    return FMath::Max(1, ComboAttackSegments.Num());
}

UAnimMontage* ABattleCharacter::GetComboMontage(int32 Step) const
{
    if (!ComboAttackSegments.IsValidIndex(Step))
    {
        return nullptr;
    }
    // 不用三元表达式：TObjectPtr 与 nullptr 的三元会让编译器去猜公共类型，
    // 写成显式分支既没有歧义，读起来也更直白。
    return ComboAttackSegments[Step].Montage;
}

float ABattleCharacter::GetComboCancelWindowTime(int32 Step) const
{
    if (!ComboAttackSegments.IsValidIndex(Step))
    {
        return 0.0f;
    }
    // -1（未配置）与显式填 0 语义一致：都表示立即可打断。
    // 这里统一收敛成 >= 0，调用方就不用再判 -1 了。
    return FMath::Max(0.0f, ComboAttackSegments[Step].CancelWindowTime);
}

int32 ABattleCharacter::FindComboStepIndex(const UAnimMontage* Montage) const
{
    if (!Montage)
    {
        return INDEX_NONE;
    }
    for (int32 Step = 0; Step < ComboAttackSegments.Num(); ++Step)
    {
        // ★ 同一段蒙太奇可以在数组里出现多次（例如故意让第 4、6 段复用同一动画）——
        //   这种情况下返回【最先匹配】的那一段。窗口时间因此以更靠前的那段为准，
        //   想让两段窗口不同就得用不同的蒙太奇资产。
        if (ComboAttackSegments[Step].Montage.Get() == Montage)
        {
            return Step;
        }
    }
    return INDEX_NONE;
}

void ABattleCharacter::PerformComboAttack(int32 Step)
{
    bIsAttacking = true;
    CurrentAttackCooldown = AttackCooldown;

    // ---- 普攻根运动补丁（修复段间位置回跳，与技能/大招同方案）----
    EnsureAttackRootMotion();

    // ---- 攻击索敌：出招瞬间转向怪物——有索敌锁定目标则优先朝向锁定目标，
    // 未开启索敌时回退为 AttackAimRadius(200cm) 内最近怪物（无目标保持原朝向）----
    AimAttackAtNearestMonster();

    // ---- 取当前段的蒙太奇 ----
    // 以前是 switch(Step) 硬编码 1~4 四个字段；现在段数可配，改成按段号查数组。
    // 段号越界 / 该段没填蒙太奇 → nullptr，走下面的报错分支。
    UAnimMontage* TargetMontage = GetComboMontage(Step);

    // 【修复】增加 GetMesh() 判空保护，防止空指针崩溃
    if (TargetMontage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            AnimInstance->Montage_Play(TargetMontage);
            UE_LOG(LogTemp, Warning, TEXT("Combo Attack %d executed!"), Step + 1);
        }
    }
    else
    {
        // ★ 报错要指明【是哪种原因】：段号越界（数组比段号短）和该段没填蒙太奇
        //   是两种不同的修法，只说一句 "not set" 会让人不知道去改哪儿。
        UE_LOG(LogTemp, Warning,
            TEXT("[Combo] 第 %d 段无法播放：%s（当前共 %d 段，数组元素 %d 个）"),
            Step + 1,
            ComboAttackSegments.IsValidIndex(Step)
                ? TEXT("该段没填蒙太奇")
                : TEXT("段号超出数组长度"),
            MaxComboStep,
            ComboAttackSegments.Num());
    }

    // ---- 【新增】武器攻击 ----
    // 近战武器（阔刃/迅刀/臂铠）：延迟到打击帧做球形扫掠命中判定
    // 远程武器（手枪/音感仪）：立即朝角色前方发射弹丸
    if (CurrentWeapon)
    {
        CurrentWeapon->BeginAttack(Step);
    }

    // 推进连击段数（MaxComboStep 由 EnsureComboSegments 从数组长度算出，至少 1）
    CurrentComboStep = (CurrentComboStep + 1) % FMath::Max(1, MaxComboStep);
    ComboTimer = 0.0f;

    // ---- 旋转守卫：记录最近一次普攻时间窗（Tick 中无蒙太奇播放时逐帧清残留旋转）----
    AttackRotationGuardRemaining = FMath::Max(0.3f, AttackRotationGuardDuration);

    // 最后一段打完 → 回到第 1 段
    if (CurrentComboStep == 0)
    {
        ResetCombo();
        UE_LOG(LogTemp, Warning, TEXT("[Combo] 第 %d 段（最后一段）打完，连段回到第 1 段。"), MaxComboStep);
    }

    // 重置攻击状态（实际应由动画通知触发）
    GetWorld()->GetTimerManager().SetTimerForNextTick([this]()
        {
            bIsAttacking = false;
        });
}

void ABattleCharacter::ResetCombo()
{
    CurrentComboStep = 0;
    ComboTimer = 0.0f;
    UE_LOG(LogTemp, Warning, TEXT("Combo reset!"));
}

void ABattleCharacter::ForceResetCombo()
{
    ResetCombo();
}

void ABattleCharacter::OnComboAttackEnd()
{
    // 连击结束处理（由动画通知调用）
    bIsAttacking = false;

    // 取消尚未执行的武器命中判定
    if (CurrentWeapon)
    {
        CurrentWeapon->EndAttack();
    }
}

// ---- 当前正在播放的普攻蒙太奇（遍历 ComboAttackSegments），无则 nullptr ----
UAnimMontage* ABattleCharacter::GetActiveComboMontage() const
{
    // 段数可配之后，这里不能再用"1~4 四个 if"——改成遍历段列表。
    // 段数改成 6 段也能自动覆盖，不需要再来改这段代码。
    if (UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
    {
        for (const FAttackComboSegment& Segment : ComboAttackSegments)
        {
            if (Segment.Montage && AnimInst->Montage_IsPlaying(Segment.Montage))
            {
                return Segment.Montage;
            }
        }
    }
    return nullptr;
}

// ---- 当前普攻段是否已进入可打断窗口（播放进度 >= 该段的 CancelWindowTime）----
bool ABattleCharacter::IsComboCancelWindowReached() const
{
    if (!bEnableComboCancelWindow)
    {
        return true; // 关闭窗口机制 → 始终可打断（回退旧行为，衔接不受动画进度限制）
    }

    UAnimMontage* Active = GetActiveComboMontage();
    if (!Active || !GetMesh())
    {
        return false;
    }

    UAnimInstance* AnimInst = GetMesh()->GetAnimInstance();
    if (!AnimInst)
    {
        return false;
    }

    // 当前段索引：正在播放的蒙太奇对应的段号（0 基，段数可配之后不再固定 0~3）
    const int32 ActiveStep = FindComboStepIndex(Active);
    if (ActiveStep == INDEX_NONE)
    {
        return false;
    }

    // 该段的打断窗口时间（未配置 → 0.0 = 立即可打断，即旧行为）
    const float WindowTime = GetComboCancelWindowTime(ActiveStep);

    // 当前播放进度（秒）
    const float CurrentPos = AnimInst->Montage_GetPosition(Active);
    return CurrentPos >= WindowTime;
}

// ---- 闪避系统 ----
// 完美闪避检测：任一怪物处于攻击前摇窗口（攻击已发起、伤害未结算）且玩家在其攻击范围内
bool ABattleCharacter::CheckPerfectDodgeWindow() const
{
    TArray<AMonsterBase*> Targets;
    GatherPerfectDodgeTargets(Targets);
    return Targets.Num() > 0;
}

// 收集可被完美闪避的怪物：攻击前摇内 + 在角色为中心的球体范围（PerfectDodgeRadius）内
void ABattleCharacter::GatherPerfectDodgeTargets(TArray<AMonsterBase*>& OutTargets) const
{
    OutTargets.Reset();
    if (!GetWorld())
        return;

    TArray<AActor*> Monsters;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

    const FVector PlayerLoc = GetActorLocation();
    for (AActor* Actor : Monsters)
    {
        AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster || !Monster->IsInPerfectDodgeWindow())
            continue;

        // 球体范围（3D 距离，以角色为中心）：取 PerfectDodgeRadius 与该怪物实际攻击范围
        // （含各段 HitRadius）的较大值——攻击范围超过球体半径的 Boss/大范围怪也能被
        // 完美闪避停滞，避免"弹了完美闪避 UI 但怪没被停滞、无敌结束后照样被打中"。
        // 狂暴声波攻击阶段额外覆盖声波扩散半径：波圈最远到 EnrageSoundWaveRadius，玩家在
        // 圈内任意距离闪避都必须能探测到该怪的完美闪避窗口
        float MonsterRange = Monster->GetAttackRange();
        if (Monster->IsEnrageSoundWaveStage())
        {
            MonsterRange = FMath::Max(MonsterRange, Monster->GetEnrageSoundWaveRadius());
        }
        // 毒刺远程攻击阶段额外覆盖毒刺射程：玩家在毒刺飞行途中任意距离闪避
        // 都必须能探测到该怪的完美闪避窗口（射程由 BossStingMaxFlightDistance 蓝图配置）
        if (Monster->IsBossStingFlying())
        {
            MonsterRange = FMath::Max(MonsterRange, Monster->GetBossStingMaxFlightDistance());
        }
        const float EffectiveRadius = FMath::Max(PerfectDodgeRadius, MonsterRange + 100.0f);
        if (FVector::Dist(PlayerLoc, Monster->GetActorLocation()) <= EffectiveRadius)
        {
            OutTargets.Add(Monster);
        }
    }
}

void ABattleCharacter::Dodge()
{
    // 受击硬直中禁止闪避
    if (bIsHitReaction)
        return;

    // ---- 大招期间闪避无效：大招全程不可被打断 ----
    if (bIsUltimateCasting)
    {
        UE_LOG(LogTemp, Warning, TEXT("Dodge: Blocked during ultimate!"));
        return;
    }

    // ---- 技能期间闪避 ----
    // 当前段允许打断 → 打断技能（停止后续段、立刻进入冷却），闪避照常执行；
    // 当前段不允许打断 → 闪避无效
    if (bIsSkillCasting)
    {
        if (IsCurrentSkillSegmentDodgeInterruptible())
        {
            UE_LOG(LogTemp, Warning, TEXT("Dodge interrupted the skill! Entering cooldown immediately."));
            EndSkill();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Dodge: Current skill segment is not interruptible!"));
            return;
        }
    }

    // 完美闪避后的锁定期间无法再次闪避（默认 0.2s）
    if (CurrentDodgeLockout > 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Dodge locked after perfect dodge! (%.2fs left)"), CurrentDodgeLockout);
        return;
    }

    // ---- 完美闪避判定：怪物攻击前摇窗口内闪避 ----
    const bool bPerfectDodge = CheckPerfectDodgeWindow();

    // 空中且非完美闪避 → 闪避键执行二段跳（完美闪避优先，空中也可触发）
    if (!bPerfectDodge && GetCharacterMovement() && !GetCharacterMovement()->IsMovingOnGround())
    {
        PerformDoubleJump();
        return;
    }

    if (bIsDodging)
        return;

    // 普通闪避需要耐力；完美闪避不消耗耐力
    if (!bPerfectDodge)
    {
        if (CurrentStamina < 20.0f)
            return;

        CurrentStamina -= 20.0f;
        UpdateStaminaBar();
    }

    // ---- 闪避打断普攻（闪避确认执行时，普攻任意期间都可被打断）----
    // 普攻进行中（段列表里任一段蒙太奇播放或攻击状态位有效；下落攻击除外——空中归二段跳体系）。
    // 打断瞬间若命中判定恰好到出伤时刻（本帧内应触发）→ 先执行一次接触判定出伤
    // （前方有怪物接触才掉血），实现"打断恰在出伤时间且有模型接触 → 先出伤后打断"，
    // 随后取消武器判定并停掉普攻蒙太奇（跨蒙太奇组也能可靠打断）
    UAnimInstance* DodgeInterruptAnim = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
    // 下落攻击归空中体系（空中闪避=二段跳），不在此打断范围内
    // ★ 段列表里"任一段在播"直接复用 GetActiveComboMontage()（它内部就是遍历 ComboAttackSegments），
    //   不再像以前那样手写 4 个 Montage_IsPlaying —— 段数改成几段都自动覆盖。
    const bool bGroundAttackActive = !bIsFallAttacking &&
        (bIsAttacking || GetActiveComboMontage() != nullptr);

    if (bGroundAttackActive)
    {
        if (CurrentWeapon)
        {
            // 已到出伤时刻且有接触 → 内部立即结算伤害并返回 true（判定随之清除）；
            // 未到出伤时刻 → 返回 false，走 EndAttack 取消未执行的判定
            if (!CurrentWeapon->TryResolveDueMeleeHit())
            {
                CurrentWeapon->EndAttack();
            }
        }
        bIsAttacking = false;

        // 停掉所有正在播的普攻段（同样遍历段列表，段数可配）
        for (const FAttackComboSegment& Segment : ComboAttackSegments)
        {
            if (Segment.Montage && DodgeInterruptAnim && DodgeInterruptAnim->Montage_IsPlaying(Segment.Montage))
            {
                DodgeInterruptAnim->Montage_Stop(0.06f, Segment.Montage);
            }
        }
    }
    else if (CurrentWeapon)
    {
        // 无普攻可打断：维持原逻辑——取消尚未执行的武器命中判定
        CurrentWeapon->EndAttack();
    }

    // ---- 冲刺中触发闪避（完美/普通）→ 先停止冲刺、恢复基础移速 ----
    // 否则闪避位移会沿用冲刺加速后的 MaxWalkSpeed，导致完美闪避时角色仍带疾跑加速；
    // 闪避与疾跑互斥，闪避确认执行时统一退出疾跑状态
    if (bIsSprinting)
    {
        StopSprint();
        UE_LOG(LogTemp, Warning, TEXT("Sprint stopped on dodge (perfect=%d)."), bPerfectDodge ? 1 : 0);
    }

    bIsDodging = true;

    // ---- 闪避/完美闪避根运动补丁：为 Dodge/Dash/PerfectDodge 蒙太奇启用根运动，
    // 位移真实驱动胶囊（修复蒙太奇结束模型跳回原点的衔接问题）
    EnsureDodgeRootMotion();

    if (bPerfectDodge)
    {
        // ---- 完美闪避反馈：不打断怪物攻击，角色靠无敌帧规避伤害 ----
        // 无敌时长 = max(基础完美闪避无敌时间, 所有怪物"该段攻击判定窗口剩余时间")：
        // 从完美闪避触发直到该段攻击的受击判定时间结束全程无敌（期间其他怪物的攻击同样打不中）
        TArray<AMonsterBase*> Targets;
        GatherPerfectDodgeTargets(Targets);
        float PerfectInvincibilityTime = PerfectDodgeInvincibilityTime;
        for (AMonsterBase* Monster : Targets)
        {
            const float SuggestedInvincibility = Monster->StaggerByPerfectDodge(PerfectDodgeStaggerDuration);
            PerfectInvincibilityTime = FMath::Max(PerfectInvincibilityTime, SuggestedInvincibility);
        }

        // ---- 完美闪避：更长无敌 + 专用蒙太奇 + 闪避锁定 ----
        CurrentDodgeLockout = PerfectDodgeLockoutTime;

        // 无敌帧 GAS 化（P3）：应用无敌 GE，到期自动移除 State.Invincible Tag
        GrantInvincibility(PerfectInvincibilityTime);

        // 播放完美闪避蒙太奇（未配置则退回普通闪避蒙太奇）
        UAnimMontage* TargetMontage = PerfectDodgeMontage ? PerfectDodgeMontage
            : (IsMoving() ? DashMontage : DodgeMontage);
        if (TargetMontage && GetMesh())
        {
            if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
            {
                AnimInstance->Montage_Play(TargetMontage);
            }
        }

        UE_LOG(LogTemp, Warning, TEXT("PERFECT DODGE! No stamina cost, invincible for %.2fs (covers hit-judgement window), dodge locked for %.2fs."),
            PerfectInvincibilityTime, PerfectDodgeLockoutTime);

        // 飘字"闪避"反馈（角色附近出现，上浮+淡出后自动销毁）
        ShowPerfectDodgeText();

        // ---- 蓝色残影：完美闪避触发生成拖尾残影（持续 PerfectDodgeGhostDuration 秒）----
        StartPerfectDodgeGhosts();

        // 触发极限闪避成功事件（协奏能量奖励等，蓝图可扩展时停/特效）
        OnLimitDodgeSuccess();
    }
    else
    {
        // ---- 普通闪避：0.3s 无敌（GAS 化）----
        GrantInvincibility(0.3f);

        if (IsMoving())
        {
            // 【修复】增加 GetMesh() 判空保护
            if (DashMontage && GetMesh())
            {
                if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
                {
                    AnimInstance->Montage_Play(DashMontage);
                }
            }
        }
        else
        {
            // 【修复】增加 GetMesh() 判空保护
            if (DodgeMontage && GetMesh())
            {
                if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
                {
                    AnimInstance->Montage_Play(DodgeMontage);
                }
            }
        }
    }

    // 闪避位移（完美/普通闪避共用）
    // 使用角色当前移速作为闪避速度，保持原有移动速度不受叠加影响出现加速
    if (IsMoving())
    {
        FVector DashDirection = GetActorForwardVector();
        float DashSpeed = GetCharacterMovement() ? GetCharacterMovement()->MaxWalkSpeed : 600.0f;

        if (GetCharacterMovement())
        {
            FVector CurrentVelocity = GetCharacterMovement()->Velocity;
            float VerticalSpeed = CurrentVelocity.Z;
            FVector NewVelocity = DashDirection * DashSpeed;
            NewVelocity.Z = VerticalSpeed;
            GetCharacterMovement()->Velocity = NewVelocity;
        }

        UE_LOG(LogTemp, Warning, TEXT("Dash forward!"));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Dodge in place!"));
    }

    GetWorld()->GetTimerManager().SetTimerForNextTick([this]()
        {
            bIsDodging = false;
            // ---- 闪避旋转守卫：覆盖闪避蒙太奇结束 + BlendOut 混合输出期，
            // 无蒙太奇播放时每帧清除根运动残留的 Pitch/Roll（防闪避后模型歪斜，
            // 复用普攻旋转守卫通道；蒙太奇仍在播时守卫自动跳过不打断根运动）
            AttackRotationGuardRemaining = FMath::Max(AttackRotationGuardRemaining, FMath::Max(0.3f, AttackRotationGuardDuration));
        });
}

// ---- 弹刀系统（命中自动触发，无专门按键）----
// 角色任意攻击（近战扫掠/远程弹丸/下落攻击）命中怪物时调用：
// 该怪物正处于可弹刀前摇窗口 → 自动触发弹刀，打断该次攻击（怪物硬直）
bool ABattleCharacter::TryParryOnHit(AMonsterBase* HitMonster)
{
    if (!HitMonster)
        return false;

    // 冷却检查（仅成功弹刀才进入冷却，普通命中不受影响）
    if (CurrentParryCooldown > 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Parry on hit skipped: cooldown remaining %.2fs."), CurrentParryCooldown);
        return false;
    }

    // 必须处于弹刀时间窗口（蒙太奇播放位置在 [ParryWindowStart, ParryWindowEnd] 内、该段标记为可弹刀）
    if (!HitMonster->IsTelegraphingParryableAttack())
        return false;

    // 弹刀成功：打断怪物攻击（停蒙太奇/取消伤害结算/怪物硬直）
    const bool bInterrupted = HitMonster->InterruptAttackByParry(this);
    CurrentParryCooldown = ParryCooldown;

    // 弹刀镜头拉近（可配置距离/时长，弹刀成功瞬间触发）
    TriggerParryCameraZoom();

    // 播放弹刀蒙太奇（未配置则跳过，逻辑不受影响）
    if (ParryMontage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            AnimInstance->Montage_Play(ParryMontage);
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("PARRY! Attack hit parryable telegraph of '%s' -> attack interrupted.%s"),
        *HitMonster->GetName(), bInterrupted ? TEXT("") : TEXT(" (interrupt failed)"));
    return true;
}

// ---- 段根运动补丁（修复技能/大招段间位置回跳）----
void ABattleCharacter::EnsureSegmentRootMotion()
{
    if (!bEnableSegmentRootMotion)
    {
        return;
    }

    // 动画实例必须允许处理蒙太奇根运动（模板默认即 RootMotionFromMontagesOnly，此处防御性纠正）
    if (UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
    {
        if (AnimInst->RootMotionMode == ERootMotionMode::IgnoreRootMotion ||
            AnimInst->RootMotionMode == ERootMotionMode::NoRootMotionExtraction)
        {
            AnimInst->RootMotionMode = ERootMotionMode::RootMotionFromMontagesOnly;
            UE_LOG(LogTemp, Warning, TEXT("[SegmentRootMotion] AnimInstance was ignoring root motion, switched to RootMotionFromMontagesOnly."));
        }
    }

    if (bSegmentRootMotionInitialized)
    {
        return;
    }
    bSegmentRootMotionInitialized = true;

    int32 PatchedCount = 0;
    auto PatchSegmentArray = [&PatchedCount](const TArray<FComboMontageSegment>& Segments, const TCHAR* ArrayName)
    {
        for (int32 SegIdx = 0; SegIdx < Segments.Num(); ++SegIdx)
        {
            UAnimMontage* Montage = Segments[SegIdx].Montage;
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

                    // 诊断：确认根骨骼确实带位移数据（位移烤进 pelvis 等姿势骨骼的动画无法靠根运动移动角色）
                    const float SeqLength = Seq->GetPlayLength();
                    if (SeqLength > 0.05f)
                    {
                        const FTransform RootTravel = Seq->ExtractRootMotionFromRange(0.0, SeqLength, FAnimExtractContext(0.0, true));
                        const float TravelDist = RootTravel.GetTranslation().Size2D();
                        if (TravelDist < 1.0f)
                        {
                            UE_LOG(LogTemp, Warning, TEXT("[SegmentRootMotion] %s Segment %d '%s': NO root travel (%.1fcm) - movement is baked into pose bones, root motion cannot carry this asset."),
                                ArrayName, SegIdx, *Seq->GetName(), TravelDist);
                        }
                        else
                        {
                            UE_LOG(LogTemp, Warning, TEXT("[SegmentRootMotion] %s Segment %d '%s': root travel %.1fcm - character will move with animation."),
                                ArrayName, SegIdx, *Seq->GetName(), TravelDist);
                        }
                    }
                }
            }
        }
    };

    PatchSegmentArray(SkillSegments, TEXT("Skill"));
    PatchSegmentArray(UltimateSegments, TEXT("Ultimate"));
    // 能量技（Q）段蒙太奇同款补丁——此前未覆盖：根运动未启用时根骨骼位移烤进姿势，
    // 播放期间模型视觉位移、蒙太奇结束回跳原点（本次 Q 技能位置 bug 的根因）
    PatchSegmentArray(TArray<FComboMontageSegment>{ EnergySkillSegment }, TEXT("Energy"));
    UE_LOG(LogTemp, Warning, TEXT("[SegmentRootMotion] Runtime root motion enabled on %d sequence(s)."), PatchedCount);
}

// ---- 普攻根运动补丁（修复连击段间位置回跳）----
void ABattleCharacter::EnsureAttackRootMotion()
{
    if (!bEnableAttackRootMotion)
    {
        return;
    }

    // 动画实例必须允许处理蒙太奇根运动（防御性纠正，与段根运动补丁一致）
    if (UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
    {
        if (AnimInst->RootMotionMode == ERootMotionMode::IgnoreRootMotion ||
            AnimInst->RootMotionMode == ERootMotionMode::NoRootMotionExtraction)
        {
            AnimInst->RootMotionMode = ERootMotionMode::RootMotionFromMontagesOnly;
            UE_LOG(LogTemp, Warning, TEXT("[AttackRootMotion] AnimInstance was ignoring root motion, switched to RootMotionFromMontagesOnly."));
        }
    }

    if (bAttackRootMotionPatched && AttackRootMotionPatchedComboCount == ComboAttackSegments.Num())
    {
        return;
    }
    bAttackRootMotionPatched = true;
    AttackRootMotionPatchedComboCount = ComboAttackSegments.Num();

    int32 PatchedCount = 0;
    auto PatchMontage = [&PatchedCount](UAnimMontage* Montage, const TCHAR* Name)
    {
        if (!Montage)
        {
            return;
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
                    UE_LOG(LogTemp, Warning, TEXT("[AttackRootMotion] '%s' (%s): root travel %.1fcm%s"),
                        Name, *Seq->GetName(), TravelDist,
                        TravelDist < 1.0f ? TEXT(" - NO root travel, movement baked into pose bones!") : TEXT(" - OK."));
                }
            }
        }
    };

    // 遍历段列表逐段补丁 —— 段数可配，不能再用 4 个硬编码调用。
    // 名字里带上段号，日志才能对上"第几段是哪条动画"。
    for (int32 Step = 0; Step < ComboAttackSegments.Num(); ++Step)
    {
        UAnimMontage* SegmentMontage = ComboAttackSegments[Step].Montage;
        if (!SegmentMontage)
        {
            continue;
        }
        PatchMontage(SegmentMontage, *FString::Printf(TEXT("第 %d 段"), Step + 1));
    }
    UE_LOG(LogTemp, Warning,
        TEXT("[AttackRootMotion] 普攻段根运动补丁：本次覆盖 %d 段 / 新增开启 %d 个序列。"),
        ComboAttackSegments.Num(), PatchedCount);
}

// ---- 闪避/完美闪避根运动补丁（修复闪避动画段末位置回跳）----
void ABattleCharacter::EnsureDodgeRootMotion()
{
    if (!bEnableDodgeRootMotion)
    {
        return;
    }

    // 动画实例必须允许处理蒙太奇根运动（防御性纠正，与普攻/技能段根运动补丁一致）
    if (UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
    {
        if (AnimInst->RootMotionMode == ERootMotionMode::IgnoreRootMotion ||
            AnimInst->RootMotionMode == ERootMotionMode::NoRootMotionExtraction)
        {
            AnimInst->RootMotionMode = ERootMotionMode::RootMotionFromMontagesOnly;
            UE_LOG(LogTemp, Warning, TEXT("[DodgeRootMotion] AnimInstance was ignoring root motion, switched to RootMotionFromMontagesOnly."));
        }
    }

    if (bDodgeRootMotionPatched)
    {
        return;
    }
    bDodgeRootMotionPatched = true;

    int32 PatchedCount = 0;
    auto PatchDodgeMontage = [&PatchedCount](UAnimMontage* Montage, const TCHAR* Name)
    {
        if (!Montage)
        {
            return;
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

                // 诊断：确认根骨骼确实带位移数据（位移烤进 pelvis 等姿势骨骼的动画无法靠根运动移动角色）
                const float SeqLength = Seq->GetPlayLength();
                if (SeqLength > 0.05f)
                {
                    const FTransform RootTravel = Seq->ExtractRootMotionFromRange(0.0, SeqLength, FAnimExtractContext(0.0, true));
                    const float TravelDist = RootTravel.GetTranslation().Size2D();
                    UE_LOG(LogTemp, Warning, TEXT("[DodgeRootMotion] '%s' (%s): root travel %.1fcm%s"),
                        Name, *Seq->GetName(), TravelDist,
                        TravelDist < 1.0f ? TEXT(" - NO root travel, movement baked into pose bones!") : TEXT(" - OK."));
                }
            }
        }
    };

    PatchDodgeMontage(PerfectDodgeMontage, TEXT("PerfectDodgeMontage"));
    PatchDodgeMontage(DodgeMontage, TEXT("DodgeMontage"));
    PatchDodgeMontage(DashMontage, TEXT("DashMontage"));
    UE_LOG(LogTemp, Warning, TEXT("[DodgeRootMotion] Runtime root motion enabled on %d sequence(s)."), PatchedCount);
}

// ---- 是否有普攻蒙太奇正在播放（ComboAttackSegments 里任一段 / 下落攻击）----
bool ABattleCharacter::IsAttackMontagePlaying() const
{
    // 普攻段：复用 GetActiveComboMontage()（它内部就是遍历 ComboAttackSegments，段数可配）
    if (GetActiveComboMontage())
    {
        return true;
    }

    // 下落攻击蒙太奇属于空中体系，不在普攻段列表里，单独判
    if (UAnimInstance* AnimInst = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
    {
        return FallAttackMontage != nullptr && AnimInst->Montage_IsPlaying(FallAttackMontage);
    }
    return false;
}

// ---- 清除根运动旋转残留（修复技能/大招结束后模型歪斜）----
void ABattleCharacter::ResetMontageRotation()
{
    // Actor 旋转只保留 Yaw：清除根运动残留的 Pitch/Roll（前倾/侧倾）
    const float Yaw = GetActorRotation().Yaw;
    SetActorRotation(FRotator(0.0f, Yaw, 0.0f));

    // 防御：恢复 Mesh 基准相对变换（个别路径下根运动会把相对旋转烘焙到 Mesh 上）
    if (USkeletalMeshComponent* MeshComp = GetMesh())
    {
        MeshComp->SetRelativeLocationAndRotation(BaseMeshRelativeLocation, BaseMeshRelativeRotation);
    }
}

// ---- 技能系统（E 键：多段蒙太奇按顺序播放一次）----
void ABattleCharacter::Skill()
{
    if (bIsSkillCasting || bIsAttacking || bIsUltimateCasting || bIsHitReaction || bIsEnergySkillCasting)
    {
        UE_LOG(LogTemp, Warning, TEXT("Skill: Cannot use skill right now!"));
        return;
    }
    if (CurrentSkillCooldown > 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Skill: Cooldown remaining %f"), CurrentSkillCooldown);
        return;
    }

    // 至少配置一段有效蒙太奇才可施放
    bool bHasValidSegment = false;
    for (const FComboMontageSegment& Segment : SkillSegments)
    {
        if (Segment.Montage)
        {
            bHasValidSegment = true;
            break;
        }
    }
    if (!bHasValidSegment)
    {
        UE_LOG(LogTemp, Warning, TEXT("Skill: No valid montage segment configured in BP_PlayerCharacter!"));
        return;
    }

    // ---- 血量燃烧：技能实际施放成功 → 扣除最大生命 15%（保底 1 点）并累积 1 格能量 ----
    SpendHealthForEnergy();

    // ---- 护盾：技能实际施放成功 → 获得最大血量 8% 的护盾（无法叠加，每次施放刷新为满值）----
    ApplyShield();

    // 段根运动补丁（每局一次）+ 动画实例根运动模式纠正
    EnsureSegmentRootMotion();

    bIsSkillCasting = true;

    // Cooldown starts at cast time (not at end)
    CurrentSkillCooldown = SkillCooldown;

    // 技能期间武器显形（施放全程由 Tick 持续刷新）
    if (CurrentWeapon)
    {
        CurrentWeapon->ShowForCombatAction(0.5f);
    }

    // 出招索敌：起手按技能索敌半径瞬间转向最近的怪物（施放期间由 UpdateSkillAim 持续跟踪）
    AimAttackAtNearestMonster();

    // 播放第一段（跳过未配置蒙太奇的空段；冷却已在施放瞬间启动）
    int32 FirstIndex = 0;
    while (SkillSegments.IsValidIndex(FirstIndex) && !SkillSegments[FirstIndex].Montage)
        ++FirstIndex;
    PlaySkillSegment(FirstIndex);
}

// ---- 大招系统（R 键：多段蒙太奇按顺序播放一次，全程不可被打断）----
void ABattleCharacter::Ultimate()
{
    if (bIsUltimateCasting || bIsAttacking || bIsSkillCasting || bIsHitReaction || bIsEnergySkillCasting)
    {
        UE_LOG(LogTemp, Warning, TEXT("Ultimate: Cannot use ultimate right now!"));
        return;
    }
    if (CurrentUltimateCooldown > 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Ultimate: Cooldown remaining %f"), CurrentUltimateCooldown);
        return;
    }

    bool bHasValidSegment = false;
    for (const FComboMontageSegment& Segment : UltimateSegments)
    {
        if (Segment.Montage)
        {
            bHasValidSegment = true;
            break;
        }
    }
    if (!bHasValidSegment)
    {
        UE_LOG(LogTemp, Warning, TEXT("Ultimate: No valid montage segment configured in BP_PlayerCharacter!"));
        return;
    }

    // 段根运动补丁（每局一次）+ 动画实例根运动模式纠正
    EnsureSegmentRootMotion();

    bIsUltimateCasting = true;

    // Cooldown starts at cast time (not at end)
    CurrentUltimateCooldown = UltimateCooldown;

    // 大招镜头效果：画面放大 + 锁定视角旋转（EndUltimate 恢复）
    ApplyUltimateCameraEffect();

    // ---- 大招停滞怪物：施放瞬间冻结所有存活怪物（不打断攻击，EndUltimate 恢复）----
    // CustomTimeDilation=0 让怪物动画/移动/AI 暂停、攻击蒙太奇暂停而非停止，
    // 营造"大招时间停止"演出；怪物在大招结束（EndUltimate）时统一恢复
    if (bUltimateFreezeMonsters && GetWorld())
    {
        TArray<AActor*> Monsters;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);
        for (AActor* Actor : Monsters)
        {
            if (AMonsterBase* Monster = Cast<AMonsterBase>(Actor))
            {
                Monster->ApplyUltimateFreeze();
            }
        }
        UE_LOG(LogTemp, Warning, TEXT("Ultimate: froze %d monsters (time-stop effect)."), Monsters.Num());
    }

    // 大招期间武器显形（施放全程由 Tick 持续刷新）
    if (CurrentWeapon)
    {
        CurrentWeapon->ShowForCombatAction(0.5f);
    }

    AimAttackAtNearestMonster();

    // ---- 大招增伤状态：施放瞬间激活（持续 UltimateBuffDuration 秒自身伤害提升，不可叠加）----
    ApplyUltimateDamageBuff();

    int32 FirstIndex = 0;
    while (UltimateSegments.IsValidIndex(FirstIndex) && !UltimateSegments[FirstIndex].Montage)
        ++FirstIndex;
    PlayUltimateSegment(FirstIndex);
}

// ---- 技能段播放 ----
void ABattleCharacter::PlaySkillSegment(int32 Index)
{
    if (!SkillSegments.IsValidIndex(Index) || !SkillSegments[Index].Montage || !GetMesh())
    {
        EndSkill();
        return;
    }

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (!AnimInstance)
    {
        EndSkill();
        return;
    }

    const FComboMontageSegment& Segment = SkillSegments[Index];
    CurrentSkillSegmentIndex = Index;
    CurrentSkillMontage = Segment.Montage;
    SkillSegmentHitTimes.Reset();
    SkillMontageSilentTime = 0.0f;

    // 先 Play 再绑结束回调（蒙太奇实例已存在，绑定才生效）
    AnimInstance->Montage_Play(Segment.Montage, Segment.PlayRate);
    FOnMontageEnded EndDelegate;
    EndDelegate.BindUObject(this, &ABattleCharacter::OnSkillSegmentMontageEnded);
    AnimInstance->Montage_SetEndDelegate(EndDelegate, Segment.Montage);

    UE_LOG(LogTemp, Warning, TEXT("Skill segment %d/%d playing (Multiplier=%.1f, DodgeInterruptible=%d)."),
        Index + 1, SkillSegments.Num(), Segment.Multiplier, Segment.bDodgeInterruptible ? 1 : 0);
}

// ---- 大招段播放 ----
void ABattleCharacter::PlayUltimateSegment(int32 Index)
{
    if (!UltimateSegments.IsValidIndex(Index) || !UltimateSegments[Index].Montage || !GetMesh())
    {
        EndUltimate();
        return;
    }

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (!AnimInstance)
    {
        EndUltimate();
        return;
    }

    const FComboMontageSegment& Segment = UltimateSegments[Index];
    CurrentUltimateSegmentIndex = Index;
    CurrentUltimateMontage = Segment.Montage;
    UltimateSegmentHitTimes.Reset();
    UltimateMontageSilentTime = 0.0f;

    AnimInstance->Montage_Play(Segment.Montage, Segment.PlayRate);
    FOnMontageEnded EndDelegate;
    EndDelegate.BindUObject(this, &ABattleCharacter::OnUltimateSegmentMontageEnded);
    AnimInstance->Montage_SetEndDelegate(EndDelegate, Segment.Montage);

    UE_LOG(LogTemp, Warning, TEXT("Ultimate segment %d/%d playing (Multiplier=%.1f)."),
        Index + 1, UltimateSegments.Num(), Segment.Multiplier);
}

// ---- 技能段蒙太奇结束回调 ----
void ABattleCharacter::OnSkillSegmentMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
    // 技能已结束（闪避打断主动结束/迟到回调）→ 忽略
    if (!bIsSkillCasting || Montage != CurrentSkillMontage)
        return;

    // 被打断（蒙太奇被受击等其他动画抢占）→ 结束技能并进入冷却
    if (bInterrupted)
    {
        UE_LOG(LogTemp, Warning, TEXT("Skill interrupted by montage takeover. Entering cooldown."));
        EndSkill();
        return;
    }

    // 正常播完 → 推进下一段（跳过未配置蒙太奇的空段）
    int32 NextIndex = CurrentSkillSegmentIndex + 1;
    while (SkillSegments.IsValidIndex(NextIndex) && !SkillSegments[NextIndex].Montage)
        ++NextIndex;

    if (SkillSegments.IsValidIndex(NextIndex))
    {
        PlaySkillSegment(NextIndex);
    }
    else
    {
        // 全部段播完 → 结束技能并进入冷却
        EndSkill();
    }
}

// ---- 大招段蒙太奇结束回调 ----
void ABattleCharacter::OnUltimateSegmentMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
    if (!bIsUltimateCasting || Montage != CurrentUltimateMontage)
        return;

    if (bInterrupted)
    {
        // 大招理论上不可被打断；此分支仅为兜底（异常抢占时也要正确结束并进入冷却）
        UE_LOG(LogTemp, Warning, TEXT("Ultimate montage unexpectedly interrupted. Entering cooldown."));
        EndUltimate();
        return;
    }

    int32 NextIndex = CurrentUltimateSegmentIndex + 1;
    while (UltimateSegments.IsValidIndex(NextIndex) && !UltimateSegments[NextIndex].Montage)
        ++NextIndex;

    if (UltimateSegments.IsValidIndex(NextIndex))
    {
        PlayUltimateSegment(NextIndex);
    }
    else
    {
        EndUltimate();
    }
}

// ---- 结束技能：停止蒙太奇、解锁操作、进入冷却 ----
void ABattleCharacter::EndSkill()
{
    if (!bIsSkillCasting)
        return;

    // 停止当前段蒙太奇（正常结束时蒙太奇已播完，此处自然跳过）
    if (CurrentSkillMontage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            if (AnimInstance->Montage_IsPlaying(CurrentSkillMontage))
            {
                AnimInstance->Montage_Stop(0.12f, CurrentSkillMontage);
            }
        }
    }

    bIsSkillCasting = false;
    CurrentSkillSegmentIndex = -1;
    CurrentSkillMontage = nullptr;
    SkillSegmentHitTimes.Reset();
    SkillMontageSilentTime = 0.0f;

    // Cooldown already started at cast time in Skill() - do NOT restart here
    // CurrentSkillCooldown = SkillCooldown;

    // 清除根运动残留旋转（模型歪斜）：立即清一次
    ResetMontageRotation();
    // 蒙太奇混合输出期间（0.12s）根运动旋转仍以递减权重继续应用 → 混合完成后再补一次
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(RootRotationResetTimer);
        World->GetTimerManager().SetTimer(RootRotationResetTimer,
            FTimerDelegate::CreateUObject(this, &ABattleCharacter::ResetMontageRotation), 0.16f, false);
    }

    UE_LOG(LogTemp, Warning, TEXT("Skill ended (cooldown already running since cast)."));

    // ---- 切人（技能保留形态）：旧角色放完技能 → 清理屏幕 UI + 销毁武器 + 自毁 ----
    // EndSkill 是技能结束的唯一收敛点（段播完 / 闪避打断 / 蒙太奇回调丢失强制结束都走这里），
    // 在这里挂自毁才能保证「旧角色保留技能释放，直到释放完后消失」必然兑现。
    if (bPendingDestroyAfterSkillFinish)
    {
        bPendingDestroyAfterSkillFinish = false;
        UE_LOG(LogTemp, Log, TEXT("[Switch] 旧角色技能已放完 → 退役销毁（技能保留切人完成）。"));
        ReleaseWeaponClaimForRetire();
        CleanupRetiredScreenUI();
        UnequipWeapon(); // 销毁手中的武器 Actor，防止武器残影留在场上
        RemoveSwitchTeamHUD();
        Destroy();
    }
}

// ---- 结束大招：停止蒙太奇、解锁操作、进入冷却 ----
void ABattleCharacter::EndUltimate()
{
    if (!bIsUltimateCasting)
        return;

    if (CurrentUltimateMontage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            if (AnimInstance->Montage_IsPlaying(CurrentUltimateMontage))
            {
                AnimInstance->Montage_Stop(0.12f, CurrentUltimateMontage);
            }
        }
    }

    bIsUltimateCasting = false;
    CurrentUltimateSegmentIndex = -1;
    CurrentUltimateMontage = nullptr;
    UltimateSegmentHitTimes.Reset();
    UltimateMontageSilentTime = 0.0f;

    // 恢复大招镜头效果：还原原始 FOV、解锁视角旋转
    RestoreUltimateCameraEffect();

    // ---- 解除大招停滞：恢复所有怪物时间流速（大招结束，怪物从暂停处继续动作）----
    if (bUltimateFreezeMonsters && GetWorld())
    {
        TArray<AActor*> Monsters;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);
        for (AActor* Actor : Monsters)
        {
            if (AMonsterBase* Monster = Cast<AMonsterBase>(Actor))
            {
                Monster->RestoreFromUltimateFreeze();
            }
        }
        UE_LOG(LogTemp, Warning, TEXT("Ultimate ended: restored %d monsters."), Monsters.Num());
    }

    // Cooldown already started at cast time in Ultimate() - do NOT restart here
    // CurrentUltimateCooldown = UltimateCooldown;

    // 清除根运动残留旋转（模型歪斜）：立即清一次
    ResetMontageRotation();
    // 蒙太奇混合输出期间（0.12s）根运动旋转仍以递减权重继续应用 → 混合完成后再补一次
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(RootRotationResetTimer);
        World->GetTimerManager().SetTimer(RootRotationResetTimer,
            FTimerDelegate::CreateUObject(this, &ABattleCharacter::ResetMontageRotation), 0.16f, false);
    }

    UE_LOG(LogTemp, Warning, TEXT("Ultimate ended (cooldown already running since cast)."));
}

// ---- 当前技能段是否可被闪避打断 ----
bool ABattleCharacter::IsCurrentSkillSegmentDodgeInterruptible() const
{
    return bIsSkillCasting
        && SkillSegments.IsValidIndex(CurrentSkillSegmentIndex)
        && SkillSegments[CurrentSkillSegmentIndex].bDodgeInterruptible;
}

// ---- Tick：技能段攻击窗口检测 ----
void ABattleCharacter::UpdateSkillSegmentCombat(float DeltaTime)
{
    if (!bIsSkillCasting)
        return;

    if (!SkillSegments.IsValidIndex(CurrentSkillSegmentIndex) || !CurrentSkillMontage || !GetMesh())
        return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();

    // 兜底：蒙太奇已停且迟迟未推进（结束回调丢失）→ 直接结束技能，防止操作永久锁死
    if (!AnimInstance || !AnimInstance->Montage_IsPlaying(CurrentSkillMontage))
    {
        SkillMontageSilentTime += DeltaTime;
        if (SkillMontageSilentTime > 0.3f)
        {
            UE_LOG(LogTemp, Warning, TEXT("Skill montage end delegate lost, force ending skill."));
            EndSkill();
        }
        return;
    }
    SkillMontageSilentTime = 0.0f;

    const FComboMontageSegment& Segment = SkillSegments[CurrentSkillSegmentIndex];

    // 有效攻击时间段（-1 回落：起始=0，结束=蒙太奇全长）
    const float WindowStart = (Segment.AttackWindowStartTime >= 0.0f) ? Segment.AttackWindowStartTime : 0.0f;
    const float WindowEnd = (Segment.AttackWindowEndTime >= 0.0f) ? Segment.AttackWindowEndTime : CurrentSkillMontage->GetPlayLength();

    const float MontagePos = AnimInstance->Montage_GetPosition(CurrentSkillMontage);
    if (MontagePos >= WindowStart && MontagePos <= WindowEnd)
    {
        ApplySegmentContactDamage(Segment, SkillSegmentHitTimes);
    }
}

// ---- Tick：大招段攻击窗口检测 ----
void ABattleCharacter::UpdateUltimateSegmentCombat(float DeltaTime)
{
    if (!bIsUltimateCasting)
        return;

    if (!UltimateSegments.IsValidIndex(CurrentUltimateSegmentIndex) || !CurrentUltimateMontage || !GetMesh())
        return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();

    if (!AnimInstance || !AnimInstance->Montage_IsPlaying(CurrentUltimateMontage))
    {
        UltimateMontageSilentTime += DeltaTime;
        if (UltimateMontageSilentTime > 0.3f)
        {
            UE_LOG(LogTemp, Warning, TEXT("Ultimate montage end delegate lost, force ending ultimate."));
            EndUltimate();
        }
        return;
    }
    UltimateMontageSilentTime = 0.0f;

    const FComboMontageSegment& Segment = UltimateSegments[CurrentUltimateSegmentIndex];

    const float WindowStart = (Segment.AttackWindowStartTime >= 0.0f) ? Segment.AttackWindowStartTime : 0.0f;
    const float WindowEnd = (Segment.AttackWindowEndTime >= 0.0f) ? Segment.AttackWindowEndTime : CurrentUltimateMontage->GetPlayLength();

    const float MontagePos = AnimInstance->Montage_GetPosition(CurrentUltimateMontage);
    if (MontagePos >= WindowStart && MontagePos <= WindowEnd)
    {
        // 镜头振动（可配置幅度/时长/频率）：大招段攻击窗口内实际命中怪物时触发。
        // 一次扫掠同时命中多只怪只振一次（不叠加）；同一怪受 HitInterval 冷却控制。
        // 技能/能量技段的同款命中结算不触发振动
        if (ApplySegmentContactDamage(Segment, UltimateSegmentHitTimes) > 0)
        {
            TriggerHitCameraShake();
        }
    }
}

// ---- 能量技（Q 键）：4 格能量满 + 不在冷却 + 增伤未激活时释放 ----
void ABattleCharacter::EnergySkill()
{
    // 解锁条件：能量满格 + 不在冷却（增伤持续期间不可重复释放，能量在 buff 结束时清空）
    if (EnergyStacks < MaxEnergyStacks)
    {
        UE_LOG(LogTemp, Warning, TEXT("EnergySkill: Not enough energy (%d/%d)."), EnergyStacks, MaxEnergyStacks);
        return;
    }
    if (CurrentEnergySkillCooldown > 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("EnergySkill: Cooldown remaining %.1fs."), CurrentEnergySkillCooldown);
        return;
    }
    if (bEnergyBuffActive)
    {
        UE_LOG(LogTemp, Warning, TEXT("EnergySkill: Buff already active, wait until it ends."));
        return;
    }
    if (bIsEnergySkillCasting || bIsSkillCasting || bIsAttacking || bIsUltimateCasting || bIsHitReaction)
    {
        UE_LOG(LogTemp, Warning, TEXT("EnergySkill: Cannot use right now!"));
        return;
    }
    if (!EnergySkillSegment.Montage || !GetMesh())
    {
        UE_LOG(LogTemp, Warning, TEXT("EnergySkill: No montage configured in EnergySkillSegment (set it in BP_PlayerCharacter)!"));
        return;
    }

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (!AnimInstance)
    {
        return;
    }

    bIsEnergySkillCasting = true;
    CurrentEnergySkillCooldown = EnergySkillCooldown;
    CurrentEnergyMontage = EnergySkillSegment.Montage;
    EnergySegmentHitTimes.Reset();
    EnergyMontageSilentTime = 0.0f;

    // 能量技期间武器显形（施放全程由 Tick 持续刷新）
    if (CurrentWeapon)
    {
        CurrentWeapon->ShowForCombatAction(0.5f);
    }

    // 出招索敌：起手朝最近的怪物
    AimAttackAtNearestMonster();

    // 【根运动修复】与技能/大招同款补丁：纠正 AnimInstance 根运动模式 +
    // 启用 Q 蒙太奇序列的根运动提取（否则位移烤进姿势，结束回跳原点）
    EnsureSegmentRootMotion();

    // 先 Play 再绑结束回调（蒙太奇实例已存在，绑定才生效）
    AnimInstance->Montage_Play(EnergySkillSegment.Montage, EnergySkillSegment.PlayRate);
    FOnMontageEnded EndDelegate;
    EndDelegate.BindUObject(this, &ABattleCharacter::OnEnergySkillMontageEnded);
    AnimInstance->Montage_SetEndDelegate(EndDelegate, EnergySkillSegment.Montage);

    // ---- 增伤 buff：自释放瞬间起持续 EnergySkillBuffDuration 秒，结束后清空全部能量 ----
    bEnergyBuffActive = true;
    if (GetWorld())
    {
        GetWorld()->GetTimerManager().ClearTimer(EnergyBuffTimerHandle);
        GetWorld()->GetTimerManager().SetTimer(
            EnergyBuffTimerHandle, this, &ABattleCharacter::OnEnergyBuffEnded, EnergySkillBuffDuration, false);
    }

    // 吸血状态：记录获得时间戳 + 刷新状态图标（吸血状态与能量技增伤同生命周期）
    LifestealApplyTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    RefreshStatusIcons();

    UE_LOG(LogTemp, Warning, TEXT("EnergySkill released! Damage +%.0f%% + Lifesteal %.0f%% for %.1fs (energy clears after buff), cooldown %.1fs."),
        EnergySkillDamageBoost * 100.0f, EnergyBuffLifestealRatio * 100.0f, EnergySkillBuffDuration, EnergySkillCooldown);
}

// ---- 能量技蒙太奇结束回调（正常播完/被打断均结束施放状态）----
void ABattleCharacter::OnEnergySkillMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
    // 能量技已结束（迟到回调）→ 忽略
    if (!bIsEnergySkillCasting || Montage != CurrentEnergyMontage)
        return;

    if (bInterrupted)
    {
        UE_LOG(LogTemp, Warning, TEXT("EnergySkill interrupted by montage takeover."));
    }
    EndEnergySkill();
}

// ---- 结束能量技施放状态 ----
void ABattleCharacter::EndEnergySkill()
{
    bIsEnergySkillCasting = false;
    CurrentEnergyMontage = nullptr;
    EnergyMontageSilentTime = 0.0f;
}

// ---- Tick：能量技攻击窗口检测（与技能/大招段同款接触伤害结算）----
void ABattleCharacter::UpdateEnergySkillCombat(float DeltaTime)
{
    if (!bIsEnergySkillCasting)
        return;

    if (!CurrentEnergyMontage || !GetMesh())
        return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();

    // 兜底：蒙太奇已停且迟迟未推进（结束回调丢失）→ 直接结束，防止操作永久锁死
    if (!AnimInstance || !AnimInstance->Montage_IsPlaying(CurrentEnergyMontage))
    {
        EnergyMontageSilentTime += DeltaTime;
        if (EnergyMontageSilentTime > 0.3f)
        {
            UE_LOG(LogTemp, Warning, TEXT("EnergySkill montage end delegate lost, force ending."));
            EndEnergySkill();
        }
        return;
    }
    EnergyMontageSilentTime = 0.0f;

    // 有效攻击时间段（-1 回落：起始=0，结束=蒙太奇全长）
    const float WindowStart = (EnergySkillSegment.AttackWindowStartTime >= 0.0f) ? EnergySkillSegment.AttackWindowStartTime : 0.0f;
    const float WindowEnd = (EnergySkillSegment.AttackWindowEndTime >= 0.0f) ? EnergySkillSegment.AttackWindowEndTime : CurrentEnergyMontage->GetPlayLength();

    const float MontagePos = AnimInstance->Montage_GetPosition(CurrentEnergyMontage);
    if (MontagePos >= WindowStart && MontagePos <= WindowEnd)
    {
        ApplySegmentContactDamage(EnergySkillSegment, EnergySegmentHitTimes);
    }
}

// ---- 技能实际施放成功时扣血并累积能量 ----
void ABattleCharacter::SpendHealthForEnergy()
{
    // 扣除最大生命的 EnergyHPCostPercent；血量不足时保底扣至 1 点（放技能不会致死）。
    // 直接修改血量：不触发受伤 UI / 受击硬直（主动消耗而非受击）
    const float HPCost = MaxHealth * EnergyHPCostPercent;
    CurrentHealth = FMath::Max(1.0f, CurrentHealth - HPCost);
    // GAS 化：同步 AttributeSet（唯一真源）
    if (AttributeSet)
    {
        AttributeSet->SetHealth(CurrentHealth);
    }
    UpdateHealthBar();

    UE_LOG(LogTemp, Warning, TEXT("Skill HP cost: %.1f ( %.1f%% of max %.1f ), Health: %.1f / %.1f"),
        HPCost, EnergyHPCostPercent * 100.0f, MaxHealth, CurrentHealth, MaxHealth);

    GainEnergyStack();
}

// ---- 累积 1 格能量并刷新进度条（已满则忽略）----
void ABattleCharacter::GainEnergyStack()
{
    if (EnergyStacks >= MaxEnergyStacks)
        return;

    ++EnergyStacks;
    UpdateEnergyBars();

    if (EnergyStacks >= MaxEnergyStacks)
    {
        UE_LOG(LogTemp, Warning, TEXT("Energy FULL (%d/%d): EnergySkill unlocked, press Q!"), EnergyStacks, MaxEnergyStacks);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Energy gained (%d/%d)."), EnergyStacks, MaxEnergyStacks);
    }
}

// ---- 增伤结束：关闭 buff、清空全部能量并刷新进度条 ----
void ABattleCharacter::OnEnergyBuffEnded()
{
    bEnergyBuffActive = false;
    EnergyStacks = 0;
    LastSyncedEnergyStacks = -1;   // 强制下一帧 Tick 重新同步进度条（清空显示）
    UpdateEnergyBars();
    RefreshStatusIcons(); // 清除吸血状态图标
    UE_LOG(LogTemp, Warning, TEXT("Energy buff ended, all energy cleared (bars reset to %d/%d)."),
        EnergyStacks, MaxEnergyStacks);
}

// ---- 大招增伤状态：激活（无法重复叠加，GAS 化：应用增伤 GE Grant Buff.DamageBoost Tag）----
void ABattleCharacter::ApplyUltimateDamageBuff()
{
    // 已激活：无法重复叠加，直接忽略（不重置计时、不叠加层数）
    if (IsUltimateDamageBuffActive())
    {
        UE_LOG(LogTemp, Warning, TEXT("ApplyUltimateDamageBuff ignored: buff already active (no stacking)."));
        return;
    }

    // ---- GAS 化：应用增伤 GameplayEffect（HasDuration），到期自动移除 Tag ----
    if (AbilitySystemComponent && DamageBoostEffectClass)
    {
        FGameplayEffectContextHandle EffectContext = AbilitySystemComponent->MakeEffectContext();
        FGameplayEffectSpecHandle SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(
            DamageBoostEffectClass, /*Level=*/1.0f, EffectContext);

        if (SpecHandle.IsValid())
        {
            // 时长、增伤幅度通过 SetByCaller 传入
            SpecHandle.Data->SetSetByCallerMagnitude(
                WutheringWavesTags::Buff_DamageBoost_Duration_Tag(), FMath::Max(0.1f, UltimateBuffDuration));
            SpecHandle.Data->SetSetByCallerMagnitude(
                WutheringWavesTags::Buff_DamageBoost_Multiplier_Tag(), UltimateDamageBoost);

            AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
        }
    }

    bUltimateBuffActive = true;

    // 记录获得时间戳（用于「按获得时间顺序」分配状态图标槽位）
    UltimateBuffApplyTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

    // 启动到期定时器（到期关闭增伤 + 清除状态图标；与 GE Duration 对齐做镜像清理兜底）
    if (GetWorld())
    {
        GetWorld()->GetTimerManager().ClearTimer(UltimateBuffTimerHandle);
        GetWorld()->GetTimerManager().SetTimer(
            UltimateBuffTimerHandle, this, &ABattleCharacter::OnUltimateBuffEnded, UltimateBuffDuration, false);
    }

    // 刷新状态图标（按获得时间顺序分配槽位）
    RefreshStatusIcons();

    UE_LOG(LogTemp, Warning, TEXT("Ultimate damage buff active! Damage +%.0f%% for %.1fs."),
        UltimateDamageBoost * 100.0f, UltimateBuffDuration);
}

// ---- 大招增伤到期回调（镜像清理；GE 到期自动移除 Tag）----
void ABattleCharacter::OnUltimateBuffEnded()
{
    bUltimateBuffActive = false;
    RefreshStatusIcons();
    UE_LOG(LogTemp, Warning, TEXT("Ultimate damage buff ended."));
}

// ---- 是否处于大招增伤状态（GAS 化：查 Buff.DamageBoost Tag，回退镜像字段）----
bool ABattleCharacter::IsUltimateDamageBuffActive() const
{
    if (AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(WutheringWavesTags::Buff_DamageBoost_Tag()))
    {
        return true;
    }
    return bUltimateBuffActive;
}

// ---- 能量进度条刷新（WBP_HealthBar 的 Passive_eng_1~4：有能量的格子=1，否则=0）----
// Tick 每帧调用：格数未变化时一次整型比较直接返回（零开销）；
// 宿主 widget 重建（如关卡重载）时自动重建控件缓存——任何路径清空/增加能量，UI 必然同步
void ABattleCharacter::UpdateEnergyBars()
{
    // 帧驱动短路：能量格数与上次同步值相同 → 不做任何控件操作
    if (LastSyncedEnergyStacks == EnergyStacks)
    {
        return;
    }

    UUserWidget* Widget = HealthBarWidget ? HealthBarWidget->GetWidget() : nullptr;
    if (!Widget)
    {
        return;
    }

    // 缓存未建 / 宿主 widget 重建 → 按名重新查找 Passive_eng_1~4
    if (EnergyBarCache.Num() != MaxEnergyStacks || EnergyBarHostWidget.Get() != Widget)
    {
        EnergyBarCache.Reset();
        for (int32 i = 1; i <= MaxEnergyStacks; ++i)
        {
            const FString BarName = FString::Printf(TEXT("Passive_eng_%d"), i);
            EnergyBarCache.Add(Cast<UProgressBar>(Widget->GetWidgetFromName(*BarName)));
        }
        EnergyBarHostWidget = Widget;
    }

    for (int32 i = 1; i <= MaxEnergyStacks; ++i)
    {
        if (UProgressBar* Bar = EnergyBarCache[i - 1].Get())
        {
            Bar->SetPercent(EnergyStacks >= i ? 1.0f : 0.0f);
        }
    }

    LastSyncedEnergyStacks = EnergyStacks;
    UE_LOG(LogTemp, Warning, TEXT("Energy bars synced: %d/%d."), EnergyStacks, MaxEnergyStacks);
}

// ---- 玩家造成伤害的统一乘数 ----
// 叠加所有独立增伤 buff（乘法叠加）：能量技增伤 ×(1+EnergySkillDamageBoost)，
// 大招增伤 ×(1+UltimateDamageBoost)。两者可同时生效时相乘。
// 大招增伤 GAS 化：查 Buff.DamageBoost Tag（由增伤 GE Grant），增伤幅度仍用 UltimateDamageBoost 字段。
float ABattleCharacter::GetOutgoingDamageMultiplier() const
{
    float Multiplier = 1.0f;
    if (bEnergyBuffActive)
    {
        Multiplier *= (1.0f + EnergySkillDamageBoost);
    }
    if (IsUltimateDamageBuffActive())
    {
        Multiplier *= (1.0f + UltimateDamageBoost);
    }
    return Multiplier;
}

// ---- 当前攻击力：技能倍率的伤害基数 ----
// 读 GAS AttributeSet->Attack（唯一真源），未初始化/无 AttributeSet 时回落 BaseAttack。
// 伤害结算公式：最终伤害 = GetAttackPower() × 技能倍率(Multiplier) × GetOutgoingDamageMultiplier()
float ABattleCharacter::GetAttackPower() const
{
    if (AttributeSet)
    {
        return AttributeSet->GetAttack();
    }
    return BaseAttack;
}

// ---- 当前武器攻击力加成（装备加成，无武器=0）----
float ABattleCharacter::GetWeaponAttackPower() const
{
    if (CurrentWeapon)
    {
        return CurrentWeapon->GetAttackPower();
    }
    return 0.0f;
}

// ---- 装备总攻击力加成（装备系统未开发，暂固定 0）----
float ABattleCharacter::GetEquipmentAttackPower() const
{
    // 装备系统（护符/饰品等）尚未开发，暂返回 0。
    // 后续装备系统落地后，在此累加所有已装备物品的攻击力加成。
    return 0.0f;
}

// ---- 总攻击值 = 基础 + 武器 + 装备（面板显示与伤害结算的统一基数）----
float ABattleCharacter::GetTotalAttackPower() const
{
    return GetAttackPower() + GetWeaponAttackPower() + GetEquipmentAttackPower();
}

// ---- 当前暴击率（整数百分比）：读 GAS AttributeSet->CritRate，未初始化回落 BaseCritRate ----
float ABattleCharacter::GetCritRate() const
{
    if (AttributeSet)
    {
        return AttributeSet->GetCritRate();
    }
    return BaseCritRate;
}

// ---- 当前暴击伤害（整数百分比）：读 GAS AttributeSet->CritDamage，未初始化回落 BaseCritDamage ----
float ABattleCharacter::GetCritDamage() const
{
    if (AttributeSet)
    {
        return AttributeSet->GetCritDamage();
    }
    return BaseCritDamage;
}

// ---- 统一伤害结算 ----
// 最终伤害 = 总攻击力 × 倍率 × [暴击伤害(仅暴击)] × (1+伤害提升) × 等级系数 × 0.9
// 详见 BattleCharacter.h 中 ComputeFinalDamage 的注释
float ABattleCharacter::ComputeFinalDamage(float Multiplier, AMonsterBase* TargetMonster) const
{
    // 旧签名包装：暴击结果丢弃（保持既有调用点行为不变）
    bool bUnusedCrit = false;
    return ComputeFinalDamage(Multiplier, TargetMonster, bUnusedCrit);
}

float ABattleCharacter::ComputeFinalDamage(float Multiplier, AMonsterBase* TargetMonster, bool& bOutWasCrit) const
{
    bOutWasCrit = false;

    // 总攻击力（基础 + 武器 + 装备）× 倍率 × (1+伤害提升)
    float Damage = GetTotalAttackPower() * Multiplier * GetOutgoingDamageMultiplier();

    // 暴击判定：按暴击率概率触发，触发时才计入暴击伤害（整数百分比 ÷100）
    const float CritRate = GetCritRate();
    if (CritRate > 0.0f && FMath::FRand() < CritRate / 100.0f)
    {
        bOutWasCrit = true;
        Damage *= GetCritDamage() / 100.0f;
    }

    // 等级系数：怪物为 nullptr 时回落怪物等级 = 1
    const int32 MonLevel = TargetMonster ? TargetMonster->GetMonsterLevel() : 1;
    const float LevelFactor = (100.0f + CharacterLevel) / (199.0f + CharacterLevel + MonLevel);

    // × 等级系数 × 0.9 固定系数
    Damage *= LevelFactor * 0.9f;

    return FMath::Max(0.0f, Damage);
}

// ---- 护盾状态：获得护盾（E 技能施放时调用，无法叠加，每次刷新为满值）----
void ABattleCharacter::ApplyShield()
{
    // 护盾量 = 最大血量 × ShieldMaxHPRatio（默认 8%）。每次施放 E 刷新为满值，不叠加。
    const float MaxShield = MaxHealth * ShieldMaxHPRatio;

    // GAS 化：写 AttributeSet 的 Shield（唯一真源），再同步镜像字段
    if (AttributeSet)
    {
        AttributeSet->SetShield(MaxShield);
        CurrentShield = AttributeSet->GetShield();
    }
    else
    {
        CurrentShield = MaxShield;
    }

    // 记录获得时间戳（用于「按获得时间顺序」分配状态图标槽位）
    ShieldApplyTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

    // 刷新护盾进度条 + 状态图标（护盾状态同中毒规则：按时间顺序占槽位）
    UpdateShieldBar();
    RefreshStatusIcons();

    UE_LOG(LogTemp, Warning, TEXT("Shield applied: %.1f (%.0f%% of max %.1f HP), icon slot %d."),
        CurrentShield, ShieldMaxHPRatio * 100.0f, MaxHealth, ShieldSlotIndex);
}

// ---- 护盾吸收伤害：受击时优先扣护盾，超出部分返回给调用方继续扣血 ----
float ABattleCharacter::AbsorbDamageWithShield(float DamageAmount)
{
    if (DamageAmount <= 0.0f || CurrentShield <= 0.0f)
    {
        // 无护盾/无伤害：全额落到血量上
        return DamageAmount;
    }

    // 护盾吸收量 = min(护盾剩余, 伤害)
    const float Absorbed = FMath::Min(CurrentShield, DamageAmount);
    CurrentShield -= Absorbed;
    // GAS 化：同步 AttributeSet（唯一真源）
    if (AttributeSet)
    {
        AttributeSet->SetShield(CurrentShield);
    }

    // 超出护盾部分继续扣血
    const float Overflow = DamageAmount - Absorbed;

    // 刷新护盾进度条
    UpdateShieldBar();

    // 护盾扣完 → 状态图标消失
    if (CurrentShield <= 0.0f)
    {
        ShieldSlotIndex = -1;
        RefreshStatusIcons();
        UE_LOG(LogTemp, Warning, TEXT("Shield broken! (absorbed %.1f, overflow %.1f to health)."), Absorbed, Overflow);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Shield absorbed %.1f damage, remaining shield %.1f."), Absorbed, CurrentShield);
    }

    return Overflow;
}

// ---- 刷新护盾进度条（WBP_HealthBar 的 shield_bar）----
// 显示：当前护盾量 / 「100% 血量值」的护盾量。即分母固定为最大血量 MaxHealth：
// 护盾条满值 = 100% 血量（MaxHealth），护盾量上限为 ShieldMaxHPRatio * MaxHealth（默认 8%）。
// 因此护盾条实际最多填到 8%（默认），视觉上表示「护盾相对于总血量的占比」。
void ABattleCharacter::UpdateShieldBar()
{
    if (!HealthBarWidget)
        return;

    UUserWidget* Widget = HealthBarWidget->GetWidget();
    if (!Widget)
        return;

    // 分母 = 100% 血量值（最大血量），表示护盾量占总血量的比例
    const float ShieldPercent = MaxHealth > 0.0f ? CurrentShield / MaxHealth : 0.0f;

    // 更新进度条（WBP_HealthBar 里需有名为 shield_bar 的进度条）
    if (UProgressBar* ShieldBar = Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("shield_bar"))))
    {
        ShieldBar->SetPercent(ShieldPercent);
    }
}

// ---- 增伤窗口吸血：按实际造成伤害的 EnergyBuffLifestealRatio 回血 ----
// 直接修改 CurrentHealth：主动回复不触发受伤 UI / 受击硬直，仅刷新血条
void ABattleCharacter::ApplyEnergyBuffLifesteal(float DamageDealt)
{
    if (!bEnergyBuffActive || EnergyBuffLifestealRatio <= 0.0f || DamageDealt <= 0.0f)
        return;

    const float HealAmount = DamageDealt * EnergyBuffLifestealRatio;
    CurrentHealth = FMath::Min(MaxHealth, CurrentHealth + HealAmount);
    // GAS 化：同步 AttributeSet（唯一真源）
    if (AttributeSet)
    {
        AttributeSet->SetHealth(CurrentHealth);
    }
    UpdateHealthBar();

    UE_LOG(LogTemp, Warning, TEXT("Energy buff lifesteal: +%.1f HP (%.0f%% of %.1f dealt), Health: %.1f / %.1f"),
        HealAmount, EnergyBuffLifestealRatio * 100.0f, DamageDealt, CurrentHealth, MaxHealth);
}

// ---- 能量技当前是否可释放（4 格能量 + 不在冷却 + 增伤未激活）----
bool ABattleCharacter::IsEnergySkillUnlocked() const
{
    return EnergyStacks >= MaxEnergyStacks && CurrentEnergySkillCooldown <= 0.0f && !bEnergyBuffActive;
}

// ---- 段内接触伤害结算（技能/大招共用）----
// 攻击窗口开启期间检测角色胶囊与怪物模型包围盒是否接触：
// 接触时按段的 Multiplier（技能倍率）结算伤害，同一怪物按 HitInterval 控制重复结算节奏
//（HitInterval<=0 表示该段内每个怪物只结算一次）。
// 注：此处不触发弹刀——弹刀会播放角色弹刀蒙太奇抢占技能/大招段蒙太奇，导致技能/大招被打断
// 技能/大招/能量技段通用：攻击窗口内与怪物接触 → 按 HitInterval 冷却结算伤害。
// 返回本次实际命中（成功结算伤害）的怪物数；技能/能量技调用点忽略返回值，
// 大招调用点据此决定是否触发镜头振动
int32 ABattleCharacter::ApplySegmentContactDamage(const FComboMontageSegment& Segment, TMap<TWeakObjectPtr<AMonsterBase>, float>& HitTimes)
{
    int32 HitCount = 0;
    if (!GetWorld() || Segment.Multiplier <= 0.0f)
        return HitCount;

    // 角色接触体：胶囊中心（近似身体位置），接触半径 = 胶囊半径 + 10cm 容差
    const FVector PlayerLoc = GetActorLocation();
    const float ContactRadius = (GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleRadius() : 35.0f) + 10.0f;
    const float Now = GetWorld()->GetTimeSeconds();

    TArray<AActor*> Monsters;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

    for (AActor* Actor : Monsters)
    {
        AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster || Monster->IsDead())
            continue;

        // 怪物模型包围盒（与相机防贴脸检测一致的判定方式）
        const FBox BodyBox = Monster->GetMesh()
            ? Monster->GetMesh()->Bounds.GetBox()
            : FBox(Monster->GetActorLocation() - FVector(50.0f), Monster->GetActorLocation() + FVector(50.0f));

        const FVector ClosestPoint = BodyBox.GetClosestPointTo(PlayerLoc);

        // 与怪物模型接触
        if (FVector::Dist(ClosestPoint, PlayerLoc) <= ContactRadius)
        {
            const TWeakObjectPtr<AMonsterBase> MonsterKey(Monster);
            const float* LastHitTime = HitTimes.Find(MonsterKey);
            const bool bCanHit = (Segment.HitInterval <= 0.0f)
                ? (LastHitTime == nullptr)
                : (LastHitTime == nullptr || Now - *LastHitTime >= Segment.HitInterval);

            if (bCanHit)
            {
                const FVector Direction = (Monster->GetActorLocation() - PlayerLoc).GetSafeNormal();
                // 最终伤害 = 统一伤害公式（攻击力×倍率×[暴击]×(1+增伤)×等级系数×0.9），返回值为实际结算伤害
                bool bWasCrit = false;
                const float FinalDamage = ComputeFinalDamage(Segment.Multiplier, Monster, bWasCrit);
                // 预设暴击标志：怪物 TakeDamage 按实际扣血量弹出（暴击/普通）伤害飘字
                Monster->SetPendingDamageNumberCrit(bWasCrit);
                const float ActualDamage = UGameplayStatics::ApplyPointDamage(Monster, FinalDamage, Direction, FHitResult(), GetController(), this, nullptr);
                // 增伤窗口内按实际伤害吸血回血
                ApplyEnergyBuffLifesteal(ActualDamage);
                HitTimes.Add(MonsterKey, Now);
                ++HitCount;
            }
        }
    }

    return HitCount;
}

bool ABattleCharacter::CanAttack() const
{
    // 被技能/大招/受击/能量技占用时不可普攻
    if (bIsSkillCasting || bIsUltimateCasting || bIsHitReaction || bIsEnergySkillCasting)
    {
        return false;
    }

    // 下落攻击进行中（空中体系，落地结算）→ 不可再发起普攻
    if (bIsFallAttacking)
    {
        return false;
    }

    // 硬性最小间隔（AttackCooldown）未结束 → 不可普攻
    if (CurrentAttackCooldown > 0.0f)
    {
        return false;
    }

    // ---- 普攻段间打断窗口（衔接手感）----
    // 若当前有普攻蒙太奇在播放，且尚未到达该段的可打断窗口 → 禁止打断衔接
    // （表现为：连按时需等当前段播到窗口时间点才切入下一段，避免动画硬切）
    // 无普攻蒙太奇播放（全新起手 / 已播完）→ 允许普攻
    if (GetActiveComboMontage() && !IsComboCancelWindowReached())
    {
        return false;
    }

    return true;
}

// ---- 协奏能量 ----
void ABattleCharacter::AddConcertoEnergy(float Amount)
{
    ConcertoEnergy = FMath::Clamp(ConcertoEnergy + Amount, 0.0f, MaxConcertoEnergy);

    if (IsConcertoFull())
    {
        UE_LOG(LogTemp, Warning, TEXT("Concerto Energy FULL!"));
    }
}

bool ABattleCharacter::IsConcertoFull() const
{
    return ConcertoEnergy >= MaxConcertoEnergy;
}

// ---- 闪避辅助 ----
void ABattleCharacter::PerformDodge()
{
    Dodge();
}

void ABattleCharacter::OnLimitDodgeSuccess_Implementation()
{
    UE_LOG(LogTemp, Warning, TEXT("Limit Dodge Success!"));
    AddConcertoEnergy(10.0f);
}

// ---- 韧性系统 ----
void ABattleCharacter::ApplyPoiseDamage(float Amount)
{
    UE_LOG(LogTemp, Warning, TEXT("Poise Damage: %f"), Amount);
}

void ABattleCharacter::OnPoiseBroken_Implementation()
{
    UE_LOG(LogTemp, Warning, TEXT("POISE BROKEN!"));
}

// ---- 闪避无敌状态 ----
void ABattleCharacter::DisableInvincibility()
{
    bIsInvincible = false;
    UE_LOG(LogTemp, Warning, TEXT("Invincibility disabled."));
}

// ---- 无敌帧查询（P3 GAS 化）：查 ASC 的 State.Invincible Tag ----
bool ABattleCharacter::IsInvincibleNow() const
{
    if (AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(WutheringWavesTags::State_Invincible_Tag()))
    {
        return true;
    }
    return bIsInvincible || (bUltimateInvincible && bIsUltimateCasting);
}

// ---- 授予无敌帧（P3 GAS 化）：应用无敌 GE，Grant State.Invincible Tag ----
void ABattleCharacter::GrantInvincibility(float Duration)
{
    if (Duration <= 0.0f)
    {
        return;
    }

    // 配置了无敌 GE → 走 GAS：应用带时长的无敌 GE，到期自动移除 Tag
    if (AbilitySystemComponent && InvincibilityEffectClass)
    {
        FGameplayEffectContextHandle EffectContext = AbilitySystemComponent->MakeEffectContext();
        FGameplayEffectSpecHandle SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(
            InvincibilityEffectClass, 1.0f, EffectContext);

        if (SpecHandle.IsValid())
        {
            // 时长通过 SetByCaller 传入
            SpecHandle.Data->SetSetByCallerMagnitude(
                WutheringWavesTags::State_Invincible_Duration_Tag(), Duration);
            AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
            return;
        }
    }

    // 回退：旧逻辑（无 GE / ASC 时）
    bIsInvincible = true;
    if (GetWorld())
    {
        GetWorld()->GetTimerManager().SetTimer(
            InvincibilityTimer, this, &ABattleCharacter::DisableInvincibility, Duration, false);
    }
}

// ---- 受击反应：随机播放一段受击蒙太奇 ----
void ABattleCharacter::PlayHitReaction()
{
    // 大招无敌：TakeDamage 已在入口完全免疫（bUltimateInvincible），此处防御性兜底——
    // 即便开关关闭仍保留旧霸体行为（不播受击动画，伤害照常结算）
    if (bIsUltimateCasting)
        return;

    // 技能霸体（可配置）：技能播放期间不播受击反应（受击动画会抢占技能段蒙太奇
    // 导致技能被打断），伤害照常结算，只跳过硬直动画与操作锁定
    if (bSkillHyperArmor && bIsSkillCasting)
        return;

    // 能量技霸体（可配置）：Q 技能施放期间不播受击反应（防被打断），伤害照常结算
    if (bEnergySkillHyperArmor && bIsEnergySkillCasting)
        return;

    // 未配置受击蒙太奇 → 跳过
    if (HitReactionMontages.Num() == 0)
        return;

    // 冷却中 → 跳过
    if (HitReactionCooldownRemaining > 0.0f)
        return;

    // 霸体模式（不打断攻击）：攻击进行中不播放受击反应
    if (!bHitReactionInterruptsAttack && bIsAttacking)
        return;

    // 打断模式：取消正在进行的武器攻击判定
    if (bHitReactionInterruptsAttack && bIsAttacking)
    {
        if (CurrentWeapon)
        {
            CurrentWeapon->EndAttack();
        }
        bIsAttacking = false;
    }

    // 随机选一段受击蒙太奇
    const int32 Index = FMath::RandRange(0, HitReactionMontages.Num() - 1);
    UAnimMontage* Montage = HitReactionMontages[Index];
    if (Montage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            // 绑定蒙太奇结束回调，播放完毕前锁定一切操作
            // 记录本次播放的受击蒙太奇（硬直结束时用于精确停止）
            CurrentHitReactionMontage = Montage;

            AnimInstance->Montage_Play(Montage, HitReactionPlayRate);

            // 进入受击硬直：锁定全部操作。
            // 硬直时长由 HitReactionDuration 决定、Tick 计时解除，
            // 不再绑定蒙太奇结束回调（蒙太奇被打断/替换/播放失败时回调不触发，会导致永久锁死）
            bIsHitReaction = true;
            // 普通受击蒙太奇替换击飞/站立蒙太奇：退出击飞与站立阶段（防状态残留）
            bIsKnockback = false;
            bIsGettingUp = false;
            bGetupMontageStarted = false;
            GetupTimeRemaining = 0.0f;
            HitReactionTimeRemaining = HitReactionDuration;

            UE_LOG(LogTemp, Warning, TEXT("Player plays hit reaction #%d (interrupt=%d). Locked all input."),
                Index, bHitReactionInterruptsAttack ? 1 : 0);
        }
    }

    // 进入受击反应冷却
    HitReactionCooldownRemaining = HitReactionCooldown;
}

// ---- 受击蒙太奇结束回调：解除操作锁定 ----
void ABattleCharacter::EndHitReaction()
{
    HitReactionTimeRemaining = 0.0f;
    bIsHitReaction = false;

    // 击飞/站立阶段清理
    bIsKnockback = false;
    bIsGettingUp = false;
    bGetupMontageStarted = false;
    GetupTimeRemaining = 0.0f;

    // 硬直结束时蒙太奇若仍未播完，短暂混合截停（避免受击动画在解锁后继续残留播放）
    if (CurrentHitReactionMontage && GetMesh())
    {
        if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
        {
            if (AnimInstance->Montage_IsPlaying(CurrentHitReactionMontage))
            {
                AnimInstance->Montage_Stop(0.15f, CurrentHitReactionMontage);
            }
        }
    }
    CurrentHitReactionMontage = nullptr;

    UE_LOG(LogTemp, Warning, TEXT("Hit reaction stun ended (duration-based). Input unlocked."));
}

// ---- 击飞反应：怪物"大幅度攻击"命中角色时由 MonsterBase::DealAttackDamageToPlayer 调用 ----
// 播放击飞蒙太奇并锁定操作（复用受击硬直状态机：bIsHitReaction + Tick 计时解除）。
// 普通受击反应已在本函数之前由 TakeDamage 播放，此处直接覆盖为击飞蒙太奇与专用硬直时长
void ABattleCharacter::PlayKnockbackReaction(AActor* Attacker)
{
    // 大招无敌：伤害已被 TakeDamage 完全免疫（返回 0 不会走到这里），防御性兜底
    if (bIsUltimateCasting)
        return;

    // 技能霸体（可配置）：技能期间不被击飞（与普通受击同规则，伤害照常结算）
    if (bSkillHyperArmor && bIsSkillCasting)
        return;

    // 能量技霸体（可配置）：Q 技能施放期间不被击飞（伤害照常结算）
    if (bEnergySkillHyperArmor && bIsEnergySkillCasting)
        return;

    // 未配置击飞蒙太奇 → 保持 TakeDamage 已播的普通受击反应
    if (!KnockbackMontage)
        return;

    UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
    if (!AnimInstance)
        return;

    // ---- 击飞前立即面向攻击者 ----
    // 无论当前朝向如何，先水平旋转至面向怪物（仅 Yaw，保持水平），避免击飞蒙太奇
    // 与后续衔接动作出现朝向怪异的接续。同时写 Controller 与 Actor：
    // 若 bUseControllerRotationYaw 开启，Movement 每帧以控制器 Yaw 驱动 Actor，
    // 只 SetActorRotation 会被下一帧拉回；相机不跟随控制器旋转
    // （bUsePawnControlRotation = false），因此改控制器旋转不会造成镜头跳动
    if (Attacker)
    {
        const FVector ToAttacker = (Attacker->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
        if (!ToAttacker.IsNearlyZero())
        {
            const FRotator FaceRot(0.0f, ToAttacker.Rotation().Yaw, 0.0f);
            if (AController* Ctrl = GetController())
            {
                Ctrl->SetControlRotation(FaceRot);
            }
            SetActorRotation(FaceRot);
        }
    }

    // 打断进行中的武器攻击判定（击飞必然打断攻击）
    if (CurrentWeapon)
    {
        CurrentWeapon->EndAttack();
    }
    bIsAttacking = false;

    // 可选击飞位移（KnockbackLaunchSpeed > 0 时生效）：朝远离攻击者的水平方向 + 向上初速度
    if (KnockbackLaunchSpeed > 0.0f && Attacker)
    {
        const FVector KnockDir = (GetActorLocation() - Attacker->GetActorLocation()).GetSafeNormal2D();
        if (!KnockDir.IsNearlyZero())
        {
            LaunchCharacter(KnockDir * KnockbackLaunchSpeed + FVector(0.0f, 0.0f, KnockbackLaunchZ), true, true);
        }
    }

    // 播放击飞蒙太奇（覆盖 TakeDamage 刚播的普通受击蒙太奇）
    CurrentHitReactionMontage = KnockbackMontage;
    AnimInstance->Montage_Play(KnockbackMontage, KnockbackMontagePlayRate);

    // 击飞僵直：按 bKnockbackLockInput 决定是否锁定全部操作（Tick 计时到时解除）。
    // true = 击飞硬直期间无法进行任何操作；false = 不锁操作，仅播击飞动画。
    // 击飞状态仍以 bIsKnockback 标记驱动计时流转（见 Tick 硬直分支）。
    // 若此前正处于站立阶段（连续被击飞），先退出站立阶段——站立蒙太奇会被击飞蒙太奇覆盖
    bIsGettingUp = false;
    bGetupMontageStarted = false;
    GetupTimeRemaining = 0.0f;

    // 清零移动输入缓存：击飞期间禁止移动键驱动本体，且解锁后不因残留输入突然冲出
    MovementInputValue = FVector2D::ZeroVector;
    bIsMovingInput = false;

    bIsHitReaction = bKnockbackLockInput;
    bIsKnockback = true;
    HitReactionTimeRemaining = KnockbackStunDuration;

    UE_LOG(LogTemp, Warning, TEXT("Player KNOCKED BACK by heavy attack! Stun=%.2fs, Launch=%.0f cm/s."),
        KnockbackStunDuration, KnockbackLaunchSpeed);
}

// ---- 站立反应：击飞硬直结束后自动播放站立蒙太奇（期间保持操作锁定）----
// Tick 驱动：击飞硬直（KnockbackStunDuration）到时 → 本函数 → 站立蒙太奇按
// GetupMontagePlayRate 播完（时长 = 蒙太奇长度 / 速率）→ EndHitReaction 解锁。
// 未配置 GetupMontage 时由调用方直接 EndHitReaction（不进入站立阶段）
void ABattleCharacter::PlayGetupReaction()
{
    if (!GetupMontage)
    {
        EndHitReaction();
        return;
    }

    // 进入站立阶段：bIsHitReaction 按 bGetupLockInput 决定是否保持操作锁定
    // （true = 站立僵直，期间无法进行任何操作；false = 站立阶段不锁操作，可提前打断站立）。
    // 先按 GetupDelay 等待（角色保持躺地僵直），延迟结束后由 Tick 播放站立蒙太奇并重新计时
    bIsGettingUp = true;
    bGetupMontageStarted = false;
    CurrentHitReactionMontage = GetupMontage;
    bIsHitReaction = bGetupLockInput;
    GetupTimeRemaining = FMath::Max(0.0f, GetupDelay);

    // 清零移动输入缓存：站立期间禁止移动键驱动本体（UpdateMovement 无条件拦截），
    // 且站立结束解锁后不因残留输入突然冲出
    MovementInputValue = FVector2D::ZeroVector;
    bIsMovingInput = false;

    UE_LOG(LogTemp, Warning, TEXT("Player GETUP phase after knockback. Delay=%.2fs, LockInput=%d, MontageDuration=%.2fs."),
        GetupDelay, bGetupLockInput ? 1 : 0,
        GetupMontage->GetPlayLength() / FMath::Max(0.05f, GetupMontagePlayRate));
}

// ---- 判断是否移动 ----
bool ABattleCharacter::IsMoving() const
{
    if (!GetCharacterMovement())
        return false;

    FVector Velocity = GetCharacterMovement()->Velocity;
    Velocity.Z = 0.0f;

    return Velocity.SizeSquared() > 100.0f;
}

// ---- 更新摄像机朝向 ----
void ABattleCharacter::UpdateCameraRotation()
{
    if (CameraBoom)
    {
        FRotator TargetRotation = FRotator(CameraWorldPitch, CameraWorldYaw, 0.0f);
        CameraBoom->SetWorldRotation(TargetRotation);
    }
}

// ---- 相机防贴脸推开 ----
// 摄像机（原始位置）与小怪/Boss 距离 < CameraPushbackTriggerDistance 时，沿相机臂方向
// 把摄像机往远处推 CameraPushbackAmount；距离恢复后平滑收回，回到玩家设定的原臂长。
// 迟滞设计：推开期间用"去除推开偏移后的虚拟原位相机"测距，避免推开后距离立刻变大
// 导致推开/收回来回抖动——只有怪物真的远离（原位距离 > 触发距离）才收回
void ABattleCharacter::UpdateCameraPushback(float DeltaTime)
{
    if (!CameraBoom || !FollowCamera)
        return;

    float TargetPushback = 0.0f;

    if (bEnableCameraPushback && GetWorld())
    {
        // 相机臂方向（弹簧臂沿其旋转的 -X 方向伸出摄像机）
        const FVector BoomDir = -FRotationMatrix(CameraBoom->GetComponentRotation()).GetUnitAxis(EAxis::X);

        // 虚拟"原位相机"位置 = 当前相机位置沿臂方向退回推开偏移
        //（即没有推开时相机应在的位置，即玩家设定臂长对应的相机位置）
        const FVector VirtualCamLoc = FollowCamera->GetComponentLocation() - BoomDir * CurrentCameraPushback;

        // 找到与虚拟原位相机最近的存活怪物（用网格体包围盒最近点，Boss 高模型侧面贴近也能判到）
        float MinDistSq = TNumericLimits<float>::Max();
        TArray<AActor*> Monsters;
        UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Monsters);

        for (AActor* Actor : Monsters)
        {
            AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
            if (!Monster || Monster->IsDead())
                continue;

            if (Monster->GetMesh())
            {
                const FBox BodyBox = Monster->GetMesh()->Bounds.GetBox();
                const FVector Closest = BodyBox.GetClosestPointTo(VirtualCamLoc);
                MinDistSq = FMath::Min(MinDistSq, FVector::DistSquared(Closest, VirtualCamLoc));
            }
            else
            {
                MinDistSq = FMath::Min(MinDistSq, FVector::DistSquared(Monster->GetActorLocation(), VirtualCamLoc));
            }
        }

        // 原位相机与最近怪物距离小于触发距离 → 需要推开
        if (MinDistSq < FMath::Square(CameraPushbackTriggerDistance))
        {
            TargetPushback = CameraPushbackAmount;
        }
    }

    // 状态切换日志（只在进出推开状态时打一条，避免刷屏）
    const bool bShouldPush = TargetPushback > 0.0f;
    if (bShouldPush != bCameraPushbackActive)
    {
        bCameraPushbackActive = bShouldPush;
        UE_LOG(LogTemp, Warning, TEXT("Camera pushback %s (amount=%.0f cm)."),
            bShouldPush ? TEXT("ENGAGED") : TEXT("RELEASED"), CameraPushbackAmount);
    }

    // 平滑插值推开偏移（玩家缩放值 + 推开偏移 - 弹刀拉近）
    CurrentCameraPushback = FMath::FInterpTo(CurrentCameraPushback, TargetPushback, DeltaTime, CameraPushbackInterpSpeed);

    // 弹刀镜头拉近：保持期内目标=拉近距离，到期后目标=0（平滑恢复原距离）
    const float TargetParryZoomIn = (ParryCameraZoomTimeRemaining > 0.0f) ? ParryCameraZoomInDistance : 0.0f;
    CurrentParryCameraZoomIn = FMath::FInterpTo(CurrentParryCameraZoomIn, TargetParryZoomIn, DeltaTime, ParryCameraInterpSpeed);

    // 最终臂长（限制最小 60cm，防止拉近过量时相机穿进角色）
    CameraBoom->TargetArmLength = FMath::Max(UserCameraArmLength + CurrentCameraPushback - CurrentParryCameraZoomIn, 60.0f);
}

// ---- 弹刀镜头拉近：弹刀成功瞬间触发 ----
void ABattleCharacter::TriggerParryCameraZoom()
{
    // 距离/时长任一 <= 0 视为禁用
    if (ParryCameraZoomInDistance <= 0.0f || ParryCameraZoomDuration <= 0.0f)
        return;

    ParryCameraZoomTimeRemaining = ParryCameraZoomDuration;

    UE_LOG(LogTemp, Warning, TEXT("Parry camera zoom-in: %.0f cm for %.2fs."),
        ParryCameraZoomInDistance, ParryCameraZoomDuration);
}

// ---- 命中镜头振动：大招段攻击窗口实际命中怪物时触发（UpdateUltimateSegmentCombat 内调用）----
// 弹刀/技能/能量技/普攻命中不再触发振动
void ABattleCharacter::TriggerHitCameraShake()
{
    // 开关关闭 / 幅度或时长不合法 → 无操作
    if (!bEnableHitCameraShake || HitCameraShakeAmplitude <= 0.0f || HitCameraShakeDuration <= 0.0f)
        return;

    // 重新计时：振动中再次命中会重置（连段命中持续刷新振动，打击感连贯）
    // 【修复】Delay=0 时直接进入振动期：旧逻辑两个计时器同时为 0，
    // Tick 的延迟/振动分支都进不去（状态机死区），振动永远不会启动
    if (HitCameraShakeDelay > 0.0f)
    {
        HitShakeDelayRemaining = HitCameraShakeDelay;
        HitShakeTimeRemaining = 0.0f;
    }
    else
    {
        HitShakeDelayRemaining = 0.0f;
        HitShakeTimeRemaining = HitCameraShakeDuration;
    }
    HitShakeElapsed = 0.0f;
}

// ---- 每帧驱动命中振动：延迟倒计时 → 正弦衰减左右振动 → 结束复位相机相对位置 ----
void ABattleCharacter::UpdateHitCameraShake(float DeltaTime)
{
    if (!FollowCamera)
        return;

    FVector ShakeOffset = FVector::ZeroVector;

    if (HitShakeDelayRemaining > 0.0f)
    {
        // 延迟期：倒计时，归零后进入振动期
        HitShakeDelayRemaining -= DeltaTime;
        if (HitShakeDelayRemaining <= 0.0f)
        {
            HitShakeDelayRemaining = 0.0f;
            HitShakeTimeRemaining = HitCameraShakeDuration;
            HitShakeElapsed = 0.0f;
        }
    }
    else if (HitShakeTimeRemaining > 0.0f)
    {
        // 振动期：沿相机右方向（局部 Y）正弦往复，幅度线性衰减至 0
        HitShakeElapsed += DeltaTime;
        HitShakeTimeRemaining -= DeltaTime;
        if (HitShakeTimeRemaining < 0.0f)
        {
            HitShakeTimeRemaining = 0.0f;
        }

        const float Duration = FMath::Max(HitCameraShakeDuration, KINDA_SMALL_NUMBER);
        const float Alpha = FMath::Clamp(HitShakeElapsed / Duration, 0.0f, 1.0f);
        const float Fade = 1.0f - Alpha; // 线性衰减：收尾平滑无突兀归位
        const float Phase = HitShakeElapsed * HitCameraShakeFrequency * 2.0f * PI;
        const float Lateral = FMath::Sin(Phase) * HitCameraShakeAmplitude * Fade;
        ShakeOffset = FVector(0.0f, Lateral, 0.0f);
    }

    // 振动只改相机相对位置（基准为零偏移），不动臂长 —— 与弹刀拉近/防贴脸推开互不干扰
    FollowCamera->SetRelativeLocation(ShakeOffset);
}

// ---- GAS：复制 ASC 与 AttributeSet（为多人/联机预留；单机 Demo 亦无害）----
void ABattleCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(ABattleCharacter, AbilitySystemComponent);
    DOREPLIFETIME(ABattleCharacter, AttributeSet);
}

// ---- 结束时清理屏幕 UI，防止控件残留在视口 ----
void ABattleCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // ★ 运行时创建的 IMC/IA 都 NewObject(this)（Outer=本实例）→ 本实例销毁后这些对象被 GC，
    //   subsystem 里若还挂着它们的映射就是悬空指针 → 污染 EnhancedInput 求值（1/2/3 静默失灵）。
    //   ★ 不能用 GetController() 拿 Subsystem：切人流程先 UnPossess 再 Destroy，EndPlay 时
    //     Controller 已经是 null → 清理永远跳过 → 旧 IMC 残留（2026-09-17「切不回去」实锤根因）。
    //   改从 World 拿本地玩家 —— EndPlay 时 World 仍有效。
    UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
    if (UWorld* World = GetWorld())
    {
        if (ULocalPlayer* LP = World->GetFirstLocalPlayerFromController())
        {
            Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LP);
        }
    }

    // 移除运行时创建的步行 Ctrl 映射（防止角色销毁/重生后本地子系统残留指向已销毁对象的映射）
    if (WalkToggleMappingContext && bWalkContextAdded && Subsystem)
    {
        Subsystem->RemoveMappingContext(WalkToggleMappingContext);
    }

    HideLockOnIndicator();
    HidePerfectDodgeUI();

    // 切人输入映射：角色销毁/重生后移除本实例创建的 IMC（防止子系统残留指向已销毁对象的映射）
    if (SwitchMappingContext && bSwitchContextAdded && Subsystem)
    {
        Subsystem->RemoveMappingContext(SwitchMappingContext);
    }
    else if (SwitchMappingContext && bSwitchContextAdded)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Switch] ★ 影响：%s 销毁时拿不到 EnhancedInput 子系统，1/2/3 映射未能摘除 —— 可能残留悬空映射导致后续切人键失灵。"),
            *GetName());
    }

    // ★ 背包 B 键映射：与切人同源 —— NewObject(this) 的 IMC，实例销毁后不摘就变悬空，
    //   切到非 BP_PlayerCharacter 角色后按 B 打不开背包（2026-09-17 实锤根因）。
    if (BagMappingContext && bBagContextAdded && Subsystem)
    {
        Subsystem->RemoveMappingContext(BagMappingContext);
    }

    // ★ 编队 L 键映射：同上，运行时创建、实例销毁后必须摘除，否则切人后 L 键失灵。
    if (CharaTeamMappingContext && bCharaTeamContextAdded && Subsystem)
    {
        Subsystem->RemoveMappingContext(CharaTeamMappingContext);
    }

    // 右上角编队 HUD 是本实例创建的屏幕控件 → 随实例一起移除（切人后新实例会建自己的）
    RemoveSwitchTeamHUD();

    // ---- ★ 在场角色实例注册表：注销（value 指向自己才移除，防止 A 销毁时误删 B 的登记）----
    {
        const FName MyRow = GetMyCharaRow();
        if (!MyRow.IsNone())
        {
            if (ABattleCharacter* const* Registered = ActiveCharaInstances.Find(MyRow))
            {
                if (*Registered == this)
                {
                    ActiveCharaInstances.Remove(MyRow);
                }
            }
        }
    }

    // GAS 收尾：解除 ASC 与 Actor 的绑定（释放能力上下文与监听）
    if (AbilitySystemComponent)
    {
        AbilitySystemComponent->ClearActorInfo();
    }

    Super::EndPlay(EndPlayReason);
}

// ---- 完美闪避 UI：优先使用自定资产，未配置时纯 C++ 构建屏幕空间控件（屏幕中心偏右）----
void ABattleCharacter::ShowPerfectDodgeText()
{
    // 已有 UI 在播 → 先移除旧的（连续完美闪避时覆盖）
    HidePerfectDodgeUI();

    if (!GetWorld())
        return;

    // ---- 路径一：使用用户自建的 Widget Blueprint 资产 ----
    if (PerfectDodgeUIClass)
    {
        PerfectDodgeUIWidget = CreateWidget<UUserWidget>(GetWorld(), PerfectDodgeUIClass);
        if (PerfectDodgeUIWidget)
        {
            PerfectDodgeUITextBlock = nullptr; // 资产自带视觉内容，不需要代码文字块
            if (bAnimatePerfectDodgeUI)
            {
                PerfectDodgeUIWidget->SetRenderOpacity(0.0f); // 淡入动画起点
            }
            PerfectDodgeUIWidget->AddToViewport(10);
            PerfectDodgeTextElapsed = 0.0f;
            UE_LOG(LogTemp, Warning, TEXT("Perfect dodge UI shown (custom asset: %s)."),
                *PerfectDodgeUIClass->GetName());
            return;
        }
        // 资产创建失败（极少见）→ 落回内置控件
        UE_LOG(LogTemp, Warning, TEXT("Failed to create custom PerfectDodgeUI widget, falling back to built-in."));
    }

    // ---- 路径二：动态创建 UserWidget 并用代码搭建 CanvasPanel + TextBlock（无需资产）----
    PerfectDodgeUIWidget = CreateWidget<UUserWidget>(GetWorld(), UUserWidget::StaticClass());
    if (!PerfectDodgeUIWidget)
        return;

    UCanvasPanel* RootCanvas = PerfectDodgeUIWidget->WidgetTree
        ? PerfectDodgeUIWidget->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"))
        : nullptr;
    if (!RootCanvas)
    {
        PerfectDodgeUIWidget = nullptr;
        return;
    }
    PerfectDodgeUIWidget->WidgetTree->RootWidget = RootCanvas;

    PerfectDodgeUITextBlock = PerfectDodgeUIWidget->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PerfectDodgeText"));
    if (!PerfectDodgeUITextBlock)
    {
        PerfectDodgeUIWidget = nullptr;
        return;
    }

    // 文字"闪避"用 Unicode 转义避免源文件编码问题：闪=\u95EA 避=\u907F
    PerfectDodgeUITextBlock->SetText(FText::FromString(TEXT("\u95EA\u907F")));
    PerfectDodgeUITextBlock->SetColorAndOpacity(PerfectDodgeUIColor);

    // 字体：保留默认字体对象（走 Slate 字体回退链，可显示中文），只改字号
    FSlateFontInfo FontInfo = PerfectDodgeUITextBlock->GetFont();
    FontInfo.Size = FMath::Max(8, PerfectDodgeUIFontSize);
    PerfectDodgeUITextBlock->SetFont(FontInfo);

    // 放入画布：锚点=屏幕中心，位置=中心偏右
    UCanvasPanelSlot* TextSlot = RootCanvas->AddChildToCanvas(PerfectDodgeUITextBlock);
    if (TextSlot)
    {
        TextSlot->SetAnchors(FAnchors(0.5f, 0.5f));
        TextSlot->SetPosition(FVector2D(PerfectDodgeUIOffsetX, PerfectDodgeUIOffsetY));
        TextSlot->SetAlignment(FVector2D(0.5f, 0.5f)); // 以文字中心对齐偏移点
        TextSlot->SetAutoSize(true);
    }

    // 初始透明（淡入动画起点）
    if (bAnimatePerfectDodgeUI)
    {
        PerfectDodgeUIWidget->SetRenderOpacity(0.0f);
    }
    PerfectDodgeUIWidget->AddToViewport(10); // 高层级，不被血条/耐力条遮挡

    PerfectDodgeTextElapsed = 0.0f;

    UE_LOG(LogTemp, Warning, TEXT("Perfect dodge UI shown at screen center-right (built-in widget)."));
}

// ---- 移除完美闪避 UI ----
void ABattleCharacter::HidePerfectDodgeUI()
{
    if (PerfectDodgeUIWidget)
    {
        PerfectDodgeUIWidget->RemoveFromParent();
        PerfectDodgeUIWidget = nullptr;
    }
    PerfectDodgeUITextBlock = nullptr;
    PerfectDodgeTextElapsed = 0.0f;
}

// ---- 完美闪避蓝色残影：开始生成（完美闪避触发时调用）----
void ABattleCharacter::StartPerfectDodgeGhosts()
{
    if (!bPerfectDodgeGhost || !PerfectDodgeGhostMaterial || !GetMesh() || !GetWorld())
        return;

    // 重置计时：先立即生成第一份残影，之后按间隔继续生成直到时长耗尽
    GhostTrailTimeRemaining = PerfectDodgeGhostDuration;
    GhostTrailSpawnAccumulator = 0.0f;
    SpawnDodgeGhost();

    UE_LOG(LogTemp, Warning, TEXT("Perfect dodge ghosts started: %d ghosts over %.2fs."),
        PerfectDodgeGhostCount, PerfectDodgeGhostDuration);
}

// ---- 完美闪避蓝色残影：Tick 按间隔生成 ----
void ABattleCharacter::UpdatePerfectDodgeGhosts(float DeltaTime)
{
    if (GhostTrailTimeRemaining <= 0.0f)
        return;

    GhostTrailTimeRemaining -= DeltaTime;
    if (GhostTrailTimeRemaining <= 0.0f)
    {
        GhostTrailTimeRemaining = 0.0f;
        return;
    }

    // 生成间隔 = 总时长 / 数量（均匀分布的拖尾）
    const int32 GhostCount = FMath::Max(1, PerfectDodgeGhostCount);
    const float SpawnInterval = PerfectDodgeGhostDuration / static_cast<float>(GhostCount);

    GhostTrailSpawnAccumulator += DeltaTime;
    while (GhostTrailSpawnAccumulator >= SpawnInterval && GhostTrailTimeRemaining > 0.0f)
    {
        GhostTrailSpawnAccumulator -= SpawnInterval;
        SpawnDodgeGhost();
    }
}

// ---- 完美闪避蓝色残影：在角色骨骼网格世界位置生成一份（同位同高，定格姿势与位置，寿命 = 残影拖尾剩余时间）----
void ABattleCharacter::SpawnDodgeGhost()
{
    if (!PerfectDodgeGhostMaterial || !GetMesh() || !GetWorld())
    {
        UE_LOG(LogTemp, Warning, TEXT("SpawnDodgeGhost SKIPPED (Material=%s Mesh=%s World=%s)"),
            PerfectDodgeGhostMaterial ? TEXT("OK") : TEXT("NULL"),
            GetMesh() ? TEXT("OK") : TEXT("NULL"),
            GetWorld() ? TEXT("OK") : TEXT("NULL"));
        return;
    }

    // 生成点 = 角色骨骼网格的世界位置（GetComponentLocation，与角色同位同高）。
    // 残影生成后挂接到角色网格（SnapToTarget 贴合），再按蓝图参数叠加偏移：
    // GhostBackOffset 沿移动方向（正=身后，负=身前）+ GhostSideOffset 垂直方向（正=右，负=左）
    FVector MoveDir = GetVelocity().GetSafeNormal2D();
    if (MoveDir.IsNearlyZero())
    {
        MoveDir = GetActorForwardVector();
    }
    const FVector SpawnLocation = GetMesh()->GetComponentLocation();
    const FRotator SpawnRotation = GetMesh()->GetComponentRotation();

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AGhostAfterimage* Ghost = GetWorld()->SpawnActor<AGhostAfterimage>(
        AGhostAfterimage::StaticClass(), SpawnLocation, SpawnRotation, SpawnParams);
    if (Ghost)
    {
        Ghost->InitGhost(GetMesh(), PerfectDodgeGhostMaterial, PerfectDodgeGhostColor,
            FMath::Max(0.1f, GhostTrailTimeRemaining));

        // 前后/左右偏移：沿移动方向反向为"后"（正=身后，负=身前），
        // 垂直方向（Up×MoveDir = 右）为"左/右"（正=右，负=左），世界方向 → 网格本地空间，
        // 随角色朝向正确分解；两者均为 0 时完全贴身
        if (Ghost->GhostMesh && (!FMath::IsNearlyZero(GhostBackOffset) || !FMath::IsNearlyZero(GhostSideOffset)))
        {
            const FTransform& MeshT = GetMesh()->GetComponentTransform();
            const FVector RightVec = FVector::CrossProduct(FVector::UpVector, MoveDir).GetSafeNormal();
            const FVector WorldOffset = -MoveDir * GhostBackOffset + RightVec * GhostSideOffset;
            const FVector LocalOffset = MeshT.InverseTransformVectorNoScale(WorldOffset);
            Ghost->GhostMesh->SetRelativeLocation(LocalOffset);
        }

        UE_LOG(LogTemp, Warning, TEXT("Ghost spawned at %s (back %.1fcm, side %.1fcm, life %.2fs)"),
            *SpawnLocation.ToString(), GhostBackOffset, GhostSideOffset, FMath::Max(0.1f, GhostTrailTimeRemaining));
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("SpawnDodgeGhost FAILED: SpawnActor returned null!"));
    }
}

// ---- 受伤提示 UI：显示（受伤时调用），到时自动隐藏 ----
void ABattleCharacter::ShowHurtUI()
{
    if (!HurtUIWidget || !GetWorld())
        return;

    // 控件实例未创建（蓝图类后配置等场景）→ 重建一次
    if (!HurtUIWidget->GetWidget() && HurtUIWidgetClass)
    {
        HurtUIWidget->SetWidgetClass(nullptr);
        HurtUIWidget->SetWidgetClass(HurtUIWidgetClass);
        HurtUIWidget->InitWidget();
    }

    // 覆盖上一次的隐藏定时（连续受伤时从最后一次受伤起算）
    GetWorld()->GetTimerManager().ClearTimer(HurtUITimerHandle);

    HurtUIWidget->SetVisibility(true, true);
    HurtUIWidget->SetHiddenInGame(false);

    // 显示瞬间立即定位（铺满视口、锁屏幕中心），不依赖下一帧 Tick
    UpdateHurtUIPosition();

    // 显示时长为 0 = 常显（直到下次调用前不隐藏）；否则到时隐藏直到下次受伤
    if (HurtUIDisplayDuration > 0.0f)
    {
        GetWorld()->GetTimerManager().SetTimer(
            HurtUITimerHandle, this, &ABattleCharacter::HideHurtUI, HurtUIDisplayDuration, false);
    }

    UE_LOG(LogTemp, Warning, TEXT("Hurt UI shown for %.3fs (camera-locked, immune to view move/zoom)."), HurtUIDisplayDuration);
}

// ---- 受伤提示 UI：隐藏（下次受伤再显示）----
void ABattleCharacter::HideHurtUI()
{
    if (HurtUIWidget)
    {
        HurtUIWidget->SetHiddenInGame(true);
    }
}

// ---- 受伤提示 UI：定位（与血条同一公式——FOV 解析、屏幕中心、每帧刚性锁定）----
// 血条是"屏幕底部居中"，受伤红屏是"全屏覆盖"：位置锁屏幕中心 + 绘制尺寸跟随视口，
// 相机 FOV/旋转/视口尺寸变化时始终铺满全屏，不会错位/飘走/留边
void ABattleCharacter::UpdateHurtUIPosition()
{
    if (!HurtUIWidget || !FollowCamera)
        return;

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return;

    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // 全屏覆盖：绘制尺寸跟随视口（受伤红屏需铺满屏幕，血条固定小尺寸故无需此步）
    HurtUIWidget->SetDrawSize(FVector2D(static_cast<float>(ViewportX), static_cast<float>(ViewportY)));

    // 屏幕中心 = 摄像机正前方 Dist 处（与血条同一公式；屏幕空间下 Dist 只决定投影位置，不影响绘制大小）。
    // 摄像机局部空间：X 前、Y 右、Z 上；屏幕中心偏移 (0,0) → 局部 X=Dist，Y/Z=0
    const float Dist = 300.0f;
    HurtUIWidget->SetRelativeLocation(FVector(Dist, 0.0f, 0.0f));
}

// ---- 获得物品提示 UI：定位（与受伤提示同一套公式：位置锁屏幕中心 + 绘制尺寸跟随视口）----
// 提示列表显示在屏幕左侧，但整个控件是铺满全屏的 —— 条目的具体位置由控件内部的
// 【锚点】决定，因此换分辨率时提示仍停在「屏幕左侧固定比例位置」，不会飘走或跑出屏幕。
void ABattleCharacter::UpdateItemPickupTipPosition()
{
    if (!ItemPickupTipWidget || !FollowCamera)
        return;

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return;

    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // ---- 全屏铺满：绘制尺寸跟随视口 ----
    // ★ 只在尺寸真的变了时才 SetDrawSize。
    //   为什么：Screen 空间下每次 SetDrawSize 都会让这棵 Slate 控件树按新尺寸重新布局，
    //   而控件内部的锚点（提示列表钉在屏幕左侧）正是按「控件自身尺寸」算出来的。
    //   每帧无条件重设，会让「已经铺满」和「布局还没算完」两种状态交替出现 ——
    //   表现就是提示位置抖动、甚至整条错到屏幕外。
    const FVector2D WantedDrawSize(static_cast<float>(ViewportX), static_cast<float>(ViewportY));
    if (!ItemPickupTipWidget->GetDrawSize().Equals(WantedDrawSize, 0.5f))
    {
        ItemPickupTipWidget->SetDrawSize(WantedDrawSize);
        ItemPickupTipWidget->RequestRenderUpdate();
        UE_LOG(LogTemp, Log, TEXT("[Drop][Layout] 提示画布尺寸已同步为视口 %d × %d"), ViewportX, ViewportY);
    }

    // 屏幕中心（屏幕空间下 Dist 只决定投影位置，不影响绘制大小）
    const float Dist = 300.0f;

    // ★ pivot 必须钉成 (0.5, 0.5)：widget 会以投影点为中心向四周铺开，
    //   铺满尺寸的画布才能正好盖住整个屏幕。
    //   如果 pivot 是别的值（蓝图里被改过、或组件是从别处复制来的），
    //   画布左上角就会跑到屏幕外 —— 现象正是「提示往屏幕外错位、显示不全」。
    ItemPickupTipWidget->SetPivot(FVector2D(0.5f, 0.5f));
    ItemPickupTipWidget->SetRelativeLocation(FVector(Dist, 0.0f, 0.0f));

    // ---- 「布局体检」的触发时机 ----
    // ★★ 这里原来写的是「只打一次」：`if (bItemPickupLayoutDiagnosed) { return; }`
    //    后果（2026-09-14 实测日志）：体检在 PIE 启动后 0.175s 就触发，那一刻列表是空的，
    //    「逐条实测」一整段全是空的 —— 而「提示显示不全 / 位置错位」恰恰只在【有内容】
    //    时才看得出来。12 秒后真的掉落了提示，再没有任何一次体检，等于全程瞎。
    //    现在的时机 = ① 首次绘制后 ② 列表里多出条目时（限流 1s）③ 视口变化后（见 CheckViewportResize）
    UItemPickupTipsWidget* TipsWidget = Cast<UItemPickupTipsWidget>(ItemPickupTipWidget->GetWidget());
    if (!TipsWidget)
    {
        return;
    }

    // 必须等控件真的被绘制过再报：GetCachedGeometry() 在未绘制时恒为 0×0，
    // 拿它当结论会得出「提示尺寸坏了」这种假结论。
    const FVector2D TipsMeasured = TipsWidget->GetCachedGeometry().GetLocalSize();
    if (TipsMeasured.X <= 1.0f || TipsMeasured.Y <= 1.0f)
    {
        return;   // 还没画过，下一帧再看
    }

    const int32 TipCountNow = TipsWidget->GetTipCount();
    const bool bFirstDiagnosis = !bItemPickupLayoutDiagnosed;
    const bool bNewContent = TipCountNow > LastDiagnosedTipCount;

    if (!bFirstDiagnosis && !bNewContent)
    {
        return;
    }

    // 限流：一次掉 6 件会连加 6 条，没必要把同一份布局刷 6 遍。
    // 首次那一次不限流 —— 保证「画布还没铺满就被问」的情况也能留下一条记录。
    if (!bFirstDiagnosis)
    {
        const float NowSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.0f;
        if (LastItemPickupDiagTime >= 0.0f && NowSeconds - LastItemPickupDiagTime < 1.0f)
        {
            return;
        }
    }

    bItemPickupLayoutDiagnosed = true;
    LastDiagnosedTipCount = TipCountNow;
    LastItemPickupDiagTime = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.0f;

    // ---- 组件侧的事实 ----
    // ★ 投影值只能当【参考】，不能当判据。
    //   本项目在耐力条/血条里已经踩过这个坑并写明了原因（见 UpdateStaminaBarPosition 的注释）：
    //   「反投影使用的是上一帧的渲染视角（PlayerCameraManager 缓存），而组件渲染跟随当前帧
    //     摄像机变换：转动视角时两者每帧错位」。所以这里把它标成「仅供参考」，
    //   真正可信的是下面那条【解析式】结论。
    FVector2D Projected(0.0f, 0.0f);
    const bool bProjected = UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(
        PC, ItemPickupTipWidget->GetComponentLocation(), Projected, false);

    // 解析式判据（可信，不依赖任何缓存视角）：
    // 组件挂在相机上、相对位置 (Dist, 0, 0)、pivot (0.5, 0.5)，且 DrawSize == 视口
    // ⇒ 不管相机怎么转，投影点恒为屏幕中心、画布恒等于屏幕 ⇒ 锚点比例算出来的就是屏幕位置。
    // 这三个前提里坏掉任何一个，「提示错位」都会跟着来，所以逐个报出来。
    const bool bDrawSizeMatchesViewport = ItemPickupTipWidget->GetDrawSize().Equals(
        FVector2D(static_cast<float>(ViewportX), static_cast<float>(ViewportY)), 0.5f);
    const FVector2D Pivot = ItemPickupTipWidget->GetPivot();
    const bool bPivotCentered = Pivot.Equals(FVector2D(0.5f, 0.5f), 0.001f);

    const FString DrawSizeDesc = bDrawSizeMatchesViewport
        ? FString(TEXT("=="))
        : FString(TEXT("!= ← 画布和屏幕不等大，锚点比例算出来的位置会整块偏掉"));
    const FString PivotDesc = bPivotCentered
        ? FString(TEXT("(0.50, 0.50)"))
        : FString::Printf(TEXT("(%.2f, %.2f) ← 不是中心，整块会偏半个画布"), Pivot.X, Pivot.Y);
    const TCHAR* CanvasCoversScreen = (bDrawSizeMatchesViewport && bPivotCentered)
        ? TEXT("= 屏幕 ✅")
        : TEXT("≠ 屏幕 ★ 异常：整块画布会偏");

    UE_LOG(LogTemp, Log,
        TEXT("[Drop][Layout] 组件事实：视口=%d×%d | DrawSize=%.0f×%.0f | DPI缩放=%.2f | 列表 %d 条\n"
             "      组件世界位置=%s（相对相机 X=%.0f，pivot=(%.2f, %.2f)）\n"
             "      投影到屏幕=%s（%s）  ← 仅供参考：它用的是上一帧缓存视角，不作为位置判据\n"
             "      ★ 解析式判据（可信）：DrawSize %s 视口、pivot %s ⇒ 画布 %s，投影点恒为屏幕中心 (%.0f, %.0f)"),
        ViewportX, ViewportY,
        ItemPickupTipWidget->GetDrawSize().X, ItemPickupTipWidget->GetDrawSize().Y,
        UWidgetLayoutLibrary::GetViewportScale(PC), TipCountNow,
        *ItemPickupTipWidget->GetComponentLocation().ToCompactString(),
        ItemPickupTipWidget->GetRelativeLocation().X, Pivot.X, Pivot.Y,
        *Projected.ToString(),
        bProjected ? TEXT("成功") : TEXT("失败：点算在相机背后"),
        *DrawSizeDesc, *PivotDesc, CanvasCoversScreen,
        ViewportX * 0.5f, ViewportY * 0.5f);

    // 控件侧的布局事实 + 槽位判据（由控件自己摊开：锚点 / 偏移 / 对齐 / 期望 vs 实测尺寸 / 与基准的偏差 / 容量）
    TipsWidget->LogLayoutDiagnostics(
        bFirstDiagnosis ? TEXT("首次绘制后自动触发")
                        : *FString::Printf(TEXT("列表内容变化后自动触发，当前 %d 条"), TipCountNow));
}

// ---- 技能/大招图标 UI：BeginPlay 创建一次并常驻屏幕显示 ----
void ABattleCharacter::InitSkillIconUI()
{
    // 已创建则不重复创建
    if (SkillIconUIWidget)
        return;

    if (!GetWorld())
        return;

    if (SkillIconUIClass)
    {
        SkillIconUIWidget = CreateWidget<UUserWidget>(GetWorld(), SkillIconUIClass);
        if (SkillIconUIWidget)
        {
            // 层级 5：高于血条/耐力条（WidgetComponent 屏幕空间），低于完美闪避提示（10）
            SkillIconUIWidget->AddToViewport(5);

            // 缓存冷却倒计时文本控件（WBP_Skill 内命名：skill_cd=技能，UIminate_cd=大招，
            // passive_cd=能量技Q）。若蓝图里控件被重命名/类型非 Text Block 会取不到，按控件实际命名调整。
            SkillCDText = SkillIconUIWidget->WidgetTree->FindWidget<UTextBlock>(FName("skill_cd"));
            UltimateCDText = SkillIconUIWidget->WidgetTree->FindWidget<UTextBlock>(FName("UIminate_cd"));
            PassiveCDText = SkillIconUIWidget->WidgetTree->FindWidget<UTextBlock>(FName("passive_cd"));
            // passive_cd 兼容 ProgressBar 类型（做成进度条时按冷却剩余比例填充）
            PassiveCDProgress = SkillIconUIWidget->WidgetTree->FindWidget<UProgressBar>(FName("passive_cd"));
            // sock 遮罩图像：能量未满/冷却未结束/增伤中 → 显示；可释放 Q → 隐藏
            SockWidget = SkillIconUIWidget->WidgetTree->FindWidget<UWidget>(FName("sock"));
            if (!SkillCDText)
            {
                UE_LOG(LogTemp, Warning, TEXT("WBP_Skill: TextBlock 'skill_cd' NOT found (skill cooldown text disabled)."));
            }
            if (!UltimateCDText)
            {
                UE_LOG(LogTemp, Warning, TEXT("WBP_Skill: TextBlock 'UIminate_cd' NOT found (ultimate cooldown text disabled)."));
            }
            if (!PassiveCDText && !PassiveCDProgress)
            {
                UE_LOG(LogTemp, Warning, TEXT("WBP_Skill: 'passive_cd' NOT found as TextBlock/ProgressBar (energy cooldown UI disabled)."));
            }
            if (!SockWidget)
            {
                UE_LOG(LogTemp, Warning, TEXT("WBP_Skill: Widget 'sock' NOT found (energy ready mask disabled)."));
            }

            // 初始状态：未冷却，立即隐藏冷却文本（防止第一帧前闪现）
            UpdateSkillIconCooldownUI();

            UE_LOG(LogTemp, Warning, TEXT("Skill icon UI shown (class: %s)."), *SkillIconUIClass->GetName());
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Failed to create SkillIconUI widget! Check WBP_Skill class."));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SkillIconUIClass not set! Create WBP_Skill in /Content/UI or assign it in BP_PlayerCharacter."));
    }
}

// ---- 每帧更新技能/大招冷却倒计时：冷却中显示剩余秒数，未冷却隐藏文本 ----
void ABattleCharacter::UpdateSkillIconCooldownUI()
{
    // 控件未创建/未缓存到文本时跳过
    if (!SkillIconUIWidget)
        return;

    // 技能冷却：CurrentSkillCooldown > 0 = 冷却中
    if (SkillCDText)
    {
        const float Remaining = FMath::Max(0.0f, CurrentSkillCooldown);
        if (Remaining > 0.0f)
        {
            // 值变化超过 0.05s 才刷新文本（避免每帧重建 FText 的 GC 压力）
            if (FMath::Abs(Remaining - LastShownSkillCD) >= 0.05f)
            {
                SkillCDText->SetText(FText::FromString(FString::Printf(TEXT("%.1f"), Remaining)));
                LastShownSkillCD = Remaining;
            }
            if (SkillCDText->GetVisibility() != ESlateVisibility::Visible)
            {
                SkillCDText->SetVisibility(ESlateVisibility::Visible);
            }
        }
        else
        {
            // 冷却结束/未进入冷却：无条件隐藏（含初始状态）
            if (SkillCDText->GetVisibility() != ESlateVisibility::Hidden)
            {
                SkillCDText->SetVisibility(ESlateVisibility::Hidden);
            }
            LastShownSkillCD = -1.0f;
        }
    }

    // 大招冷却：CurrentUltimateCooldown > 0 = 冷却中
    if (UltimateCDText)
    {
        const float Remaining = FMath::Max(0.0f, CurrentUltimateCooldown);
        if (Remaining > 0.0f)
        {
            if (FMath::Abs(Remaining - LastShownUltimateCD) >= 0.05f)
            {
                UltimateCDText->SetText(FText::FromString(FString::Printf(TEXT("%.1f"), Remaining)));
                LastShownUltimateCD = Remaining;
            }
            if (UltimateCDText->GetVisibility() != ESlateVisibility::Visible)
            {
                UltimateCDText->SetVisibility(ESlateVisibility::Visible);
            }
        }
        else
        {
            if (UltimateCDText->GetVisibility() != ESlateVisibility::Hidden)
            {
                UltimateCDText->SetVisibility(ESlateVisibility::Hidden);
            }
            LastShownUltimateCD = -1.0f;
        }
    }

    // ---- 能量技（Q）冷却：passive_cd 实时显示 + sock 遮罩按可释放状态切换 ----
    // 文本模式：冷却中显示剩余秒数，未冷却隐藏（与 skill_cd/UIminate_cd 同款）
    if (PassiveCDText)
    {
        const float Remaining = FMath::Max(0.0f, CurrentEnergySkillCooldown);
        if (Remaining > 0.0f)
        {
            if (FMath::Abs(Remaining - LastShownPassiveCD) >= 0.05f)
            {
                PassiveCDText->SetText(FText::FromString(FString::Printf(TEXT("%.1f"), Remaining)));
                LastShownPassiveCD = Remaining;
            }
            if (PassiveCDText->GetVisibility() != ESlateVisibility::Visible)
            {
                PassiveCDText->SetVisibility(ESlateVisibility::Visible);
            }
        }
        else
        {
            if (PassiveCDText->GetVisibility() != ESlateVisibility::Hidden)
            {
                PassiveCDText->SetVisibility(ESlateVisibility::Hidden);
            }
            LastShownPassiveCD = -1.0f;
        }
    }
    // 进度条模式：按冷却剩余比例填充（冷却中从满格递减到空）
    else if (PassiveCDProgress)
    {
        const float Ratio = (EnergySkillCooldown > 0.0f)
            ? FMath::Clamp(CurrentEnergySkillCooldown / EnergySkillCooldown, 0.0f, 1.0f)
            : 0.0f;
        PassiveCDProgress->SetPercent(Ratio);
    }

    // sock 遮罩：能量未满 / 冷却未结束 / 增伤进行中 → 显示；
    // 满足释放条件（4 格能量 + 无冷却 + 无增伤）→ 隐藏，直到条件再次不满足
    if (SockWidget)
    {
        const ESlateVisibility TargetVisibility = IsEnergySkillUnlocked()
            ? ESlateVisibility::Hidden
            : ESlateVisibility::Visible;
        if (SockWidget->GetVisibility() != TargetVisibility)
        {
            SockWidget->SetVisibility(TargetVisibility);
        }
    }
}

// ---- 每帧更新 UI：淡入 + 轻微上移 + 淡出 + 超时移除 ----
void ABattleCharacter::UpdatePerfectDodgeText(float DeltaTime)
{
    if (!PerfectDodgeUIWidget)
        return;

    PerfectDodgeTextElapsed += DeltaTime;

    // 总时长 1.0s：0~0.12s 淡入；0.12~0.5s 保持；0.5~1.0s 淡出并轻微上移
    constexpr float TotalDuration = 1.0f;
    constexpr float FadeInTime = 0.12f;
    constexpr float FadeOutStartTime = 0.5f;
    constexpr float RiseSpeed = 40.0f; // px/s 屏幕空间上移速度

    if (bAnimatePerfectDodgeUI)
    {
        float Opacity = 1.0f;
        if (PerfectDodgeTextElapsed < FadeInTime)
        {
            Opacity = PerfectDodgeTextElapsed / FadeInTime;
        }
        else if (PerfectDodgeTextElapsed > FadeOutStartTime)
        {
            Opacity = 1.0f - (PerfectDodgeTextElapsed - FadeOutStartTime) / (TotalDuration - FadeOutStartTime);
        }
        Opacity = FMath::Clamp(Opacity, 0.0f, 1.0f);
        PerfectDodgeUIWidget->SetRenderOpacity(Opacity);

        // 淡出阶段整体轻微上移（渲染偏移，不动布局槽位）
        if (PerfectDodgeTextElapsed > FadeOutStartTime)
        {
            const float RiseProgress = (PerfectDodgeTextElapsed - FadeOutStartTime) / (TotalDuration - FadeOutStartTime);
            PerfectDodgeUIWidget->SetRenderTranslation(FVector2D(0.0f, -RiseSpeed * RiseProgress));
        }
        else
        {
            PerfectDodgeUIWidget->SetRenderTranslation(FVector2D::ZeroVector);
        }
    }

    // 超时移除
    if (PerfectDodgeTextElapsed >= TotalDuration)
    {
        HidePerfectDodgeUI();
    }
}

// ---- 摄像机控制 ----
void ABattleCharacter::AddCameraRotation(float DeltaX, float DeltaY)
{
    // 大招期间锁定视角旋转（视角固定，大招结束后由 EndUltimate 解锁）
    if (bIsUltimateCasting && bUltimateLockCameraRotation)
        return;

    if (FMath::Abs(DeltaX) < 0.01f && FMath::Abs(DeltaY) < 0.01f)
        return;

    CameraWorldYaw += DeltaX * CameraSensitivity;
    CameraWorldPitch += DeltaY * CameraSensitivity;
    CameraWorldPitch = FMath::Clamp(CameraWorldPitch, -80.0f, 80.0f);
}

void ABattleCharacter::ResetCameraView()
{
    CameraWorldYaw = 0.0f;
    CameraWorldPitch = -10.0f;
    UpdateCameraRotation();
    UE_LOG(LogTemp, Warning, TEXT("Camera view reset!"));
}

// ---- 大招镜头效果：画面放大 N 倍 + 锁定视角旋转（EndUltimate 恢复）----
void ABattleCharacter::ApplyUltimateCameraEffect()
{
    if (!FollowCamera)
        return;

    // 首次施放记录原始 FOV：C++ 构造函数读不到蓝图覆盖值，首次施放时读取最准；
    // 只记录一次，避免多次施放读到缩放后的 FOV 造成累积误差
    if (!bUltimateFovRecorded)
    {
        DefaultFieldOfView = FollowCamera->FieldOfView;
        bUltimateFovRecorded = true;
    }

    // 画面放大 N 倍 = 垂直 FOV 按透视关系精确缩小：tan(newFov/2) = tan(oldFov/2) / N
    const float Zoom = FMath::Max(1.0f, UltimateZoomFovMultiplier);
    const float HalfFovRad = FMath::DegreesToRadians(DefaultFieldOfView * 0.5f);
    const float NewFov = FMath::RadiansToDegrees(2.0f * FMath::Atan(FMath::Tan(HalfFovRad) / Zoom));
    FollowCamera->SetFieldOfView(FMath::Clamp(NewFov, 10.0f, DefaultFieldOfView));

    UE_LOG(LogTemp, Warning, TEXT("Ultimate camera zoom ON: FOV %.1f -> %.1f (x%.2f)."), DefaultFieldOfView, NewFov, Zoom);
}

void ABattleCharacter::RestoreUltimateCameraEffect()
{
    if (FollowCamera)
    {
        FollowCamera->SetFieldOfView(DefaultFieldOfView);
        UE_LOG(LogTemp, Warning, TEXT("Ultimate camera restored: FOV %.1f."), DefaultFieldOfView);
    }
}

void ABattleCharacter::ZoomCamera(float Delta)
{
    if (!CameraBoom)
        return;

    // 缩放只改玩家设定臂长；当前推开偏移叠加在其上，弹刀拉近偏移扣除（拉近期间缩放同样生效）
    float NewLength = UserCameraArmLength + Delta * CameraZoomSpeed;
    NewLength = FMath::Clamp(NewLength, CameraMinDistance, CameraMaxDistance);
    UserCameraArmLength = NewLength;
    CameraBoom->TargetArmLength = FMath::Max(NewLength + CurrentCameraPushback - CurrentParryCameraZoomIn, 60.0f);
}

void ABattleCharacter::SetCameraToBackView()
{
    if (!CameraBoom)
        return;

    FRotator ActorRotation = GetActorRotation();
    float BackYaw = ActorRotation.Yaw + 0.0f;

    CameraWorldYaw = BackYaw;
    CameraWorldPitch = -10.0f;

    UpdateCameraRotation();
    UE_LOG(LogTemp, Warning, TEXT("Camera set to back view! Yaw: %f"), CameraWorldYaw);
}

// ---- 移动控制 ----
void ABattleCharacter::SetMovementInput(float ForwardValue, float RightValue)
{
    MovementInputValue.X = RightValue;
    MovementInputValue.Y = ForwardValue;

    bIsMovingInput = !MovementInputValue.IsNearlyZero();
}

void ABattleCharacter::UpdateMovement(float DeltaTime)
{
    // 受击硬直/击飞/站立期间禁止输入驱动移动（保留重力、击飞弹出等物理效果）。
    // bIsKnockback/bIsGettingUp 兜底：即使 bKnockbackLockInput/bGetupLockInput 关闭
    // （bIsHitReaction=false），击飞/站立阶段也不允许按移动键移动本体。
    // 同时消费输入向量：防御蓝图侧 AddMovementInput 直接驱动 CharacterMovement 的路径
    if (bIsHitReaction || bIsKnockback || bIsGettingUp)
    {
        if (GetCharacterMovement())
        {
            GetCharacterMovement()->ConsumeInputVector();
        }
        // 惯性缓存清零：这些状态由外部速度（击飞/受击弹开）驱动，恢复移动时不应带旧速度
        SmoothedMoveVelocity = FVector::ZeroVector;
        return;
    }

    // 技能/大招播放期间锁移动（防蒙太奇滑步；特殊位移由动画根位移表现）
    if (bIsSkillCasting || bIsUltimateCasting)
    {
        SmoothedMoveVelocity = FVector::ZeroVector;
        return;
    }

    // 闪避/下落攻击期间不覆写速度（让特殊位移向量保持，不被移动逻辑覆盖）
    if (bIsDodging || bIsFallAttacking)
    {
        SmoothedMoveVelocity = FVector::ZeroVector;
        return;
    }

    // 直接使用存储的摄像机角度，而不是从 CameraBoom 读取
    FRotator CameraRotation = FRotator(0.0f, CameraWorldYaw, 0.0f);

    FVector ForwardDirection = FRotationMatrix(CameraRotation).GetUnitAxis(EAxis::X);
    FVector RightDirection = FRotationMatrix(CameraRotation).GetUnitAxis(EAxis::Y);

    // W 键：远离摄像机，S 键：靠近摄像机
    FVector MoveDirection = (ForwardDirection * MovementInputValue.Y) + (RightDirection * MovementInputValue.X);

    if (MoveDirection.IsNearlyZero())
    {
        // 无移动输入：普攻播放中且有锁定目标时仍继续转向索敌目标（不触发行走）
        if (!(bAttackAimOnMoveInput && IsAttackMontagePlaying() && GetValidLockedTarget()))
        {
            // ★ 关键：早退前必须先做「停步制动」，否则上一帧设的满速会一直残留 → 角色滑行不停。
            //   把缓存速度按制动力收敛到 0，并同步写回 CharacterMovement。
            if (GetCharacterMovement())
            {
                FVector CurrentVel2D(SmoothedMoveVelocity.X, SmoothedMoveVelocity.Y, 0.0f);
                FVector NewVel2D = FMath::VInterpConstantTo(CurrentVel2D, FVector::ZeroVector, DeltaTime, MoveBrakingDeceleration);

                // 微速度归零（同主路径）：避免残速导致缓慢漂移
                if (NewVel2D.SizeSquared2D() < 25.0f)
                {
                    NewVel2D = FVector::ZeroVector;
                }

                SmoothedMoveVelocity = FVector(NewVel2D.X, NewVel2D.Y, 0.0f);

                FVector NewVelocity = SmoothedMoveVelocity;
                NewVelocity.Z = GetCharacterMovement()->Velocity.Z;
                GetCharacterMovement()->Velocity = NewVelocity;
            }
            else
            {
                SmoothedMoveVelocity = FVector::ZeroVector;
            }
            return;
        }
    }
    else
    {
        MoveDirection.Normalize();
    }

    // ---- 普攻期间位移输入 → 只改变攻击朝向，不触发行走 ----
    // 普攻蒙太奇播放中：清零水平速度（防滑步、行走动画不触发），
    // 角色平滑转向目标方向 —— 后续普攻与根运动突进方向随之改变（= 改变攻击方向）
    if (bAttackAimOnMoveInput && IsAttackMontagePlaying())
    {
        if (GetCharacterMovement())
        {
            FVector V = GetCharacterMovement()->Velocity;
            V.X = 0.0f;
            V.Y = 0.0f;
            GetCharacterMovement()->Velocity = V;
        }
        // ★ 同步清空惯性缓存：普攻期间水平速度已被清零，若不清缓存，
        //   普攻结束后会拿旧速度「弹射」出去（起步惯性复位）。
        SmoothedMoveVelocity = FVector::ZeroVector;

        // 朝向基准：有锁定目标（存活且在范围内）→ 优先朝索敌目标（普攻期间持续贴脸转向），
        // 否则沿用移动输入方向（旧逻辑）
        FVector AimDir = MoveDirection;
        if (AMonsterBase* Lock = GetValidLockedTarget())
        {
            AimDir = (Lock->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
        }
        if (AimDir.IsNearlyZero())
            return;

        FRotator AimTargetRotation = AimDir.Rotation();
        AimTargetRotation.Pitch = 0.0f;
        AimTargetRotation.Roll = 0.0f;
        SetActorRotation(FMath::RInterpTo(GetActorRotation(), AimTargetRotation, DeltaTime, AttackAimInterpSpeed));
        return;
    }

    // ---- 移动速度：惯性插值（对标鸣潮的起步加速 / 停步制动手感）----
    // 旧实现每帧 `Velocity = MoveDirection * MoveSpeed` 硬赋值 → 起步瞬间满速、
    // 停步瞬间归零、变向速度矢量突变（无惯性，手感生硬）。
    // 新实现把「目标速度」按加速度/制动率逐帧逼近，得到「起步有冲劲、停步有短滑行」的手感。
    FVector ActualMoveDir = MoveDirection;
    if (GetCharacterMovement())
    {
        const float MoveSpeed = GetCharacterMovement()->MaxWalkSpeed;
        const FVector DesiredVelocity(MoveDirection.X * MoveSpeed, MoveDirection.Y * MoveSpeed, 0.0f);

        // 有输入 → 用起步加速度逼近目标速度；无输入 → 用制动力把速度收敛到 0
        const float AccelRate = MoveDirection.IsNearlyZero() ? MoveBrakingDeceleration : MoveAcceleration;

        FVector CurrentVel2D(SmoothedMoveVelocity.X, SmoothedMoveVelocity.Y, 0.0f);

        // 速度矢量朝目标速度按【等加速度】限步逼近（线性加速，非指数衰减），
        // 得到「起步有冲劲、停步有短滑行」的手感。
        FVector NewVel2D = FMath::VInterpConstantTo(CurrentVel2D, DesiredVelocity, DeltaTime, AccelRate);

        // ★ 微速度归零：残速 < 5cm/s 时直接清零，避免「缓慢漂移」与朝向抖动
        //   （VInterpConstantTo 是限步逼近，理论上会到位，但浮点残差可能留极小值）。
        if (MoveDirection.IsNearlyZero() && NewVel2D.SizeSquared2D() < 25.0f)
        {
            NewVel2D = FVector::ZeroVector;
        }

        SmoothedMoveVelocity = FVector(NewVel2D.X, NewVel2D.Y, 0.0f);

        FVector NewVelocity = SmoothedMoveVelocity;
        NewVelocity.Z = GetCharacterMovement()->Velocity.Z;
        GetCharacterMovement()->Velocity = NewVelocity;

        // 转身方向以「实际速度方向」为准（无输入减速时保持原朝向，不突然扭头）
        if (!SmoothedMoveVelocity.IsNearlyZero())
        {
            ActualMoveDir = SmoothedMoveVelocity.GetSafeNormal2D();
        }
    }

    if (!ActualMoveDir.IsNearlyZero())
    {
        CurrentMoveDirection = ActualMoveDir;

        FRotator TargetRotation = ActualMoveDir.Rotation();
        TargetRotation.Pitch = 0.0f;
        TargetRotation.Roll = 0.0f;

        // ★ 转向走「最短路径」：FMath::RInterpTo 对 Yaw 插值用的是未归一化的差值，
        //   相机朝 -x（Yaw 跨 ±180°）时会绕远路（170° vs -170° 实差 20° 却绕 340°）。
        //   修法：用 FindDeltaAngleDegrees 求 Yaw 最短差值，按【度/秒】限速旋转
        //   （MoveTurnSpeedDegPerSec），转身干脆且速度可配（对标鸣潮的快速急转手感）。
        FRotator CurrentRotation = GetActorRotation();
        const float MaxTurnThisFrame = MoveTurnSpeedDegPerSec * DeltaTime;

        const float CurrentYaw = FRotator::NormalizeAxis(CurrentRotation.Yaw);
        const float DesiredYaw = FRotator::NormalizeAxis(TargetRotation.Yaw);
        const float DeltaYaw = FMath::FindDeltaAngleDegrees(CurrentYaw, DesiredYaw);

        // 限速旋转：本帧最多转 MaxTurnThisFrame 度（不足则直接到位）
        const float AppliedYaw = FMath::Clamp(DeltaYaw, -MaxTurnThisFrame, MaxTurnThisFrame);

        FRotator NewRotation = CurrentRotation;
        NewRotation.Yaw = FRotator::NormalizeAxis(CurrentYaw + AppliedYaw);
        NewRotation.Pitch = 0.0f;
        NewRotation.Roll = 0.0f;
        SetActorRotation(NewRotation);
    }
    else if (SmoothedMoveVelocity.IsNearlyZero())
    {
        // 完全静止：清零缓存速度，防止残留微速度导致「缓慢漂移」
        SmoothedMoveVelocity = FVector::ZeroVector;
    }
}

// ---- 疾跑系统 ----
void ABattleCharacter::StartSprint()
{
    // 步行中禁止疾跑：Ctrl 步行锁定速度优先，疾跑不生效（需再次按 Ctrl 退出步行）
    if (bWalking)
    {
        UE_LOG(LogTemp, Warning, TEXT("Sprint: ignored while walking (press Ctrl again to exit walk mode)."));
        return;
    }

    // 条件：未在疾跑、在地面、正在输入移动方向、还有耐力、未在受击硬直/击飞/站立中
    if (bIsSprinting || bIsDodging || bIsHitReaction || bIsKnockback || bIsGettingUp || bIsSkillCasting || bIsUltimateCasting)
        return;

    if (!GetCharacterMovement() || !GetCharacterMovement()->IsMovingOnGround())
        return;

    if (!bIsMovingInput || CurrentStamina <= 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Sprint: need to be moving with stamina!"));
        return;
    }

    bIsSprinting = true;
    SprintElapsedTime = 0.0f;
    GetCharacterMovement()->MaxWalkSpeed = BaseWalkSpeed * SprintSpeedMultiplier;

    UE_LOG(LogTemp, Warning, TEXT("Sprint ON! Speed: %.0f, Duration: %.1fs"), GetCharacterMovement()->MaxWalkSpeed, SprintDuration);
}

void ABattleCharacter::StopSprint()
{
    if (!bIsSprinting)
        return;

    bIsSprinting = false;

    // 恢复原速度
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->MaxWalkSpeed = BaseWalkSpeed;
    }

    UE_LOG(LogTemp, Warning, TEXT("Sprint OFF. Speed restored to %.0f"), BaseWalkSpeed);
}

// ---- 步行系统（Ctrl 切换）----
void ABattleCharacter::ToggleWalkState()
{
    bWalking = !bWalking;

    if (!GetCharacterMovement())
        return;

    if (bWalking)
    {
        // 进入步行：若正疾跑先停止（步行锁定优先于疾跑）
        if (bIsSprinting)
        {
            StopSprint();
        }

        // 步行速度完全等于蓝图 WalkSpeed（自由调整，不再按基础移速比例折算）
        GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;

        // 步行动画播放速率倍率：作用于整个角色动画的全局播放速率（含行走/跑步混合空间动画）
        if (GetMesh())
        {
            GetMesh()->GlobalAnimRateScale = FMath::Max(0.01f, WalkAnimPlayRate);
        }

        UE_LOG(LogTemp, Warning, TEXT("Walk ON! Speed locked to %.0f cm/s, AnimRate x%.2f"), WalkSpeed, WalkAnimPlayRate);
    }
    else
    {
        // 退出步行：恢复基础移速（与疾跑结束恢复一致）+ 恢复动画全局播放速率
        GetCharacterMovement()->MaxWalkSpeed = BaseWalkSpeed;

        if (GetMesh())
        {
            GetMesh()->GlobalAnimRateScale = 1.0f;
        }

        UE_LOG(LogTemp, Warning, TEXT("Walk OFF. Speed restored to %.0f"), BaseWalkSpeed);
    }
}

// 惰性创建 Ctrl 步行输入：运行时自建 UInputAction + UInputMappingContext 并挂到本地子系统，
// 无需在编辑器创建 IA/IMC 资产或改动 IMC_Player。左/右 Ctrl 均映射，按下沿触发一次。
bool ABattleCharacter::EnsureWalkToggleInput()
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return false;

    UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
    if (!Subsystem)
        return false;

    if (!WalkToggleAction)
    {
        WalkToggleAction = NewObject<UInputAction>(this);
        WalkToggleAction->ValueType = EInputActionValueType::Boolean;
        // 显式“按下”触发器：保证 Triggered 仅在按键按下沿触发一次（按住不重复触发 → 不会来回切换）
        WalkToggleAction->Triggers.Add(NewObject<UInputTriggerPressed>(WalkToggleAction));
    }

    if (!WalkToggleMappingContext)
    {
        WalkToggleMappingContext = NewObject<UInputMappingContext>(this);
        WalkToggleMappingContext->MapKey(WalkToggleAction, EKeys::LeftControl);
        WalkToggleMappingContext->MapKey(WalkToggleAction, EKeys::RightControl);
    }

    if (!bWalkContextAdded)
    {
        // 优先级 1（高于蓝图默认 0）：Ctrl 未被其他动作占用，避免冲突
        Subsystem->AddMappingContext(WalkToggleMappingContext, 1);
        bWalkContextAdded = true;
    }

    return true;
}

// ---- 实际可达最大跳跃高度（按物理推导）----
// JumpZVelocity（=JumpHeight/DoubleJumpHeight，cm/s）只赋给跳跃初速度，
// 真实顶点高度 = v? / (2·|重力|)，GravityScale 已包含在 GetGravityZ() 中。
// 最大可达 = 一段跳顶点 + 二段跳顶点（脚部基准）
float ABattleCharacter::GetMaxJumpHeight() const
{
    if (const UCharacterMovementComponent* Movement = GetCharacterMovement())
    {
        const float Gravity = FMath::Abs(Movement->GetGravityZ());
        if (Gravity > KINDA_SMALL_NUMBER)
        {
            const float H1 = (JumpHeight * JumpHeight) / (2.0f * Gravity);
            const float H2 = (DoubleJumpHeight * DoubleJumpHeight) / (2.0f * Gravity);
            return H1 + H2;
        }
    }
    return 600.0f;
}

// ---- 跳跃系统 ----
void ABattleCharacter::PerformJump()
{
    if (!CanPerformJump())
        return;

    Jump();
    CurrentJumpCount--;

    UE_LOG(LogTemp, Warning, TEXT("Jump! Remaining: %d"), CurrentJumpCount);
}

void ABattleCharacter::Jump()
{
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->JumpZVelocity = JumpHeight;
        GetCharacterMovement()->DoJump(false, GetWorld()->GetDeltaSeconds());

        // 【修复】增加 GetMesh() 判空保护
        if (JumpMontage && GetMesh())
        {
            if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
            {
                float Duration = AnimInstance->Montage_Play(JumpMontage, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, true);
                UE_LOG(LogTemp, Warning, TEXT("Jump: Playing JumpMontage! Duration: %f"), Duration);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Jump: JumpMontage is not set in BP_PlayerCharacter or Mesh is null!"));
        }
    }
}

void ABattleCharacter::PerformDoubleJump()
{
    if (!CanPerformDoubleJump())
        return;

    DoubleJump();
    CurrentJumpCount--;

    UE_LOG(LogTemp, Warning, TEXT("Double Jump! Remaining: %d"), CurrentJumpCount);
}

void ABattleCharacter::DoubleJump()
{
    if (GetCharacterMovement())
    {
        GetCharacterMovement()->JumpZVelocity = DoubleJumpHeight;
        GetCharacterMovement()->DoJump(false, GetWorld()->GetDeltaSeconds());

        // 【修复】先获取 SkeletalMesh，避免直接调用 GetMesh() 导致的空指针崩溃
        USkeletalMeshComponent* SkeletalMesh = GetMesh();
        if (!SkeletalMesh)
        {
            UE_LOG(LogTemp, Error, TEXT("DoubleJump: SkeletalMesh is null!"));
            return;
        }

        UAnimInstance* AnimInstance = SkeletalMesh->GetAnimInstance();
        if (!AnimInstance)
        {
            UE_LOG(LogTemp, Error, TEXT("DoubleJump: No AnimInstance!"));
            return;
        }

        if (DoubleJumpMontage)
        {
            float Duration = AnimInstance->Montage_Play(DoubleJumpMontage, 0.6f, EMontagePlayReturnType::MontageLength, 0.0f, true);
            UE_LOG(LogTemp, Warning, TEXT("DoubleJump: Playing DoubleJumpMontage! Duration: %f"), Duration);
        }
        else if (JumpMontage)
        {
            float Duration = AnimInstance->Montage_Play(JumpMontage, 0.8f);
            UE_LOG(LogTemp, Warning, TEXT("DoubleJump: Falling back to JumpMontage! Duration: %f"), Duration);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("DoubleJump: No jump montage set!"));
        }
    }
}

bool ABattleCharacter::CanPerformJump() const
{
    return GetCharacterMovement() &&
        GetCharacterMovement()->IsMovingOnGround() &&
        CurrentJumpCount > 0 && !bIsHitReaction &&
        !bIsSkillCasting && !bIsUltimateCasting;
}

bool ABattleCharacter::CanPerformDoubleJump() const
{
    return GetCharacterMovement() &&
        !GetCharacterMovement()->IsMovingOnGround() &&
        CurrentJumpCount > 0 && !bIsHitReaction &&
        !bIsSkillCasting && !bIsUltimateCasting;
}

void ABattleCharacter::ResetJumpCount()
{
    CurrentJumpCount = MaxJumpCount;
    UE_LOG(LogTemp, Warning, TEXT("Jump count reset! Max: %d"), MaxJumpCount);
}

bool ABattleCharacter::IsCharacterJumping() const
{
    return GetCharacterMovement() && !GetCharacterMovement()->IsMovingOnGround();
}

void ABattleCharacter::Landed(const FHitResult& Hit)
{
    Super::Landed(Hit);
    ResetJumpCount();

    // 下落攻击落地：结算范围伤害并结束下落攻击状态
    if (bIsFallAttacking)
    {
        bIsFallAttacking = false;
        bIsAttacking = false;
        ApplyFallAttackImpact();
    }
}

void ABattleCharacter::UpdateStaminaBar()
{
    if (!StaminaBarWidget)
        return;

    UUserWidget* Widget = StaminaBarWidget->GetWidget();
    if (!Widget)
        return;

    // 计算耐力百分比
    StaminaPercent = MaxStamina > 0.0f ? CurrentStamina / MaxStamina : 0.0f;

    // 更新 Progress Bar
    UProgressBar* ProgressBar = Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("StaminaProgressBar")));
    if (ProgressBar)
    {
        ProgressBar->SetPercent(StaminaPercent);
    }
}

// ---- 耐力条显隐控制 ----
void ABattleCharacter::SetStaminaBarVisible(bool bVisible)
{
    bStaminaBarVisible = bVisible;

    if (StaminaBarWidget)
    {
        StaminaBarWidget->SetHiddenInGame(!bVisible);
        StaminaBarWidget->SetVisibility(bVisible);
    }
}

void ABattleCharacter::UpdateStaminaBarVisibility()
{
    const UWorld* World = GetWorld();
    if (!World || !StaminaBarWidget)
    {
        return;
    }

    const float Now = World->GetTimeSeconds();

    // 检测耐力值是否发生变动（消耗或恢复都算变动）
    if (!FMath::IsNearlyEqual(CurrentStamina, LastStaminaValue))
    {
        LastStaminaValue = CurrentStamina;
        LastStaminaChangeTime = Now;

        // 有变动：确保耐力条显示
        if (!bStaminaBarVisible)
        {
            SetStaminaBarVisible(true);
            UE_LOG(LogTemp, Warning, TEXT("Stamina changed! Bar shown."));
        }
    }
    // 无变动超过延迟时间：隐藏耐力条
    else if (bStaminaBarVisible && (Now - LastStaminaChangeTime) >= StaminaBarHideDelay)
    {
        SetStaminaBarVisible(false);
        UE_LOG(LogTemp, Warning, TEXT("Stamina idle for %.1fs! Bar hidden."), StaminaBarHideDelay);
    }
}

void ABattleCharacter::UpdateStaminaBarPosition()
{
    if (!StaminaBarWidget || !FollowCamera)
        return;

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return;

    // 获取视口尺寸
    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // 目标屏幕位置 = 屏幕中心 + 偏移
    const float TargetScreenX = ViewportX * 0.5f + StaminaBarScreenOffset.X;
    const float TargetScreenY = ViewportY * 0.5f + StaminaBarScreenOffset.Y;

    // 【防抖修复】不再用 DeprojectScreenPositionToWorld 反投影。
    // 反投影使用的是上一帧的渲染视角（PlayerCameraManager 缓存），而组件渲染
    // 跟随当前帧摄像机变换：转动视角时两者每帧错位 → 抖动。
    // 改为直接用 FOV + 视口尺寸解析计算摄像机局部偏移（局部偏移是常量）。
    const float Dist = 300.0f; // 放置距离（屏幕空间下不影响绘制大小，只决定投影位置）
    const float HalfFovRad = FMath::DegreesToRadians(FollowCamera->FieldOfView * 0.5f);

    // 距摄像机 Dist 处，1 个屏幕像素对应的摄像机局部空间单位数（UE 的 FieldOfView 是水平 FOV）
    const float UnitsPerPixel = 2.0f * Dist * FMath::Tan(HalfFovRad) / static_cast<float>(ViewportX);

    // 摄像机局部空间：X 前、Y 右、Z 上；屏幕 Y 向下为正 → 局部 Z 取负
    StaminaBarWidget->SetRelativeLocation(FVector(
        Dist,
        StaminaBarScreenOffset.X * UnitsPerPixel,
        -StaminaBarScreenOffset.Y * UnitsPerPixel));
}

// ---- 血条UI更新 ----
void ABattleCharacter::UpdateHealthBar()
{
    if (!HealthBarWidget)
        return;

    UUserWidget* Widget = HealthBarWidget->GetWidget();
    if (!Widget)
        return;

    // 计算生命百分比
    HealthPercent = MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;

    // 更新 Progress Bar（WBP_HealthBar 里需有名为 HealthProgressBar 的进度条）
    UProgressBar* ProgressBar = Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("HealthProgressBar")));
    if (ProgressBar)
    {
        ProgressBar->SetPercent(HealthPercent);
    }

    // 更新实时最大血量文本（WBP_HealthBar 里需有名为 Max_Health_Num 的文本）
    if (UTextBlock* MaxHealthText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("Max_Health_Num"))))
    {
        MaxHealthText->SetText(FText::AsNumber(FMath::RoundToInt(MaxHealth)));
    }

    // 更新实时当前血量文本（WBP_HealthBar 里需有名为 Curr_Health_Num 的文本）
    if (UTextBlock* CurrHealthText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("Curr_Health_Num"))))
    {
        CurrHealthText->SetText(FText::AsNumber(FMath::RoundToInt(CurrentHealth)));
    }

    // 更新角色等级文本（WBP_HealthBar 里需有名为 char_LV_num 的文本）
    if (UTextBlock* LevelText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("char_LV_num"))))
    {
        LevelText->SetText(FText::AsNumber(CharacterLevel));
    }
}

void ABattleCharacter::UpdateHealthBarPosition()
{
    if (!HealthBarWidget || !FollowCamera)
        return;

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return;

    // 获取视口尺寸
    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // 目标屏幕位置 = 屏幕底部居中 + 偏移
    // X：水平中心 + 水平偏移；Y：从底部上移（底边距 + 半个条高，使血条下边缘贴着底边距）
    const float TargetScreenX = ViewportX * 0.5f + HealthBarHorizontalOffset;
    const float TargetScreenY = ViewportY - HealthBarBottomMargin - HealthBarDrawSize.Y * 0.5f;

    // 【防抖修复】不再用 DeprojectScreenPositionToWorld 反投影。
    // 反投影使用的是上一帧的渲染视角（PlayerCameraManager 缓存），而组件渲染
    // 跟随当前帧摄像机变换：转动视角时两者每帧错位，算出的局部偏移来回摆动 → 抖动。
    // 改为直接用 FOV + 视口尺寸解析计算摄像机局部偏移：
    // 目标点（屏幕底部居中）在摄像机局部空间是常量，挂死后刚性跟随，零抖动。
    const float Dist = 300.0f; // 放置距离（屏幕空间下不影响绘制大小，只决定投影位置）
    const float HalfFovRad = FMath::DegreesToRadians(FollowCamera->FieldOfView * 0.5f);

    // 距摄像机 Dist 处，1 个屏幕像素对应的摄像机局部空间单位数。
    // UE 的 FieldOfView 是水平 FOV：水平方向宽度 = 2*Dist*tan(HalfFov)，覆盖 ViewportX 个像素；
    // 像素是正方形，垂直方向比例相同。
    const float UnitsPerPixel = 2.0f * Dist * FMath::Tan(HalfFovRad) / static_cast<float>(ViewportX);

    // 目标点相对屏幕中心的像素偏移（屏幕坐标 Y 向下为正）
    const float OffsetX = TargetScreenX - ViewportX * 0.5f;
    const float OffsetY = TargetScreenY - ViewportY * 0.5f;

    // 摄像机局部空间：X 前、Y 右、Z 上；屏幕 Y 向下为正 → 局部 Z 取负
    HealthBarWidget->SetRelativeLocation(FVector(
        Dist,
        OffsetX * UnitsPerPixel,
        -OffsetY * UnitsPerPixel));
}

// ---- Viewport resize detection: force-refresh screen-space UI widgets ----
// Known UE issue: a Screen Space WidgetComponent's Slate window becomes invalid after
// the viewport is resized (window stretch / resolution change) -> the widget vanishes.
// Destroying and re-creating the widget restores it; position/size stay adaptive because
// the per-frame positioning logic re-reads the viewport size every tick.
void ABattleCharacter::CheckViewportResize()
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
        return;

    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX <= 0 || ViewportY <= 0)
        return;

    // First frame: record the baseline size only
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
        UE_LOG(LogTemp, Warning, TEXT("Viewport resized to %dx%d: refreshing screen-space UI widgets."),
            ViewportX, ViewportY);

        // Refresh all camera-attached screen-space widgets (health bar / stamina bar / hurt screen)
        ForceRefreshScreenWidget(HealthBarWidget);
        ForceRefreshScreenWidget(StaminaBarWidget);
        ForceRefreshScreenWidget(HurtUIWidget);
        ForceRefreshScreenWidget(ItemPickupTipWidget);

        // ★ 视口变了 ⇒ 画布尺寸变了 ⇒ 之前那份「布局体检」的结论作废。
        //   不重置的话，换一次分辨率就再也看不到体检结论（原来就是「只打一次」，一换就哑）。
        //   重置后下一帧会重新体检一次并留记录，且会带上新的画布尺寸。
        bItemPickupLayoutDiagnosed = false;
        LastDiagnosedTipCount = -1;
        LastItemPickupDiagTime = -1.0f;
        UE_LOG(LogTemp, Log,
            TEXT("[Drop][Layout] 视口已变为 %d×%d → 提示列表的布局体检已重置，下一帧会重新体检一次。"),
            ViewportX, ViewportY);
    }
}

// ---- Force-recreate the Slate widget of a screen-space WidgetComponent ----
void ABattleCharacter::ForceRefreshScreenWidget(UWidgetComponent* WidgetComp)
{
    if (!WidgetComp)
        return;

    const bool bWasHidden = WidgetComp->bHiddenInGame;
    UUserWidget* Widget = WidgetComp->GetWidget();

    // Destroy the invalid Slate widget, then re-create it for the current viewport
    WidgetComp->SetWidget(nullptr);
    WidgetComp->SetWidget(Widget);

    // Re-creation does not change visibility, but write it back defensively
    WidgetComp->SetHiddenInGame(bWasHidden);
}

// ---- 生命值操作 ----
float ABattleCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    // 闪避无敌状态（GAS 化：查 State.Invincible Tag）：免疫伤害
    if (IsInvincibleNow())
    {
        UE_LOG(LogTemp, Warning, TEXT("TakeDamage blocked by invincibility!"));
        return 0.0f;
    }

    // 大招无敌（可配置）：大招施放期间完全免疫——不扣血、不播受击动画、不触发受伤UI
    if (bUltimateInvincible && bIsUltimateCasting)
    {
        UE_LOG(LogTemp, Warning, TEXT("TakeDamage blocked by ultimate invincibility!"));
        return 0.0f;
    }

    const float ActualDamage = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);

    // 受击打断加速（加速期间受到攻击立即停止加速）
    if (bIsSprinting)
    {
        StopSprint();
        UE_LOG(LogTemp, Warning, TEXT("Sprint interrupted by damage!"));
    }

    // ---- GAS 化伤害结算：护盾先吸收、剩余扣血，由 UDamageExecutionCalculation 完成 ----
    ApplyDamageViaGAS(ActualDamage);

    // ---- 受伤提示 UI：实际受到伤害（>0）时闪现，HurtUIDisplayDuration 秒后隐藏直到下次受伤 ----
    if (ActualDamage > 0.0f)
    {
        ShowHurtUI();
    }

    UE_LOG(LogTemp, Warning, TEXT("TakeDamage: %.1f, Health: %.1f / %.1f"), ActualDamage, CurrentHealth, MaxHealth);

    if (CurrentHealth <= 0.0f)
    {
        // 死亡处理占位（后续可扩展：死亡动画/重生/游戏结束UI）
        UE_LOG(LogTemp, Warning, TEXT("Character died!"));
    }
    else
    {
        // 存活：播放受击反应蒙太奇
        PlayHitReaction();
    }

    return ActualDamage;
}

// ---- GAS 伤害结算：通过伤害 GameplayEffect（Execution）扣护盾/扣血，并同步镜像字段 + 刷新血条 ----
void ABattleCharacter::ApplyDamageViaGAS(float DamageAmount)
{
    if (DamageAmount <= 0.0f || !AbilitySystemComponent || !AttributeSet)
    {
        return;
    }

    // 未配置伤害 GE → 回退到旧手写扣血逻辑（保底，防蓝图误删）
    if (!DamageEffectClass)
    {
        const float DamageToHealth = AbsorbDamageWithShield(DamageAmount);
        CurrentHealth = FMath::Clamp(CurrentHealth - DamageToHealth, 0.0f, MaxHealth);
        UpdateHealthBar();
        return;
    }

    // 构建伤害 GE 上下文，通过 SetByCaller 传入伤害值
    FGameplayEffectContextHandle EffectContext = AbilitySystemComponent->MakeEffectContext();
    FGameplayEffectSpecHandle SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(
        DamageEffectClass, /*Level=*/1.0f, EffectContext);

    if (!SpecHandle.IsValid())
    {
        return;
    }

    // 伤害值通过 SetByCaller 标签 "Damage.SetByCaller" 传入 Execution
    SpecHandle.Data->SetSetByCallerMagnitude(WutheringWavesTags::Damage_SetByCaller_Tag(), DamageAmount);

    // 应用到自身（角色是目标）
    AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());

    // ---- 同步镜像字段（从 AttributeSet 回读，保持 UI/旧代码可读）----
    CurrentHealth = AttributeSet->GetHealth();
    CurrentShield = AttributeSet->GetShield();

    // 护盾被本次伤害扣完 → 状态图标消失（与原 AbsorbDamageWithShield 行为一致）
    if (CurrentShield <= 0.0f && ShieldSlotIndex != -1)
    {
        ShieldSlotIndex = -1;
        RefreshStatusIcons();
    }

    UpdateHealthBar();
    UpdateShieldBar();
}

void ABattleCharacter::HealHealth(float Amount)
{
    // GAS 化：直接写 AttributeSet 的 Health（唯一真源），再同步镜像字段
    if (AttributeSet)
    {
        AttributeSet->SetHealth(FMath::Clamp(AttributeSet->GetHealth() + Amount, 0.0f, MaxHealth));
        CurrentHealth = AttributeSet->GetHealth();
    }
    else
    {
        CurrentHealth = FMath::Clamp(CurrentHealth + Amount, 0.0f, MaxHealth);
    }
    UpdateHealthBar();

    UE_LOG(LogTemp, Warning, TEXT("Heal: +%.1f, Health: %.1f / %.1f"), Amount, CurrentHealth, MaxHealth);
}

// ---- 静默扣血：毒刺命中用（扣血+血条+受伤UI，但不触发受击/击飞动画、不锁操作）----
float ABattleCharacter::ApplySilentDamage(float DamageAmount, AActor* DamageCauser)
{
    // 无敌帧免疫（完美闪避/大招期间不扣血，GAS 化查 Tag），返回 0
    if (IsInvincibleNow())
    {
        UE_LOG(LogTemp, Warning, TEXT("ApplySilentDamage blocked by invincibility!"));
        return 0.0f;
    }
    if (bUltimateInvincible && bIsUltimateCasting)
    {
        UE_LOG(LogTemp, Warning, TEXT("ApplySilentDamage blocked by ultimate invincibility!"));
        return 0.0f;
    }

    // ---- GAS 化伤害结算：护盾先吸收、剩余扣血（与普通受击一致，毒刺/毒伤同样优先扣护盾）----
    ApplyDamageViaGAS(DamageAmount);

    // 实际受到伤害（>0）时闪现受伤 UI，但不触发受击/击飞动画
    if (DamageAmount > 0.0f)
    {
        ShowHurtUI();
    }

    UE_LOG(LogTemp, Warning, TEXT("ApplySilentDamage: %.1f, Health: %.1f / %.1f (no hit/knockback anim)."),
        DamageAmount, CurrentHealth, MaxHealth);

    if (CurrentHealth <= 0.0f)
    {
        UE_LOG(LogTemp, Warning, TEXT("Character died (silent damage)!"));
    }

    return DamageAmount;
}

// ---- 中毒状态：施加（无法重复叠加，GAS 化：应用中毒 GE Grant State.Poisoned Tag）----
bool ABattleCharacter::ApplyPoison()
{
    // 已中毒：无法重复叠加，直接忽略（不重置计时、不叠加层数）
    if (IsPoisoned())
    {
        UE_LOG(LogTemp, Warning, TEXT("ApplyPoison ignored: already poisoned (no stacking)."));
        return false;
    }

    // 已死亡：不施加
    if (CurrentHealth <= 0.0f)
    {
        return false;
    }

    // ---- GAS 化：应用中毒 GameplayEffect（HasDuration + Periodic），周期毒伤由 Execution 自动结算 ----
    if (AbilitySystemComponent && PoisonEffectClass)
    {
        FGameplayEffectContextHandle EffectContext = AbilitySystemComponent->MakeEffectContext();
        FGameplayEffectSpecHandle SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(
            PoisonEffectClass, /*Level=*/1.0f, EffectContext);

        if (SpecHandle.IsValid())
        {
            // 时长、每秒毒伤比例通过 SetByCaller 传入（周期秒数由 GE 的 Period 固定）
            SpecHandle.Data->SetSetByCallerMagnitude(
                WutheringWavesTags::State_Poisoned_Duration_Tag(), FMath::Max(0.0f, PoisonDuration));
            SpecHandle.Data->SetSetByCallerMagnitude(
                WutheringWavesTags::State_Poisoned_DamagePerSecondRatio_Tag(), PoisonDamagePerSecondRatio);

            AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
        }
    }

    // ---- 镜像字段（供 UI/旧代码可读）----
    bIsPoisoned = true;
    PoisonRemaining = FMath::Max(0.0f, PoisonDuration);
    PoisonTickTimer = 0.0f;

    // 记录获得时间戳（用于「按获得时间顺序」分配状态图标槽位）
    PoisonApplyTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

    // 刷新状态图标（按获得时间顺序分配槽位 + 填充 poisoning 图像）
    RefreshStatusIcons();

    UE_LOG(LogTemp, Warning, TEXT("Character POISONED! Duration=%.1fs, %.1f%% max HP per second (icon slot %d)."),
        PoisonRemaining, PoisonDamagePerSecondRatio * 100.0f, PoisonSlotIndex);
    return true;
}

// ---- 中毒 Tick 驱动（GAS 化）：毒伤由中毒 GE 的 Periodic Execution 自动结算，
//      此函数仅负责到期清理镜像字段 + 状态图标同步 ----
void ABattleCharacter::UpdatePoison(float DeltaTime)
{
    // 未中毒直接返回（GAS 化后由 Tag 驱动，但保留镜像字段快速短路）
    if (!bIsPoisoned)
    {
        return;
    }

    // 递减镜像剩余时间（实际到期由 GE 的 Duration 驱动，这里做 UI 同步兜底）
    PoisonRemaining -= DeltaTime;

    // GE 到期 → Tag 被移除 → 判定结束中毒，清理镜像
    if (PoisonRemaining <= 0.0f || !IsPoisoned())
    {
        bIsPoisoned = false;
        PoisonRemaining = 0.0f;
        PoisonTickTimer = 0.0f;
        PoisonSlotIndex = -1; // 释放状态图标槽位
        RefreshStatusIcons(); // 清除图像 + 隐藏
        UE_LOG(LogTemp, Warning, TEXT("Poison expired."));
        return;
    }

    // 结算后若已死亡，结束中毒（毒伤由 GE 结算，可能直接致死）
    if (CurrentHealth <= 0.0f)
    {
        bIsPoisoned = false;
        PoisonRemaining = 0.0f;
        PoisonTickTimer = 0.0f;
        PoisonSlotIndex = -1;
        RefreshStatusIcons();
    }
}

// ---- 是否中毒（GAS 化：查 State.Poisoned Tag，回退镜像字段）----
bool ABattleCharacter::IsPoisoned() const
{
    if (AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(WutheringWavesTags::State_Poisoned_Tag()))
    {
        return true;
    }
    return bIsPoisoned;
}

// ---- 中毒剩余时间（从 ASC 读取剩余时长；未配置 GE 时回退镜像字段）----
float ABattleCharacter::GetPoisonRemaining() const
{
    if (AbilitySystemComponent)
    {
        FGameplayEffectQuery Query;
        Query.EffectTagQuery = FGameplayTagQuery::MakeQuery_MatchAnyTags(
            FGameplayTagContainer(WutheringWavesTags::State_Poisoned_Tag()));
        TArray<float> RemainingTimes = AbilitySystemComponent->GetActiveEffectsTimeRemaining(Query);
        if (RemainingTimes.Num() > 0)
        {
            // 取最大剩余时长（正常情况下只有一个中毒 GE）
            float MaxRemaining = 0.0f;
            for (float T : RemainingTimes)
            {
                MaxRemaining = FMath::Max(MaxRemaining, T);
            }
            return MaxRemaining;
        }
    }
    return PoisonRemaining;
}

// ---- 状态图标刷新（WBP_HealthBar 的 state_01~04）----
// 按占用槽位填充图像并可视化；未占用槽位清除图像 + 隐藏。
// 槽位分配按「获得状态的时间顺序」：先获得的状态占更小的槽位号。
void ABattleCharacter::RefreshStatusIcons()
{
    UUserWidget* Widget = HealthBarWidget ? HealthBarWidget->GetWidget() : nullptr;
    if (!Widget)
    {
        return;
    }

    // 缓存未建 / 宿主 widget 重建 → 按名重新查找 state_01~04
    const int32 SlotCount = 4;
    if (StatusIconCache.Num() != SlotCount || StatusIconHostWidget.Get() != Widget)
    {
        StatusIconCache.Reset();
        for (int32 i = 1; i <= SlotCount; ++i)
        {
            const FString IconName = FString::Printf(TEXT("state_%02d"), i);
            StatusIconCache.Add(Cast<UImage>(Widget->GetWidgetFromName(*IconName)));
        }
        StatusIconHostWidget = Widget;
    }

    // ---- 收集当前所有活跃状态（图标 + 获得时间戳 + 状态 ID）----
    // 通用多状态槽位管理：中毒 + 大招增伤 + 吸血 + 护盾（未来可扩展更多）。
    enum class EStatusType : uint8 { Poison, UltimateBuff, Lifesteal, Shield };
    struct FActiveStatus
    {
        EStatusType Type = EStatusType::Poison;
        UTexture2D* Icon = nullptr;
        float ApplyTime = 0.0f;
    };
    TArray<FActiveStatus> ActiveStatuses;

    if (IsPoisoned() && PoisonIconTexture)
    {
        FActiveStatus S;
        S.Type = EStatusType::Poison;
        S.Icon = PoisonIconTexture;
        S.ApplyTime = PoisonApplyTime;
        ActiveStatuses.Add(S);
    }
    if (IsUltimateDamageBuffActive() && UltimateBuffIconTexture)
    {
        FActiveStatus S;
        S.Type = EStatusType::UltimateBuff;
        S.Icon = UltimateBuffIconTexture;
        S.ApplyTime = UltimateBuffApplyTime;
        ActiveStatuses.Add(S);
    }
    if (bEnergyBuffActive && LifestealIconTexture)
    {
        FActiveStatus S;
        S.Type = EStatusType::Lifesteal;
        S.Icon = LifestealIconTexture;
        S.ApplyTime = LifestealApplyTime;
        ActiveStatuses.Add(S);
    }
    if (CurrentShield > 0.0f && ShieldIconTexture)
    {
        FActiveStatus S;
        S.Type = EStatusType::Shield;
        S.Icon = ShieldIconTexture;
        S.ApplyTime = ShieldApplyTime;
        ActiveStatuses.Add(S);
    }

    // 按获得时间顺序排序（先获得的排前，占更小的槽位号）
    ActiveStatuses.Sort([](const FActiveStatus& A, const FActiveStatus& B)
    {
        return A.ApplyTime < B.ApplyTime;
    });

    // 回写各状态的槽位索引（供外部查询/日志用）
    PoisonSlotIndex = -1;
    UltimateBuffSlotIndex = -1;
    LifestealSlotIndex = -1;
    ShieldSlotIndex = -1;
    for (int32 i = 0; i < ActiveStatuses.Num(); ++i)
    {
        switch (ActiveStatuses[i].Type)
        {
        case EStatusType::Poison:
            PoisonSlotIndex = i;
            break;
        case EStatusType::UltimateBuff:
            UltimateBuffSlotIndex = i;
            break;
        case EStatusType::Lifesteal:
            LifestealSlotIndex = i;
            break;
        case EStatusType::Shield:
            ShieldSlotIndex = i;
            break;
        }
    }

    // 逐槽位刷新：已分配的状态显示对应图像，未分配槽位清除 + 隐藏
    for (int32 i = 0; i < SlotCount; ++i)
    {
        UImage* Icon = StatusIconCache[i].Get();
        if (!Icon)
        {
            continue;
        }

        if (ActiveStatuses.IsValidIndex(i))
        {
            // 填充对应状态图像 + 可视化
            Icon->SetBrushFromTexture(ActiveStatuses[i].Icon);
            Icon->SetVisibility(ESlateVisibility::Visible);
        }
        else
        {
            // 清除图像 + 隐藏
            Icon->SetBrushFromTexture(nullptr);
            Icon->SetVisibility(ESlateVisibility::Hidden);
        }
    }
}

// ---- 武器系统 ----
bool ABattleCharacter::EquipWeapon(TSubclassOf<AWeaponBase> WeaponClass)
{
    if (!WeaponClass || !GetWorld())
    {
        UE_LOG(LogTemp, Warning, TEXT("EquipWeapon: invalid weapon class!"));
        return false;
    }

    // ---- 职位约束：武器类别必须匹配当前职位 ----
    // 为什么不放在 Spawn 之后：拦截越早，越不会产生「先卸下旧武器再发现装不上」的副作用。
    if (!CanEquipWeaponClassForJob(WeaponClass))
    {
        const AWeaponBase* CDO = WeaponClass->GetDefaultObject<AWeaponBase>();
        UE_LOG(LogTemp, Warning,
            TEXT("[Job] 无法装备武器「%s」：其类别(%d) 与当前职位(%d，可用类别 %d) 不匹配。\n"
                 "      职位对应关系：剑士→轻剑、法师→法器、枪手→枪械。\n"
                 "      修法（二选一）：\n"
                 "        ① 改角色职位：BP_PlayerCharacter → Job → Job Class 改成与武器匹配的职位\n"
                 "        ② 改武器类别：打开 %s → Class Defaults → Weapon → Weapon Category 改成职位对应的类别"),
            *WeaponClass->GetName(),
            CDO ? static_cast<int32>(CDO->GetWeaponCategory()) : -1,
            static_cast<int32>(JobClass),
            static_cast<int32>(FJobWeaponRules::GetCategoryForJob(JobClass)),
            *WeaponClass->GetName());
        return false;
    }

    // 已有武器先卸下
    if (CurrentWeapon)
    {
        UnequipWeapon();
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AWeaponBase* NewWeapon = GetWorld()->SpawnActor<AWeaponBase>(WeaponClass, GetActorLocation(), GetActorRotation(), SpawnParams);
    if (!NewWeapon)
    {
        UE_LOG(LogTemp, Warning, TEXT("EquipWeapon: failed to spawn weapon!"));
        return false;
    }

    NewWeapon->SetInstigator(this);

    // ---- 按职位自动套插槽：法师 → magic_WeaponSocket，剑士/枪手 → HandGrip_R ----
    // 在 OnEquipped（内部用 AttachSocketName 吸附）之前覆盖插槽名。
    {
        const FName JobSocket = ResolveEquipSocketName(NewWeapon);
        if (JobSocket != NewWeapon->GetAttachSocketName())
        {
            UE_LOG(LogTemp, Log, TEXT("[Job] 按职位(%d)套插槽：%s → %s"),
                static_cast<int32>(JobClass), *NewWeapon->GetAttachSocketName().ToString(), *JobSocket.ToString());
            NewWeapon->SetAttachSocketName(JobSocket);
        }
    }

    NewWeapon->OnEquipped(this);
    CurrentWeapon = NewWeapon;

    // ---- 武器蓝图声明了「我属于背包哪一行」→ 顺手把背包的装备状态认到那一行 ----
    // 为什么要做：装备入口不止「武器背包点格子」一条（蓝图里也能直接调 Equip Weapon）。
    // 声明写在武器蓝图上，就该在任何装备路径下都生效，否则背包高亮会与实际不符。
    {
        const FName DeclaredRow = GetBagItemRowForWeaponClass(WeaponClass);
        if (!DeclaredRow.IsNone())
        {
            // ★ 先换算成【真实行名】：声明可能是 tool_id，而「装备中」标记是按行名查的
            //   —— 直接写 tool_id 进去，背包里一格都不会高亮（看着像功能坏了）。
            const FName BagRow = ResolveBagKeyToInventoryRow(DeclaredRow);

            // ★ 再核【这一行属不属于背包物品表】—— 不核的话，一个填错表的声明
            //   就会把这个行号写进 EquippedWeaponRow，于是背包里那件**掉落物**
            //   被标成「装备中」、武器页显示它的名字。本轮实测正是如此。
            if (InventoryComponent && !InventoryComponent->IsToolTableRow(BagRow))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Arm] ⚠ 武器蓝图 %s 声明的背包行『%s』不属于背包物品表（Data_tool）→ "
                         "不写入装备记录。\n"
                         "      （若写进去，背包会把那一行的**掉落物**标成「装备中」，"
                         "武器页也会显示它的名字和图标。）\n"
                         "      修法：打开 %s → Class Defaults → Weapon|Bag → Bag Item Row，\n"
                         "            改成 Data_tool 里武器行的行名。"),
                    *WeaponClass->GetName(), *BagRow.ToString(), *WeaponClass->GetName());
            }
            else if (EquippedWeaponRow != BagRow)
            {
                EquippedWeaponRow = BagRow;
                // 这条装备路径不来自背包具体某一把（实例号无意义），重置为 0。
                EquippedWeaponInstanceNo = 0;
                UE_LOG(LogTemp, Log, TEXT("[Arm] 按武器蓝图声明同步背包装备行：%s → 『%s』%s"),
                    *WeaponClass->GetName(), *BagRow.ToString(),
                    BagRow != DeclaredRow
                        ? *FString::Printf(TEXT("（蓝图声明的是 tool_id『%s』，已换算成行名）"),
                            *DeclaredRow.ToString())
                        : TEXT(""));
            }
        }
    }

    OnWeaponEquipped(NewWeapon);
    UE_LOG(LogTemp, Warning, TEXT("Weapon equipped (Category=%d)!"), static_cast<int32>(NewWeapon->GetWeaponCategory()));
    return true;
}

void ABattleCharacter::UnequipWeapon()
{
    if (!CurrentWeapon)
        return;

    OnWeaponUnequipped();
    CurrentWeapon->EndAttack();
    CurrentWeapon->OnUnequipped();
    CurrentWeapon->Destroy();
    CurrentWeapon = nullptr;

    UE_LOG(LogTemp, Warning, TEXT("Weapon unequipped."));
}

// ---- 职位系统 ----
void ABattleCharacter::SetJobClass(EJobClass NewJob)
{
    if (JobClass == NewJob)
        return;

    const EJobClass OldJob = JobClass;
    JobClass = NewJob;

    UE_LOG(LogTemp, Log, TEXT("[Job] 职位切换：%d → %d（插槽 %s）"),
        static_cast<int32>(OldJob), static_cast<int32>(NewJob),
        *GetJobSocketName().ToString());

    // 切换职位后，若当前武器不属于新职位 → 自动卸下，避免「法师手上拿着剑」的非法状态。
    if (CurrentWeapon && !CanUseWeaponCategory(CurrentWeapon->GetWeaponCategory()))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Job] 当前武器「%s」不属于新职位（%d）→ 自动卸下。"),
            *CurrentWeapon->GetName(), static_cast<int32>(NewJob));
        UnequipWeapon();
    }
}

FName ABattleCharacter::GetJobSocketName() const
{
    return FJobWeaponRules::GetSocketForJob(JobClass);
}

bool ABattleCharacter::CanUseWeaponCategory(EWeaponCategory Category) const
{
    return FJobWeaponRules::CanJobUseCategory(JobClass, Category);
}

bool ABattleCharacter::CanEquipWeaponClassForJob(TSubclassOf<AWeaponBase> WeaponClass) const
{
    if (!WeaponClass)
        return false;

    // 读武器蓝图 CDO 的 WeaponCategory（不 Spawn，纯读默认值）。
    // 用 GetDefaultObject 而非 GetDefault：蓝图类（UBlueprintGeneratedClass）的
    // 默认值读法要经过它，才能拿到蓝图里改过的 CDO 值。
    const AWeaponBase* CDO = WeaponClass->GetDefaultObject<AWeaponBase>();
    if (!CDO)
        return false;

    return CanUseWeaponCategory(CDO->GetWeaponCategory());
}

FName ABattleCharacter::ResolveEquipSocketName(AWeaponBase* Weapon) const
{
    // 按职位自动套插槽：法师 → magic_WeaponSocket，剑士/枪手 → HandGrip_R。
    // 统一走职位规则，武器蓝图不必各自填插槽。
    return GetJobSocketName();
}

void ABattleCharacter::OnWeaponEquipped_Implementation(AWeaponBase* NewWeapon)
{
    // 默认空实现：蓝图里可加装备音效/特效/属性加成
}

void ABattleCharacter::OnWeaponUnequipped_Implementation()
{
    // 默认空实现
}

// =====================================================================
// ---- 切人系统（1/2/3 键；鸣潮式切人）----
// =====================================================================
// 需求原文的三种形态与实现对应：
//   ① 战斗 + 技能中切人：操控立刻转移；旧角色留下把技能放完（bPendingDestroyAfterSkillFinish
//      → EndSkill 唯一收敛点自毁）；新角色出现在【怪物附近】且不与旧角色重合。
//   ② 战斗 + 普攻中切人：仅在当前段 CancelWindowTime 可打断窗口内允许（复用蓝图已配的
//      普攻衔接窗口，与「普攻打断普攻」同一套手感）；新角色出现在原位置 + 自动打出
//      第 SwitchInAttackComboStep+1 段普攻（各角色蓝图可各自设置）；旧角色立即销毁。
//   ③ 非战斗切人：原角色马上消失、新角色原地登场；继承移速向量 → 移动动画自然衔接。
// 拒绝：大招中（R 演出禁止打断）｜受击硬直｜死亡｜目标 CD 中｜槽位空｜切自己。
// CD：成功切人后换上/换下两个角色行名各自独立进 CD（SwitchCooldown，默认 1s，蓝图可调，per-row 独立计时）。

void ABattleCharacter::RequestSwitchToTeamSlot(int32 SlotIndex)
{
    // 只对「当前被玩家操控的角色」生效。
    // 退役旧实例（等技能放完的那位）身上切人输入已解绑，这里是第二道闸。
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC || PC->GetPawn() != this)
    {
        return;
    }

    if (SlotIndex < 0 || SlotIndex > 2)
    {
        UE_LOG(LogTemp, Log, TEXT("[Switch] 槽位 %d 非法（只支持 0~2，对应 1/2/3 键）。"), SlotIndex);
        return;
    }

    // 目标行名：当前队伍的第 SlotIndex 槽
    EnsureTeamsArray();
    const TArray<FName>& Members = GetTeamMembers(CurrentTeamIndex);
    if (!Members.IsValidIndex(SlotIndex) || Members[SlotIndex].IsNone())
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] ★ 影响：本次按键无效果 —— 第 %d 队的第 %d 槽是空的。\n"
                 "      修法：编队界面（L 键）给当前队伍配满成员后再切。"),
            CurrentTeamIndex + 1, SlotIndex + 1);
        return;
    }
    const FName TargetRow = Members[SlotIndex];

    const FName MyRow = GetMyCharaRow();
    if (!MyRow.IsNone() && TargetRow == MyRow)
    {
        UE_LOG(LogTemp, Log, TEXT("[Switch] 目标就是当前操控角色（行 %s），忽略。"), *TargetRow.ToString());
        return;
    }

    // CD 闸门：目标角色切人 CD 期间无法被切入
    if (const float Remaining = GetSwitchCooldownRemaining(TargetRow); Remaining > 0.0f)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] ★ 影响：切人被 CD 挡下 —— %s 还有 %.1fs 切人 CD（%.1fs 内该角色无法被切入）。"),
            *TargetRow.ToString(), Remaining, Remaining);
        return;
    }

    // 旧角色状态闸门
    if (bIsUltimateCasting)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] ★ 影响：大招（R）演出期间禁止切人 —— 等大招结束再按。"));
        return;
    }
    if (bIsHitReaction)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] ★ 影响：受击硬直期间禁止切人 —— 硬直结束再按。"));
        return;
    }
    if (CurrentHealth <= 0.0f)
    {
        UE_LOG(LogTemp, Log, TEXT("[Switch] ★ 影响：当前角色已倒下，禁止切人。"));
        return;
    }

    ExecuteSwitchToRow(SlotIndex, TargetRow);
}

bool ABattleCharacter::IsSwitchCooldownActive(FName CharaRow) const
{
    return GetSwitchCooldownRemaining(CharaRow) > 0.0f;
}

float ABattleCharacter::GetSwitchCooldownRemaining(FName CharaRow) const
{
    if (CharaRow.IsNone() || !GetWorld())
    {
        return 0.0f;
    }
    if (const double* Until = SwitchCooldownUntilRow.Find(CharaRow))
    {
        // ★ 时间源必须是进程级单调时钟 FPlatformTime::Seconds()，不能用 GetWorld()->GetTimeSeconds()：
        //   GetTimeSeconds() 是「世界时间」，每个 PIE 实例独立、每轮从 0 重新计时；而 SwitchCooldownUntilRow
        //   是 static，跨 PIE 轮次不重置。若写入用世界时间、下一轮世界时间又归零，残留的旧 Until 会
        //   让 CD「虚高」——第 2 轮立刻切人时 Remaining = 上轮世界时间 + CD - 0，远超设定的 CD 值。
        //   用进程级单调时钟后，Until 是绝对进程时间戳，跨轮依然单调递增，旧轮 Until 早已过期会正确归 0。
        const float Remaining = static_cast<float>(*Until - FPlatformTime::Seconds());
        return FMath::Max(0.0f, Remaining);
    }
    return 0.0f;
}

bool ABattleCharacter::ExecuteSwitchToRow(int32 SlotIndex, FName TargetRow)
{
    if (bSwitchInProgress)
    {
        return false;
    }
    TGuardValue<bool> SwitchGuard(bSwitchInProgress, true);

    if (!GetWorld())
    {
        return false;
    }

    // ---- ① 解析目标角色类 ----
    EnsureCharaInfoTable();
    FCharaInfoEntry Info;
    if (!GetCharaInfoByRow(TargetRow, Info) || !Info.CharaClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Switch] ★ 影响：切人失败 —— 编队行『%s』在 Data_chara_imfor 里不存在或没填 CharaClass。\n"
                 "      修法：打开 /Game/UI/chara_imf/Data_chara_imfor，确认该行存在且 CharaClass 已填角色蓝图。"),
            *TargetRow.ToString());
        return false;
    }

    // ---- ①-2 ★ 目标角色已有一份实例在场 → 归还操控权，而非再 spawn 一份 ----
    // 技能中切人（形态①）旧角色留场放技能、不销毁，若在它技能放完前切回，
    // 注册表里还查得到它 → 直接把操控权交回给它，避免「原角色多出一个新的」。
    if (ABattleCharacter* const* Existing = ActiveCharaInstances.Find(TargetRow))
    {
        if (*Existing && IsValid(*Existing) && *Existing != this)
        {
            return ResumeControlToExistingInstance(*Existing, TargetRow);
        }
        // 命中但已失效/指向自己 → 清掉脏登记，走正常 spawn 分支
        ActiveCharaInstances.Remove(TargetRow);
    }

    // ---- ② 判定切人形态（技能保留 / 普攻衔接 / 原地切换）----
    // 技能中（E 技 / Q 能量技）→ 形态①；普攻蒙太奇在播 → 形态②（先验窗口）；其余 → 形态③。
    const bool bSkillKept = (bIsSkillCasting || bIsEnergySkillCasting);
    const bool bComboPlaying = (GetActiveComboMontage() != nullptr);

    if (!bSkillKept && bComboPlaying && !IsComboCancelWindowReached())
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] ★ 影响：普攻尚未进入可打断窗口，切人被挡 —— 播到该段 CancelWindowTime 后再按（与普攻衔接同一窗口）。\n"
                 "      该窗口在角色蓝图 Combo Attack Segments → 当前段 → Cancel Window Time 配置。"));
        return false;
    }

    // ---- ③ 计算新角色出生点 ----
    FVector SpawnLoc = GetActorLocation();
    FRotator SpawnRot = GetActorRotation();
    bool bNearMonster = false;
    if (bSkillKept)
    {
        SpawnLoc = FindSwitchSpawnPoint(GetActorLocation(), bNearMonster);
        // 出生朝向怪物（切人即投入战斗）
        if (AMonsterBase* Monster = FindNearestAliveMonsterForSwitch(SwitchCombatMonsterSearchRadius))
        {
            const FVector ToMonster = Monster->GetActorLocation() - SpawnLoc;
            SpawnRot = FRotator(0.0f, ToMonster.Rotation().Yaw, 0.0f);
        }
    }

    // ---- ④ spawn 新角色 ----
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    Params.Owner = GetOwner();
    ABattleCharacter* NewChar = GetWorld()->SpawnActor<ABattleCharacter>(Info.CharaClass, SpawnLoc, SpawnRot, Params);
    if (!NewChar)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Switch] ★ 影响：切人失败 —— 新角色 spawn 失败（类=%s）。检查出生点是否被堵死或类不可实例化。"),
            *Info.CharaClass->GetName());
        return false;
    }

    // ---- ④-2 ★ 在场实例注册表：新实例登记（行名 → 实例）----
    // 覆盖式 Add：若同进程上次运行残留了同名行（EndPlay 已清，但防御覆盖），用最新实例替换。
    ActiveCharaInstances.Add(TargetRow, NewChar);

    // ---- ⑤ 玩家级状态迁移：编队 + 背包运行时数据 + 角色表缓存 ----
    // ★ 必须在 Possess 之前：新角色的 HUD 构建 / 装备恢复都依赖这份数据。
    TransferPlayerStateTo(NewChar);

    // ---- ⑥ 非技能形态：继承移速（移动中切人 → 新角色动画自然衔接）----
    if (!bSkillKept && GetCharacterMovement() && NewChar->GetCharacterMovement())
    {
        NewChar->GetCharacterMovement()->Velocity = GetCharacterMovement()->Velocity;
    }

    // ---- ⑦ 操控权转移 ----
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
    {
        // 理论上到不了（RequestSwitchToTeamSlot 已验过）；防御：回收新角色
        UE_LOG(LogTemp, Warning, TEXT("[Switch] ★ 影响：切人失败 —— 当前角色没有 PlayerController（非玩家状态）。"));
        NewChar->Destroy();
        return false;
    }
    PC->Possess(NewChar); // 内部自动 UnPossess 本实例 + 相机 ViewTarget 切到新角色

    // ★ 切人修复核心：Possess 走 DispatchRestart(false)，只调 Restart() 不调 PawnClientRestart()，
    //   因此 SetupPlayerInputComponent 不会被引擎调用 → 新角色 C++ 输入（含 1/2/3 切人键）整体丢失。
    //   这里手动补绑（幂等：已有 InputComponent 则跳过），否则「切不回去」。
    NewChar->EnsurePlayerInputBound();

    // 新角色的 HUD：BeginPlay 时它还没被 Possess（Controller 空）建不了 → 这里补建。
    NewChar->BuildSwitchTeamHUD();

    // ---- ⑦-2 相机角度校准（仅非战斗切人）----
    // ★ 根因：新角色 BeginPlay 把 CameraWorldYaw 硬重置为 0，而切人没迁移相机角度 →
    //   ① 镜头瞬间跳回默认朝向（抖动）；② 移动方向基准（UpdateMovement 用 CameraWorldYaw
    //   定方向）跟着变 → 移动方向翻转。
    //   仅形态③（非战斗原地切人）对齐相机：战斗态（技能保留①/普攻衔接②）相机由战斗逻辑
    //   接管（如朝向怪物），不该被旧角度覆盖。
    if (!bSkillKept && !bComboPlaying)
    {
        NewChar->CameraWorldYaw = CameraWorldYaw;
        NewChar->CameraWorldPitch = CameraWorldPitch;
        NewChar->UpdateCameraRotation();
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] 非战斗切人：相机角度已对齐（Yaw=%.1f°，Pitch=%.1f°）——避免镜头抖动与移动方向翻转。"),
            NewChar->CameraWorldYaw, NewChar->CameraWorldPitch);
    }

    // ---- ⑧ 恢复该角色类上次装备的背包武器（切回时武器跟着回来；占用表同步重建）----
    NewChar->TryRestoreEquippedWeaponFromClaim();

    // ---- ⑨ CD：换上 + 换下两个角色行名各自独立进 CD（★ 在销毁旧实例之前写，static 表不受销毁影响）----
    // ★ 用进程级单调时钟 FPlatformTime::Seconds()（与 GetSwitchCooldownRemaining 读侧一致），
    //   不能混用世界时间——世界时间跨 PIE 轮次归零，会导致 static 表里残留的 Until 让下一轮 CD 虚高。
    const double Now = FPlatformTime::Seconds();
    const FName MyRow = GetMyCharaRow();
    if (!MyRow.IsNone())
    {
        SwitchCooldownUntilRow.Add(MyRow, Now + SwitchCooldown);
    }
    SwitchCooldownUntilRow.Add(TargetRow, Now + SwitchCooldown);

    // ---- ⑩ 结论日志（先打日志再处置旧实例，避免销毁后访问 this）----
    const TCHAR* FormText = bSkillKept
        ? TEXT("技能保留切人（旧角色留场放完技能后消失，新角色登场于怪物附近）")
        : (bComboPlaying
            ? TEXT("普攻衔接切人（新角色原地登场并自动接普攻）")
            : TEXT("原地切人"));
    UE_LOG(LogTemp, Warning,
        TEXT("[Switch] ★ 切人成功（%s）：操控 %s → %s｜两队位置独立 CD %.1fs｜出生点=%s（%s）。"),
        FormText,
        MyRow.IsNone() ? TEXT("(未知行)") : *MyRow.ToString(),
        *TargetRow.ToString(),
        SwitchCooldown,
        bSkillKept ? (bNearMonster ? TEXT("怪物附近") : TEXT("怪物附近（半径内无存活怪 → 侧移退化）")) : TEXT("原位置"),
        *SpawnLoc.ToCompactString());

    // ---- ⑪ 旧角色处置（必须最后做；Destroy 之后不得再访问 this）----
    if (bSkillKept)
    {
        // 形态①：留着把技能放完。屏幕 HUD 立刻交出去（新实例已建自己的，不移除会叠两层），
        // 武器保留（技能演出手里要有刀），技能放完由 EndSkill 收敛点统一清理 + 自毁。
        // ★ 切人输入映射此刻就要摘掉：旧角色留场期间若还挂着 1/2/3 映射，会同优先级遮蔽
        //   新角色的切人键 → 技能期间「切不回去」（2026-09-18 实锤根因）。
        RemoveSwitchInputMapping();
        CleanupRetiredScreenUI();
        bPendingDestroyAfterSkillFinish = true;
    }
    else
    {
        // 形态②③：立即退役 —— 清屏幕 UI + 释放武器占用 + 销毁武器 Actor + 销毁实例
        // ★ 主动摘除切人映射（与形态①一致）：Destroy 延迟到帧末才走 EndPlay 清理，当帧内
        //   旧 IMC 仍会遮蔽新角色切人键；这里当帧就摘，避免短暂失灵。
        RemoveSwitchInputMapping();
        ReleaseWeaponClaimForRetire();
        CleanupRetiredScreenUI();
        RemoveSwitchTeamHUD();
        UnequipWeapon(); // 销毁手中的武器 Actor，防止武器残影留在场上
        Destroy();
    }

    // ---- ⑫ 新角色自动打出第 N 段普攻（蓝图可配 SwitchInAttackComboStep，0 基）----
    // ★ 段数从【新角色】读（各角色蓝图各自设置），不读旧角色。
    //   任意切人形态都触发：技能中切人（①，出生怪物附近朝怪物）/ 普攻中切人（②，原地）/
    //   非战斗原地切人（③，原地）——新角色登场即自动打出配置段数的普攻。
    // ★ 战斗门控（新增）：仅当附近存在「已进入战斗」的怪物（仇恨已建立）才自动普攻；
    //   脱战/未引起仇恨时切人不自动普攻，避免空挥。
    if (NewChar->HasMonsterInCombat())
    {
        const int32 Step = FMath::Clamp(NewChar->SwitchInAttackComboStep, 0, FMath::Max(0, NewChar->MaxComboStep - 1));
        NewChar->CurrentComboStep = Step;
        NewChar->PerformComboAttack(Step);
        UE_LOG(LogTemp, Log, TEXT("[Switch] 新角色自动打出第 %d 段普攻（SwitchInAttackComboStep=%d，0 基）。"), Step + 1, NewChar->SwitchInAttackComboStep);
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("[Switch] 附近无已进入战斗的怪物（未建立仇恨），切人不自动普攻。"));
    }

    // ---- ⑬ 输入自愈校验：下一帧复核新角色的 1/2/3 映射是否真的在输入栈里 ----
    // 旧实例 Destroy（含其 IMC 清理）发生在本帧末尾，晚于新实例 AddMappingContext ——
    // 一旦清理失效残留悬空映射，会污染 EnhancedInput 求值 → 1/2/3 静默失灵。
    // 下一帧校验能在出问题的当场补回并打告警（CreateUObject 绑定新实例，若它销毁委托自动失效）。
    NewChar->GetWorldTimerManager().SetTimerForNextTick(
        FTimerDelegate::CreateUObject(NewChar, &ABattleCharacter::VerifySwitchInputAfterPossess));

    return true;
}

bool ABattleCharacter::ResumeControlToExistingInstance(ABattleCharacter* ExistingChar, FName TargetRow)
{
    if (!ExistingChar || !IsValid(ExistingChar) || ExistingChar == this)
    {
        return false;
    }

    // ---- ① 记录当前实例形态（用于切走后的处置：技能中留场，否则退役销毁）----
    const bool bSkillKept = (bIsSkillCasting || bIsEnergySkillCasting);

    // ---- ② 恢复目标实例的「待自毁」状态：切回就是要继续用它，不能再让它放完技能自毁 ----
    ExistingChar->bPendingDestroyAfterSkillFinish = false;

    // ---- ③ 恢复目标实例的血条显示（切走时被 CleanupRetiredScreenUI 隐藏了）----
    if (ExistingChar->HealthBarWidget)
    {
        ExistingChar->HealthBarWidget->SetHiddenInGame(false);
        ExistingChar->HealthBarWidget->SetVisibility(true);
    }

    // ---- ④ 重新 Possess 目标实例（内部自动 UnPossess 当前实例 + 相机 ViewTarget 切换）----
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] ★ 影响：切回失败 —— 当前角色没有 PlayerController。"));
        return false;
    }
    PC->Possess(ExistingChar);

    // ---- ⑤ 补绑输入（幂等）+ 重建 HUD（留场实例切走时 HUD 已移除）----
    ExistingChar->EnsurePlayerInputBound();
    // ★ 恢复切人映射：该留场实例切走时（形态①）RemoveSwitchInputMapping 已把 bSwitchContextAdded
    //   置 false、映射摘除；EnsurePlayerInputBound 因 bPlayerInputBound 已 true 不会重跑 Setup、
    //   也就不会重挂映射。这里幂等重挂（bSwitchContextAdded=false → 重新 AddMappingContext），
    //   否则切回后 1/2/3 键失灵（要等下一帧 VerifySwitchInputAfterPossess 才自愈，且当帧有窗口）。
    ExistingChar->EnsureSwitchInput();
    ExistingChar->BuildSwitchTeamHUD();

    // ---- ⑤-2 ★ 恢复技能 UI（WBP_Skill）：切走时被 CleanupRetiredScreenUI RemoveFromParent 置空，
    //   切回留场角色（技能中切人）后必须重建，否则 WBP_Skill 一直不可视（2026-09-17 bug）。
    //   InitSkillIconUI 幂等（SkillIconUIWidget 非空即 return），安全复用。----
    ExistingChar->InitSkillIconUI();

    // ---- ⑥ 相机角度对齐：切回留场实例时，把它相机角度对齐到当前操控实例（避免镜头跳变）----
    // 留场实例切走时相机没被带走，切回时若它 CameraWorldYaw 还是切走前的旧值，会和当前
    // 视角不一致。这里沿用当前实例的角度，保证「切回即所见」。
    ExistingChar->CameraWorldYaw = CameraWorldYaw;
    ExistingChar->CameraWorldPitch = CameraWorldPitch;
    ExistingChar->UpdateCameraRotation();

    // ---- ⑦ CD：换上 + 换下两个角色行名各自独立进 CD（与正常切人一致）----
    const double Now = FPlatformTime::Seconds();
    const FName MyRow = GetMyCharaRow();
    if (!MyRow.IsNone())
    {
        SwitchCooldownUntilRow.Add(MyRow, Now + SwitchCooldown);
    }
    SwitchCooldownUntilRow.Add(TargetRow, Now + SwitchCooldown);

    // ---- ⑧ 结论日志 ----
    UE_LOG(LogTemp, Warning,
        TEXT("[Switch] ★ 切回已在场角色（%s）：操控归还给留场实例，未重复 spawn。CD %.1fs。"),
        *TargetRow.ToString(), SwitchCooldown);

    // ---- ⑨ 处置当前实例（切走；与正常切人⑪段一致）----
    if (bSkillKept)
    {
        // ★ 切人输入映射此刻就要摘掉（同 ExecuteSwitchToRow 形态①）：切走时若本实例
        //   留场放技能还挂着 1/2/3 映射，会遮蔽被切回角色的切人键。
        RemoveSwitchInputMapping();
        CleanupRetiredScreenUI();
        bPendingDestroyAfterSkillFinish = true;
    }
    else
    {
        // ★ 主动摘除切人映射（同 ExecuteSwitchToRow 形态②③）：Destroy 延迟到帧末，当帧旧 IMC 遮蔽被切回角色的切人键。
        RemoveSwitchInputMapping();
        ReleaseWeaponClaimForRetire();
        CleanupRetiredScreenUI();
        RemoveSwitchTeamHUD();
        UnequipWeapon();
        Destroy();
    }

    // ---- ⑩ 切回后下一帧输入自愈校验（与正常切人一致，防悬空映射）----
    ExistingChar->GetWorldTimerManager().SetTimerForNextTick(
        FTimerDelegate::CreateUObject(ExistingChar, &ABattleCharacter::VerifySwitchInputAfterPossess));

    return true;
}

FName ABattleCharacter::FindCharaRowForClass(const UClass* CharaClass)
{
    if (!CharaClass || !EnsureCharaInfoTable())
    {
        return NAME_None;
    }

    for (const FName RowName : CharaInfoTable->GetRowNames())
    {
        if (const FCharaInfoEntry* Entry = FindCharaInfo(RowName))
        {
            // ★ 严格相等，不用 IsChildOf：BP_Chara_magic 继承 BP_PlayerCharacter 时，
            //   用 IsChildOf 会把「父类的那一行」也命中（行序先到先得）→ 反查出行名 10002 而不是 10001。
            if (Entry->CharaClass && Entry->CharaClass.Get() == CharaClass)
            {
                return RowName;
            }
        }
    }
    return NAME_None;
}

FName ABattleCharacter::GetMyCharaRow()
{
    if (!CachedMyCharaRow.IsNone())
    {
        return CachedMyCharaRow;
    }
    CachedMyCharaRow = FindCharaRowForClass(GetClass());
    return CachedMyCharaRow;
}

AMonsterBase* ABattleCharacter::FindNearestAliveMonsterForSwitch(float Radius)
{
    // 锁定目标优先（与普攻/技能索敌同一目标 → 切人后打的就是正在打的那只）
    if (AMonsterBase* Locked = GetValidLockedTarget())
    {
        return Locked;
    }

    if (!GetWorld())
    {
        return nullptr;
    }

    TArray<AActor*> Actors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), AMonsterBase::StaticClass(), Actors);

    AMonsterBase* Best = nullptr;
    float BestDistSq = Radius * Radius;
    for (AActor* Actor : Actors)
    {
        AMonsterBase* Monster = Cast<AMonsterBase>(Actor);
        if (!Monster || Monster->IsDead())
        {
            continue;
        }
        const float DistSq = FVector::DistSquared(GetActorLocation(), Monster->GetActorLocation());
        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            Best = Monster;
        }
    }
    return Best;
}

FVector ABattleCharacter::FindSwitchSpawnPoint(const FVector& OldLoc, bool& bOutNearMonster)
{
    bOutNearMonster = false;

    // 无怪兜底：旧位置侧移（切向，保证不与旧角色重合）
    auto FallbackSideStep = [this, &OldLoc]() -> FVector
    {
        const FVector Side = FVector::CrossProduct(FVector::UpVector, GetActorForwardVector()).GetSafeNormal();
        return OldLoc + Side * FMath::Max(250.0f, SwitchMinSeparation);
    };

    AMonsterBase* Monster = FindNearestAliveMonsterForSwitch(SwitchCombatMonsterSearchRadius);
    if (!Monster)
    {
        return FallbackSideStep();
    }
    bOutNearMonster = true;

    // 基准方向：怪 → 旧角色（进场点落在怪物「面向玩家」的斜侧位，而不是怪脸上）
    const FVector AwayFromMonster = OldLoc - Monster->GetActorLocation();
    const float BaseYaw = (AwayFromMonster.SizeSquared() > 1.0f)
        ? AwayFromMonster.Rotation().Yaw
        : GetActorRotation().Yaw;

    // 候选：斜侧 ±45° → 正侧 ±90° → 玩家背后 180°，取第一个「与旧角色拉开足够距离」的点
    static const float CandidateDeltas[] = { 45.0f, -45.0f, 90.0f, -90.0f, 180.0f };
    for (const float Delta : CandidateDeltas)
    {
        const FVector Dir = FVector::ForwardVector.RotateAngleAxis(BaseYaw + Delta, FVector::UpVector);
        FVector Candidate = Monster->GetActorLocation() + Dir * SwitchSpawnDistanceFromMonster;

        if (FVector::Dist(Candidate, OldLoc) < SwitchMinSeparation)
        {
            continue;
        }

        // 贴地：向下射线把出生点放到地面上（防止出生在怪身体高度/悬空）
        FHitResult Hit;
        FCollisionQueryParams QueryParams;
        QueryParams.AddIgnoredActor(Monster);
        QueryParams.AddIgnoredActor(this);
        if (GetWorld()->LineTraceSingleByChannel(Hit,
                Candidate + FVector(0.0f, 0.0f, 200.0f),
                Candidate - FVector(0.0f, 0.0f, 500.0f),
                ECC_Visibility, QueryParams))
        {
            Candidate.Z = Hit.ImpactPoint.Z;
        }
        return Candidate;
    }

    // 全部候选都太近 → 侧移兜底
    return FallbackSideStep();
}

void ABattleCharacter::TransferPlayerStateTo(ABattleCharacter* Target)
{
    if (!Target || Target == this)
    {
        return;
    }

    // ---- 编队数据（切人后 L 键编队 / 1/2/3 键切人都依赖这份数据跟着「玩家」走）----
    Target->EnsureTeamsArray();
    Target->Teams = Teams;
    Target->CurrentTeamIndex = CurrentTeamIndex;
    Target->OwnedCharaRows = OwnedCharaRows;
    // 角色表缓存（懒加载结果直接带过去，省一次加载）
    Target->CharaInfoTable = CharaInfoTable;

    // ---- 背包运行时数据（背包是玩家资产，不是角色资产；不迁移 = 切人后背包清空）----
    // 只搬「运行时真源」，网格/颜色等蓝图配置留在新角色自己的组件上（继承自父类蓝图，本就一致）。
    if (InventoryComponent && Target->InventoryComponent)
    {
        Target->InventoryComponent->AllItems = InventoryComponent->AllItems;
        Target->InventoryComponent->bLoaded = InventoryComponent->bLoaded;
        Target->InventoryComponent->RuntimeAcquisitions = InventoryComponent->RuntimeAcquisitions;
        Target->InventoryComponent->BonusCounts = InventoryComponent->BonusCounts;
        Target->InventoryComponent->ItemCatalog = InventoryComponent->ItemCatalog;
        Target->InventoryComponent->ToolTableRowNames = InventoryComponent->ToolTableRowNames;
        Target->InventoryComponent->DropCatalog = InventoryComponent->DropCatalog;
        Target->InventoryComponent->ConflictingRowNames = InventoryComponent->ConflictingRowNames;
    }
}

void ABattleCharacter::ReleaseWeaponClaimForRetire()
{
    if (EquippedWeaponRow.IsNone())
    {
        return;
    }

    // ① 释放唯一装备占用：不释放的话这把武器永远显示「已装备」却没人拿着（死锁）
    WeaponInstanceOwner.Remove(MakeWeaponInstanceKey(EquippedWeaponRow, EquippedWeaponInstanceNo));

    // ② 记到 per-class 表：切回（重新 spawn 本类）时按它恢复装备
    ClassEquippedBagRow.Add(GetClass(), EquippedWeaponRow);

    EquippedWeaponRow = NAME_None;
    bEquippedWeaponRowFromBag = false;
}

void ABattleCharacter::TryRestoreEquippedWeaponFromClaim()
{
    // 切人流程在数据迁移完成后调用（BeginPlay 时背包还是空的，不能在那里调）
    const FName* Claimed = ClassEquippedBagRow.Find(GetClass());
    if (!Claimed || Claimed->IsNone())
    {
        return;
    }
    if (!EquippedWeaponRow.IsNone())
    {
        // 手上已有装备行（异常时序），不清记录直接退出
        return;
    }

    bool bRestored = false;
    FBagItemEntry Item;
    if (InventoryComponent
        && InventoryComponent->GetItemByRow(*Claimed, Item)
        && IsWeaponItem(Item)
        && CanEquipWeaponItem(Item))
    {
        // 卸掉 BeginPlay 自动装的默认武器，再装回上次那把（内部会重建占用）
        UnequipWeapon();
        bRestored = EquipWeaponFromItemRow(*Claimed, Item.InstanceNo);
    }

    if (bRestored)
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] %s 切回 → 已恢复上次装备的武器（行『%s』）。"),
            *GetCharacterDisplayName(), *Claimed->ToString());
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] %s 切回 → 上次装备的武器（行『%s』）已不可用（背包里没了/职位不符/被占用），保持默认武器。"),
            *GetCharacterDisplayName(), *Claimed->ToString());
    }

    // 无论成败都清记录：成功=已恢复；失败=别每次切回都反复试同一把
    ClassEquippedBagRow.Remove(GetClass());
}

void ABattleCharacter::CleanupRetiredScreenUI()
{
    // 本实例创建的屏幕 UI 一律摘除（切人后新实例会建自己的；不摘会叠两层）。
    // 注意不走 Close* 函数：那些函数会拿 GetController() 恢复输入模式，
    // 而退役实例已被 UnPossess（Controller 为 null）→ 半关闭状态。
    if (SkillIconUIWidget)
    {
        SkillIconUIWidget->RemoveFromParent();
        SkillIconUIWidget = nullptr;
    }
    if (LockOnIndicatorWidget)
    {
        LockOnIndicatorWidget->RemoveFromParent();
        LockOnIndicatorWidget = nullptr;
    }
    if (PerfectDodgeUIWidget)
    {
        PerfectDodgeUIWidget->RemoveFromParent();
        PerfectDodgeUIWidget = nullptr;
    }
    // 模态面板（角色面板/背包/编队）打开时游戏处于暂停态、1/2/3 不会触发；
    // 这里纯防御：万一开着就直接摘，防止销毁后残留。
    if (CharacterPanelWidget)
    {
        CharacterPanelWidget->RemoveFromParent();
        CharacterPanelWidget = nullptr;
        bCharacterPanelOpen = false;
    }
    if (BagWidget)
    {
        BagWidget->RemoveFromParent();
        BagWidget = nullptr;
        bBagOpen = false;
    }
    if (CharaTeamWidget)
    {
        CharaTeamWidget->RemoveFromParent();
        CharaTeamWidget = nullptr;
    }
    // ★ 血条（WBP_HealthBar 的 WidgetComponent）也要随退役隐藏：切走时旧角色若留场（技能保留形态）
    //   血条会残留和新角色叠加；非留场形态旧角色虽会 Destroy，但显式隐藏保证「切走即消失」，
    //   直到该角色下一次被切回（重新 spawn → BeginPlay 重新初始化血条显示）。
    if (HealthBarWidget)
    {
        HealthBarWidget->SetHiddenInGame(true);
        HealthBarWidget->SetVisibility(false);
    }
}

// ---- 切人输入：动态 IA/IMC（数字键 1/2/3），与背包 B 键同一套 Ensure 幂等模式 ----
bool ABattleCharacter::EnsureSwitchInput()
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] %s 的 1/2/3 输入没法装：此刻没有 PlayerController（未 Possess）。"), *GetName());
        return false;
    }
    UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] %s 的 1/2/3 输入没法装：EnhancedInput 子系统不存在（LocalPlayer 未就绪）。"), *GetName());
        return false;
    }

    if (!SwitchSlotAction1 || !SwitchSlotAction2 || !SwitchSlotAction3)
    {
        auto MakePressedAction = [this](UInputAction*& OutAction)
        {
            if (!OutAction)
            {
                OutAction = NewObject<UInputAction>(this);
                OutAction->ValueType = EInputActionValueType::Boolean;
                OutAction->Triggers.Add(NewObject<UInputTriggerPressed>(OutAction));
            }
        };
        MakePressedAction(SwitchSlotAction1);
        MakePressedAction(SwitchSlotAction2);
        MakePressedAction(SwitchSlotAction3);
    }

    if (!SwitchMappingContext)
    {
        SwitchMappingContext = NewObject<UInputMappingContext>(this);
        SwitchMappingContext->MapKey(SwitchSlotAction1, EKeys::One);
        SwitchMappingContext->MapKey(SwitchSlotAction2, EKeys::Two);
        SwitchMappingContext->MapKey(SwitchSlotAction3, EKeys::Three);
    }

    if (!bSwitchContextAdded)
    {
        // 优先级 2（与背包/编队同层，高于步行 Ctrl 的 1）：1/2/3 未被其他动作占用
        Subsystem->AddMappingContext(SwitchMappingContext, 2);
        bSwitchContextAdded = true;
    }

    return true;
}

// ---- 移除本实例的切人 1/2/3 输入映射（形态①技能保留切人、旧角色留场退役时调用）----
// ★ 根因（2026-09-18）：形态①旧角色留场放技能、不立即销毁，其 SwitchMappingContext 仍挂在
//   EnhancedInput 输入栈里（优先级 2），与新角色同优先级同键（1/2/3）互相遮蔽 —— 旧角色先
//   注册、遮蔽新角色。技能期间按切回键，引擎只触发旧角色的 IA，而旧角色已被 UnPossess
//   （GetController()->GetPawn() != 旧角色）→ RequestSwitchToTeamSlot 第一道闸静默 return（无日志），
//   新角色回调根本不被触发 → 技能期间「切不回去」。必须在此刻就摘掉旧角色映射。
// ★ Subsystem 不能用 GetController() 拿：旧角色已被 UnPossess，Controller 已 null；
//   改从 World 拿本地玩家（与 EndPlay 清理同一套取法）。
void ABattleCharacter::RemoveSwitchInputMapping()
{
    if (!bSwitchContextAdded || !SwitchMappingContext)
    {
        return;
    }

    UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
    if (UWorld* World = GetWorld())
    {
        if (ULocalPlayer* LP = World->GetFirstLocalPlayerFromController())
        {
            Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LP);
        }
    }

    if (Subsystem)
    {
        Subsystem->RemoveMappingContext(SwitchMappingContext);
        bSwitchContextAdded = false;
        UE_LOG(LogTemp, Log,
            TEXT("[Switch] %s 留场退役：已摘除 1/2/3 切人映射（避免残留遮蔽新角色的切人键）。"),
            *GetName());
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Switch] ★ 影响：%s 留场退役时拿不到 EnhancedInput 子系统，1/2/3 映射未摘除 —— 可能遮蔽新角色切人键导致技能期间切不回去。"),
            *GetName());
    }
}


// ---- 切人后下一帧的输入自愈校验 ----
// Possess 时引擎链（OnPossess → ClientRestart → PawnClientRestart）正常会调 SetupPlayerInputComponent，
// 1/2/3 就绑好了。但旧实例销毁晚于新实例注册映射，若旧 IMC 清理失效（残留在 subsystem）会污染映射栈，
// 表现成「资产型输入（普攻/移动）正常、运行时 1/2/3 静默失灵」。这里下一帧复核：映射不在栈里就补回。
void ABattleCharacter::VerifySwitchInputAfterPossess()
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] 切人后自愈校验：%s 没有 PlayerController，跳过。"), *GetName());
        return;
    }
    UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
    if (!Subsystem)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] 切人后自愈校验：%s 拿不到 EnhancedInput 子系统，跳过。"), *GetName());
        return;
    }

    // 幂等补建：IA/IMC 对象若悬空（随旧实例被 GC 的引用迁移场景）会在这里重建
    if (!EnsureSwitchInput())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] ★ 影响：切人后自愈校验失败 —— %s 的 1/2/3 输入链不完整，按 1/2/3 将无反应。"), *GetName());
        return;
    }

    if (SwitchMappingContext && bSwitchContextAdded && !Subsystem->HasMappingContext(SwitchMappingContext))
    {
        // IMC 曾加过但此刻不在栈里（被移除/污染挤掉）→ 补回
        Subsystem->AddMappingContext(SwitchMappingContext, 2);
        UE_LOG(LogTemp, Warning,
            TEXT("[Switch] ★ 影响：切人后 %s 的 1/2/3 映射不在输入栈里（疑似被旧实例残留映射污染挤掉）→ 已补回。"),
            *GetName());
    }

    UE_LOG(LogTemp, Log, TEXT("[Switch] 切人后自愈校验：%s 输入就位（IMC 在栈=%d）。"),
        *GetName(),
        (SwitchMappingContext && Subsystem->HasMappingContext(SwitchMappingContext)) ? 1 : 0);
}

// ---- 幂等补绑输入组件（切人修复核心）----
// ★ 根因：AController::Possess → OnPossess → Pawn->DispatchRestart(false) → 只调 Restart()，
//   不调 PawnClientRestart() → SetupPlayerInputComponent 不会被调用（引擎 Pawn.cpp / Controller.cpp 实锤）。
//   因此切人 spawn 的新角色 C++ 输入（含 1/2/3 切人键、普攻、技能等）整体丢失 —— 「切不回去」。
//   这里在 Possess 后手动补上「创建 InputComponent + SetupPlayerInputComponent + 注册」这条链。
void ABattleCharacter::EnsurePlayerInputBound()
{
    // ★ 取证日志：区分「引擎链已调过 Setup（标志=true，静默 return 正常）」vs「从没人绑过（标志=false，走补绑）」。
    UE_LOG(LogTemp, Log, TEXT("[Switch] EnsurePlayerInputBound：%s（已绑标志=%d，输入组件=%s）。"),
        *GetName(), bPlayerInputBound ? 1 : 0, InputComponent ? TEXT("非空") : TEXT("空"));
    // ★ 修复「切不回去」：用显式标志判断，而不是 InputComponent 是否非空。
    //   切人 spawn 的新角色 InputComponent 可能已在别的路径被建出（非空），但
    //   SetupPlayerInputComponent 从未被调用 → 旧写法 `if(InputComponent) return;`
    //   会误判「已绑」而跳过，1/2/3 键与全部 C++ 输入整体失灵。
    if (bPlayerInputBound)
    {
        return;
    }

    // 手动走 PawnClientRestart 里被跳过的三段：创建 → 绑定 → 注册
    if (!InputComponent)
    {
        InputComponent = CreatePlayerInputComponent();
    }
    if (!InputComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] ★ 影响：新角色输入组件创建失败 —— 1/2/3 键与 C++ 按键将整体失灵。"));
        return;
    }

    SetupPlayerInputComponent(InputComponent);
    if (!InputComponent->IsRegistered())
    {
        InputComponent->RegisterComponent();
    }

    // 蓝图输入委托绑定（原 PawnClientRestart 里 UInputDelegateBinding::BindInputDelegatesWithSubojects 这段也补上）
PRAGMA_DISABLE_DEPRECATION_WARNINGS
    if (UInputDelegateBinding::SupportsInputDelegate(GetClass()))
PRAGMA_ENABLE_DEPRECATION_WARNINGS
    {
        InputComponent->bBlockInput = bBlockInput;
        UInputDelegateBinding::BindInputDelegatesWithSubojects(this, InputComponent);
    }

    bPlayerInputBound = true;

    UE_LOG(LogTemp, Log, TEXT("[Switch] 已为 %s 补绑输入组件（Possess 后手动补 SetupPlayerInputComponent）。"),
        *GetName());
}

// 三个按键回调：转发到 RequestSwitchToTeamSlot（槽位 = 键号 - 1）。
// OnClicked/BindAction 这类宏与委托不支持带参绑定，参照 chara_1~4 的专用回调模式。
// ★ 入口取证日志：这条没出现 = 输入绑定/映射链失灵（查 EnsureSwitchInput 链）；
//   出现了但后续被拒 = 逻辑闸门（看紧随其后的 [Switch] 拒绝分支日志）。
void ABattleCharacter::OnSwitchSlot1Pressed()
{
    UE_LOG(LogTemp, Log, TEXT("[Switch] 按下 1 键（回调已触发）→ 切换到编队第 1 位。"));
    RequestSwitchToTeamSlot(0);
}
void ABattleCharacter::OnSwitchSlot2Pressed()
{
    UE_LOG(LogTemp, Log, TEXT("[Switch] 按下 2 键（回调已触发）→ 切换到编队第 2 位。"));
    RequestSwitchToTeamSlot(1);
}
void ABattleCharacter::OnSwitchSlot3Pressed()
{
    UE_LOG(LogTemp, Log, TEXT("[Switch] 按下 3 键（回调已触发）→ 切换到编队第 3 位。"));
    RequestSwitchToTeamSlot(2);
}

// =====================================================================
// ---- 切人 HUD（右上角编队头像：当前高亮框 + CD 进度条/秒数 + 槽位数字）----
// =====================================================================
// 两套路径：
//   ① WBP 路径（推荐）：蓝图上配 SwitchHUDClass = WBP_SwitchHUD，C++ 按名字 FindWidget 填数据。
//      控件约定名（WBP 内）：slot_0~2（每个格子容器，内含 frame/img_icon/txt_key/cd_mask/txt_cd）。
//   ② 纯 C++ 兜底（没配 WBP 时）：代码搭树。每格结构（自底向上）：
//      金色高亮框（72×92，仅当前操控者显示）→ 深色底（66×86 居中，自身留出 3px 金边）
//      → 头像（60×60 顶部居中）→ 槽位数字（左上角小字）→ CD 进度条（66×86，半透明黑）
//      → CD 剩余秒数（居中）。

// HUD 展示用的队伍成员（含「未编队默认出战」兜底）。
// ★ 需求：一进游戏右上角就该显示编队 HUD，即使还没编队。默认出战的角色（GameMode 配的
//   DefaultPawnClass，本项目是 BP_PlayerCharacter）也算「编了队」，slot_0 显示它自己。
//   注意：只改「展示用」的数组，不写回 Teams——真编队数据仍以编队界面为准。
//   Build / Update 两处共用本函数，保证比较口径一致（否则每帧误判「队伍变了」无限重建）。
TArray<FName> ABattleCharacter::GetEffectiveTeamMembersForHUD()
{
    EnsureTeamsArray();
    TArray<FName> Effective = GetTeamMembers(CurrentTeamIndex);

    bool bAnyMember = false;
    for (const FName& Row : Effective)
    {
        if (!Row.IsNone())
        {
            bAnyMember = true;
            break;
        }
    }

    if (!bAnyMember)
    {
        const FName MyRow = GetMyCharaRow();
        if (!MyRow.IsNone())
        {
            Effective[0] = MyRow;
            // ★ 日志不放这里：Update 每 Tick 调本函数做比较，放这会每帧刷屏（60fps 一条）。
            //   「默认出战」结论改在 BuildSwitchTeamHUD（真正构建时）打一次。
        }
    }

    return Effective;
}

void ABattleCharacter::BuildSwitchTeamHUD()
{
    RemoveSwitchTeamHUD();

    if (!GetWorld())
    {
        return;
    }

    EnsureTeamsArray();

    EnsureCharaInfoTable();

    // ---- 默认出战兜底：未编队（队伍全空）时，把「当前操控角色自己」视作 slot_0 成员显示 ----
    const TArray<FName> EffectiveMembers = GetEffectiveTeamMembersForHUD();
    {
        const TArray<FName>& RawMembers = GetTeamMembers(CurrentTeamIndex);
        bool bRawAnyMember = false;
        for (const FName& Row : RawMembers)
        {
            if (!Row.IsNone()) { bRawAnyMember = true; break; }
        }
        if (!bRawAnyMember && EffectiveMembers.IsValidIndex(0) && !EffectiveMembers[0].IsNone())
        {
            UE_LOG(LogTemp, Log,
                TEXT("[Switch] 队伍 %d 未编队，默认出战 %s 视作 slot_0 成员显示（仅展示；编队数据不变）。"),
                CurrentTeamIndex + 1, *EffectiveMembers[0].ToString());
        }
    }

    // ---- 路径①：蓝图配了 WBP → 按名字 FindWidget 填数据（可视化 UI）----
    if (SwitchHUDClass)
    {
        // 传完整 3 槽（含 NAME_None 空槽）→ 空槽隐藏、有角色槽显示
        BuildSwitchTeamHUD_FromWBP(EffectiveMembers);
        return;
    }

    // ---- 路径②：没配 WBP → 纯 C++ 兜底搭树 ----
    // ★ UUserWidget 是 Abstract 类（UE5 起），CreateWidget(UUserWidget::StaticClass()) 必然报
    //   「抽象、废弃或替代类不能被用于构造用户控件。UserWidget 即为其中之一。」并返回 null。
    //   这里需要非抽象宿主类；但工程当前没有现成的，兜底路径用「运行时创建宿主」不可行时直接告警。
    //   —— 为规避 Abstract 限制，兜底改用 WBP 未配时的明确日志，不再尝试非法构造。
    UE_LOG(LogTemp, Warning,
        TEXT("[Switch] ★ 影响：切人 HUD 未显示 —— 没配 SwitchHUDClass（WBP_SwitchHUD），纯 C++ 兜底暂不可用。\n"
             "      修法：BP_PlayerCharacter / BP_Chara_magic → Switch → Switch HUD Class → 选 WBP_SwitchHUD。"));
    return;
}

// ---- 路径①：从 WBP 按名字取控件并填数据 ----
// 深度查找：GetWidgetFromName 只查直接子级，格子内部的 frame/img_icon/cd_mask/txt_cd
// 可能包在 Overlay/Border 里或嵌套 UserWidget 里 → 递归下钻按名字找。
static UWidget* FindSwitchHUDNamedWidget(UWidget* Root, const TCHAR* Name)
{
    if (!Root)
    {
        return nullptr;
    }
    if (Root->GetFName() == FName(Name) || Root->GetName() == Name)
    {
        return Root;
    }
    // 嵌套 UserWidget：下钻到它的 RootWidget
    if (UUserWidget* Nested = Cast<UUserWidget>(Root))
    {
        return FindSwitchHUDNamedWidget(Nested->GetRootWidget(), Name);
    }
    if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
    {
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
        {
            if (UWidget* Found = FindSwitchHUDNamedWidget(Panel->GetChildAt(i), Name))
            {
                return Found;
            }
        }
    }
    return nullptr;
}

// ★ 前缀匹配版：WBP 里把 slot_0 复制成 slot_1/slot_2 时，UMG 会给内部控件自动加序号后缀
//   （frame → frame_1/frame_2、img_icon → img_icon_1/...）。精确名匹配全部 miss →
//   格子 2/3 的头像/高亮框/CD 全不显示（2026-09-17 实锤）。
//   搜索范围已限定在「该槽子树」内，同类控件每槽唯一 → 前缀匹配安全。
static UWidget* FindSwitchHUDNamedWidgetPrefix(UWidget* Root, const TCHAR* Name)
{
    if (!Root)
    {
        return nullptr;
    }
    if (Root->GetName().StartsWith(Name))
    {
        return Root;
    }
    if (UUserWidget* Nested = Cast<UUserWidget>(Root))
    {
        return FindSwitchHUDNamedWidgetPrefix(Nested->GetRootWidget(), Name);
    }
    if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
    {
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
        {
            if (UWidget* Found = FindSwitchHUDNamedWidgetPrefix(Panel->GetChildAt(i), Name))
            {
                return Found;
            }
        }
    }
    return nullptr;
}

// 格子内部控件查找：先精确名，miss 后前缀兜底（命中 frame_N/img_icon_N/cd_mask_N/txt_cd_N）。
static UWidget* FindSwitchHUDCellChildWidget(UWidget* Root, const TCHAR* Name)
{
    if (UWidget* Exact = FindSwitchHUDNamedWidget(Root, Name))
    {
        return Exact;
    }
    return FindSwitchHUDNamedWidgetPrefix(Root, Name);
}

void ABattleCharacter::BuildSwitchTeamHUD_FromWBP(const TArray<FName>& Rows)
{
    bSwitchHUDUsesWBP = true;

    SwitchTeamHUDWidget = CreateWidget<UUserWidget>(GetWorld(), SwitchHUDClass);
    if (!SwitchTeamHUDWidget)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Switch] ★ 影响：切人 HUD 建不出来 —— SwitchHUDClass（%s）创建失败。检查该 WBP 父类是否为 UUserWidget。"),
            *SwitchHUDClass->GetName());
        bSwitchHUDUsesWBP = false;
        return;
    }

    // 按名字取 3 个格子容器（固定 3 槽，对应编队 1/2/3 号位）。格子本身可能也是嵌套 UserWidget，
    // 用深度查找统一兜住（GetWidgetFromName 只查直接子级，漏嵌套）。
    // ★ 修复「空槽不可视化」：固定遍历 slot_0~2（不再按 Rows 数量截断）——
    //   有角色的槽填数据 + 显示；空槽（NAME_None）隐藏整个格子容器。
    static const TCHAR* SlotNames[3] = { TEXT("slot_0"), TEXT("slot_1"), TEXT("slot_2") };
    bool bAnyCellFound = false;
    for (int32 i = 0; i < 3; ++i)
    {
        UWidget* Cell = FindSwitchHUDNamedWidget(SwitchTeamHUDWidget->GetRootWidget(), SlotNames[i]);
        if (!Cell)
        {
            // 格子容器不存在：只对「有角色的槽」告警（空槽本来就不需要显示）
            if (Rows.IsValidIndex(i) && !Rows[i].IsNone())
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[Switch] ★ 影响：切人 HUD 第 %d 格缺失 —— WBP_SwitchHUD 里没找到控件 '%s'。\n"
                         "      修法：确认 WBP 里存在命名 %s 的格子容器。"),
                    i + 1, SlotNames[i], SlotNames[i]);
            }
            // 占位：保持各数组下标对齐（空槽/缺格都占一个 null 位）
            SwitchHudSlotWidgets.Add(nullptr);
            SwitchHudFrameWidgets.Add(nullptr);
            SwitchHudIconWidgets.Add(nullptr);
            SwitchHudMaskWidgets.Add(nullptr);
            SwitchHudCdTextWidgets.Add(nullptr);
            SwitchHudRows.Add(Rows.IsValidIndex(i) ? Rows[i] : NAME_None);
            continue;
        }

        bAnyCellFound = true;
        const bool bEmpty = !(Rows.IsValidIndex(i)) || Rows[i].IsNone();

        // ★ 空槽：隐藏整个格子容器（其余子控件不再填数据）
        if (bEmpty)
        {
            Cell->SetVisibility(ESlateVisibility::Collapsed);
            SwitchHudSlotWidgets.Add(Cell);
            SwitchHudFrameWidgets.Add(nullptr);
            SwitchHudIconWidgets.Add(nullptr);
            SwitchHudMaskWidgets.Add(nullptr);
            SwitchHudCdTextWidgets.Add(nullptr);
            SwitchHudRows.Add(NAME_None);
            continue;
        }

        // 有角色：格子容器显示
        Cell->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

        // 格子内部控件：frame（高亮框）/ img_icon（头像）/ cd_mask（进度条）/ txt_cd（秒数）
        // 统一深度查找，兼容格子是 Overlay/Border/嵌套 UserWidget 的任意形态；
        // ★ 再前缀兜底：UMG 复制格子后内部控件名是 frame_N/img_icon_N/cd_mask_N/txt_cd_N。
        UWidget* Frame = FindSwitchHUDCellChildWidget(Cell, TEXT("frame"));
        UWidget* Icon = FindSwitchHUDCellChildWidget(Cell, TEXT("img_icon"));
        UWidget* Mask = FindSwitchHUDCellChildWidget(Cell, TEXT("cd_mask"));
        UWidget* CdText = FindSwitchHUDCellChildWidget(Cell, TEXT("txt_cd"));

        if (!Frame) UE_LOG(LogTemp, Warning, TEXT("[Switch] WBP_SwitchHUD: 格子 %d 里没找到 'frame'（含 _%d 后缀变体，当前操控高亮框不显示）。"), i + 1, i);
        if (!Icon)  UE_LOG(LogTemp, Warning, TEXT("[Switch] WBP_SwitchHUD: 格子 %d 里没找到 'img_icon'（含 _%d 后缀变体，头像不显示）。"), i + 1, i);
        if (!Mask)  UE_LOG(LogTemp, Warning, TEXT("[Switch] WBP_SwitchHUD: 格子 %d 里没找到 'cd_mask'（含 _%d 后缀变体，CD 进度条不显示）。"), i + 1, i);
        if (!CdText) UE_LOG(LogTemp, Warning, TEXT("[Switch] WBP_SwitchHUD: 格子 %d 里没找到 'txt_cd'（含 _%d 后缀变体，CD 秒数不显示）。"), i + 1, i);

        // ★ 取证：cd_mask 找到后确认真实类型。三种可驱动类型（ProgressBar/Image/Border）
        //   都能实时显示 CD 进度；只有都不是时才告警（提示换成 ProgressBar）。
        if (Mask && !Cast<UProgressBar>(Mask) && !Cast<UImage>(Mask) && !Cast<UBorder>(Mask))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[Switch] WBP_SwitchHUD: 格子 %d 的 'cd_mask' 是 %s（非 ProgressBar/Image/Border），CD 进度无法驱动 —— 请换成 ProgressBar。"),
                i + 1, *Mask->GetClass()->GetName());
        }

        SwitchHudSlotWidgets.Add(Cell);
        SwitchHudFrameWidgets.Add(Frame);
        SwitchHudIconWidgets.Add(Icon);
        SwitchHudMaskWidgets.Add(Mask);
        SwitchHudCdTextWidgets.Add(CdText);
        SwitchHudRows.Add(Rows[i]);

        // 头像纹理：从 Data_chara_imfor 的 HeadIcon 填
        if (UImage* IconImg = Cast<UImage>(Icon))
        {
            if (const FCharaInfoEntry* Entry = FindCharaInfo(Rows[i]))
            {
                if (UTexture2D* HeadTex = Entry->HeadIcon.Get())
                {
                    IconImg->SetBrushFromTexture(HeadTex, false);
                }
            }
        }
    }

    if (!bAnyCellFound)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Switch] ★ 影响：切人 HUD 一个格子都没找到 —— WBP_SwitchHUD 里 slot_0~2 一个都不存在。"));
        RemoveSwitchTeamHUD();
        return;
    }

    SwitchTeamHUDWidget->AddToViewport(8); // 高于技能图标(5)，低于完美闪避提示(10)
    SwitchTeamHUDWidget->SetVisibility(ESlateVisibility::HitTestInvisible); // 整块不吃点击

    // 立即刷一帧（初始高亮框/CD 状态）
    UpdateSwitchTeamHUD();

    UE_LOG(LogTemp, Log, TEXT("[Switch] 编队 HUD 已构建（WBP 路径）：%d 个成员格（右上角）。"), SwitchHudRows.Num());
}

// ---- 纯 C++ 兜底路径的占位（原 BuildSwitchTeamHUD 的代码搭树逻辑）----
// ★ 原逻辑已迁走：当前兜底路径因为 UUserWidget Abstract 限制暂不可用（见 BuildSwitchTeamHUD 路径②告警）。
//   保留此注释说明迁移去向，避免后续误认为「代码搭树逻辑丢失」。

void ABattleCharacter::UpdateSwitchTeamHUD()
{
    if (!SwitchTeamHUDWidget)
    {
        return;
    }

    // 队伍成员变了（编队 UI 里改过）→ 自动重建，让 HUD 跟上
    // ★ 用与 Build 相同的「含默认出战兜底」口径比较，避免空队伍时每帧误判重建。
    const TArray<FName> Members = GetEffectiveTeamMembersForHUD();
    if (Members != SwitchHudRows)
    {
        BuildSwitchTeamHUD();
        if (!SwitchTeamHUDWidget)
        {
            return;
        }
    }

    const FName MyRow = GetMyCharaRow();
    for (int32 i = 0; i < SwitchHudRows.Num(); ++i)
    {
        const bool bActive = (SwitchHudRows[i] == MyRow);
        if (SwitchHudFrameWidgets.IsValidIndex(i) && SwitchHudFrameWidgets[i])
        {
            SwitchHudFrameWidgets[i]->SetVisibility(bActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
        }

        const float Remaining = GetSwitchCooldownRemaining(SwitchHudRows[i]);
        const bool bInCD = Remaining > 0.0f;

        // CD 遮罩/进度条（WBP 约定名 cd_mask）：CD 中按「剩余比例」实时缩短（1.0→0.0，遮罩逐渐消失）；
        // 未 CD 隐藏。三种类型都降级支持，避免「类型对不上 → 静默不刷新」：
        //   UProgressBar → SetPercent（最直观的进度条）
        //   UImage/UBorder（旧遮罩形态）→ SetRenderOpacity（随 CD 递减淡出，同「遮罩消失」语义）
        if (SwitchHudMaskWidgets.IsValidIndex(i) && SwitchHudMaskWidgets[i])
        {
            UWidget* Mask = SwitchHudMaskWidgets[i];
            if (bInCD)
            {
                Mask->SetVisibility(ESlateVisibility::HitTestInvisible);
                const float Ratio = (SwitchCooldown > 0.0f)
                    ? FMath::Clamp(Remaining / SwitchCooldown, 0.0f, 1.0f)
                    : 0.0f;

                if (UProgressBar* Bar = Cast<UProgressBar>(Mask))
                {
                    Bar->SetPercent(Ratio);
                }
                else if (UImage* Img = Cast<UImage>(Mask))
                {
                    Img->SetRenderOpacity(Ratio);
                }
                else if (UBorder* Border = Cast<UBorder>(Mask))
                {
                    Border->SetRenderOpacity(Ratio);
                }
                else
                {
                    // ★ 取证：cd_mask 找到了但三种可驱动类型都不是 —— 打真实类型便于排查
                    UE_LOG(LogTemp, Warning,
                        TEXT("[Switch] CD 进度不刷新：格子 %d 的 cd_mask 是 %s（非 ProgressBar/Image/Border），无法驱动 —— 请换成 ProgressBar。"),
                        i + 1, *Mask->GetClass()->GetName());
                }
            }
            else
            {
                Mask->SetVisibility(ESlateVisibility::Collapsed);
            }
        }

        // CD 剩余秒数文本
        if (SwitchHudCdTextWidgets.IsValidIndex(i) && SwitchHudCdTextWidgets[i])
        {
            UWidget* CdTextWidget = SwitchHudCdTextWidgets[i];
            if (bInCD)
            {
                CdTextWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
                if (UTextBlock* CdText = Cast<UTextBlock>(CdTextWidget))
                {
                    CdText->SetText(FText::FromString(FString::Printf(TEXT("%.1f"), Remaining)));
                }
            }
            else
            {
                CdTextWidget->SetVisibility(ESlateVisibility::Collapsed);
            }
        }
    }
}

void ABattleCharacter::RemoveSwitchTeamHUD()
{
    if (SwitchTeamHUDWidget)
    {
        SwitchTeamHUDWidget->RemoveFromParent();
        SwitchTeamHUDWidget = nullptr;
    }
    bSwitchHUDUsesWBP = false;
    SwitchHudSlotWidgets.Reset();
    SwitchHudFrameWidgets.Reset();
    SwitchHudIconWidgets.Reset();
    SwitchHudMaskWidgets.Reset();
    SwitchHudCdTextWidgets.Reset();
    SwitchHudRows.Reset();
}