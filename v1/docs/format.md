# The authoring format

Logic in `*.behavior.yaml`, layout in a sidecar. `.uasset` is binary: it does
not diff, does not merge, and cannot be code-reviewed, and the workarounds —
asset locking, "only one person touches AI" — are workflow damage.

---

## A complete file

```yaml
version: 1
type: utility
name: GuardBehavior

inputs:
  - { name: DistanceToTarget, source: blackboard, key: TargetActor, transform: distance }
  - { name: Health, source: attribute, key: HealthPercent }

actions:
  - name: TakeCover
    bucket: 0
    considerations:
      - input: Health
        range: [0, 1]
        curve: { type: logistic, m: -1.0, k: 1.0, b: 0.0, c: 0.0 }
    task: { type: MoveToCover, params: { searchRadius: 1500 } }

policy:
  evaluationHz: 5
  currentActionBonus: 1.1
  minCommitmentSeconds: 0.5
  switchThreshold: 0.05
  bucketSatisfiedThreshold: 0.3
```

## Fields

### `inputs[]`

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | referenced by considerations |
| `source` | yes | `blackboard`, `attribute`, `constant`, `custom` |
| `key` | yes | the blackboard key or attribute name |
| `transform` | no | `distance`, `elapsed`, `pathlength`, ... |

**Order is significant.** Inputs become indices in the compiled asset, and
those indices appear in every recording. The formatter never re-sorts them,
because doing so would invalidate recorded sessions for a cosmetic reason.

### `actions[]`

| Field | Default | Meaning |
|---|---|---|
| `name` | required | unique |
| `bucket` | required | 0 is highest priority |
| `weight` | 1.0 | a thumb on the scale before considerations |
| `interruptible` | true | false holds through a force-reevaluate |
| `considerations` | required | **order is significant** — see below |
| `task` | required | must be a registered task type |

**Consideration order is significant** and is never sorted: early-out reads
them front to back, so re-ordering them is a silent performance change with
identical behavior. Put the cheap, discriminating ones first.

### `considerations[]`

| Field | Meaning |
|---|---|
| `input` | must name a declared input |
| `range` | `[min, max]`. **Inverted is legal** and means "less is better". Zero-width is an error. |
| `curve` | `{ type, m, k, b, c }` |

---

## Canonical formatting

The emitter owns the layout, the way `gofmt` does: parse leniently, emit
canonically. `aibc fmt` rewrites a file into canonical form, and round-trip
byte equality then holds by construction.

The rules:

- **Actions sorted by bucket, then name.** Considerations and inputs never
  sorted.
- **Object keys in a fixed order**, and curve keys follow the formula
  (`type, m, k, b, c`) rather than the alphabet.
- **Defaults omitted.** A file mentions only what was decided, so a two-line
  change does not read as a rewrite.
- **Curve parameters written as floats** — `m: -1.0`, not `m: -1`. Purely about
  how the diff reads, which is the point of the format.

The test that matters is `changing_one_curve_exponent_changes_exactly_one_line`.

## The layout sidecar

```
ai/guard.behavior.yaml     logic. Reviewed, merged, meaningful.
ai/guard.layout.json       positions and colors. Usually gitignored.
```

Node positions in the logic file would mean **moving a node produces a diff**,
and the signal-to-noise ratio of review collapses. The editor can lay a
behavior out from scratch deterministically, so a team that gitignores layout
entirely gets conflict-free AI authoring — an unusual property, and the reason
auto-layout is a requirement rather than a convenience.

---

## Compiling

```
aibc compile ai/guard.behavior.yaml -o ai/guard.aib
```

YAML is for humans; the runtime loads the binary. Flat arrays, resolved
indices, no string lookups, no parsing at load. Run it in the cook step so
shipped builds never see YAML.

**The compiler is where validation happens**, and errors exit non-zero — a
dangling task reference should fail the build, not fail in front of a player.

### The binary layout

Little-endian, fixed. Read by `aib::Deserialize` in C++ and `decodeBehavior` in
TypeScript; the cross-check asserts the two agree.

```
u32   magic 'AAIB'
u32   format version
u32   content version (FNV-1a over everything that changes behavior)
str   name
u16 + str[]   input names
u16 + str[]   task names
f32 x5        policy
u16           action count
  per action:
    str  name
    u8   bucket
    u8   interruptible
    f32  weight
    u16  task index
    u16  consideration count
      per consideration:
        u16  input index
        f32  input min, input max
        u8   curve type
        f32  m, k, b, c
```

The bucket index is **derived on load**, not stored: recomputing it costs
microseconds and removes a way for the file to contradict itself.

Every read is bounds-checked, an unknown curve type is rejected rather than
defaulted, and an out-of-range index fails the load. A silently misinterpreted
float array produces an agent that behaves *almost* correctly, which is far
worse than one that fails to load.

### The content version

Hashes exactly what changes behavior: names, indices, curve parameters, ranges,
buckets, weights, policy. Not whitespace, not key order, not the sign of zero.

Recordings carry it, so replay can tell "the behavior was edited" from "the
core is non-deterministic". Reformatting a file does **not** change it — if it
did, the canonical formatter would invalidate every recording made against the
file and the two features would be at war.

---

## Validation

| Check | Severity |
|---|---|
| Unknown input name | error |
| Task type not registered | error |
| Curve produces NaN or constant output | error |
| Consideration range is zero-width | error |
| Action can never score above zero | warning |
| Action always wins its bucket | warning |
| Bucket empty | warning |
| Action has no considerations | warning |
| Expensive consideration ordered first | hint |

The always-wins and never-wins checks sample the input space on a deterministic
low-discrepancy lattice — deterministic so a warning appears on every machine
and in CI, because a validator that flags things intermittently is one people
learn to ignore.

**What it cannot see:** the probe covers the space the *behavior* declares, so
a range that sits entirely outside what the game actually produces looks
reasonable. Closing that gap needs a recording, which is what replay is for.
