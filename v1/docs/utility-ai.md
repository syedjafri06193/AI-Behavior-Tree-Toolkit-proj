# Utility AI: curves, compensation, and tuning

The reference model is Dave Mark's Infinite Axis Utility System. Each candidate
**action** has N **considerations**; each takes a raw input, normalizes it,
passes it through a **response curve**, and produces a value in [0,1]. The
action's score is the product, compensated.

```
consideration:  raw input -> normalize -> response curve -> [0,1]
action score:   product of considerations, then compensated
selection:      highest score in the highest satisfied bucket
```

---

## The four curves

All four take the same four knobs, which is what makes the curve editor a
single surface rather than four.

| Type | Shape | Reach for it when |
|---|---|---|
| `linear` | straight | the relationship really is proportional |
| `polynomial` | `y = m(x-c)^k + b` | falloff that accelerates (`k=2`) or flattens (`k=0.5`) |
| `logistic` | S: slow, fast, slow | **the default for "more is better"** — a soft threshold with soft ends |
| `logit` | inverse-S: fast, slow, fast | a hard threshold, where the middle barely matters |

```
m  slope        negative inverts the curve
k  exponent     polynomial sharpness, or logistic amplitude
b  vertical     shifts the whole output up or down
c  horizontal   moves where the interesting part happens
```

### The clamp is load-bearing

Every curve output is clamped to [0,1], and every path is checked to be finite.
This is not defensive programming for its own sake:

- **The logit diverges** at both ends of its domain.
- **`pow()` with a negative base and a fractional exponent is NaN**, which you
  reach with `c > 0` and `k = 0.5`.
- **The logistic overflows** `exp()` with a large `|m|`, and infinity/infinity
  is NaN.

An unclamped NaN propagates through the score multiply and produces an action
that **never wins, forever, with no error anywhere**. It presents as "this
behavior never fires", and it costs a day.

`curves_never_produce_nan` walks 56,700 points of the parameter grid.

### Worked examples

**"Low health means take cover."** A falling logistic:
`{ type: logistic, m: -1.0, k: 1.0, b: 0.0, c: 0.0 }` — near 1 at low health,
near 0 at full, with a soft transition in the middle rather than a cliff.

**"Closer is more urgent, sharply."** A quadratic falloff:
`{ type: polynomial, m: -1.0, k: 2.0, b: 1.0, c: 0.0 }` over `range: [0, 3000]`
— 1.0 at zero distance, 0.75 at half range, 0 at the far end. The quadratic
holds its value near zero longer than a linear would, which is usually what
"urgent when close" actually means.

**"Closer is better" without a mirrored curve.** Invert the range:
`range: [3000, 0]` with a rising curve. The normalization flips the input.

---

## Compensation, and why you cannot skip it

Scores multiply and every consideration is ≤ 1, so **more considerations mean a
lower score regardless of how good the action is**:

| considerations at 0.9 | product |
|---|---|
| 2 | 0.810 |
| 4 | 0.656 |
| 8 | 0.431 |

A carefully-authored eight-consideration action loses to a crude two-
consideration one **by construction**. Designers notice, and respond by
deleting considerations, which is exactly backwards.

```
mod    = 1 - 1/n
makeup = (1 - score) * mod
final  = score + makeup * score
```

| n | raw | compensated |
|---|---|---|
| 2 | 0.810 | 0.887 |
| 4 | 0.656 | 0.825 |
| 8 | 0.431 | 0.645 |

The 2-vs-8 gap falls from 0.380 to 0.242 — reduced by about a third, not
eliminated. That is intended: an action with more requirements *should* be
slightly harder to satisfy, it just should not be impossible.

> Note: the design document's section 16.1 asserts this gap is under 0.15,
> which is not true of the formula it specifies. See
> [`notes-on-the-spec.md`](notes-on-the-spec.md).

---

## Stopping the agent twitching

Two actions at 0.71 and 0.70 will swap every evaluation as inputs jitter. Three
mitigations, and you want all three:

| | Default | What it does |
|---|---|---|
| `currentActionBonus` | 1.1 | makes the incumbent harder to beat |
| `switchThreshold` | 0.05 | a challenger must clear the incumbent by a margin |
| `minCommitmentSeconds` | 0.5 | no switching at all below this |

Plus the one that is free:

| | Default | |
|---|---|---|
| `evaluationHz` | 5 | **the largest single lever** |

Re-evaluating at 5–10 Hz rather than 60 is an order of magnitude cheaper, is
indistinguishable to the player, and reduces flicker for nothing.

`all_three_mitigations_are_load_bearing` turns each off and measures: 600
frames of jittered near-ties produce **~300 switches** uncontrolled and **under
10** with all three.

One property worth knowing: **on smooth inputs the three mitigations change
nothing at all.** They only do work when the inputs are noisy, which is why
leaving them on by default is safe.

### Interrupting a commitment

`minCommitmentSeconds` is a floor, not a lock. `ForceReevaluate()` breaks it —
"combat begins" should interrupt a committed idle immediately. An action marked
`interruptible: false` holds anyway, which is the difference between "drop what
you are doing" and "you are mid-reload, finish it".

---

## Buckets

Pure scoring makes everything compete with everything, so a lucky `idle` can
beat a well-tuned `flee`. Buckets impose priority tiers:

```
Bucket 0  Emergency    flee, revive, take cover under fire
Bucket 1  Combat       attack, reload, reposition
Bucket 2  Investigate  check noise, search last known position
Bucket 3  Ambient      idle, patrol, chat, use object
```

A winner above `bucketSatisfiedThreshold` (default 0.3) means **lower buckets
are never scored at all** — an emergency that fires makes the entire ambient
tier free. That is the largest measured saving in the benchmark.

---

## Ordering considerations

Early-out abandons an action as soon as it cannot beat the best so far, and it
reads considerations **front to back**. So put the cheap, discriminating ones
first:

```yaml
considerations:
  - input: TargetIsAlive      # one branch, eliminates the action outright
  - input: AmmoRatio          # a float read
  - input: PathLengthToTarget # a navmesh query -- microseconds
```

Reversed, every evaluation pays for the navmesh query before discovering the
target is dead. The validator hints when an expensive input is ordered first.

**How much this is worth depends entirely on what the considerations cost.**
On pure arithmetic, early-out prunes 35% of the work and saves no measurable
time — the bound check costs about what it skips. On a consideration doing a
line trace, skipping one is worth thousands of bound checks.

---

## A tuning workflow that is not guesswork

1. **Record a session** of the situation you care about.
2. **Change the curve.**
3. **Replay the recording against both versions** and diff the decisions:

```
$ aibc diff before.behavior.yaml after.behavior.yaml encounter.arec

47 of 300 decisions changed (15.7%).
  TakeCover: 12.3 percentage points more often
  Attack: 9.1 percentage points less often
  the agent changed its mind 4 times less (18 -> 14)
```

That is a design review conversation nobody currently gets to have, and it is a
small amount of code on top of the recording.

---

## Symptoms and causes

| Symptom | Cause |
|---|---|
| Actions with more considerations never win | Compensation missing |
| Agent flips between two actions constantly | No hysteresis, or `evaluationHz` too high |
| Scoring tanks the frame rate | No buckets, expensive considerations ordered first |
| One action always wins, others are dead | Check the `always-wins` warning |
| An action never fires and nothing is logged | A NaN, or a range the input never reaches |
| "Why did it pick that?" | `aibc explain`, or the breakdown view |
