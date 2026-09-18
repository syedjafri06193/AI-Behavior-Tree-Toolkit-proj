/**
 * Record and replay (design.md section 9).
 *
 * "Recording a session and replaying it in the editor is probably the
 * highest-value debugging feature available, and almost nothing in this space
 * has it."
 *
 * The recording format mirrors aib::Recording in core/include/aib/record.h --
 * the game writes it, the editor reads it, so the two have to agree byte for
 * byte. You do not need the game state, only what the AI saw and decided:
 * about 200 bytes per agent per frame, small enough to record continuously.
 *
 * Section 9.4 is what makes this more than a reproduction aid: replay a
 * recorded session against an EDITED behavior and diff the decisions. "Your
 * curve change makes the guard take cover 40% more often in this recorded
 * encounter" is a design review conversation nobody currently gets to have,
 * and it turns tuning from guesswork into measurement.
 */

import type { CompiledBehavior } from "../runtime/compiled.js";
import { newSelectionState, select } from "../runtime/score.js";

const REC_MAGIC = 0x43455241; // 'AREC'
const REC_VERSION = 1;

export interface RecordedFrame {
  time: number;
  agentId: number;
  /** The evaluation delta, so replay does not depend on the rate it runs at. */
  dt: number;
  inputs: number[];
  selectedAction: number;
  selectedScore: number;
  switched: boolean;
}

export interface Recording {
  behaviorName: string;
  behaviorVersion: number;
  rngSeed: bigint;
  frames: RecordedFrame[];
}

// --------------------------------------------------------------- encoding

export function decodeRecording(bytes: Uint8Array): Recording {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let pos = 0;
  const need = (n: number): void => {
    if (pos + n > bytes.length) throw new Error("recording ends mid-record");
  };
  const u32 = (): number => {
    need(4);
    const v = view.getUint32(pos, true);
    pos += 4;
    return v;
  };
  const u64 = (): bigint => {
    need(8);
    const v = view.getBigUint64(pos, true);
    pos += 8;
    return v;
  };
  const f32 = (): number => {
    need(4);
    const v = view.getFloat32(pos, true);
    pos += 4;
    return v;
  };
  const f64 = (): number => {
    need(8);
    const v = view.getFloat64(pos, true);
    pos += 8;
    return v;
  };
  const str = (): string => {
    const n = u32();
    need(n);
    const s = new TextDecoder().decode(bytes.subarray(pos, pos + n));
    pos += n;
    return s;
  };

  if (u32() !== REC_MAGIC) throw new Error("not an .arec recording");
  if (u32() !== REC_VERSION) throw new Error("unsupported recording version");

  const behaviorName = str();
  const behaviorVersion = u32();
  const rngSeed = u64();
  const frameCount = u32();

  const frames: RecordedFrame[] = [];
  for (let i = 0; i < frameCount; i++) {
    const time = f64();
    const agentId = u32();
    const dt = f32();
    const inputCount = u32();
    const inputs: number[] = [];
    for (let j = 0; j < inputCount; j++) inputs.push(f32());
    const selectedAction = u32() | 0; // written as u32, read back signed
    const selectedScore = f32();
    need(1);
    const switched = bytes[pos++] !== 0;
    frames.push({ time, agentId, dt, inputs, selectedAction, selectedScore, switched });
  }
  return { behaviorName, behaviorVersion, rngSeed, frames };
}

export function encodeRecording(recording: Recording): Uint8Array {
  const out: number[] = [];
  const scratch = new DataView(new ArrayBuffer(8));
  const push = (n: number): void => {
    for (let i = 0; i < n; i++) out.push(scratch.getUint8(i));
  };
  const u32 = (v: number): void => {
    scratch.setUint32(0, v >>> 0, true);
    push(4);
  };
  const u64 = (v: bigint): void => {
    scratch.setBigUint64(0, v, true);
    push(8);
  };
  const f32 = (v: number): void => {
    scratch.setFloat32(0, v, true);
    push(4);
  };
  const f64 = (v: number): void => {
    scratch.setFloat64(0, v, true);
    push(8);
  };
  const str = (s: string): void => {
    const enc = new TextEncoder().encode(s);
    u32(enc.length);
    for (const b of enc) out.push(b);
  };

  u32(REC_MAGIC);
  u32(REC_VERSION);
  str(recording.behaviorName);
  u32(recording.behaviorVersion);
  u64(recording.rngSeed);
  u32(recording.frames.length);

  for (const f of recording.frames) {
    f64(f.time);
    u32(f.agentId);
    f32(f.dt);
    u32(f.inputs.length);
    for (const v of f.inputs) f32(v);
    u32(f.selectedAction);
    f32(f.selectedScore);
    out.push(f.switched ? 1 : 0);
  }
  return Uint8Array.from(out);
}

// ----------------------------------------------------------------- replay

export interface Divergence {
  frameIndex: number;
  time: number;
  recordedAction: number;
  replayedAction: number;
  recordedScore: number;
  replayedScore: number;
}

export interface ReplayResult {
  frames: number;
  matching: number;
  divergences: Divergence[];
  /**
   * True when the recording was made against different content. Divergence is
   * expected then and is the whole point of section 9.4; divergence WITHOUT it
   * is a determinism bug, and telling the two apart is what the version is
   * for.
   */
  versionMismatch: boolean;
  deterministic: boolean;
}

