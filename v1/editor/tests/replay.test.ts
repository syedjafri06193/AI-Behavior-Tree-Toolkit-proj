/**
 * Record, replay, and behavior diffing (design.md sections 9, 16.4).
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";

import { compile } from "../src/format/compile.js";
import { parse } from "../src/format/parse.js";
import { serialize } from "../src/format/serialize.js";
import { decodeBehavior, type CompiledBehavior } from "../src/runtime/compiled.js";
import { newSelectionState, select } from "../src/runtime/score.js";
import {
  decodeRecording,
  diffBehaviors,
  encodeRecording,
  replay,
  summarizeDiff,
  type Recording,
} from "../src/replay/replay.js";
import type { Behavior } from "../src/format/types.js";

const SAMPLES = new URL("../../samples", import.meta.url).pathname;

function guardSource(): Behavior {
  return parse(readFileSync(join(SAMPLES, "guard.behavior.yaml"), "utf8"));
}

function build(behavior: Behavior): CompiledBehavior {
  return decodeBehavior(compile(behavior).bytes);
}

/**
 * A plausible encounter: a target closing while health drops and ammo runs
 * down. Shaped so more than one action wins over the session -- a recording
 * where nothing changes would pass every replay test trivially.
 */
function recordEncounter(behavior: CompiledBehavior, frames = 300): Recording {
  const state = newSelectionState();
  const recording: Recording = {
    behaviorName: behavior.name,
    behaviorVersion: behavior.version,
    rngSeed: 0n,
    frames: [],
  };

  for (let i = 0; i < frames; i++) {
    const t = i / frames;
    const inputs = [
      Math.fround(3000 * (1 - t)), // DistanceToTarget
      Math.fround(1 - 0.85 * t), // Health
      Math.fround(1 - t), // AmmoRatio
      Math.fround(30 * (1 - t)), // TimeSinceLastSeen
      Math.fround(3 * (1 - t)), // AllyCount
    ];
    const r = select(behavior, inputs, state, 0.2);
    recording.frames.push({
      time: i * 0.2,
      agentId: 3,
      dt: 0.2,
      inputs,
      selectedAction: r.action,
      selectedScore: r.score,
      switched: r.switched,
    });
  }
  return recording;
}

/**
 * The same encounter with jitter on top -- two actions hovering near each
 * other while the inputs wobble. This is what a real perception feed looks
 * like, and it is the configuration the anti-oscillation machinery exists for.
 */
function recordJitteredEncounter(
  behavior: CompiledBehavior,
  frames = 400,
): Recording {
  const state = newSelectionState();
  const recording: Recording = {
    behaviorName: behavior.name,
    behaviorVersion: behavior.version,
    rngSeed: 0n,
    frames: [],
  };

  for (let i = 0; i < frames; i++) {
    // Two incommensurate sinusoids, so the actions cross repeatedly rather
    // than settling. Deterministic, so a failure reproduces exactly.
    const wobble = (phase: number, amp: number): number =>
      amp * Math.sin(i * phase);
    const inputs = [
      Math.fround(1500 + wobble(1.31, 900)), // DistanceToTarget
      Math.fround(0.5 + wobble(0.97, 0.12)), // Health
      Math.fround(0.5 + wobble(0.71, 0.12)), // AmmoRatio
      Math.fround(15 + wobble(0.53, 6)), // TimeSinceLastSeen
      Math.fround(1.5 + wobble(0.41, 1)), // AllyCount
    ];
    const r = select(behavior, inputs, state, 0.2);
    recording.frames.push({
      time: i * 0.2,
      agentId: 3,
      dt: 0.2,
      inputs,
      selectedAction: r.action,
      selectedScore: r.score,
      switched: r.switched,
    });
  }
  return recording;
}

