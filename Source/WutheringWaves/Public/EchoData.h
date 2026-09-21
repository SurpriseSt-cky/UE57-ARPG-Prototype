#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "EchoData.generated.h"

UENUM(BlueprintType)
enum class EEchoCost : uint8
{
    Cost1 UMETA(DisplayName = "1 Cost"),
    Cost3 UMETA(DisplayName = "3 Cost"),
    Cost4 UMETA(DisplayName = "4 Cost")
};

// 【修复】为 UHT 提供最明确的反射元数据 DisplayName
UENUM(BlueprintType)
enum class EEchoMainStatType : uint8
{
    CritRate       UMETA(DisplayName = "Crit Rate"),
    CritDamage     UMETA(DisplayName = "Crit Damage"),
    AttackPercent  UMETA(DisplayName = "Attack %"),
    HPPercent      UMETA(DisplayName = "HP %"),
    ElementDamage  UMETA(DisplayName = "Element Damage"),
    HealingBonus   UMETA(DisplayName = "Healing Bonus")
};

USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FEchoData : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo")
    FName EchoID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo")
    EEchoCost Cost = EEchoCost::Cost4;

    // 这里的 UPROPERTY 保持原样，配合修复后的枚举即可
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo")
    EEchoMainStatType MainStatType;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo")
    FName SetID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo")
    TSoftObjectPtr<class UGameplayAbility> EchoAbility;
};