function runThrough(
  recording: Recording,
  behavior: CompiledBehavior,
): { decisions: number[]; scores: number[]; switches: number } {
  const state = newSelectionState();
  const decisions: number[] = [];
  const scores: number[] = [];
  let switches = 0;

  for (const frame of recording.frames) {
    // frame.dt, never a wall clock. Section 9.3: no dependence on wall-clock
    // time and no dependence on frame rate, or the replay is not a replay.
    const r = select(behavior, frame.inputs, state, frame.dt);
    decisions.push(r.action);
    scores.push(r.score);
    if (r.switched) switches++;
  }
  return { decisions, scores, switches };
}

export function replay(
  recording: Recording,
  behavior: CompiledBehavior,
): ReplayResult {
  const { decisions, scores } = runThrough(recording, behavior);
  const divergences: Divergence[] = [];
  let matching = 0;

  recording.frames.forEach((f, i) => {
    if (decisions[i] === f.selectedAction) {
      matching++;
      return;
    }
    divergences.push({
      frameIndex: i,
      time: f.time,
      recordedAction: f.selectedAction,
      replayedAction: decisions[i]!,
      recordedScore: f.selectedScore,
      replayedScore: scores[i]!,
    });
  });

  return {
    frames: recording.frames.length,
    matching,
    divergences,
    versionMismatch:
      recording.behaviorVersion !== 0 &&
      behavior.version !== 0 &&
      recording.behaviorVersion !== behavior.version,
    deterministic: divergences.length === 0,
  };
}

// ------------------------------------------------------- comparing versions

export interface ActionShare {
  action: number;
  name: string;
  frames: number;
  fraction: number;
}

export interface BehaviorDiff {
  frames: number;
  decisionsChanged: number;
  changedFraction: number;
  switchesBefore: number;
  switchesAfter: number;
  before: ActionShare[];
  after: ActionShare[];
  /** Per-action change in share, sorted by magnitude -- the headline number. */
  shifts: { action: number; name: string; delta: number }[];
}

function shares(decisions: readonly number[], behavior: CompiledBehavior): ActionShare[] {
  const counts = new Map<number, number>();
  for (const d of decisions) counts.set(d, (counts.get(d) ?? 0) + 1);

  return [...counts.entries()]
    .map(([action, frames]) => ({
      action,
      name: behavior.actions[action]?.name ?? "(none)",
      frames,
      fraction: decisions.length === 0 ? 0 : frames / decisions.length,
    }))
    .sort((a, b) => (b.frames !== a.frames ? b.frames - a.frames : a.action - b.action));
}

/**
 * Replay one recording against two behaviors and report how the decisions
 * differ (section 9.4).
 */
export function diffBehaviors(
  recording: Recording,
  before: CompiledBehavior,
  after: CompiledBehavior,
): BehaviorDiff {
  const a = runThrough(recording, before);
  const b = runThrough(recording, after);

  let decisionsChanged = 0;
  for (let i = 0; i < a.decisions.length; i++) {
    if (a.decisions[i] !== b.decisions[i]) decisionsChanged++;
  }

  const beforeShares = shares(a.decisions, before);
  const afterShares = shares(b.decisions, after);

  // Shares are keyed by action name rather than index: an edit that adds or
  // removes an action renumbers everything after it, and a diff that compared
  // by index would report every later action as changed.
  const byName = new Map<string, { before: number; after: number; action: number }>();
  for (const s of beforeShares) {
    byName.set(s.name, { before: s.fraction, after: 0, action: s.action });
  }
  for (const s of afterShares) {
    const entry = byName.get(s.name);
    if (entry) entry.after = s.fraction;
    else byName.set(s.name, { before: 0, after: s.fraction, action: s.action });
  }

  const shifts = [...byName.entries()]
    .map(([name, v]) => ({ action: v.action, name, delta: v.after - v.before }))
    .filter((s) => Math.abs(s.delta) > 1e-9)
    .sort((x, y) => Math.abs(y.delta) - Math.abs(x.delta));

  return {
    frames: recording.frames.length,
    decisionsChanged,
    changedFraction:
      recording.frames.length === 0 ? 0 : decisionsChanged / recording.frames.length,
    switchesBefore: a.switches,
    switchesAfter: b.switches,
    before: beforeShares,
    after: afterShares,
    shifts,
  };
}

/**
 * The sentence a designer actually wants out of section 9.4.
 *
 * "Your curve change makes the guard take cover 40% more often in this
 * recorded encounter."
 */
export function summarizeDiff(diff: BehaviorDiff): string {
  if (diff.decisionsChanged === 0) {
    return `No decisions changed across ${diff.frames} recorded frames.`;
  }

  const lines: string[] = [];
  lines.push(
    `${diff.decisionsChanged} of ${diff.frames} decisions changed ` +
      `(${(diff.changedFraction * 100).toFixed(1)}%).`,
  );

  for (const shift of diff.shifts.slice(0, 5)) {
    const direction = shift.delta > 0 ? "more" : "less";
    lines.push(
      `  ${shift.name}: ${Math.abs(shift.delta * 100).toFixed(1)} ` +
        `percentage points ${direction} often`,
    );
  }

  if (diff.switchesAfter !== diff.switchesBefore) {
    const delta = diff.switchesAfter - diff.switchesBefore;
    lines.push(
      `  the agent changed its mind ${Math.abs(delta)} ` +
        `time${Math.abs(delta) === 1 ? "" : "s"} ` +
        `${delta > 0 ? "more" : "less"} ` +
        `(${diff.switchesBefore} -> ${diff.switchesAfter})`,
    );
  }
  return lines.join("\n");
}
