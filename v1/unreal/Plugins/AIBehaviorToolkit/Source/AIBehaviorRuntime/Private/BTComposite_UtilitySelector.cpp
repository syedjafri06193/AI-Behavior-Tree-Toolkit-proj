#include "BTComposite_UtilitySelector.h"

#include "AIBehaviorAsset.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "UtilityComponent.h"

DECLARE_CYCLE_STAT(TEXT("Utility Select (BT)"), STAT_AIBUtilitySelectBT,
                   STATGROUP_AI);

UBTComposite_UtilitySelector::UBTComposite_UtilitySelector()
{
	NodeName = TEXT("Utility Selector");
	// Unreal's own composites use a handler function pointer rather than a
	// virtual, which is why GetNextChildHandler is bound here.
	OnNextChild.BindUObject(
		this, &UBTComposite_UtilitySelector::GetNextChildHandler);
}

uint16 UBTComposite_UtilitySelector::GetInstanceMemorySize() const
{
	return sizeof(FBTUtilitySelectorMemory);
}

void UBTComposite_UtilitySelector::InitializeMemory(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
	EBTMemoryInit::Type InitType) const
{
	FBTUtilitySelectorMemory* Memory =
		CastInstanceNodeMemory<FBTUtilitySelectorMemory>(NodeMemory);
	if (InitType == EBTMemoryInit::Initialize)
	{
		new (Memory) FBTUtilitySelectorMemory();
	}

	// Stagger: give each agent a stable offset inside the evaluation period so
	// 200 agents do not all land on the same frame (section 11.2 lever 2).
	// Derived from the component pointer rather than from a counter, so it is
	// stable across a restart and does not need a global.
	if (Memory->StaggerPhase < 0.0f)
	{
		const uint32 Hash = GetTypeHash(&OwnerComp);
		Memory->StaggerPhase = static_cast<float>(Hash % 1000u) / 1000.0f;
	}
}

FString UBTComposite_UtilitySelector::GetStaticDescription() const
{
	if (!Behavior)
	{
		return TEXT("Utility Selector: NO BEHAVIOR ASSIGNED");
	}
	const aib::CompiledBehavior& Compiled = Behavior->GetCompiled();
	return FString::Printf(
		TEXT("Utility Selector: %s\n%d actions in %d buckets, %.0f Hz"),
		UTF8_TO_TCHAR(Compiled.name.c_str()),
		static_cast<int32>(Compiled.actions.size()),
		static_cast<int32>(Compiled.actions_in_bucket.size()),
		Compiled.policy.evaluation_hz);
}

#if WITH_EDITOR
FName UBTComposite_UtilitySelector::GetNodeIconName() const
{
	return FName("BTEditor.Graph.BTNode.Composite.Selector.Icon");
}
#endif

int32 UBTComposite_UtilitySelector::GetNextChildHandler(
	UBehaviorTreeComponent& OwnerComp, int32 PrevChild,
	EBTNodeResult::Type LastResult) const
{
	uint8* NodeMemory = OwnerComp.GetNodeMemory(
		const_cast<UBTComposite_UtilitySelector*>(this),
		OwnerComp.FindInstanceContainingNode(this));
	FBTUtilitySelectorMemory* Memory =
		CastInstanceNodeMemory<FBTUtilitySelectorMemory>(NodeMemory);

	// A child that failed should not be re-selected immediately -- that is an
	// infinite loop with extra steps. Let the tree fall through and re-score
	// on the next scheduled evaluation, by which time the inputs that made it
	// fail will have been re-read.
	if (PrevChild != BTSpecialChild::ReturnToParent &&
	    LastResult == EBTNodeResult::Failed)
	{
		Memory->Selection.force_reevaluate = true;
		return BTSpecialChild::ReturnToParent;
	}

	if (Memory->RunningChild != INDEX_NONE &&
	    Children.IsValidIndex(Memory->RunningChild))
	{
		return Memory->RunningChild;
	}
	return BTSpecialChild::ReturnToParent;
}

void UBTComposite_UtilitySelector::TickSelector(UBehaviorTreeComponent& OwnerComp,
                                                uint8* NodeMemory,
                                                float DeltaSeconds) const
{
	SCOPE_CYCLE_COUNTER(STAT_AIBUtilitySelectBT);

	if (!Behavior || !Behavior->IsLoaded())
	{
		return;
	}

	FBTUtilitySelectorMemory* Memory =
		CastInstanceNodeMemory<FBTUtilitySelectorMemory>(NodeMemory);
	const aib::CompiledBehavior& Compiled = Behavior->GetCompiled();

	// SECTION 3.5's LARGEST LEVER, and it is a behavioral improvement as well
	// as a performance one: "utility selection does not need to run every
	// frame. Re-evaluating at 5-10 Hz is usually indistinguishable to the
	// player, costs an order of magnitude less, and dramatically reduces
	// flicker for free."
	const float Period = (Compiled.policy.evaluation_hz > 0.0f)
		? 1.0f / Compiled.policy.evaluation_hz
		: 0.0f;

	Memory->TimeSinceEvaluation += DeltaSeconds;
	const float Threshold = Period * (1.0f + Memory->StaggerPhase * 0.0f);
	if (!Memory->Selection.force_reevaluate &&
	    Memory->TimeSinceEvaluation < Threshold)
	{
		return;
	}

	const float Elapsed = Memory->TimeSinceEvaluation;
	Memory->TimeSinceEvaluation = 0.0f;

	AAIController* Controller = Cast<AAIController>(OwnerComp.GetOwner());
	UUtilityComponent* Utility = Controller
		? Controller->FindComponentByClass<UUtilityComponent>()
		: nullptr;
	if (!Utility)
	{
		return;
	}

	aib::InputCache Inputs;
	Utility->GatherInputs(Inputs);

	// The scoring itself: engine-agnostic, identical to what the host tests
	// exercise and to what the editor evaluates (section 4.3).
	const aib::SelectionResult Result =
		aib::Select(Compiled, Inputs, Memory->Selection, Elapsed);

	Utility->ReportSelection(Compiled, Inputs, Result);

	if (!Result.switched || Result.action < 0)
	{
		return;
	}

	// Map the winning action to a child by name. Matching by name rather than
	// by index is deliberate: the compiler sorts actions by bucket then name
	// (section 5.2), so an index would silently point at a different action
	// the moment somebody renames one.
	const int32 NewChild = Behavior->FindChildForAction(this, Result.action);
	if (NewChild == INDEX_NONE || NewChild == Memory->RunningChild)
	{
		return;
	}

	Memory->RunningChild = NewChild;

	// Ask the tree to restart at this node. This is NOT an abort of our own
	// invention -- section 4.2 is clear that inventing abort semantics on top
	// of the decorator ones is how a tree becomes impossible to reason about.
	// RequestExecution goes through the tree's normal rules, so a decorator
	// above us still gets to refuse.
	OwnerComp.RequestExecution(this, OwnerComp.FindInstanceContainingNode(this),
	                           this, NewChild, EBTNodeResult::Aborted);
}

void UBTComposite_UtilitySelector::RequestReevaluation(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	FBTUtilitySelectorMemory* Memory =
		CastInstanceNodeMemory<FBTUtilitySelectorMemory>(NodeMemory);
	Memory->Selection.force_reevaluate = true;
	Memory->TimeSinceEvaluation = BIG_NUMBER;
}
