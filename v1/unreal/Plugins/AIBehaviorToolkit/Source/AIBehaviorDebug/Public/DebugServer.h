// The debug transport (design.md section 7).
//
// "First-party debugging works in the editor. The bugs that matter often don't
// reproduce there -- dedicated server AI, cooked-build timing,
// platform-specific behavior, and anything that only appears with 200 agents."
//
// So this is a TCP listener inside the game process, not an editor module. It
// is compiled out of Shipping by WITH_AIBEHAVIOR_DEBUG and present in
// Development and Test -- including cooked Test builds, which is the entire
// point.
//
// The wire format is the one in editor/src/debug/protocol.ts. That file is the
// specification; this is the other end of it.

#pragma once

#include "CoreMinimal.h"
#include "Containers/CircularBuffer.h"
#include "HAL/Runnable.h"

#include "aib/selection.h"

#if WITH_AIBEHAVIOR_DEBUG

class FSocket;
class ISocketSubsystem;

enum class EAIBDetailLevel : uint8
{
	Summary = 0,
	Scores = 1,
	FullBreakdown = 2,
};

/**
 * What the editor has asked for.
 *
 * SECTION 7.2 IS THE REASON THIS TYPE EXISTS:
 *
 *     200 agents x 30 actions x 5 considerations x 10 Hz = 300,000 values/s
 *
 * "At even 8 bytes each that's 2.4 MB/s of raw floats, before framing -- and
 * it's useless data, because nobody is looking at 200 agents."
 */
struct FAIBSubscription
{
	TArray<uint32> AgentIds;          // usually exactly one
	EAIBDetailLevel Detail = EAIBDetailLevel::Summary;
	float MaxHz = 10.0f;
	bool bDeltasOnly = true;

	bool Wants(uint32 AgentId) const
	{
		return Detail == EAIBDetailLevel::Summary || AgentIds.Contains(AgentId);
	}
};

struct FAIBDebugFrame
{
	double Time = 0.0;
	uint32 AgentId = 0;
	int32 SelectedAction = INDEX_NONE;
	TArray<float> Scores;

	/** Ignores Time, so an agent whose situation is stable compares equal to
	 *  itself a frame later and sends nothing (section 15.3). */
	bool EqualsIgnoringTime(const FAIBDebugFrame& Other) const;
};

/**
 * Fixed-capacity history (section 7.3).
 *
 * "Fixed capacity is the point -- a debug buffer that grows will eventually be
 * the reason a long play session runs out of memory." An overnight soak is
 * exactly when the interesting AI bugs appear and exactly when an unbounded
 * buffer kills the process before anyone reads it.
 */
class AIBEHAVIORDEBUG_API FAIBDebugRingBuffer
{
public:
	/** 600 frames is 60 seconds at 10 Hz. */
	explicit FAIBDebugRingBuffer(uint32 InCapacity = 600);

	void Push(FAIBDebugFrame&& Frame);
	TArray<FAIBDebugFrame> Range(double StartTime, double EndTime) const;
	int32 Num() const { return Count; }

private:
	TCircularBuffer<FAIBDebugFrame> Frames;
	uint32 Head = 0;
	int32 Count = 0;
};

/**
 * The listener.
 *
 * Binds to loopback by default with an explicit opt-in for remote, per section
 * 7.1. A debug listener on 0.0.0.0 in a Test build that somebody ships to
 * playtesters is a remote code execution surface, and the commands in section
 * 7.4 include "force an action" and "hot-reload a behavior".
 */
class AIBEHAVIORDEBUG_API FAIBDebugServer : public FRunnable
{
public:
	static FAIBDebugServer& Get();

	/** `-aibdebug` to enable, `-aibdebugport=N`, `-aibdebugremote` to bind
	 *  beyond loopback. Off unless asked for. */
	bool StartFromCommandLine();
	bool Start(uint16 Port, bool bAllowRemote);
	void Stop();

	bool IsConnected() const { return Socket != nullptr && bClientConnected; }

	/** Called once per agent per evaluation. Cheap and non-blocking when
	 *  nobody is subscribed, which is the common case. */
	void ReportFrame(uint32 AgentId, double Time,
	                 const aib::CompiledBehavior& Behavior,
	                 const aib::InputCache& Inputs,
	                 const aib::SelectionResult& Result,
	                 const TArray<float>& Scores);

	/** Commands from the editor (section 7.4). Applied on the game thread. */
	DECLARE_MULTICAST_DELEGATE_FourParams(FAIBCommandDelegate, uint8 /*Command*/,
	                                      uint32 /*AgentId*/, int32 /*Index*/,
	                                      float /*Value*/);
	FAIBCommandDelegate OnCommand;

	DECLARE_MULTICAST_DELEGATE_OneParam(FAIBHotReloadDelegate, const FString&);
	FAIBHotReloadDelegate OnHotReload;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Exit() override;

private:
	void SendFrame(const FAIBDebugFrame& Frame);

	FSocket* Socket = nullptr;
	FSocket* ClientSocket = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bRunning{false};
	FThreadSafeBool bClientConnected{false};

	FCriticalSection SubscriptionLock;
	FAIBSubscription Subscription;

	/** Last frame sent per agent, for the delta comparison. */
	TMap<uint32, FAIBDebugFrame> LastSent;
	TMap<uint32, double> LastSendTime;
	TMap<uint32, FAIBDebugRingBuffer> History;
	TArray<uint8> ScratchBuffer;
};

#endif  // WITH_AIBEHAVIOR_DEBUG
