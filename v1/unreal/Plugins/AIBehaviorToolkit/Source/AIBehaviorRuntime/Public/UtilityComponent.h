// The per-agent utility runtime.
//
// One component per AI controller. It owns the input gathering, the selection
// state, and the debug reporting, so the BT composite and the StateTree
// evaluator are both thin -- they ask this for a decision and act on it.
//
// That shared middle is what makes supporting both hosts affordable, which is
// the strategic decision in section 2.1: "A toolkit that only targets BT is
// targeting a system Epic has stopped iterating on. A toolkit that only
// targets StateTree abandons every shipped project."

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "aib/record.h"
#include "aib/selection.h"

#include "InputSources.h"

#include "UtilityComponent.generated.h"

class UAIBehaviorAsset;
class UBlackboardComponent;

/** Broadcast when the agent changes its mind, for gameplay code that wants to
 *  react without polling. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAIBActionChanged,
                                             FName, NewAction,
                                             float, Score);

UCLASS(ClassGroup = AI, meta = (BlueprintSpawnableComponent))
class AIBEHAVIORRUNTIME_API UUtilityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UUtilityComponent();

	UPROPERTY(EditAnywhere, Category = "Utility")
	TObjectPtr<UAIBehaviorAsset> Behavior;

	UPROPERTY(EditAnywhere, Category = "Utility")
	TArray<FAIBInputSource> InputSources;

	/**
	 * Level of detail for this agent (section 11.2 lever 7).
	 *
	 * "Distant agents evaluate at 1 Hz with a reduced action set. Nobody
	 * watches a guard 200 metres away closely enough to notice." The document
	 * calls this underused and very effective, and it is the cheapest lever on
	 * the list after the evaluation rate itself.
	 */
	UPROPERTY(EditAnywhere, Category = "Utility|LOD")
	float NearDistance = 2000.0f;

	UPROPERTY(EditAnywhere, Category = "Utility|LOD")
	float FarDistance = 8000.0f;

	/** Multipliers applied to the behavior's evaluation period per tier. */
	UPROPERTY(EditAnywhere, Category = "Utility|LOD")
	float MediumPeriodScale = 2.0f;

	UPROPERTY(EditAnywhere, Category = "Utility|LOD")
	float FarPeriodScale = 5.0f;

	UPROPERTY(BlueprintAssignable, Category = "Utility")
	FAIBActionChanged OnActionChanged;

	/** Gather this agent's inputs once for this evaluation (section 11.2
	 *  lever 5 -- shared across every consideration that reads them). */
	void GatherInputs(aib::InputCache& OutCache) const;

	/** Hand the result to the debug server and the recorder. Called by
	 *  whichever host made the decision. */
	void ReportSelection(const aib::CompiledBehavior& Compiled,
	                     const aib::InputCache& Inputs,
	                     const aib::SelectionResult& Result);

	/** "Combat begins" -- break commitment now (section 3.5). */
	UFUNCTION(BlueprintCallable, Category = "Utility")
	void ForceReevaluate();

	UFUNCTION(BlueprintCallable, Category = "Utility")
	FName GetCurrentAction() const;

	UFUNCTION(BlueprintCallable, Category = "Utility")
	float GetCurrentScore() const;

	/** Debug commands (section 7.4). */
	void SetInputOverride(int32 InputIndex, float Value);
	void ClearInputOverrides();
	void SetPaused(bool bPaused) { bDebugPaused = bPaused; }
	bool IsPaused() const { return bDebugPaused; }
	void StepOnce() { bDebugStepRequested = true; }

	/** Which evaluation period this agent is on right now, after LOD. */
	float GetEffectivePeriod() const;

	aib::SelectionState& GetSelectionState() { return SelectionState; }

	virtual void BeginPlay() override;

protected:
	UPROPERTY(Transient)
	TObjectPtr<UBlackboardComponent> Blackboard;

private:
	FAIBInputGatherer Gatherer;
	aib::SelectionState SelectionState;
	bool bDebugPaused = false;
	bool bDebugStepRequested = false;
	uint32 AgentId = 0;
};
