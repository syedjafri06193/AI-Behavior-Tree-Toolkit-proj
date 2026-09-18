#include "StateTreeUtilityEvaluator.h"

#include "AIBehaviorAsset.h"

DECLARE_CYCLE_STAT(TEXT("Utility Select (StateTree)"), STAT_AIBUtilitySelectST,
                   STATGROUP_AI);

void FStateTreeUtilityEvaluator::TreeStart(
	FStateTreeExecutionContext& Context) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Selection = aib::SelectionState{};
	Data.TimeSinceEvaluation = 0.0f;
	Data.bBindingChecked = false;
	Data.SelectedAction = NAME_None;
	Data.SelectedScore = 0.0f;
	Data.bSelectionChanged = false;
}

void FStateTreeUtilityEvaluator::Tick(FStateTreeExecutionContext& Context,
                                      const float DeltaTime) const
{
	SCOPE_CYCLE_COUNTER(STAT_AIBUtilitySelectST);

	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.bSelectionChanged = false;

	if (!Behavior || !Behavior->IsLoaded())
	{
		return;
	}
	const aib::CompiledBehavior& Compiled = Behavior->GetCompiled();
	const int32 ExpectedInputs = static_cast<int32>(Compiled.input_names.size());

	// Checked once, loudly. StateTree binds by position, so a bound array that
	// is the wrong length reads every consideration against the wrong input --
	// an agent that is subtly and consistently wrong, with nothing in the log.
	// Better to say so on the first tick than to let somebody spend a day on
	// it.
	if (!Data.bBindingChecked)
	{
		Data.bBindingChecked = true;
		if (Data.Inputs.Num() != ExpectedInputs)
		{
			UE_LOG(LogTemp, Error,
			       TEXT("FStateTreeUtilityEvaluator: behavior '%s' declares %d "
			            "inputs but %d are bound. Bind one property per input, "
			            "in the order the behavior file declares them."),
			       UTF8_TO_TCHAR(Compiled.name.c_str()), ExpectedInputs,
			       Data.Inputs.Num());
		}
	}
	if (Data.Inputs.Num() != ExpectedInputs)
	{
		return;
	}

	// Same rate policy as the BT path (section 3.5). StateTree ticks its
	// evaluators every frame, so the gate lives here rather than in the host.
	const float Period = (Compiled.policy.evaluation_hz > 0.0f)
		? 1.0f / Compiled.policy.evaluation_hz
		: 0.0f;
	Data.TimeSinceEvaluation += DeltaTime;
	if (!Data.Selection.force_reevaluate && Data.TimeSinceEvaluation < Period)
	{
		return;
	}
	const float Elapsed = Data.TimeSinceEvaluation;
	Data.TimeSinceEvaluation = 0.0f;

	aib::InputCache Inputs;
	Inputs.values.resize(static_cast<size_t>(ExpectedInputs));
	for (int32 i = 0; i < ExpectedInputs; ++i)
	{
		Inputs.values[static_cast<size_t>(i)] = Data.Inputs[i];
	}

	// The identical call the BT composite makes, on the identical core.
	const aib::SelectionResult Result =
		aib::Select(Compiled, Inputs, Data.Selection, Elapsed);

	if (Result.action >= 0 &&
	    Result.action < static_cast<int>(Compiled.actions.size()))
	{
		const FName NewAction(
			UTF8_TO_TCHAR(Compiled.actions[Result.action].name.c_str()));
		Data.bSelectionChanged = NewAction != Data.SelectedAction;
		Data.SelectedAction = NewAction;
		Data.SelectedScore = Result.score;
	}
}

#if WITH_EDITOR
FText FStateTreeUtilityEvaluator::GetDescription(
	const FGuid& ID, FStateTreeDataView InstanceDataView,
	const IStateTreeBindingLookup& BindingLookup,
	EStateTreeNodeFormatting Formatting) const
{
	if (!Behavior)
	{
		return FText::FromString(TEXT("Utility Select: <no behavior>"));
	}
	return FText::FromString(FString::Printf(
		TEXT("Utility Select: %s"),
		UTF8_TO_TCHAR(Behavior->GetCompiled().name.c_str())));
}
#endif
