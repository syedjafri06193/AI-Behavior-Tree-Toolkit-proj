# Notes on the design document

Places where building this turned up something the design document did not
say, said differently, or where a choice had to be made that it left open.
Recorded here rather than silently absorbed, so the next person reading
`docs/design.md` alongside this code can see where the two diverge
and why.

---

## 1. Section 16.1's compensation test contradicts section 3.3's formula

This is the one worth leading with, because both halves are in the document and
they disagree.

Section 3.3 gives the compensation formula and works through the numbers:

> Four considerations at 0.9 gives 0.656; compensated it's 0.825. Two at 0.9
> gives 0.81; compensated 0.887. The gap narrows from 0.154 to 0.062.

Every one of those figures is correct, and `compensation_removes_the_bias_...`
pins all four.

Section 16.1 then writes the test against **eight** considerations:

```cpp
const float simple  = Compensate(std::pow(0.9f, 2), 2);
const float complex_ = Compensate(std::pow(0.9f, 8), 8);
REQUIRE(simple - complex_ < 0.15f);
```

That assertion is false for the formula the document itself specifies. Worked
out:

| n | raw | compensated | gap vs n=2 (raw) | gap vs n=2 (compensated) | reduction |
|---|---|---|---|---|---|
| 2 | 0.810 | 0.887 | — | — | — |
| 3 | 0.729 | 0.861 | 0.081 | 0.026 | 68% |
| 4 | 0.656 | 0.825 | **0.154** | **0.062** | 60% |
| 5 | 0.590 | 0.784 | 0.220 | 0.103 | 53% |
| 8 | 0.431 | 0.645 | 0.380 | **0.242** | 36% |

The 2-vs-8 compensated gap is **0.242**, not under 0.15. The 0.15 threshold
fits the 2-vs-4 case section 3.3 works through and appears to have been carried
across to the 2-vs-8 case in the test without being recomputed.

The implementation follows section 3.3, which is right, and the test asserts
what 16.1 was reaching for — that compensation substantially closes the gap —
against the number the formula actually produces.

Worth noting the shape of the residual, because it is the intent: compensation
reduces the bias, it does not eliminate it. Section 3.3 says so explicitly, and
`compensation_reduces_the_bias_without_eliminating_it` pins both directions.

## 2. Section 15.2's selection reference can lock an agent out

The reference implementation carries the incumbent's score forward:

```cpp
const bool bSwitch =
    bestIdx != S.currentAction &&
    best > S.currentScore + B.policy.switchThreshold;
...
S.currentScore = best;
```

`S.currentScore` is whatever the winner scored last time it was evaluated — and
the commitment gate returns early without updating it, so it can be several
seconds stale.

**The failure:** an action selected at 0.90 whose score then collapses to 0.10
keeps a 0.90 bar standing in front of every challenger. A challenger at 0.90
cannot clear 0.90 + 0.05, so the agent stays locked on an action scoring 0.10,
indefinitely. It surfaces as "the guard kept taking cover after the threat was
gone", which reads as a tuning problem and is not one.

`Select()` measures the incumbent's score in the same evaluation as the
challengers. Section 3.5's own wording supports this — "a challenger must beat
the incumbent by this margin" is naturally read as the incumbent *now*, not the
incumbent as of some earlier frame.
`a_collapsed_incumbent_does_not_lock_the_agent_out` is the regression.

## 3. ...and fixing that exposes an early-out interaction

Measuring the incumbent's score in the same pass introduces a second problem,
which took a cross-check between two implementations to find.

Early-out is exact for finding the *maximum*: an action it abandons provably
could not have won. But selection needs more than the maximum — it needs the
incumbent's actual score, because that is the bar. A pruned incumbent reports
zero, the bar collapses, and the agent switches on a margin it never had.

It only fires when the incumbent is losing by less than the bonus covers, which
is exactly the near-tie case the hysteresis exists to handle, so it presents as
an agent that will not stick to anything, intermittently.

**The incumbent is therefore never early-out.** The cost is one action out of
thirty, and it is the action most likely to win anyway.

Two tests cover it: `the_incumbent_bonus_does_not_get_pruned_away_by_early_out`
constructs the exact regime, and
`early_out_matches_full_scoring_with_an_incumbent_present` runs 500 frames with
state carried through. The first version of that second test used two
independent selection states, which diverge permanently after a single
disagreement and turn every later frame into noise; it now forks one shared
state per frame, which asks the sharper question.

## 4. The logit needs care beyond the clamp

Section 3.2 is right that the logit diverges at its domain boundaries and that
an unclamped NaN "silently zeroes an action forever". Clamping the argument is
necessary and not sufficient.

At the upper clamp `t` is 0.9999, and `1.0f - 0.9999f` in float32 is
catastrophic cancellation: the true 1e-4 comes back as 1.00017e-4, a relative
error of 1.7e-4 in a value the logarithm then amplifies. The C++ and the
TypeScript implementations disagreed by 8.3e-6 on exactly those points, which
the cross-check caught.

