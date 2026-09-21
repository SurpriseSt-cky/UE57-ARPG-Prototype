#pragma once
#include "CoreMinimal.h"
#include "CharacterAttributes.generated.h"

USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FCharacterAttributes
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attributes")
    float MaxHP = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attributes")
    float Attack = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attributes")
    float Defense = 50.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resources")
    float MaxStamina = 120.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resources")
    float StaminaRegenRate = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float CritRate = 0.05f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float CritDamage = 1.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float Poise = 100.0f;
};