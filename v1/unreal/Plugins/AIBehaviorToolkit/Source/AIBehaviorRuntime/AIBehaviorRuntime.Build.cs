// Copyright contributors to the AI Behavior Toolkit.

using UnrealBuildTool;

public class AIBehaviorRuntime : ModuleRules
{
    public AIBehaviorRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AIModule",          // Behavior Tree, Blackboard, AIController
            "GameplayTasks",
            "NavigationSystem",
            "StateTreeModule",
            "GameplayStateTreeModule",
            "AIBehaviorCore",
        });

        PrivateDependencyModuleNames.AddRange(new string[] { "AIBehaviorDebug" });
    }
}
