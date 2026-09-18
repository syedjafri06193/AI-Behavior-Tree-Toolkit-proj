// Copyright contributors to the AI Behavior Toolkit.

using UnrealBuildTool;

public class AIBehaviorEditor : ModuleRules
{
    public AIBehaviorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "AssetTools",
            "Projects", "AIBehaviorCore", "AIBehaviorRuntime",
        });
    }
}