Both now carry the exact epsilon rather than recomputing `1 - t`. It costs
nothing and is correct regardless of the cross-language question — a curve the
editor draws differently from the one the runtime evaluates makes every tuning
decision made in the editor a decision about the wrong curve.

Two other clamps in the same spirit, neither mentioned in the document:

- `pow()` with a negative base and a fractional exponent is NaN. Designers
  reach this by ordinary authoring: `c > 0` with `k = 0.5` makes `(x - c)`
  negative for small x. Both implementations mirror the positive branch and
  carry the sign, which is continuous through zero where returning 0 is not.
- The logistic's exponent is clamped **before** `exp()`, not after. A large
  `|m|` overflows to infinity, and infinity/infinity is NaN — clamping the
  result would be too late.

## 5. Section 10.4 lists an inverted range as an error; it should not be

The validation table has "Consideration range is inverted or zero-width" as a
single error row. Those are two different things:

- **Zero-width** divides by zero. Genuinely an error, and it is one here.
- **Inverted** (`range: [3000, 0]`) is the natural way to author "closer is
  better" — it flips the input so a rising curve expresses a falling response,
  with no mirrored curve needed. The core supports it and the sample uses the
  idea.

Only zero-width is rejected. `accepts_an_inverted_range_which_is_how_closer_
is_better_is_written` pins it.

## 6. What the "never wins" check can and cannot see

Section 10.4's always-wins and never-wins checks are described as "cheap to
compute by sampling the input space and surprisingly effective at finding
authoring mistakes". Both are true, with one boundary worth stating.

The validator probes the space the **behavior declares**, so a consideration
whose range sits entirely outside what the game actually feeds it looks
perfectly reasonable. Detecting that needs to know what the inputs really do,
which is what a recording knows and a static file does not — so the gap is
closed by section 9.4's replay rather than by a better validator.
`cannot_see_a_range_the_game_will_never_produce_and_says_so` pins it as a known
limit rather than leaving somebody to discover it by trusting a clean run.

Building the probe also surfaced a false positive worth avoiding: sampling each
input over the range of the *first* action that mentions it means a second
action declaring a different range for the same input gets probed outside its
own range and reported as never winning. Two actions caring about different
spans of the same distance is completely normal. The probe covers the union.

## 7. Early-out prunes work but does not save time on cheap considerations

Measured, at the document's own 200 × 30 × 5 shape:

```
  everything scored       0.382 ms   30,000 considerations
  + early-out             0.380 ms   19,499 considerations
  + buckets               0.215 ms   16,532 considerations
  + both                  0.216 ms   12,218 considerations
```

Early-out removes **35%** of the considerations and saves no measurable time.
The bound check costs a compensation — a divide and two multiplies — per
consideration, which is about what the arithmetic it skips costs.

That is not an argument against early-out, it is an argument about what it is
for. Section 11.1 makes the point from the other side: "a consideration that
does a navmesh query or a line trace is microseconds, not nanoseconds." Early-
out pays in proportion to how expensive the considerations it skips are, and on
pure arithmetic it is approximately free in both directions. Section 3.4's
advice to order cheap discriminating considerations first is what converts it
from free into valuable.

Buckets, by contrast, save real time immediately, because skipping a bucket
skips the loop overhead as well as the arithmetic.

## 8. The document does not say what happens when a behavior file is reformatted

It matters, because the format has a canonical emitter and recordings carry a
content version. If re-serializing a file changed its version, the formatter
would invalidate every recording made against it and the two features would be
at war.

`ComputeVersion` hashes only what changes behavior: names, indices, curve
parameters, ranges, buckets, weights, policy. Not whitespace, not key order,
not the sign of zero. `a_formatting_only_change_does_not_count_as_a_behavior_
change` pins it.

## 9. Small things

**Considerations are not sorted.** Section 5.2 says to sort actions by bucket
then name, and says considerations stay in authored order. Worth restating why,
because it looks like an oversight: their order is semantically meaningful —
early-out reads them front to back — so sorting them would be a silent
performance regression with identical behavior and different cost.

**Inputs are not sorted either**, for a different reason: their order is the
index table, and those indices appear in every recording. Re-sorting for
cosmetic reasons would invalidate recorded sessions.

**The delta mask is 32 bits.** Section 15.3's reference uses `uint32
ChangedMask` with no bound check, so a behavior with more than 32 actions would
silently never update action 33's score — an action whose number is frozen,
which nobody would attribute to the wire format. The encoder here refuses
rather than truncating.

**The task registry is per-project.** Section 5.4 wants a dangling task
reference to fail the build, which needs a list of what is registered. That
list comes from the Unreal side in a real project; the default here is what the
samples use.