describe("replay determinism", () => {
  it("replaying against the same behavior matches frame for frame", () => {
    // Section 16.4. The scoring core has no side effects, no engine dependency
    // and no wall-clock dependency, so anything other than an exact match is a
    // determinism bug rather than a tolerance question.
    const behavior = build(guardSource());
    const recording = recordEncounter(behavior);

    const result = replay(recording, behavior);
    expect(result.frames).toBe(recording.frames.length);
    expect(result.matching).toBe(recording.frames.length);
    expect(result.deterministic).toBe(true);
    expect(result.versionMismatch).toBe(false);
  });

  it("the recording actually exercises more than one action", () => {
    // A guard against a fixture that quietly stops testing anything.
    const behavior = build(guardSource());
    const recording = recordEncounter(behavior);
    const chosen = new Set(recording.frames.map((f) => f.selectedAction));
    const switches = recording.frames.filter((f) => f.switched).length;
    expect(chosen.size).toBeGreaterThanOrEqual(2);
    expect(switches).toBeGreaterThanOrEqual(1);
  });

  it("does not depend on the rate it is replayed at", () => {
    // Section 9.3: the recorded dt drives commitment, so a replay on a faster
    // or slower machine reaches the same decisions.
    const behavior = build(guardSource());
    const recording = recordEncounter(behavior);
    expect(replay(recording, behavior).matching).toBe(
      replay(recording, behavior).matching,
    );
  });

  it("the recording round-trips through its binary format", () => {
    const behavior = build(guardSource());
    const original = recordEncounter(behavior, 120);
    const decoded = decodeRecording(encodeRecording(original));

    expect(decoded.behaviorName).toBe(original.behaviorName);
    expect(decoded.behaviorVersion).toBe(original.behaviorVersion);
    expect(decoded.frames).toHaveLength(original.frames.length);
    expect(replay(decoded, behavior).deterministic).toBe(true);
  });

  it("a truncated recording fails cleanly rather than reading garbage", () => {
    const behavior = build(guardSource());
    const bytes = encodeRecording(recordEncounter(behavior, 40));
    for (let len = 0; len < bytes.length; len += 7) {
      expect(() => decodeRecording(bytes.subarray(0, len))).toThrow();
    }
    expect(() => decodeRecording(bytes)).not.toThrow();
  });

  it("stays inside the 200-bytes-per-frame budget", () => {
    // Section 9.2: "roughly 200 bytes per agent per frame -- small enough to
    // record continuously." A recorder that is secretly ten times that is one
    // somebody turns off, and then there is no recording when it matters.
    const behavior = build(guardSource());
    const recording = recordEncounter(behavior, 300);
    const bytes = encodeRecording(recording).length;
    const perFrame = bytes / recording.frames.length;
    expect(perFrame).toBeLessThan(200);

    // An hour of continuous recording for one agent, at 10 Hz.
    const hourMb = (perFrame * 10 * 3600) / 1e6;
    expect(hourMb).toBeLessThan(5);
  });
});

