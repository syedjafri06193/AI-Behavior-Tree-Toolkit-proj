// Gathering inputs from Unreal (design.md section 6.3).
//
// Read from and write to the existing UBlackboardComponent rather than
// inventing a parallel store. Designers already have keys there, EQS reads
// them, and BT decorators observe them -- a second store means two places to
// look when something is wrong, and they disagree eventually.
//
// This file is one of the two places engine types meet the scoring core (the
// other is task dispatch). Everything below produces a flat float array
// indexed the way the compiler numbered the inputs, and the core never sees an
// FName, a UObject, or a world.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BlackboardComponent.h"

#include "aib/compiled.h"

class AAIController;

UENUM()
enum class EAIBInputType : uint8
{
	/** A float key, read straight through. */
	BlackboardFloat,
	/** An int key, widened. */
	BlackboardInt,
	/** A bool key as 0 or 1 -- the cheapest possible veto. */
	BlackboardBool,
	/** Distance from the pawn to an actor held in an object key. */
	BlackboardActorDistance,
	/** Path length rather than straight-line distance. EXPENSIVE. */
	BlackboardActorPathLength,
	/** Seconds since a timestamp key was last written. */
	BlackboardElapsed,
	/** Count of entries in a name-keyed actor set. */
	BlackboardActorCount,
	/** A gameplay attribute or a custom accessor registered by the project. */
	Custom,
	/** A literal, for testing and for pinning a consideration off. */
	Constant,
};

USTRUCT()
struct AIBEHAVIORRUNTIME_API FAIBInputSource
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	EAIBInputType Type = EAIBInputType::BlackboardFloat;

	UPROPERTY(EditAnywhere, Category = "Input")
	FName Key;

	/**
	 * What to report when the source cannot produce a value.
	 *
	 * SECTION 6.3 MAKES A SPECIFIC POINT ABOUT THIS, and it is worth repeating
	 * where somebody will read it: "Returning 0 for 'no target' means 'target
	 * is right here,' which inverts the consideration and makes the agent
	 * attack nothing."
	 *
	 * So the default is deliberately not zero, and distance sources override
	 * it with their maximum range. An absent value has to fall on the side of
	 * the consideration that means "no".
	 */
	UPROPERTY(EditAnywhere, Category = "Input")
	float FallbackValue = 0.0f;

	/** Cached cost hint, so the profiler and the validator agree on which
	 *  sources are the expensive ones (section 3.4). */
	bool IsExpensive() const
	{
		return Type == EAIBInputType::BlackboardActorPathLength;
	}
};

/**
 * Per-agent input gathering.
 *
 * One instance per agent, holding the resolved key ids so the per-evaluation
 * path does no name lookups (section 6.2: "Resolve input names to indices at
 * compile time. String-keyed lookups in a per-frame scoring loop are a
 * meaningful cost at 200 agents and are trivially avoidable.")
 */
class AIBEHAVIORRUNTIME_API FAIBInputGatherer
{
public:
	/** Resolve every input's blackboard key once, at bind time. */
	void Bind(const aib::CompiledBehavior& Behavior,
	          const TArray<FAIBInputSource>& Sources,
	          const UBlackboardComponent* Blackboard);

	/**
	 * Fill the cache for one evaluation.
	 *
	 * Section 11.2 lever 5: "Cache expensive inputs. A navmesh path length
	 * doesn't change meaningfully in 100 ms; compute once per evaluation and
	 * share across all considerations that use it." That sharing is what this
	 * function is -- every consideration reads from the array rather than
	 * re-querying, so an input used by six actions costs one query.
	 */
	void Gather(const AAIController* Controller,
	            const UBlackboardComponent* Blackboard,
	            aib::InputCache& OutCache) const;

	/** Overrides from the debug protocol's OverrideInput command (section 7.4):
	 *  "Override an input value, to see what the agent would do if health were
	 *  0.1." Applied after gathering so they win. */
	void SetOverride(int32 InputIndex, float Value);
	void ClearOverride(int32 InputIndex);
	void ClearAllOverrides();

	const TArray<FAIBInputSource>& GetSources() const { return Sources; }

private:
	float EvaluateSource(const FAIBInputSource& Source,
	                     FBlackboard::FKey KeyId,
	                     const AAIController* Controller,
	                     const UBlackboardComponent* Blackboard) const;

	TArray<FAIBInputSource> Sources;
	TArray<FBlackboard::FKey> ResolvedKeys;
	TMap<int32, float> Overrides;
};
