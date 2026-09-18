# The Unreal plugin

**This module has not been compiled.** Everything under `core/`, `editor/` and
`samples/` is built and tested by `make` at the repository root; this is not,
because building it needs an Unreal installation. Saying so plainly is more
useful than implying otherwise, so:

| | Built here | Tested here |
|---|---|---|
| `core/` | yes, CMake + 56 tests | yes |
| `editor/` | yes, 81 tests | yes |
| cross-language agreement | yes | yes, 94,500 curve points and 1,500 selection frames |
| `unreal/` | **no** | **no** |

What that costs is real and worth being specific about: the `Build.cs` files,
the reflection macros, and the exact signatures of the engine virtuals are the
parts most likely to be wrong, and they are exactly the parts an Unreal
installation would catch in thirty seconds. Section 6.1 says Unreal's API
changes every major version and "the breakage is usually in editor modules and
reflection macros rather than in core math" — this code is on the wrong side of
that line and has not been through a compiler.

What it does not cost is the part that matters most. The scoring, the
selection, the bit timing of the wire format, the compiled-asset layout and the
replay determinism all live in `core/` and are covered. The plugin is a
binding layer over them by design, which is the whole argument in sections 6.1
and 4.3.

---

## Structure

```
Plugins/AIBehaviorToolkit/
├── AIBehaviorToolkit.uplugin
└── Source/
    ├── AIBehaviorCore/      compiles ../../../core/src directly -- not a copy
    ├── AIBehaviorRuntime/   UtilityComponent, BT composite, StateTree evaluator
    ├── AIBehaviorDebug/     the TCP transport, ring buffer, recorder
    └── AIBehaviorEditor/    cook-time compile
```

`AIBehaviorCore.Build.cs` points at `core/` outside the plugin rather than
vendoring a copy. That is deliberate: the code the unit tests exercise and the
code the game ships are then the same files, and cannot drift.

## The two integrations

Section 2.1's strategic point is that supporting both hosts is "the difference
between a tool with a future and one without" — Epic recommends StateTree for
new AI work, and the BT editor has had no substantive update since 5.3, but
every shipped project has BT assets.

**Behavior Tree** — `UBTComposite_UtilitySelector` scores its children and runs
the winner. Two details from section 4 shape it:

- Unreal's BT is **event-driven, not tick-from-root** (4.1). The composite
  cannot rely on being re-entered each frame, so it drives its own
  re-evaluation on the policy's schedule and calls `RequestExecution` when the
  winner changes.
- It adds **no abort semantics of its own** (4.2). Observer aborts are already
  the single most confusing part of Unreal's BT; a composite that also aborted
  would compose with a "Lower Priority" decorator in ways nobody could reason
  about.

**StateTree** — `FStateTreeUtilityEvaluator` selects among transition targets.
The only structural difference is that StateTree binds data per-state rather
than through a shared blackboard (6.4), so inputs arrive as bound properties
and the winner is published as a bound `FName` for a transition condition to
read. The scoring call is identical.

## Things in here that are easy to get wrong

**An absent target is not a near one.** `InputSources.cpp` returns the
configured fallback — the maximum range — when a blackboard actor key is null.
Section 6.3: "Returning 0 for 'no target' means 'target is right here,' which
inverts the consideration and makes the agent attack nothing."

**An unreachable target is not a near one either.** A partial navmesh path
falls back the same way, rather than to straight-line distance, which would
tell the agent to attack something across a chasm.

**The debug listener binds to loopback.** `-aibdebugremote` opts out. The
command set in section 7.4 includes forcing an action and hot-reloading a
behavior, so a listener on `0.0.0.0` in a Test build handed to playtesters is
a remote code execution surface.

**Structs for scoring, `UObject` only for tasks** (6.2). Considerations and
curves are pure math and never need Blueprint extension; tasks do. At 200
agents the allocation and GC pressure of `UObject` nodes in the hot path is a
real cost and an avoidable one.

## Before this ships

The things an engine build would tell you, in the order they are likely to
bite:

1. `ConditionalAddModuleDirectory` in `AIBehaviorCore.Build.cs` — the API for
   pulling in out-of-module sources has moved between versions; check it
   against the target engine.
2. The `FStateTreeEvaluatorCommonBase` virtuals. `GetDescription`'s signature
   in particular changed across the 5.4–5.6 range.
3. `UBTCompositeNode::OnNextChild` binding and `CastInstanceNodeMemory` — both
   stable for a long time, but both unverified here.
4. `UBlackboardComponent::GetValue<>` key type templates.
5. Version-gate anything above with `ENGINE_MAJOR_VERSION` /
   `ENGINE_MINOR_VERSION` and build against at least the current and previous
   versions in CI, per section 6.1.
