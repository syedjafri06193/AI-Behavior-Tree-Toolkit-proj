// Utility selection as a StateTree evaluator (design.md section 6.4).
//
// WHY THIS EXISTS AT ALL is the strategic argument in section 2.1: Epic
// shifted its recommendation to StateTree in 2025, StateTree reached
// production-ready in the 5.5/5.6 cycle, and "the Behavior Tree editor hasn't
// had a substantive feature update since UE 5.3". BT is not deprecated and
// still works, but building exclusively for it targets a system on
// maintenance. Supporting both is more work and it is "the difference between
// a tool with a future and one without".
//
// The shape differs from the BT integration in exactly one way, and section
// 6.4 names it: StateTree binds data per-state with explicit bindings rather
// than through a shared blackboard. So the inputs arrive as bound properties
// instead of being pulled from a UBlackboardComponent, and the output is a
// bound property a transition condition reads.
//
// The scoring core is identical. That is the payoff for keeping it
// engine-agnostic (section 4.3): the part that differs is the wiring, and the
// part that is hard is shared.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeExecutionContext.h"

#include "aib/selection.h"

#include "StateTreeUtilityEvaluator.generated.h"

class UAIBehaviorAsset;

/**
 * Instance data. StateTree keeps this per-execution rather than per-node, so
 * unlike the BT composite there is no manual instance memory to manage.
 */
USTRUCT()
struct AIBEHAVIORRUNTIME_API FStateTreeUtilityEvaluatorInstanceData
{
	GENERATED_BODY()

	/**
	 * Input values, bound per-state.
	 *
	 * This is the section 6.4 difference made concrete. With a blackboard the
	 * inputs are pulled by key; here they are pushed by binding, and the array
	 * order has to match the behavior's compiled input table. The evaluator
	 * checks that on first tick rather than trusting it, because a silent
	 * off-by-one here reads every consideration against the wrong input and
	 * produces an agent that is subtly, consistently wrong.
	 */
	UPROPERTY(EditAnywhere, Category = "Input")
	TArray<float> Inputs;

	/** Output: the winning action's name, for a transition condition to read. */
	UPROPERTY(EditAnywhere, Category = "Output")
	FName SelectedAction;

	/** Output: its score, so a condition can require a minimum confidence. */
	UPROPERTY(EditAnywhere, Category = "Output")
	float SelectedScore = 0.0f;

	/** Output: true only on the tick the selection changed, so a transition
	 *  can fire once rather than continuously. */
	UPROPERTY(EditAnywhere, Category = "Output")
	bool bSelectionChanged = false;

	aib::SelectionState Selection;
	float TimeSinceEvaluation = 0.0f;
	bool bBindingChecked = false;
};

USTRUCT(DisplayName = "Utility Select")
struct AIBEHAVIORRUNTIME_API FStateTreeUtilityEvaluator : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FStateTreeUtilityEvaluatorInstanceData;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	TObjectPtr<UAIBehaviorAsset> Behavior;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	virtual void TreeStart(FStateTreeExecutionContext& Context) const override;
	virtual void Tick(FStateTreeExecutionContext& Context,
	                  const float DeltaTime) const override;

#if WITH_EDITOR
	virtual FText GetDescription(
		const FGuid& ID, FStateTreeDataView InstanceDataView,
		const IStateTreeBindingLookup& BindingLookup,
		EStateTreeNodeFormatting Formatting) const override;
#endif
};
