# AI Behavior Toolkit — v1

Utility AI for Unreal that plugs into **both** Behavior Tree and StateTree
rather than replacing either, authored in a **diffable, mergeable text format**
with score-breakdown tooling and session record/replay — implemented from
[`../1—Documentation/design.md`](../1—Documentation/design.md).

```
make          # 56 C++ tests, 81 TypeScript tests, and the cross-check between them
make bench    # the section 11 performance budget, measured
make demo     # compile the sample and explain a decision
```

---

## The three gaps this fills

The design document's first move is to reframe the project, and the reframe is
the reason any of this is worth building. Unreal already ships a BT visual
editor, live PIE debugging and a blackboard inspector — three of the four
features in the original statement are first-party. What Epic leaves open:

| Gap | Why it's real | Where it lives here |
|---|---|---|
| **Utility AI** | No first-party scoring-based selection for BT or StateTree | `core/` |
| **Diffable authoring** | `.uasset` is binary: no diff, no merge, no code review | `editor/src/format/` |
| **Debugging beyond PIE** | First-party debugging stops at the editor; the hard bugs are in cooked builds and dedicated servers | `editor/src/debug/`, `unreal/.../AIBehaviorDebug` |

## What is here

```
core/        C++17, zero engine types. Curves, compensation, early-out,
             bucketed selection with hysteresis, the compiled format,
             record and replay. CMake + 56 tests + a benchmark.
editor/      TypeScript. Canonical YAML round-trip, validation, the
             YAML -> .aib compiler, the debug protocol, replay and
             behavior diffing. 81 tests.
unreal/      The plugin. NOT COMPILED HERE -- see unreal/README.md.
samples/     guard.behavior.yaml
docs/        the tuning guide, the format, the protocol, and where this
             diverges from the design document
```

`core/` having no engine dependency is the structural decision everything else
rests on: it is what lets the tests run without Unreal, what makes replay
determinism achievable, and what makes the plugin a binding layer rather than a
port.

## Two implementations, checked against each other

There are two scorers in this repository, and that is deliberate. The C++ one
ships inside the game. The TypeScript one answers questions with no game
running — replaying a session against an edited behavior, the always-wins /
never-wins validation, drawing what a curve change will do.

That duplication is only justified if they agree, so
`editor/tests/crosscheck.test.ts` compiles a behavior in TypeScript, loads it in
the C++ core, and compares:

- **94,500 curve points** across four families and the full parameter grid
- **2,000 input vectors** scored action by action
- **1,500 selection frames** with state carried through — decisions, scores,
  switch flags, and *work done*, so a difference in what early-out prunes shows
  up even when it has not yet changed a winner
- the content version hash, computed independently on both sides

If the editor tells a designer their curve change makes the guard take cover
40% more often and the runtime disagrees, the tool is worse than no tool: it is
a confident wrong answer feeding a design review.

**It found three real bugs**, all in `docs/notes-on-the-spec.md`.

## Three things the design document gets wrong

Found by building it, not by reading it.

**Section 16.1's compensation test contradicts section 3.3's formula.** The
worked numbers in 3.3 are right; the test in 16.1 applies the 2-vs-4 threshold
(0.15) to the 2-vs-8 case, where the true compensated gap is **0.242**.

**Section 15.2's selection reference can lock an agent out permanently.** It
compares a challenger against a *carried-forward* incumbent score. An action
selected at 0.90 that collapses to 0.10 keeps a 0.90 bar in front of every
challenger, forever. It presents as "the guard kept taking cover after the
threat was gone" — which reads as a tuning problem and is not one.

**Clamping the logit is necessary but not sufficient.** At the clamp,
`1.0f - 0.9999f` in float32 is catastrophic cancellation, and the logarithm
amplifies it. The two implementations disagreed by 8.3e-6 until both carried
the exact epsilon instead.

Fixing the second one exposed a fourth, subtler problem that only a
cross-implementation check would surface: **the incumbent must never be
early-out**. Early-out is exact for finding the maximum, but selection also
needs the incumbent's real score, because that is the bar the challenger has to
clear. A pruned incumbent reports zero and the agent switches on a margin it
never had — intermittently, and only in the near-tie case the hysteresis exists
to handle.

