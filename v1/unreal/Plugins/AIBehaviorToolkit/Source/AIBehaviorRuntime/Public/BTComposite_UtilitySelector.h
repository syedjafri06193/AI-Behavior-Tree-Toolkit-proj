// Utility selection as a Behavior Tree composite (design.md section 4.3).
//
// "Utility AI integrates as a SELECTOR, not as a replacement. A
// UtilitySelector composite that scores its children and runs the winner.
// Everything else in the tree is unchanged." Section 1's non-goals are
// explicit that replacing Epic's execution engine "means reimplementing
// something mature and losing every existing asset".
//
// TWO THINGS ABOUT UNREAL'S BT THAT SHAPE THIS NODE:
//
// 1. IT IS EVENT-DRIVEN, NOT TICK-FROM-ROOT (section 4.1). Unreal remembers
//    the currently-executing node and only re-evaluates when something tells
//    it to. So this composite cannot rely on being re-entered every frame to
//    re-score; it drives its own re-evaluation on the policy's schedule
//    (section 3.5) and requests a restart when the winner changes.
//
// 2. OBSERVER ABORTS ARE THE #1 SOURCE OF BT BUGS (section 4.2). This node
//    does not add its own abort semantics on top of the decorators above it.
//    A composite that also aborted would compose with a "Lower Priority"
//    decorator in ways nobody can reason about, and the whole complaint in
//    4.2 is that the existing semantics are already hard enough to follow.
//    It requests execution of a different child and lets the tree's normal
//    rules apply.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTCompositeNode.h"

#include "aib/selection.h"
#include "InputSources.h"

#include "BTComposite_UtilitySelector.generated.h"

class UAIBehaviorAsset;

/**
 * Per-instance state.
 *
 * Section 6.2: structs for the scoring hot path, UObject only for tasks. The
 * BT instantiates one node object shared across every agent running the tree,
 * so anything per-agent lives here rather than on the node.
 */
USTRUCT()
struct FBTUtilitySelectorMemory
{
	GENERATED_BODY()

	aib::SelectionState Selection;
	float TimeSinceEvaluation = 0.0f;
	/** Staggering offset, so 200 agents do not all evaluate on the same frame
	 *  (section 11.2 lever 2: "Spreads the cost and removes the spike"). */
	float StaggerPhase = -1.0f;
	int32 RunningChild = INDEX_NONE;
};

UCLASS(DisplayName = "Utility Selector")
class AIBEHAVIORRUNTIME_API UBTComposite_UtilitySelector : public UBTCompositeNode
{
	GENERATED_BODY()

public:
	UBTComposite_UtilitySelector();

	/** The compiled behavior. One action per child, matched by name. */
	UPROPERTY(EditAnywhere, Category = "Utility")
	TObjectPtr<UAIBehaviorAsset> Behavior;

	/**
	 * Where each input comes from. Parallel to the behavior's input table.
	 *
	 * Kept on the node rather than in the asset because the asset is the
	 * shared, diffable, designer-owned file and this is per-project wiring:
	 * two projects using the same guard behavior will name their blackboard
	 * keys differently, and that should not be a diff in the behavior.
	 */
	UPROPERTY(EditAnywhere, Category = "Utility")
	TArray<FAIBInputSource> InputSources;

	/**
	 * Re-evaluate even while a child is running.
	 *
	 * On by default, because a utility selector that only reconsiders when its
	 * child finishes is not reactive and the whole point of the paradigm is
	 * that it is. Turn it off for a selector whose children are short and
	 * atomic, where re-entering mid-task costs more than it buys.
	 */
	UPROPERTY(EditAnywhere, Category = "Utility")
	bool bReevaluateWhileRunning = true;

	virtual uint16 GetInstanceMemorySize() const override;
	virtual void InitializeMemory(UBehaviorTreeComponent& OwnerComp,
	                              uint8* NodeMemory,
	                              EBTMemoryInit::Type InitType) const override;
	virtual FString GetStaticDescription() const override;

#if WITH_EDITOR
	virtual FName GetNodeIconName() const override;
#endif

	/** Called by the tree to pick which child to run. */
	int32 GetNextChildHandler(UBehaviorTreeComponent& OwnerComp,
	                          int32 PrevChild,
	                          EBTNodeResult::Type LastResult) const;

	/** Driven by the owning component's tick, not by the tree. See the note on
	 *  event-driven execution at the top of this file. */
	void TickSelector(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
	                  float DeltaSeconds) const;

	/** Force an immediate re-evaluation -- "combat begins" (section 3.5). */
	void RequestReevaluation(UBehaviorTreeComponent& OwnerComp,
	                         uint8* NodeMemory) const;
};
