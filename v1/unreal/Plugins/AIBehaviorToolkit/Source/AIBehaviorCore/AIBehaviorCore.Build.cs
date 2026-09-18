// Copyright contributors to the AI Behavior Toolkit.

using UnrealBuildTool;
using System.IO;

/// <summary>
/// The engine-agnostic scoring core, compiled into the plugin.
///
/// This module deliberately depends on Core and nothing else. The sources it
/// compiles are the same files under core/src that the standalone CMake build
/// and the unit tests use -- not a copy, the same files. That is the point of
/// design.md section 6.1: "Keep AIBehaviorCore free of engine types entirely.
/// It's plain C++ structs and functions. It compiles standalone, it
/// unit-tests without the engine, and it survives version upgrades untouched."
///
/// If this module ever needs a second dependency, that is the signal an engine
/// type has leaked into the core and the property above has been lost.
/// </summary>
public class AIBehaviorCore : ModuleRules
{
    public AIBehaviorCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Core only. See the note above before adding anything here.
        PublicDependencyModuleNames.AddRange(new string[] { "Core" });

        // The standalone core lives outside the plugin so it can be built and
        // tested without Unreal. Pointing at it rather than vendoring a copy
        // means the tested code and the shipped code cannot drift.
        string CoreRoot = Path.GetFullPath(
            Path.Combine(ModuleDirectory, "..", "..", "..", "..", "..", "core"));

        PublicIncludePaths.Add(Path.Combine(CoreRoot, "include"));
        ConditionalAddModuleDirectory(
            new DirectoryReference(Path.Combine(CoreRoot, "src")));

        // The core is plain C++17 with no reflection, so none of Unreal's
        // macro machinery applies to it.
        bUseUnity = false;
        CppStandard = CppStandardVersion.Cpp17;
    }
}