## The multiplication problem, which is the whole thing

An action's score is the product of its considerations, and every consideration
is ≤ 1. So **more considerations mean a lower score, mechanically**:

| considerations at 0.9 | raw | compensated |
|---|---|---|
| 2 | 0.810 | 0.887 |
| 4 | 0.656 | 0.825 |
| 8 | 0.431 | 0.645 |

A carefully-authored eight-consideration action loses to a crude two-
consideration one by 0.38, by construction. Designers respond by deleting
considerations, which is exactly backwards. The design document calls the
compensation factor "the single most commonly missed piece of a utility
implementation", and its absence is invisible until somebody complains that the
detailed behaviors never fire.

Compensation reduces the bias to 0.242 without eliminating it — deliberately.
An action with more requirements *should* be slightly harder to satisfy.

## The format

```yaml
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
```

The emitter owns the formatting, the way `gofmt` does: parse leniently, emit
canonically. Round-trip byte equality then holds by construction rather than by
luck, which is what stops a save from producing a diff.

`changing_one_curve_exponent_changes_exactly_one_line` asserts the claim the
whole format exists for. Layout lives in a sidecar and the editor can lay a
behavior out from scratch, so a team can gitignore it entirely and get
conflict-free AI authoring.

## The score breakdown

```
$ make demo

Bucket 0 (Emergency)
  TakeCover            0.930
      Health                   0.22 -> 0.220 -> 0.943
      DistanceToTarget       800.00 -> 0.267 -> 0.929
      compensation x1.062 (raw 0.876)

Bucket 1 (Combat)
  Attack               0.708
      AmmoRatio                0.80 -> 0.800 -> 0.800
      DistanceToTarget       800.00 -> 0.267 -> 0.733
      compensation x1.207 (raw 0.587)
  Reload               0.126
      AmmoRatio                0.80 -> 0.800 -> 0.360
      DistanceToTarget       800.00 -> 0.267 -> 0.267
      compensation x1.457 (raw 0.086)
```

Every level: raw input, normalized value, curve output, the product, the
compensation. Section 8.2 is right that this is the feature that justifies the
tool — "a behavior tree has structure you can point at; utility AI has numbers,
and 'why did it pick that?' is genuinely hard to answer without tooling."

## Performance

```
$ make bench

  everything scored       0.382 ms   1.91 us/agent   30,000 considerations
  + early-out             0.380 ms   1.90 us/agent   19,499 considerations
  + buckets               0.215 ms   1.07 us/agent   16,532 considerations
  + both                  0.216 ms   1.08 us/agent   12,218 considerations

Per frame at 60 fps, staggered:
    60 Hz   200.0 agents/frame   0.2160 ms  within budget
    10 Hz    33.3 agents/frame   0.0360 ms  within budget
     5 Hz    16.7 agents/frame   0.0180 ms  within budget
```

An honest reading, which the benchmark prints itself: **early-out removes 35%
of the considerations and saves no measurable time** on arithmetic this cheap —
the bound check costs about what it skips. It pays in proportion to how
expensive the considerations it skips are, which is why section 3.4's advice to
order cheap discriminating considerations first is what converts it from free
into valuable. Buckets save real time immediately.

## The Unreal plugin is not built here

`unreal/` has not been through a compiler, because building it needs an Unreal
installation. [`unreal/README.md`](unreal/README.md) says exactly what that
costs and what it does not — the short version is that the `Build.cs` files and
the reflection macros are the parts most likely to be wrong, and the scoring,
the wire format, the compiled-asset layout and the replay determinism are all
in `core/` and covered.

---

## Further reading

- [`../1—Documentation/v1/notes-on-the-spec.md`](../1—Documentation/v1/notes-on-the-spec.md) — every place this
  diverges from the design document, and why
- [`../1—Documentation/v1/utility-ai.md`](../1—Documentation/v1/utility-ai.md) — curves, compensation, and how to
  tune them
- [`../1—Documentation/v1/format.md`](../1—Documentation/v1/format.md) — the YAML schema and the compiled layout
- [`../1—Documentation/v1/protocol.md`](../1—Documentation/v1/protocol.md) — the debug wire protocol