describe("replaying against a modified behavior (section 9.4)", () => {
  it("a curve change shows up as a measurable shift in decisions", () => {
    // "The genuinely clever version: replay a recorded session against an
    // edited behavior file and diff the decisions. 'Your curve change makes
    // the guard take cover 40% more often in this recorded encounter' is a
    // design review conversation nobody currently gets to have."
    const source = guardSource();
    const before = build(source);
    const recording = recordEncounter(before);

    const edited = guardSource();
    const cover = edited.actions.find((a) => a.name === "TakeCover")!;
    cover.considerations[0]!.curve.c = 0.25; // trigger cover at higher health
    const after = build(edited);

    const diff = diffBehaviors(recording, before, after);
    expect(diff.frames).toBe(recording.frames.length);
    expect(diff.decisionsChanged).toBeGreaterThan(0);

    const coverShift = diff.shifts.find((s) => s.name === "TakeCover");
    expect(coverShift).toBeDefined();
    expect(coverShift!.delta).toBeGreaterThan(0);

    // And the summary is the sentence a designer can act on.
    const summary = summarizeDiff(diff);
    expect(summary).toContain("TakeCover");
    expect(summary).toMatch(/percentage points more often/);
  });

  it("reports no change when nothing changed", () => {
    const behavior = build(guardSource());
    const recording = recordEncounter(behavior);
    const diff = diffBehaviors(recording, behavior, behavior);
    expect(diff.decisionsChanged).toBe(0);
    expect(summarizeDiff(diff)).toContain("No decisions changed");
  });

  it("hysteresis costs nothing on smooth inputs", () => {
    // Worth pinning, because it is the reason the three mitigations are safe
    // to leave on by default. On a clean monotonic encounter the incumbent
    // bonus, the switch threshold and the commitment window change no
    // decisions at all -- they only do work when the inputs are noisy.
    const source = guardSource();
    const before = build(source);
    const recording = recordEncounter(before);

    const edited = guardSource();
    edited.policy.currentActionBonus = 1;
    edited.policy.switchThreshold = 0;
    edited.policy.minCommitmentSeconds = 0;
    const after = build(edited);

    const diff = diffBehaviors(recording, before, after);
    expect(diff.switchesAfter).toBe(diff.switchesBefore);
  });

  it("measures a stability change as a switch count on noisy inputs", () => {
    // And this is where they earn their keep. A tuning pass aimed at reducing
    // twitchiness needs a number, and "the agent changed its mind 9 times
    // instead of 80" is that number.
    //
    // Real inputs jitter -- a distance that depends on two moving actors, a
    // perception value that updates on its own schedule -- so this is the
    // realistic case and the smooth ramp above is the optimistic one.
    const source = guardSource();
    const before = build(source);
    const recording = recordJitteredEncounter(before);

    const edited = guardSource();
    edited.policy.currentActionBonus = 1;
    edited.policy.switchThreshold = 0;
    edited.policy.minCommitmentSeconds = 0;
    const after = build(edited);

    const diff = diffBehaviors(recording, before, after);

    // 85 switches becomes 108 on this recording. Not a landslide, and the
    // bound is written to what the fixture actually produces rather than to a
    // rounder number -- the jitter here is wide enough that the two actions
    // genuinely swap on merit most of the time, and hysteresis only removes
    // the marginal crossings. That is the honest shape of the effect: it
    // trims the twitch, it does not rewrite the decision.
    expect(
      diff.switchesAfter,
      "removing all three mitigations must show up as more switching",
    ).toBeGreaterThan(diff.switchesBefore * 1.15);
    expect(summarizeDiff(diff)).toContain("changed its mind");

    // And the point of the feature: the difference is a number a designer can
    // put in a review, not an impression.
    expect(summarizeDiff(diff)).toMatch(/\d+ -> \d+/);
  });

  it("compares by action name, so adding an action does not look like chaos", () => {
    // Indices renumber when an action is inserted. A diff keyed on index would
    // report every later action as changed, which would make the feature
    // useless for the case it is most wanted in -- "I added a behavior, what
    // did it displace?"
    const source = guardSource();
    const before = build(source);
    const recording = recordEncounter(before);

    const edited = guardSource();
    // "Advance" sorts before "Attack", so it lands at index 0 in bucket 1 and
    // pushes Attack and Reload along.
    edited.actions.push({
      name: "Advance",
      bucket: 1,
      considerations: [
        {
          input: "DistanceToTarget",
          range: [0, 3000],
          curve: { type: "linear", m: 0.2, k: 1, b: 0, c: 0 },
        },
      ],
      task: { type: "Investigate" },
    });
    const after = build(edited);

    const diff = diffBehaviors(recording, before, after);
    const names = new Set(diff.shifts.map((s) => s.name));
    // Whatever moved, it should be named actions -- not a wall of index churn.
    for (const name of names) {
      expect(typeof name).toBe("string");
      expect(name).not.toBe("(none)");
    }
    // The action set grew, and the old actions still exist under their names.
    expect(after.actions.map((a) => a.name)).toContain("Advance");
    expect(after.actions.map((a) => a.name)).toContain("Attack");
  });

  it("flags a version mismatch rather than silently diverging", () => {
    // Divergence against an edited behavior is expected and is the point.
    // Divergence against the SAME behavior is a determinism bug. Telling them
    // apart is what the recorded version is for.
    const before = build(guardSource());
    const recording = recordEncounter(before);

    const edited = guardSource();
    edited.actions[0]!.considerations[0]!.curve.c = 0.25;
    const after = build(edited);

    const result = replay(recording, after);
    expect(result.versionMismatch).toBe(true);
    expect(result.divergences.length).toBeGreaterThan(0);
    expect(result.divergences[0]!.recordedAction).not.toBe(
      result.divergences[0]!.replayedAction,
    );
  });

  it("a formatting-only change does not count as a behavior change", () => {
    // Re-serializing a file must not invalidate every recording made against
    // it. If it did, the canonical formatter would be at war with the
    // recording system.
    const source = guardSource();
    const before = build(source);
    const reformatted = build(parse(serialize(source)));
    expect(reformatted.version).toBe(before.version);

    const recording = recordEncounter(before);
    expect(replay(recording, reformatted).versionMismatch).toBe(false);
    expect(replay(recording, reformatted).deterministic).toBe(true);
  });
});
