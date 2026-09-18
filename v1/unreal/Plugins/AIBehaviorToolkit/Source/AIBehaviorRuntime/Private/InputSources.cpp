#include "InputSources.h"

#include "AIController.h"
#include "GameFramework/Pawn.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"

void FAIBInputGatherer::Bind(const aib::CompiledBehavior& Behavior,
                             const TArray<FAIBInputSource>& InSources,
                             const UBlackboardComponent* Blackboard)
{
	Sources = InSources;
	// The compiler emits one input per index; a source list that does not
	// match it means the asset and the source table came from different
	// builds, and every subsequent read would be off by one.
	checkf(Sources.Num() == static_cast<int32>(Behavior.input_names.size()),
	       TEXT("%d input sources for a behavior with %d inputs"),
	       Sources.Num(), static_cast<int32>(Behavior.input_names.size()));

	ResolvedKeys.SetNum(Sources.Num());
	for (int32 i = 0; i < Sources.Num(); ++i)
	{
		ResolvedKeys[i] = Blackboard
			? Blackboard->GetKeyID(Sources[i].Key)
			: FBlackboard::InvalidKey;
	}
}

float FAIBInputGatherer::EvaluateSource(const FAIBInputSource& Source,
                                        FBlackboard::FKey KeyId,
                                        const AAIController* Controller,
                                        const UBlackboardComponent* Blackboard) const
{
	if (Source.Type == EAIBInputType::Constant)
	{
		return Source.FallbackValue;
	}
	if (!Blackboard || KeyId == FBlackboard::InvalidKey)
	{
		return Source.FallbackValue;
	}

	switch (Source.Type)
	{
	case EAIBInputType::BlackboardFloat:
		return Blackboard->GetValue<UBlackboardKeyType_Float>(KeyId);

	case EAIBInputType::BlackboardInt:
		return static_cast<float>(Blackboard->GetValue<UBlackboardKeyType_Int>(KeyId));

	case EAIBInputType::BlackboardBool:
		return Blackboard->GetValue<UBlackboardKeyType_Bool>(KeyId) ? 1.0f : 0.0f;

	case EAIBInputType::BlackboardActorDistance:
	{
		const AActor* Target =
			Cast<AActor>(Blackboard->GetValue<UBlackboardKeyType_Object>(KeyId));
		const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (!Target || !Pawn)
		{
			// NOT ZERO. Section 6.3: zero means "right here", which inverts
			// the consideration and makes the agent attack nothing. The
			// fallback for a distance source is its maximum range, which
			// normalizes to the far end and reads as "no target".
			return Source.FallbackValue;
		}
		return FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation());
	}

	case EAIBInputType::BlackboardActorPathLength:
	{
		// EXPENSIVE. Section 11.1: "a consideration that does a navmesh query
		// or a line trace is microseconds, not nanoseconds, and a hundred of
		// those blows the budget alone."
		//
		// It is gathered once per evaluation and shared across every
		// consideration that reads this input (section 11.2 lever 5), and the
		// validator hints when one of these is ordered first in an action,
		// because early-out reads front to back.
		const AActor* Target =
			Cast<AActor>(Blackboard->GetValue<UBlackboardKeyType_Object>(KeyId));
		const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (!Target || !Pawn)
		{
			return Source.FallbackValue;
		}

		UNavigationSystemV1* Nav =
			FNavigationSystem::GetCurrent<UNavigationSystemV1>(Pawn->GetWorld());
		if (!Nav)
		{
			return Source.FallbackValue;
		}
		const UNavigationPath* Path = Nav->FindPathToActorSynchronously(
			Pawn->GetWorld(), Pawn->GetActorLocation(),
			const_cast<AActor*>(Target));

		// An unreachable target is not a near one. Falling back to straight
		// line distance here would be worse than the fallback: it would tell
		// the agent to attack something across a chasm.
		return (Path && Path->IsValid() && !Path->IsPartial())
			? Path->GetPathLength()
			: Source.FallbackValue;
	}

	case EAIBInputType::BlackboardElapsed:
	{
		const float Stamp = Blackboard->GetValue<UBlackboardKeyType_Float>(KeyId);
		const UWorld* World = Controller ? Controller->GetWorld() : nullptr;
		if (!World || Stamp <= 0.0f)
		{
			return Source.FallbackValue;
		}
		return static_cast<float>(World->GetTimeSeconds()) - Stamp;
	}

	case EAIBInputType::BlackboardActorCount:
		return static_cast<float>(Blackboard->GetValue<UBlackboardKeyType_Int>(KeyId));

	case EAIBInputType::Custom:
	default:
		// Projects register their own accessors; without one, the fallback is
		// the honest answer rather than a zero that means something.
		return Source.FallbackValue;
	}
}

void FAIBInputGatherer::Gather(const AAIController* Controller,
                               const UBlackboardComponent* Blackboard,
                               aib::InputCache& OutCache) const
{
	OutCache.values.resize(static_cast<size_t>(Sources.Num()));
	for (int32 i = 0; i < Sources.Num(); ++i)
	{
		OutCache.values[static_cast<size_t>(i)] =
			EvaluateSource(Sources[i], ResolvedKeys[i], Controller, Blackboard);
	}

	// Debug overrides win, so "what would it do if health were 0.1" answers
	// the question rather than being immediately overwritten by the real
	// value (section 7.4).
	for (const TPair<int32, float>& Override : Overrides)
	{
		if (Sources.IsValidIndex(Override.Key))
		{
			OutCache.values[static_cast<size_t>(Override.Key)] = Override.Value;
		}
	}
}

void FAIBInputGatherer::SetOverride(int32 InputIndex, float Value)
{
	Overrides.Add(InputIndex, Value);
}

void FAIBInputGatherer::ClearOverride(int32 InputIndex)
{
	Overrides.Remove(InputIndex);
}

void FAIBInputGatherer::ClearAllOverrides()
{
	Overrides.Reset();
}
