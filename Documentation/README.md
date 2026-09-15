# AI Behavior Toolkit — Design & Build Guide

**Project:** Visual editor for authoring behavior trees and utility AI, with live in-engine debugging, blackboard inspection, and an Unreal runtime
**Language:** C++ (runtime), TypeScript (editor)
**Status of this document:** planning + reference

---

## Table of contents

1. [Executive summary and scope](#1-executive-summary-and-scope)
2. [Reality check](#2-reality-check)
3. [Utility AI](#3-utility-ai)
4. [Behavior tree semantics](#4-behavior-tree-semantics)
5. [The authoring format](#5-the-authoring-format)
6. [The Unreal runtime](#6-the-unreal-runtime)
7. [The debug protocol](#7-the-debug-protocol)
8. [Debug visualization](#8-debug-visualization)
9. [Record and replay](#9-record-and-replay)
10. [The editor](#10-the-editor)
11. [Performance](#11-performance)
12. [Tech stack and setup](#12-tech-stack-and-setup)
13. [Repository layout](#13-repository-layout)
14. [Milestone ladder](#14-milestone-ladder)
15. [Reference implementations](#15-reference-implementations)
16. [Testing](#16-testing)
17. [Stretch goals](#17-stretch-goals)
18. [References](#18-references)

---

## 1. Executive summary and scope

### The original statement

> Visual editor for authoring behavior trees and utility AI, with live in-engine debugging, blackboard inspection, and an Unreal runtime.

Five findings reshape this, and the first is a strategic one:

1. **Epic stopped investing in Behavior Trees, and shifted its recommendation to StateTree in 2025.** StateTree reached production-ready in the UE 5.5/5.6 cycle, its debugger now matches what BT has had for a decade, and **the Behavior Tree editor hasn't had a substantive feature update since UE 5.3**. BT isn't deprecated and still works in 5.7, but building new tooling exclusively for it means building for a system on maintenance. See section 2.1.
2. **Unreal already ships a BT visual editor with live PIE debugging and a blackboard inspector.** Three of the four features in the project statement are first-party. Rebuilding them is not a product. See section 2.2.
3. **Utility AI is the actual gap.** Epic ships no scoring-based action selection for either BT or StateTree. This is a genuinely different decision paradigm with no first-party support, and it's the one part of the original statement that isn't duplicative. See section 3.
4. **Naive utility scoring has a systematic bias against complex actions.** Multiplying N considerations means an action with five nuanced inputs scores lower than a crude one with two, by construction. Four considerations at 0.9 each yields 0.656 against 0.81 for two. The compensation factor is the standard fix and almost every homegrown implementation misses it. See section 3.3.
5. **StateTree does not use a shared blackboard.** Its data model is per-state and per-task with explicit bindings. So "blackboard inspection" is a BT-specific feature and doesn't transfer — which matters if you want the toolkit to survive Epic's direction. See section 2.1.

### Revised project statement

> A utility AI system for Unreal that plugs into **both** Behavior Tree and StateTree rather than replacing either, authored in a **diffable, mergeable text format** with an external visual editor, and debugged live into **any build including cooked and dedicated server** — with score-breakdown visualization and session record/replay.

That reframe turns the project from "rebuild what Epic ships" into "fill the three gaps Epic leaves."

### The three gaps, stated plainly

| Gap | Why it's real |
|---|---|
| **Utility AI** | No first-party support in BT or StateTree. Scoring-based selection is a different paradigm and a genuine need for ambient/simulation AI. |
| **Diffable authoring** | `.uasset` is binary. BT and StateTree assets don't diff, don't merge, and can't be code-reviewed. Universally complained about, never solved. |
| **Debugging beyond PIE** | First-party debugging works in the editor. Debugging a cooked client or a dedicated server is where the hard bugs live and where the tooling isn't. |

The middle one may be the most valuable and is the least glamorous. A team where two designers can't touch the same AI asset in the same week has a real workflow problem, and a text format with a visual editor solves it.

### Explicit non-goals

- **Not a BT or StateTree replacement.** You integrate with both. Replacing Epic's execution engine means reimplementing something mature and losing every existing asset.
- **Not a navigation or perception system.** NavMesh, EQS, and AIPerception stay.
- **Not a Mass/ECS runtime in v1.** Epic's high-agent-count path is Mass + StateTree; see §11.4.
- **Not machine learning.** Utility AI is hand-authored scoring, not learned policy.
- **Not multi-engine in v1.** Unity and Godot are obvious extensions but Unreal's integration surface is enough work on its own.

---

## 2. Reality check

### 2.1 Epic's direction

| | Behavior Tree | StateTree |
|---|---|---|
| Status | Works, not deprecated | **Recommended for new AI work since 2025** |
| Editor investment | **No substantive update since 5.3** | Active |
| Debugger | Mature | Now comparable |
| Data model | **Shared blackboard** | Per-state / per-task with explicit bindings |
| Execution | Event-driven with observer aborts (§4.1) | Transitions evaluated from the current state |
| Performance at scale | Slower | Reported ~4× faster at high agent counts |
| Parallel composites | Yes | **No direct equivalent** |
| Dynamic task injection | Yes | Limited (linked assets) |
| Mass/ECS integration | No | Yes (`UMassStateTreeSchema`) |

Three consequences for this project:

**Support both.** A toolkit that only targets BT is targeting a system Epic has stopped iterating on. A toolkit that only targets StateTree abandons every shipped project. Supporting both is more work and it's the difference between a tool with a future and one without.

**Blackboard inspection is BT-only.** StateTree's per-state bindings need a different inspector — a data-flow view showing which state reads what from which binding, not a flat key-value table. Plan for two inspectors, not one generalized one.

**Utility AI is orthogonal to both**, which is why it's the durable part. A utility selector can sit inside a BT as a composite node and inside a StateTree as a transition-selection evaluator. The scoring core doesn't care.

### 2.2 What Unreal already gives you

Worth being specific, because it's most of the original statement:

- **BT visual editor** — full node graph, decorators, services, tasks
- **Live PIE debugging** — node highlighting, execution path, breakpoints
- **Blackboard editor and runtime inspector**
- **EQS** with its own visual debugger
- **StateTree editor and debugger**
- **Visual Logger** — a timeline of recorded AI events, genuinely good and underused

If your tool's pitch is "visual editor with live debugging," an experienced Unreal developer's first question is "how is this different from what's in the box?" The answer needs to be one of the three gaps.

### 2.3 The failure modes

| Symptom | Cause |
|---|---|
| Actions with more considerations never win | Multiplication bias, no compensation (§3.3) |
| Agent flips between two actions every frame | No hysteresis or commitment (§3.5) |
| Utility scoring tanks the frame rate | No early-out, all actions fully scored (§3.4) |
| BT branch aborts unexpectedly | Observer decorator semantics misunderstood (§4.2) |
| Designers can't explain why an action was chosen | No score breakdown (§8.2) |
| Merge conflicts on every AI change | Binary assets, or layout data in the logic file (§5.3) |
| Debug connection floods the network | Streaming all agents instead of subscribing (§7.2) |
| Bug reproduces only in cooked builds | Debugging only works in PIE (§7.1) |
| Plugin breaks on engine upgrade | UE API churn (§6.1) |

---

## 3. Utility AI ★

### 3.1 The model

The reference design is Dave Mark's Infinite Axis Utility System. Each candidate **action** has N **considerations**. Each consideration takes a normalized input, passes it through a **response curve**, and produces a score in [0,1]. The action's score is the product.

```
consideration:  raw input → normalize → response curve → [0,1]
action score:   Π considerations, then compensated (§3.3)
selection:      highest score in the highest non-empty bucket
```

```cpp
USTRUCT()
struct FConsideration
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere) FName InputName;        // "DistanceToTarget"
    UPROPERTY(EditAnywhere) float InputMin = 0.f;   // normalization range
    UPROPERTY(EditAnywhere) float InputMax = 1.f;
    UPROPERTY(EditAnywhere) FResponseCurve Curve;
};

USTRUCT()
struct FUtilityAction
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere) FName ActionName;
    UPROPERTY(EditAnywhere) uint8 Bucket = 0;       // priority tier (§3.6)
    UPROPERTY(EditAnywhere) float Weight = 1.f;
    UPROPERTY(EditAnywhere) TArray<FConsideration> Considerations;
};
```

### 3.2 Response curves

Four parameterized families cover essentially everything designers need:

```cpp
UENUM()
enum class ECurveType : uint8 { Linear, Polynomial, Logistic, Logit };

/** y = m(x − c)^k + b, or the logistic/logit equivalents.
 *  m = slope, k = exponent, b = vertical shift, c = horizontal shift. */
float FResponseCurve::Evaluate(float X) const
{
    X = FMath::Clamp(X, 0.f, 1.f);
    float Y = 0.f;

    switch (Type)
    {
    case ECurveType::Linear:
    case ECurveType::Polynomial:
        Y = M * FMath::Pow(X - C, K) + B;
        break;

    case ECurveType::Logistic:
        // S-curve: slow, fast, slow. The default for "more is better".
        Y = (K / (1.f + FMath::Exp(-10.f * M * (X - 0.5f - C)))) + B;
        break;

    case ECurveType::Logit:
        // Inverse-S: fast, slow, fast. For thresholds.
        Y = M * FMath::Loge((X - C) / (1.f - (X - C))) / 5.f + 0.5f + B;
        break;
    }
    return FMath::Clamp(Y, 0.f, 1.f);
}
```

The clamp at both ends matters. A logit at x=0 or x=1 goes to ±infinity, and an unclamped NaN propagates silently through the multiply and produces an action that never wins — a bug that's miserable to track down.

**Curve authoring is the main design surface.** Designers spend far more time tuning curves than restructuring trees, so the curve editor (§10.3) deserves more polish than the graph editor.

### 3.3 The multiplication problem ★

Scores multiply, and every consideration is ≤ 1. So **more considerations mean a lower score, mechanically, regardless of how good the action is.**

| Considerations, each at 0.9 | Product |
|---|---|
| 2 | 0.810 |
| 3 | 0.729 |
| 4 | 0.656 |
| 5 | 0.590 |
| 8 | 0.430 |

A carefully-authored action with eight considerations loses to a crude one with two, every time. Designers respond by deleting considerations, which is exactly backwards.

The standard fix is a **compensation factor** that scales the score back up in proportion to how many considerations were multiplied:

```cpp
float ApplyCompensation(float Score, int32 NumConsiderations)
{
    if (NumConsiderations <= 1) return Score;

    const float ModificationFactor = 1.f - (1.f / NumConsiderations);
    const float MakeUpValue        = (1.f - Score) * ModificationFactor;
    return Score + (MakeUpValue * Score);
}
```

Working the numbers: four considerations at 0.9 gives 0.656; compensated it's 0.825. Two at 0.9 gives 0.81; compensated 0.887. The gap narrows from 0.154 to 0.062 — the bias is reduced substantially without being eliminated, which is the intent. An action with more requirements *should* be slightly harder to satisfy; it just shouldn't be impossible.

**This is the single most commonly missed piece of a utility implementation**, and its absence is invisible until a designer complains that "the detailed behaviors never fire."

### 3.4 Early-out ★

Because the running product only decreases, you can abandon an action as soon as it can no longer beat the best score so far:

```cpp
float ScoreAction(const FUtilityAction& Action, const FContext& Ctx, float BestSoFar)
{
    float Score = Action.Weight;
    const int32 N = Action.Considerations.Num();

    for (int32 i = 0; i < N; ++i)
    {
        // Compensation can only raise the score, so bound by what the
        // remaining considerations could contribute at best (1.0 each).
        const float BestPossible = ApplyCompensation(Score, N);
        if (BestPossible < BestSoFar)
        {
            return 0.f;   // cannot win — stop evaluating
        }
        Score *= EvaluateConsideration(Action.Considerations[i], Ctx);
    }
    return ApplyCompensation(Score, N);
}
```

**Order considerations cheapest-and-most-discriminating first.** A "is the target alive" check that returns 0 costs one branch and eliminates the action; a "path length to target" consideration that requires a navmesh query costs microseconds. Getting this order right is often a larger performance win than any micro-optimization, and the editor should let designers reorder considerations by drag.

Worth surfacing in the debugger: which consideration caused each early-out, and how often. That tells a designer where their expensive checks are.

### 3.5 Oscillation ★

Two actions scoring 0.71 and 0.70 will swap every frame as inputs jitter, and the agent visibly twitches. This is the most common complaint about utility AI in practice.

Three mitigations, and you generally want all three:

```cpp
struct FSelectionPolicy
{
    /** Multiplicative bonus to the currently-running action. */
    float CurrentActionBonus = 1.1f;

    /** Minimum time an action must run before it can be replaced. */
    float MinCommitmentSeconds = 0.5f;

    /** A challenger must beat the incumbent by this margin. */
    float SwitchThreshold = 0.05f;

    /** Re-evaluation rate. 60Hz is almost never necessary. */
    float EvaluationHz = 5.f;
};
```

The last one deserves emphasis: **utility selection does not need to run every frame.** Re-evaluating at 5–10 Hz is usually indistinguishable to the player, costs an order of magnitude less, and dramatically reduces flicker for free. Stagger agents across frames so the cost is spread rather than spiking.

Make commitment overridable — a "combat begins" event should interrupt a committed idle action immediately. Expose that as a per-action `Interruptible` flag plus an explicit force-reevaluate call.

### 3.6 Buckets

Pure utility scoring makes everything compete with everything, which means a well-tuned "flee" can lose to a lucky "idle." Buckets impose priority tiers: evaluate the highest bucket first, and only fall through if nothing in it scores above a threshold.

```
Bucket 0  Emergency    (flee, revive, take cover under fire)
Bucket 1  Combat       (attack, reload, reposition)
Bucket 2  Investigate  (check noise, search last known position)
Bucket 3  Ambient      (idle, patrol, chat, use object)
```

This also compounds with early-out: once a high bucket produces a strong winner, lower buckets are never scored at all.

---

## 4. Behavior tree semantics

### 4.1 Unreal's BT is event-driven, not tick-from-root ★

There is widespread confusion here, including in published comparisons. **Unreal's Behavior Tree does not re-walk the tree every tick.** It remembers the currently-executing node and only re-evaluates when something tells it to.

| | Classic BT (literature) | **Unreal BT** |
|---|---|---|
| Each tick | Walk from root to the running leaf | Continue the running node |
| Reactivity | Automatic — conditions re-checked on the way down | Via **observer decorators** watching blackboard keys |
| Cost per tick | O(depth) | O(1) plus active services |
| Failure mode | Expensive at scale | Abort semantics are confusing |

Epic's design is more efficient and less obvious. If you build your own runtime, **pick one explicitly and document it**, because the two produce different behavior from the same graph.

### 4.2 Observer aborts are the #1 source of BT bugs ★

A blackboard decorator with "Observer Aborts" set controls what happens when its condition changes while something else is running:

| Setting | Meaning |
|---|---|
| **None** | Condition checked only when execution passes through |
| **Self** | Abort this branch if the condition becomes false while it's running |
| **Lower Priority** | Abort lower-priority (rightward) branches if this condition becomes true |
| **Both** | Both of the above |

Getting this wrong produces behavior that's hard to reason about: a branch that never interrupts when it should, or one that interrupts constantly.

**A debugger that visualizes abort logic would address this directly**, and nothing does it well today:

- Which decorators are currently observing, and which keys
- When an abort fires: which decorator, which key changed, from what to what, which branch was aborted
- A timeline of aborts, so a designer can see the pattern

That's a concrete, valuable feature that follows from understanding the actual semantics rather than the textbook ones.

### 4.3 Where utility fits

Utility AI integrates as a **selector**, not as a replacement:

**In a BT** — a `UtilitySelector` composite that scores its children and runs the winner. Everything else in the tree is unchanged, and the scoring re-runs on the policy's schedule (§3.5) rather than every tick.

**In a StateTree** — a utility evaluator that selects among transition targets. StateTree's transition model is a natural fit for "pick the best next state."

Keeping the scoring core engine-agnostic (plain structs, no `UObject`, no engine types) is what makes both integrations thin — and makes the core unit-testable outside the engine entirely, which matters more than it sounds (§16.1).

---

## 5. The authoring format ★

### 5.1 Text, because binary assets don't merge

`.uasset` is binary. Two designers editing the same behavior tree in the same sprint produces a merge conflict that git cannot resolve and that nobody can read. The workarounds — asset locking, "only one person touches AI" — are workflow damage.

A text format with a visual editor that round-trips solves it: real diffs, real merges, real code review.

```yaml
# ai/guard.behavior.yaml
version: 1
type: utility
name: GuardBehavior

inputs:
  - { name: DistanceToTarget,  source: blackboard, key: TargetActor,  transform: distance }
  - { name: Health,            source: attribute,  key: HealthPercent }
  - { name: AmmoRatio,         source: attribute,  key: AmmoPercent }
  - { name: TimeSinceLastSeen, source: blackboard, key: LastSeenTime, transform: elapsed }

actions:
  - name: TakeCover
    bucket: 0
    considerations:
      - input: Health
        range: [0, 1]
        curve: { type: logistic, m: -1.0, k: 1.0, b: 0.0, c: 0.0 }
      - input: DistanceToTarget
        range: [0, 3000]
        curve: { type: polynomial, m: -1.0, k: 2.0, b: 1.0, c: 0.0 }
    task: { type: MoveToCover, params: { searchRadius: 1500 } }

  - name: Attack
    bucket: 1
    considerations:
      - input: DistanceToTarget
        range: [0, 3000]
        curve: { type: polynomial, m: -1.0, k: 1.0, b: 1.0, c: 0.0 }
      - input: AmmoRatio
        range: [0, 1]
        curve: { type: linear, m: 1.0, k: 1.0, b: 0.0, c: 0.0 }
    task: { type: FireAtTarget }

policy:
  evaluationHz: 5
  currentActionBonus: 1.1
  minCommitmentSeconds: 0.5
  switchThreshold: 0.05
```

That diffs beautifully. Changing a curve exponent is a one-line change a reviewer can read and reason about.

### 5.2 Ordering must be stable

A format that serializes maps in hash order produces spurious diffs on every save. Sort deterministically — actions by bucket then name, considerations in authored order (which is semantically meaningful for early-out, §3.4), keys alphabetically within objects.

Round-trip stability is testable: load, save, and assert byte equality (§16.2).

### 5.3 Layout lives in a sidecar ★

Node positions, colors, comment boxes, and collapsed groups are editor state, not behavior. Putting them in the logic file means **moving a node produces a diff**, and the signal-to-noise ratio of your reviews collapses.

```
ai/guard.behavior.yaml     ← logic. Reviewed, merged, meaningful.
ai/guard.layout.json       ← positions and colors. Usually gitignored or merged "theirs".
```

Make the editor able to auto-layout from scratch, so the sidecar is genuinely optional. A team that gitignores layout entirely and relies on auto-layout gets conflict-free AI authoring, which is a real and unusual property.

### 5.4 Compile to a runtime format

YAML is for humans. The runtime should load a compiled binary: flattened arrays, resolved indices, no string lookups, no parsing at load.

```
guard.behavior.yaml  →  [cook-time compile]  →  guard.aib (binary)
```

Compile during the Unreal cook step so shipped builds never see YAML. The compiler is also where validation happens (§10.4) — a malformed curve or a dangling task reference should fail the build, not fail at runtime in front of a player.

---

## 6. The Unreal runtime

### 6.1 Plugin structure and version churn

```
Plugins/AIBehaviorToolkit/
├── AIBehaviorToolkit.uplugin
└── Source/
    ├── AIBehaviorCore/        ← engine-agnostic scoring. No UObject, no UE types.
    ├── AIBehaviorRuntime/     ← UE integration: components, BT/StateTree nodes
    ├── AIBehaviorDebug/       ← the debug transport (§7)
    └── AIBehaviorEditor/      ← editor-only: asset types, cook-time compile
```

**Unreal's API changes every major version.** A plugin targeting 5.3 may not compile on 5.5, and the breakage is usually in editor modules and reflection macros rather than in core math.

Two mitigations:

- **Keep `AIBehaviorCore` free of engine types entirely.** It's plain C++ structs and functions. It compiles standalone, it unit-tests without the engine, and it survives version upgrades untouched. The engine-specific surface is then small enough to port in an afternoon.
- **Version-gate with `ENGINE_MAJOR_VERSION` / `ENGINE_MINOR_VERSION`** where APIs differ, and test against at least the current and previous LTS-ish versions in CI.

### 6.2 UObject nodes versus plain structs

Unreal's own BT instantiates node objects as `UObject`s, which enables Blueprint subclassing but costs allocation, GC pressure, and pointer chasing.

| | `UObject` nodes | Plain structs |
|---|---|---|
| Blueprint authoring | ✅ | ❌ (needs a bridge) |
| Per-agent state | Natural | Needs an explicit instance array |
| Performance at 200 agents | Poor | Good |
| GC pressure | Real | None |

**Recommendation: structs for the scoring core, with a `UObject` bridge for Blueprint-authored tasks.** Considerations and curves are pure math and never need to be Blueprint-extended. Tasks — the things that actually do something in the world — do.

```cpp
// Scoring: hot path, plain data, cache-friendly.
struct FUtilityRuntime
{
    TArray<FUtilityAction> Actions;        // flat, contiguous
    TArray<float>          CachedInputs;   // indexed by input id, not name
    TArray<float>          Scores;
};

// Tasks: cold path, Blueprint-extensible.
UCLASS(Blueprintable, Abstract)
class UAIBehaviorTask : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintNativeEvent) void OnStart(AAIController* C);
    UFUNCTION(BlueprintNativeEvent) EAITaskStatus OnTick(AAIController* C, float Dt);
    UFUNCTION(BlueprintNativeEvent) void OnAbort(AAIController* C);
};
```

**Resolve input names to indices at compile time** (§5.4). String-keyed lookups in a per-frame scoring loop are a meaningful cost at 200 agents and are trivially avoidable.

### 6.3 Blackboard interoperability

Read from and write to Unreal's existing `UBlackboardComponent` rather than inventing a parallel store. Designers already have keys there, EQS reads them, and BT decorators observe them.

```cpp
float FInputSource::Evaluate(const UBlackboardComponent* BB, const AAIController* C) const
{
    switch (Type)
    {
    case EInputType::BlackboardFloat:
        return BB->GetValueAsFloat(Key);

    case EInputType::BlackboardActorDistance:
    {
        const AActor* Target = Cast<AActor>(BB->GetValueAsObject(Key));
        if (!Target || !C->GetPawn()) return MaxRange;   // absent ≠ zero distance
        return FVector::Dist(C->GetPawn()->GetActorLocation(), Target->GetActorLocation());
    }
    // ...
    }
}
```

That `return MaxRange` on a null target is the kind of detail that matters. Returning 0 for "no target" means "target is right here," which inverts the consideration and makes the agent attack nothing.

### 6.4 StateTree integration

StateTree binds data per-state rather than through a shared blackboard, so the integration shape differs:

```cpp
USTRUCT()
struct FUtilitySelectEvaluator : public FStateTreeEvaluatorCommonBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = Parameter)
    TObjectPtr<UAIBehaviorAsset> Behavior;

    /** Output: the winning action, bound to a transition condition. */
    UPROPERTY(EditAnywhere, Category = Output)
    FName SelectedAction;

    virtual void Tick(FStateTreeExecutionContext& Ctx, float Dt) const override;
};
```

The scoring core is identical; only the input sourcing and the output binding change. That's the payoff for keeping the core engine-agnostic (§4.3).

---

## 7. The debug protocol ★

### 7.1 Beyond PIE

First-party debugging works in the editor. The bugs that matter often don't reproduce there — dedicated server AI, cooked-build timing, platform-specific behavior, and anything that only appears with 200 agents.

So the transport must work from a **cooked build and a dedicated server**, over a network, with the editor as a separate process.

```
┌──────────────┐        TCP / WebSocket        ┌──────────────────┐
│ Editor       │◀──────────────────────────────▶│ Game process     │
│ (TypeScript) │   subscribe / deltas / cmds    │ (PIE, cooked,    │
└──────────────┘                                │  or server)      │
                                                └──────────────────┘
```

Gate it behind a build flag so shipping builds don't carry a debug listener, and bind to loopback by default with an explicit opt-in for remote.

### 7.2 You cannot stream everything ★

The arithmetic:

```
200 agents × 30 actions × 5 considerations × 10 Hz = 300,000 values/second
```

At even 8 bytes each that's 2.4 MB/s of raw floats, before framing — and it's useless data, because nobody is looking at 200 agents.

**Subscribe to what's being watched:**

```cpp
struct FDebugSubscription
{
    TArray<uint32> AgentIds;          // usually exactly one
    EDebugDetail   Detail;            // Summary | Scores | FullBreakdown
    float          MaxHz = 10.f;
    bool           bDeltasOnly = true;
};
```

Three levels:

| Level | Content | Bandwidth |
|---|---|---|
| **Summary** | Current action per agent, on change only | Tiny — fine for all agents |
| **Scores** | All action scores for subscribed agents | Moderate |
| **FullBreakdown** | Per-consideration values and curve outputs | Large — one agent at a time |

**Deltas only.** An agent whose state hasn't changed sends nothing. Most agents are idle most of the time, and the wire goes quiet accordingly.

### 7.3 Ring buffer on the engine side

The editor should be able to scrub backward a few seconds without a full recording session. Keep a fixed-size ring buffer in the game process and let the editor request history on demand.

```cpp
class FDebugRingBuffer
{
    static constexpr int32 Capacity = 600;   // 60s at 10Hz
    TCircularBuffer<FDebugFrame> Frames;
public:
    void Push(FDebugFrame&& Frame);
    TArray<FDebugFrame> Range(double StartTime, double EndTime) const;
};
```

Fixed capacity is the point — a debug buffer that grows will eventually be the reason a long play session runs out of memory.

### 7.4 Commands back to the game

The connection is bidirectional, which enables the things that make debugging fast:

- **Pause / step** the AI without pausing the game
- **Force an action** on an agent, to test a branch without engineering the situation
- **Override an input value**, to see what the agent would do if health were 0.1
- **Hot-reload a behavior** — recompile the YAML and swap it in without restarting

Hot reload is the one that changes how it feels to work. Tuning a curve and seeing the agent's behavior change within a second, without leaving the running game, is the difference between iteration and a build cycle.

---

## 8. Debug visualization

### 8.1 The BT view

Mirror what Unreal's debugger does, plus the thing it doesn't:

- Current execution path highlighted
- Node status colors (running, success, failure)
- **Active observer decorators and the keys they're watching** (§4.2)
- **An abort log** — which decorator fired, which key changed, which branch was cancelled

That last pair is the differentiator. It directly addresses the most common source of BT confusion.

### 8.2 The utility score breakdown ★

This is the feature that justifies the tool for utility AI.

A behavior tree has structure you can point at. Utility AI has numbers, and "why did it pick that?" is genuinely hard to answer without tooling:

```
Agent: Guard_03          Selected: TakeCover (0.847)

┌─ Bucket 0 ────────────────────────────────────────────────────┐
│ ▶ TakeCover                                          0.847 ✓  │
│     Health            0.22 → logistic(m=-1.0)  → 0.91         │
│     DistanceToTarget  0.31 → poly(k=2)         → 0.93         │
│     compensation ×1.03                                        │
│                                                               │
│   Retreat                                            0.612    │
│     Health            0.22 → linear            → 0.78         │
│     AllyCount         0.00 → poly(k=1)         → 0.10  ◀ low  │
│     compensation ×1.05                                        │
└───────────────────────────────────────────────────────────────┘
┌─ Bucket 1 ──────────────────────────── not evaluated (bucket 0 won)
```

Every level is visible: raw input, normalized value, curve applied, resulting score, the product, the compensation. A designer can see immediately that `AllyCount` at zero is what killed `Retreat`.

Add a **score history graph** — each action's score over the last few seconds — and oscillation becomes visible as crossing lines rather than as a twitching character.

And show **which consideration triggered each early-out** (§3.4), so designers learn where their expensive checks are being wasted.

### 8.3 In-world visualization

Some things only make sense in the level:

- Floating text above each agent with its current action and score
- Color coding by bucket
- Lines to the target actors involved in scoring
- Integration with Unreal's **Visual Logger**, which is already good at recording AI event timelines and is underused

Use `UE_VLOG` for event recording rather than building a parallel system. It works in cooked builds, it has an existing viewer, and the data survives the session.

---

## 9. Record and replay ★

### 9.1 Why it matters most here

AI bugs are frequently non-reproducible. "The guard sometimes runs into the wall" is a bug report that takes a day to reproduce and thirty seconds to diagnose once you can see it.

**Recording a session and replaying it in the editor is probably the highest-value debugging feature available**, and almost nothing in this space has it.

### 9.2 What to record

You don't need the game state — only what the AI saw and decided:

```cpp
struct FRecordedFrame
{
    double                Time;
    uint32                AgentId;
    TArray<float>         InputValues;     // indexed, resolved at compile time
    TArray<float>         ActionScores;
    FName                 SelectedAction;
    uint32                BehaviorVersion; // which compiled asset produced this
};
```

Small enough to record continuously. At 10 Hz with 30 actions and 20 inputs that's roughly 200 bytes per agent per frame — a few megabytes for a long session with a handful of tracked agents.

### 9.3 Determinism

Replay requires that the same inputs produce the same decision. Which means:

- **Seeded RNG**, with the seed recorded, if scoring uses any randomness at all
- **No dependence on wall-clock time** — use the recorded frame time
- **No dependence on frame rate** — record the evaluation delta

The scoring core has no side effects and no engine dependencies (§4.3), so this is achievable. Record the inputs, replay them through the same core, and assert the same output.

**A test that replays a recorded session and asserts identical decisions is a strong regression suite** — it catches a curve change that alters historical behavior, which is exactly what you want to know before shipping a tuning pass.

### 9.4 Replaying against a modified behavior

The genuinely clever version: **replay a recorded session against an edited behavior file** and diff the decisions.

"Your curve change makes the guard take cover 40% more often in this recorded encounter" is a design review conversation nobody currently gets to have. It turns tuning from guesswork into measurement, and it's a small amount of code on top of §9.2.

---

## 10. The editor

### 10.1 External, not in-engine

An external editor is more work than an in-engine one, and it buys three things:

- **It can open a `.yaml` from git without the engine running** — code review, CI, quick edits
- **It survives engine version upgrades** — no Slate API churn
- **It connects to any build**, not just PIE (§7.1)

The cost is that you're not in the editor where designers already are. Mitigate with a launch button from the Unreal editor and a shared asset picker.

### 10.2 The graph view

For utility AI the graph is flatter than a behavior tree — buckets containing actions containing considerations. An outline or card layout is often clearer than a node graph, and worth prototyping before committing to graph-shaped UI.

For BTs, a node graph is right, with the usual affordances: auto-layout, collapse, comment boxes, and search.

### 10.3 The curve editor is the main surface ★

Designers spend most of their time tuning curves, not restructuring graphs. Give it real attention:

- Live curve plot with draggable parameters
- **Overlay the agent's current input value as a marker on the curve** during a live debug session — this is the single most useful curve-editing affordance, because it shows exactly where on the curve the agent is operating
- Side-by-side comparison of curves across actions competing for the same input
- Preset library
- The resulting score displayed numerically as parameters change

### 10.4 Validation at edit time

Catch what you can before the cook:

| Check | Severity |
|---|---|
| Unknown input name | Error |
| Task type not registered | Error |
| Curve parameters produce NaN or constant output | Error |
| Consideration range is inverted or zero-width | Error |
| Action can never score above zero | Warning |
| Action always scores highest in its bucket | Warning — the others are dead |
| Expensive consideration ordered first | Hint (§3.4) |
| Bucket empty | Warning |

The "always wins" and "never wins" checks are cheap to compute by sampling the input space and surprisingly effective at finding authoring mistakes.

---

## 11. Performance

### 11.1 The budget

Game AI gets a small slice of the frame. At 60 fps the whole frame is 16.7 ms and AI typically gets 1–2 ms.

```
200 agents × 30 actions × 5 considerations = 30,000 consideration evaluations
```

At 10 ns each that's 0.3 ms — fine. But a consideration that does a navmesh query or a line trace is microseconds, not nanoseconds, and a hundred of those blows the budget alone.

### 11.2 The levers, in order of effect

1. **Evaluate at 5–10 Hz, not 60** (§3.5). Roughly an order of magnitude, and it improves behavior stability too.
2. **Stagger agents across frames.** With 200 agents at 10 Hz, evaluate ~33 per frame rather than all 200 every sixth frame. Spreads the cost and removes the spike.
3. **Early-out** (§3.4), with cheap considerations ordered first.
4. **Buckets** — a strong winner in bucket 0 means buckets 1–3 are never scored.
5. **Cache expensive inputs.** A navmesh path length doesn't change meaningfully in 100 ms; compute once per evaluation, share across all considerations that use it.
6. **Resolve names to indices at compile time** (§6.2).
7. **LOD by distance.** Distant agents evaluate at 1 Hz with a reduced action set. Nobody watches a guard 200 metres away closely enough to notice.

That last one is underused and very effective. An AI LOD system with three tiers — near, medium, far — cuts total cost dramatically with no visible difference.

### 11.3 Measure in-engine

Use `SCOPE_CYCLE_COUNTER` and Unreal Insights rather than wall-clock timing:

```cpp
DECLARE_CYCLE_STAT(TEXT("Utility Score"), STAT_UtilityScore, STATGROUP_AIBehavior);

void FUtilityRuntime::Evaluate(const FContext& Ctx)
{
    SCOPE_CYCLE_COUNTER(STAT_UtilityScore);
    // ...
}
```

Insights gives you per-frame breakdown, and it works in cooked builds — which is where the performance problems actually appear.

### 11.4 Mass, for very large counts

Epic's path for thousands of agents is **Mass** (their ECS) with StateTree, via `UMassStateTreeSchema`. Traditional per-actor AI does not scale to crowd sizes.

If large counts are a target, the utility core's data layout — flat arrays, indexed inputs, no `UObject` — is already Mass-friendly. A Mass processor for utility evaluation is a plausible v2 and the core wouldn't change.

Be explicit about the supported range in the docs. "Designed for up to ~300 individually-authored agents; use Mass for crowds" is a useful and honest statement.

---

## 12. Tech stack and setup

| Layer | Choice | Why |
|---|---|---|
| **Scoring core** | C++17, no engine types | Portable, unit-testable, survives engine upgrades |
| **UE runtime** | C++ plugin, UE 5.4+ | Version-gated (§6.1) |
| **Debug transport** | TCP with a length-prefixed binary protocol | Works from cooked builds and servers |
| **Serialization** | FlatBuffers or hand-rolled for the compiled format | Zero-copy load, no parsing in the runtime |
| **Authoring format** | YAML | Diffs and merges well; JSON is acceptable |
| **Editor** | TypeScript + Electron or Tauri | Cross-platform, fast UI iteration |
| **Editor graph** | Canvas-rendered with hit testing | DOM doesn't scale past a few hundred nodes |
| **Curve plotting** | `uPlot` or hand-rolled SVG | |
| **Core tests** | Catch2 or GoogleTest | Runs without the engine |

Two setup notes:

**Build the core standalone first.** A CMake project with tests that never touches Unreal. You'll iterate on the scoring math far faster, and the engine integration becomes a thin binding layer over something already correct.

**Get a real project to test against.** Epic's free sample content or one of the AI-focused templates gives you agents, navmesh, and perception without building a game. Testing AI tooling against a two-agent test level teaches you nothing about what breaks at 200.

---

## 13. Repository layout

```
AI-Behavior-Toolkit/
├── README.md
├── docs/
│   ├── design.md               ← this document
│   ├── utility-ai.md           ← ★ curves, compensation, tuning guide
│   ├── format.md               ← the YAML schema
│   └── protocol.md             ← the debug wire protocol
├── core/                       ← ★ NO engine dependencies
│   ├── CMakeLists.txt
│   ├── include/aib/
│   │   ├── consideration.h
│   │   ├── curve.h             ← ★ response curves (§3.2)
│   │   ├── scoring.h           ← ★ compensation + early-out (§3.3, §3.4)
│   │   ├── selection.h         ← ★ hysteresis, commitment (§3.5)
│   │   └── compiled.h          ← the binary runtime format
│   └── tests/
├── unreal/Plugins/AIBehaviorToolkit/
│   ├── AIBehaviorToolkit.uplugin
│   └── Source/
│       ├── AIBehaviorCore/     ← thin wrapper over core/
│       ├── AIBehaviorRuntime/
│       │   ├── UtilityComponent.cpp
│       │   ├── BTComposite_UtilitySelector.cpp
│       │   ├── StateTreeUtilityEvaluator.cpp
│       │   └── InputSources.cpp
│       ├── AIBehaviorDebug/
│       │   ├── DebugServer.cpp
│       │   ├── RingBuffer.cpp
│       │   └── Recorder.cpp    ← ★ (§9)
│       └── AIBehaviorEditor/
│           └── Compiler.cpp    ← YAML → binary at cook time
├── editor/
│   ├── src/
│   │   ├── format/             ← parse, serialize, stable ordering
│   │   ├── graph/
│   │   ├── curves/             ← ★ the main design surface (§10.3)
│   │   ├── debug/
│   │   │   ├── client.ts
│   │   │   ├── breakdown.tsx   ← ★ score breakdown (§8.2)
│   │   │   └── replay.ts       ← ★ (§9)
│   │   └── validate/
│   └── package.json
└── samples/
    └── guard.behavior.yaml
```

The `core/` directory having its own CMake build and no engine dependency is the structural decision that makes §6.1, §9.3, and §16.1 all work.

---

## 14. Milestone ladder

### M0 — Scope and positioning ★
**Est. 3–4 days**

Decide which of the three gaps you're filling and write it down. Decide whether you support BT, StateTree, or both. Check Epic's current guidance — it shifted in 2025 and may have moved again.

**Done when:** you can answer "how is this different from what ships in the engine?" in two sentences.

---

### M1 — The scoring core, standalone ★
**Est. 1.5 weeks**

Considerations, all four response curves with clamping, **compensation**, **early-out**, selection policy with hysteresis and commitment, buckets. Plain C++, CMake, full unit tests. **No Unreal at all.**

**Done when:** a test proves that an action with eight considerations can beat one with two, and that a 0.71/0.70 pair doesn't oscillate under the default policy.

---

### M2 — Format and compiler
**Est. 1 week**

YAML schema, parser, stable serialization, layout sidecar, the compiled binary format, validation.

**Done when:** load → save → byte-identical, and a malformed file produces a message naming the line.

---

### M3 — Unreal runtime
**Est. 2 weeks**

Plugin skeleton, component, input sources reading the existing blackboard, Blueprint task bridge, cook-time compilation.

**Done when:** an agent in a test level selects actions using a YAML-authored behavior.

---

### M4 — BT and StateTree integration
**Est. 1.5 weeks**

`UtilitySelector` BT composite, StateTree evaluator.

**Done when:** the same behavior file drives an agent through both host systems with identical decisions.

---

### M5 — Debug protocol ★
**Est. 2 weeks**

TCP server, subscription model, delta encoding, ring buffer, commands including hot reload. **Verified from a cooked build and a dedicated server**, not just PIE.

**Done when:** connecting to a packaged build with 200 agents and subscribing to one costs under 50 KB/s.

---

### M6 — Editor
**Est. 3 weeks**

Graph or card view, the curve editor, validation surfacing, debug client, live connection.

---

### M7 — Score breakdown ★
**Est. 1.5 weeks**

The full per-consideration breakdown, score history graph, early-out attribution, live input markers on curves.

**This is the milestone that makes the tool worth using.** Utility AI without a breakdown view is a black box, and the black box is the reason people avoid utility AI.

---

### M8 — Record and replay ★
**Est. 1.5 weeks**

Session recording, replay in the editor, determinism verification, and **replay-against-modified-behavior diffing**.

---

### M9 — Performance
**Est. 1 week**

Staggering, LOD tiers, input caching, Insights instrumentation, a 200-agent benchmark scene.

---

## 15. Reference implementations

### 15.1 Scoring with compensation and early-out

```cpp
// core/include/aib/scoring.h — no engine types, fully unit-testable.
namespace aib {

inline float Compensate(float Score, int n)
{
    if (n <= 1) return Score;
    const float ModFactor = 1.0f - (1.0f / static_cast<float>(n));
    const float MakeUp    = (1.0f - Score) * ModFactor;
    return Score + (MakeUp * Score);
}

inline float ScoreAction(const CompiledAction& A, const InputCache& In, float BestSoFar)
{
    float Score = A.weight;
    const int n = static_cast<int>(A.considerations.size());

    for (int i = 0; i < n; ++i)
    {
        // The product only decreases, so the compensated current score
        // is an upper bound on what this action can still reach.
        if (Compensate(Score, n) < BestSoFar) return 0.0f;

        const auto& C = A.considerations[i];
        const float raw  = In.values[C.inputIndex];        // index, not name
        const float norm = Normalize(raw, C.inputMin, C.inputMax);
        Score *= EvaluateCurve(C.curve, norm);

        if (Score <= 0.0f) return 0.0f;                    // absorbing zero
    }
    return Compensate(Score, n);
}

} // namespace aib
```

The `Score <= 0` early return is worth having separately from the bound check — a consideration that returns exactly zero is a hard veto and there's no point evaluating further.

### 15.2 Selection with hysteresis

```cpp
SelectionResult Select(const CompiledBehavior& B, const InputCache& In,
                       SelectionState& S, float Dt)
{
    S.timeInCurrentAction += Dt;

    // Commitment: don't even score unless we're allowed to switch.
    const bool bCanSwitch =
        S.timeInCurrentAction >= B.policy.minCommitmentSeconds ||
        S.bForceReevaluate;

    if (!bCanSwitch) return { S.currentAction, S.currentScore, false };

    float best = 0.0f;
    int   bestIdx = -1;

    for (uint8_t bucket = 0; bucket < B.bucketCount; ++bucket)
    {
        for (int i : B.actionsInBucket[bucket])
        {
            float s = ScoreAction(B.actions[i], In, best);

            // Incumbent bonus — the main defence against flicker.
            if (i == S.currentAction) s *= B.policy.currentActionBonus;

            if (s > best) { best = s; bestIdx = i; }
        }
        // A strong winner in this bucket means lower buckets never run.
        if (best > B.policy.bucketSatisfiedThreshold) break;
    }

    // Challenger must clear the incumbent by a margin, not merely exceed it.
    const bool bSwitch =
        bestIdx != S.currentAction &&
        best > S.currentScore + B.policy.switchThreshold;

    if (bSwitch)
    {
        S.currentAction = bestIdx;
        S.timeInCurrentAction = 0.0f;
    }
    S.currentScore = best;
    S.bForceReevaluate = false;
    return { S.currentAction, best, bSwitch };
}
```

Both the bonus *and* the switch threshold are present deliberately. The bonus makes the incumbent harder to beat; the threshold prevents a switch on a trivial margin. Either alone leaves visible flicker in some configurations.

### 15.3 Delta encoding on the wire

```cpp
void FDebugServer::SendFrame(const FDebugFrame& Frame)
{
    if (Subscription.bDeltasOnly && Frame.EqualsIgnoringTime(LastSentFrame))
    {
        return;   // nothing changed — send nothing
    }

    FBufferWriter W(ScratchBuffer);
    W << static_cast<uint8>(EDebugMsg::Frame);
    W << Frame.AgentId << Frame.Time;

    // Bit mask of which action scores changed since the last frame.
    uint32 ChangedMask = 0;
    for (int32 i = 0; i < Frame.Scores.Num(); ++i)
    {
        if (!FMath::IsNearlyEqual(Frame.Scores[i], LastSentFrame.Scores[i], 0.001f))
        {
            ChangedMask |= (1u << i);
        }
    }
    W << ChangedMask;
    for (int32 i = 0; i < Frame.Scores.Num(); ++i)
    {
        if (ChangedMask & (1u << i)) W << Frame.Scores[i];
    }

    Socket->Send(ScratchBuffer);
    LastSentFrame = Frame;
}
```

The 0.001 epsilon means small numerical jitter doesn't count as a change, which cuts traffic substantially on an agent whose situation is stable.

---

## 16. Testing

### 16.1 Core tests run without the engine ★

The scoring core has no engine dependency, so its tests are fast, deterministic, and run in CI without an Unreal installation. That's the payoff for the §6.1 split.

```cpp
TEST_CASE("compensation removes the bias against complex actions") {
    // Two considerations at 0.9 versus eight at 0.9.
    const float simple  = Compensate(std::pow(0.9f, 2), 2);
    const float complex_ = Compensate(std::pow(0.9f, 8), 8);

    // Uncompensated: 0.81 vs 0.43 — a 0.38 gap.
    // Compensated the gap must be far smaller.
    REQUIRE(simple - complex_ < 0.15f);
}

TEST_CASE("early-out never changes the winner") {
    auto behavior = LoadFixture("many_actions.aib");
    auto inputs   = RandomInputs(12345);

    auto withEarlyOut    = Select(behavior, inputs, /*earlyOut=*/true);
    auto withoutEarlyOut = Select(behavior, inputs, /*earlyOut=*/false);

    REQUIRE(withEarlyOut.action == withoutEarlyOut.action);
}

TEST_CASE("curves never produce NaN") {
    for (auto type : AllCurveTypes())
      for (float m : {-2.f, -1.f, 0.f, 1.f, 2.f})
        for (float k : {0.5f, 1.f, 2.f, 4.f})
          for (float x : {0.f, 0.001f, 0.5f, 0.999f, 1.f}) {
            const float y = EvaluateCurve({type, m, k, 0.f, 0.f}, x);
            REQUIRE(std::isfinite(y));
            REQUIRE(y >= 0.0f);
            REQUIRE(y <= 1.0f);
          }
}
```

That third test matters more than it looks. The logit curve goes to infinity at the domain boundaries, and an unclamped NaN silently zeroes an action forever — a bug that presents as "this behavior never fires" with no error anywhere.

The second test is the one that keeps the optimization honest: early-out is only valid if it's provably equivalent, and asserting that across random inputs is cheap insurance.

### 16.2 Format round-trip

```ts
test.each(allFixtures())("round-trips byte-identically: %s", (path) => {
  const original = readFileSync(path, "utf8");
  expect(serialize(parse(original))).toBe(original);
});
```

This catches ordering instability, which is what produces spurious diffs (§5.2).

### 16.3 Oscillation

```cpp
TEST_CASE("near-tied actions do not thrash") {
    auto behavior = LoadFixture("two_close_actions.aib");
    SelectionState state;
    int switches = 0;

    for (int frame = 0; frame < 600; ++frame) {   // 60s at 10Hz
        auto inputs = JitteredInputs(frame, /*amplitude=*/0.02f);
        auto r = Select(behavior, inputs, state, 0.1f);
        if (r.bSwitched) ++switches;
    }
    REQUIRE(switches < 10);   // with jitter alone, uncontrolled would be ~300
}
```

### 16.4 Replay determinism

Record a session, replay it through the core, assert identical decisions frame for frame. Then replay it against a modified behavior and assert the *diff* is what you expected — that's §9.4 as a test.

### 16.5 In-engine performance

A benchmark map with 200 agents, running the scoring at the configured rate, asserting the AI stat group stays under budget. Run it in a cooked build in CI if you can — editor performance is not shipping performance.

---

## 17. Stretch goals

| Feature | Effort | Value |
|---|---|---|
| **Mass/ECS processor** | Large | Crowd-scale utility evaluation; the core's data layout is already suitable (§11.4) |
| **Goal-oriented action planning** | Large | GOAP as a third paradigm alongside BT and utility |
| **Curve auto-tuning from recordings** | Medium | "Make the guard take cover 20% more often" → suggested parameter change. Follows from §9.4. |
| **Unity and Godot runtimes** | Medium each | The core is already engine-agnostic; only the bindings differ |
| **Behavior diffing in the editor** | Small | Visual diff of two versions, for code review |
| **Multiplayer / server-authoritative debugging** | Medium | Attach to a running dedicated server with many clients |
| **Automated behavior testing** | Medium | Assert that an agent reaches a goal state in a scripted scenario, in CI |
| **In-engine editor bridge** | Medium | A Slate panel that launches and syncs with the external editor |
| **Blackboard change history** | Small | "When did this key change, and what wrote it?" — cheap and frequently wanted |
| **StateTree binding inspector** | Medium | The StateTree equivalent of blackboard inspection (§2.1) |

The automated behavior testing item is worth a second look. "Run this scenario headless and assert the guard reaches cover within 5 seconds" turns AI from something only testable by hand into something CI can regression-test — and it reuses the replay infrastructure entirely.

---

## 18. References

### Utility AI

- **Dave Mark**, *Behavioral Mathematics for Game AI* — the foundational text for this approach
- **Dave Mark and Kevin Dill**, "Improving AI Decision Modeling Through Utility Theory" (GDC) — the Infinite Axis Utility System, including response curves and the compensation factor in §3.3
- **Kevin Dill**, various GDC talks on utility-based architectures
- *Game AI Pro* series — several chapters on utility systems, curve design, and combining utility with behavior trees

### Behavior trees

- **Colledanchise & Ögren**, *Behavior Trees in Robotics and AI* — the rigorous treatment, and the clearest statement of classic tick-from-root semantics
- **Chris Simpson**, "Behavior trees for AI: How they work" — the standard introduction
- Epic's Behavior Tree documentation, particularly the observer-abort sections (§4.2)
- **Epic's StateTree documentation** — and note the 2025 guidance shift toward it for new work

### Unreal

| Source | For |
|---|---|
| Unreal plugin and module documentation | §6.1 |
| `UBlackboardComponent` API | §6.3 |
| StateTree API, `FStateTreeEvaluatorBase` | §6.4 |
| MassAI and `UMassStateTreeSchema` | §11.4 |
| Unreal Insights and `SCOPE_CYCLE_COUNTER` | §11.3 |
| Visual Logger (`UE_VLOG`) | §8.3 |

---

## Appendix A — Decision record

| Decision | Rationale |
|---|---|
| **Support both BT and StateTree, not BT alone** | Epic shifted its recommendation to StateTree in 2025 and the BT editor hasn't had a substantive update since 5.3. BT-only tooling targets a maintenance-mode system. |
| **Utility AI is the primary gap** | Epic ships no scoring-based selection for either host system. It's the one part of the original statement that isn't duplicative. |
| Integrate as a selector, never replace the host | Reimplementing a mature execution engine loses every existing asset for no gain |
| **Blackboard inspection is BT-specific** | StateTree binds per-state with explicit bindings; it needs a data-flow inspector, not a key-value table |
| **Text authoring format with a layout sidecar** | `.uasset` is binary and unmergeable; layout data in the logic file makes every node move a diff |
| Stable serialization ordering, round-trip byte-identical | Hash-ordered output produces spurious diffs on every save |
| Compile YAML to a binary runtime format at cook time | Shipped builds never parse text, and the compiler is where validation belongs |
| **Compensation factor on multiplied scores** | Eight considerations at 0.9 gives 0.43 against 0.81 for two — a systematic bias against nuanced actions, and the most commonly missed piece of a utility implementation |
| **Early-out on the compensated upper bound** | The product only decreases, so an action that can't beat the incumbent needn't be finished |
| Cheapest, most-discriminating considerations first | Often a bigger win than any micro-optimization |
| Clamp every curve output to [0,1] | Logit diverges at the domain boundaries and an unclamped NaN silently kills an action forever |
| **Incumbent bonus *and* switch threshold *and* commitment time** | Any one alone leaves visible flicker in some configurations |
| **Evaluate at 5–10 Hz, staggered across frames** | An order of magnitude cheaper, indistinguishable to the player, and reduces oscillation for free |
| Buckets for priority tiers | Stops a lucky "idle" from beating a well-tuned "flee", and compounds with early-out |
| **Scoring core has zero engine dependencies** | Survives UE API churn, unit-tests without the engine, and makes replay determinism and multi-engine ports tractable |
| Structs for scoring, `UObject` only for tasks | Considerations are pure math and never need Blueprint extension; tasks do |
| Resolve input names to indices at compile time | String lookups in a per-frame loop at 200 agents are avoidable |
| Read the existing `UBlackboardComponent` | Designers already have keys there and EQS and BT decorators already use them |
| Absent target returns max range, not zero | Zero means "right here" and inverts the consideration |
| **Debug transport works from cooked builds and dedicated servers** | The bugs that matter frequently don't reproduce in PIE |
| **Subscribe to one agent; send deltas only** | 200 agents × 30 actions × 5 considerations × 10 Hz is 300,000 values/second of data nobody is looking at |
| Fixed-capacity ring buffer in the game process | A growing debug buffer eventually ends a long session |
| Bidirectional protocol with hot reload | Tuning a curve and seeing the result in a running game changes how the work feels |
| **Score breakdown view showing every level** | Utility AI has no structure to point at; without the breakdown, "why did it pick that?" is unanswerable |
| Visualize BT observer aborts and their triggers | The single most confusing part of Unreal's BT, and nothing shows it well |
| **Record and replay sessions** | AI bugs are frequently non-reproducible, and almost nothing in this space has replay |
| Record only inputs and decisions, not game state | ~200 bytes per agent per frame — small enough to record continuously |
| **Replay against a modified behavior and diff decisions** | Turns tuning from guesswork into measurement, for very little extra code |
| Curve editor gets more polish than the graph editor | Designers spend most of their time tuning curves, not restructuring graphs |
| Live input marker on the curve during debugging | Shows exactly where on the curve the agent is operating |
| Use Visual Logger rather than a parallel event system | It works in cooked builds and already has a viewer |

---

## Appendix B — Quick reference card

```
POSITIONING — Epic shifted in 2025
  StateTree = recommended for NEW AI work (production-ready 5.5/5.6)
  Behavior Tree = still works, NOT deprecated, but no substantive
                  editor update since UE 5.3
  StateTree has NO shared blackboard (per-state bindings instead)
  → support BOTH, and lead with the three gaps:
      utility AI · diffable authoring · debugging beyond PIE

UTILITY SCORING
  consideration: input → normalize → response curve → [0,1]
  action score = Π considerations, then COMPENSATE
  curves: linear · polynomial · logistic (S) · logit (inverse-S)
          y = m(x−c)^k + b       ★ CLAMP output or logit → NaN

  ★ MULTIPLICATION BIAS
      2 considerations @ 0.9 → 0.810
      4 @ 0.9              → 0.656
      8 @ 0.9              → 0.430
    complex actions lose by construction. fix:
      mod   = 1 − 1/n
      makeup = (1 − score) × mod
      final  = score + makeup × score

  ★ EARLY-OUT: product only decreases
      if Compensate(running, n) < bestSoFar → abandon
      order cheap + discriminating considerations FIRST

  ★ OSCILLATION: need all three
      incumbent bonus ×1.1 · switch threshold +0.05 · commitment 0.5s
      and evaluate at 5–10 Hz, staggered — not 60

  buckets: emergency > combat > investigate > ambient
           strong winner in a bucket → lower buckets never scored

UNREAL BT — event-driven, NOT tick-from-root
  observer aborts:  None · Self · Lower Priority · Both
  ★ the #1 source of BT bugs — visualize which fired and why

FORMAT
  logic in .yaml  ·  layout in a SIDECAR (or auto-layout, gitignored)
  stable ordering → round-trip byte-identical
  compile to binary at cook time; runtime never parses text

DEBUG
  200 agents × 30 actions × 5 considerations × 10Hz = 300,000 values/s
  → subscribe to ONE agent · deltas only · 3 detail levels
  → fixed-capacity ring buffer, scrub backward
  → bidirectional: pause, step, force action, override input, HOT RELOAD
  → must work in COOKED builds and on DEDICATED SERVERS

CORE ARCHITECTURE
  scoring core: plain C++, zero engine types
    → survives UE API churn
    → unit tests run without Unreal
    → makes replay determinism and Unity/Godot ports possible
  structs for scoring (hot) · UObject only for tasks (Blueprint)
  resolve input names → indices at compile time
```
