// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WutheringWaves : ModuleRules
{
	public WutheringWaves(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "EnhancedInput",        // 注意这行末尾的逗号
            "GameplayAbilities",    // 注意这行末尾的逗号
            "GameplayTags",         // 注意这行末尾的逗号
            "GameplayTasks",        // 注意这行末尾的逗号
            "Niagara",              // 注意这行末尾的逗号
            "ChaosVehicles",        // 注意这行末尾的逗号
            "Chaos"                 // 最后这一行没有逗号
        });

        PrivateDependencyModuleNames.AddRange(new string[] {
            "Slate",
            "SlateCore" ,
            "UMG",
            // Weapon blueprint auto-discovery (scans /Game/characters/arms for AWeaponBase subclasses)
            "AssetRegistry"
        });

        // Uncomment if you are using Slate UI
        // PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

        // Uncomment if you are using online features
        // PrivateDependencyModuleNames.Add("OnlineSubsystem");

        // To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
    }
}
