# The debug wire protocol

The transport has to work from a **cooked build and a dedicated server**, over
a network, with the editor as a separate process. First-party debugging works
in the editor; the bugs that matter — dedicated server AI, cooked-build timing,
anything that only appears with 200 agents — frequently do not reproduce there.

```
┌──────────────┐       TCP, length-prefixed      ┌──────────────────┐
│ Editor       │◀───────────────────────────────▶│ Game process     │
│ (TypeScript) │   subscribe / deltas / commands │ (PIE, cooked,    │
└──────────────┘                                 │  or server)      │
                                                 └──────────────────┘
```

Compiled out of Shipping by `WITH_AIBEHAVIOR_DEBUG`. Present in Development
**and Test**, because a cooked Test build is the thing you are trying to debug.
Binds to loopback; `-aibdebugremote` opts out.

---

## The arithmetic that shapes everything

```
200 agents × 30 actions × 5 considerations × 10 Hz = 300,000 values/second
```

At 8 bytes each that is 2.4 MB/s before framing, and it is useless data,
because nobody is looking at 200 agents.

### Subscription

```ts
interface Subscription {
  agentIds: number[];   // usually exactly one
  detail: DetailLevel;
  maxHz: number;
  deltasOnly: boolean;
}
```

| Level | Content | Bandwidth |
|---|---|---|
| `Summary` | current action per agent, on change only | tiny — fine for all agents |
| `Scores` | all action scores for subscribed agents | moderate |
| `FullBreakdown` | per-consideration values and curve outputs | large — one agent at a time |

Subscribing to one agent at `FullBreakdown` is under 20 KB/s, three orders of
magnitude below streaming everything.

### Deltas

An agent whose state has not changed sends nothing. Scores within **0.001** of
their last value count as unchanged, so numerical jitter does not generate
traffic on an agent whose situation is stable.

**A score not on the wire is UNCHANGED, not zero.** Getting this backwards
produces a breakdown view where every stable action reads 0.000, which looks
like a scoring bug and is not one.

The changed mask is a `u32`, so a behavior with more than 32 actions is refused
rather than silently dropping action 33's score — an action whose number is
frozen is not a bug anyone would attribute to the wire format.

---

## Framing

```
u32  payload length (including the type byte)
u8   message type
...  payload
```

TCP does not preserve message boundaries. A client that assumes one read equals
one message works on localhost and fails the moment it is pointed at a real
dedicated server — which is what this exists for. `frameStream` returns
complete frames plus whatever trailing bytes to prepend to the next read; the
test feeds it a stream one byte at a time.

## Messages

| | Direction | |
|---|---|---|
| `Subscribe` / `Unsubscribe` | editor → game | |
| `Command` | editor → game | see below |
| `RequestHistory` | editor → game | scrub backward |
| `Hello` | game → editor | behavior name, content version, agent count, **whether this is a cooked build** |
| `Frame` | game → editor | delta-encoded scores |
| `Breakdown` | game → editor | per-consideration detail |
| `History` | game → editor | a range from the ring buffer |
| `AbortEvent` | game → editor | which BT decorator fired, and why |

`Hello` reports whether the attached process is a cooked build because editor
performance is not shipping performance, and it changes what the numbers mean.

## Commands

| | |
|---|---|
| `Pause` / `Resume` / `Step` | pause **the AI**, not the game |
| `ForceAction` | test a branch without engineering the situation |
| `OverrideInput` | "what would it do if health were 0.1?" |
| `ClearOverrides` | |
| `HotReload` | recompile the YAML and swap it in without restarting |

Hot reload is the one that changes how the work feels. Tuning a curve and
seeing the agent's behavior change within a second, without leaving the running
game, is the difference between iteration and a build cycle.

Overrides are applied **after** input gathering, so the answer survives the
next real read.

---

## The ring buffer

Fixed capacity, in the game process: 600 frames is 60 seconds at 10 Hz.

Fixed is the point. A debug buffer that grows will eventually be the reason a
long play session runs out of memory — and an overnight soak is exactly when
the interesting AI bugs appear and exactly when an unbounded buffer kills the
process before anyone reads it.

The editor mirror is bounded in both dimensions: frames per agent, and agents
tracked. A session that spawns and kills agents for hours would otherwise
accumulate one buffer per agent that ever existed.

---

## Recordings

A separate, persistent format — the ring buffer is for scrubbing, this is for
keeping.

```
u32  magic 'AREC'
u32  version
str  behavior name
u32  behavior content version
u64  rng seed
u32  frame count
  per frame:
    f64  time
    u32  agent id
    f32  dt          ← the evaluation delta, not a wall clock
    u32 + f32[]  inputs
    u32  selected action
    f32  selected score
    u8   switched
```

Only what the AI saw and decided — not the game state. Measured at **under 120
bytes per frame** for a five-input behavior, which is a few megabytes for an
hour of continuous recording.

`dt` is recorded rather than derived so replay does not depend on the frame
rate it is replayed at, which is one of the three conditions for determinism
along with a recorded RNG seed and no wall-clock dependency. The scoring core
has no side effects and no engine dependency, so all three are achievable.
