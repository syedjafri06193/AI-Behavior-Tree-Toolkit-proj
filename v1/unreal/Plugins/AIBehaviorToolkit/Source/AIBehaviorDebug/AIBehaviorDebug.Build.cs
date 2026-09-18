// Copyright contributors to the AI Behavior Toolkit.

using UnrealBuildTool;

/// <summary>
/// The debug transport (design.md section 7).
///
/// This module ships in Development and Test builds and is compiled out of
/// Shipping entirely -- section 7.1: "Gate it behind a build flag so shipping
/// builds don't carry a debug listener."
///
/// Note it is NOT compiled out of Test. The whole point of section 7.1 is that
/// the bugs that matter do not reproduce in PIE, so the transport has to exist
/// in the cooked build you are actually debugging.
/// </summary>
public class AIBehaviorDebug : ModuleRules
{
    public AIBehaviorDebug(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Sockets", "Networking",
            "AIBehaviorCore",
        });

        bool bEnableDebugServer = Target.Configuration != UnrealTargetConfiguration.Shipping;
        PublicDefinitions.Add("WITH_AIBEHAVIOR_DEBUG=" + (bEnableDebugServer ? "1" : "0"));
    }
}
