/**
 * The compiler: YAML -> .aib (design.md section 5.4).
 *
 * "YAML is for humans. The runtime should load a compiled binary: flattened
 * arrays, resolved indices, no string lookups, no parsing at load." Compile
 * during the Unreal cook step so shipped builds never see YAML.
 *
 * The compiler is also where validation happens: "a malformed curve or a
 * dangling task reference should fail the build, not fail at runtime in front
 * of a player."
 *
 * The output is consumed by aib::Deserialize in core/src/compiled.cpp. Two
 * languages, one format, so the layout here is normative against that reader
 * and tests/crosscheck.test.ts checks a file written here actually loads and
 * scores identically there.
 */

import type { Behavior } from "./types.js";
import { CURVE_TYPE_ID } from "./types.js";
import { sortActions } from "./serialize.js";
import { validate, type Diagnostic } from "../validate/rules.js";

export const FORMAT_VERSION = 1;
export const MAGIC = 0x42494141; // 'AAIB' little-endian

class Writer {
  private buf: number[] = [];

  u8(v: number): void {
    this.buf.push(v & 0xff);
  }
  u16(v: number): void {
    this.buf.push(v & 0xff, (v >>> 8) & 0xff);
  }
  u32(v: number): void {
    this.buf.push(
      v & 0xff,
      (v >>> 8) & 0xff,
      (v >>> 16) & 0xff,
      (v >>> 24) & 0xff,
    );
  }
  f32(v: number): void {
    // Round-trip through Float32Array so the value written is exactly the
    // float32 the C++ side will read. Skipping this would let a double that
    // the editor displayed as 0.1 differ from the 0.1f the runtime scores
    // with, and the cross-check would fail for a reason nobody could see.
    const scratch = new DataView(new ArrayBuffer(4));
    scratch.setFloat32(0, v, true);
    for (let i = 0; i < 4; i++) this.buf.push(scratch.getUint8(i));
  }
  str(s: string): void {
    const bytes = new TextEncoder().encode(s);
    this.u16(bytes.length);
    for (const b of bytes) this.buf.push(b);
  }
  bytes(): Uint8Array {
    return Uint8Array.from(this.buf);
  }
}

/** FNV-1a, matching aib::ComputeVersion. */
function fnv1a(h: number, bytes: Uint8Array): number {
  for (const b of bytes) {
    h ^= b;
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h >>> 0;
}

function hashString(h: number, s: string): number {
  return fnv1a(h, new TextEncoder().encode(s));
}

function hashF32(h: number, v: number): number {
  if (v === 0) v = 0; // normalize -0 to +0, as the C++ does
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, v, true);
  return fnv1a(h, new Uint8Array(view.buffer));
}

function hashU8(h: number, v: number): number {
  return fnv1a(h, Uint8Array.of(v & 0xff));
}

function hashU16(h: number, v: number): number {
  return fnv1a(h, Uint8Array.of(v & 0xff, (v >>> 8) & 0xff));
}

export interface CompileResult {
  bytes: Uint8Array;
  version: number;
  inputNames: string[];
  taskNames: string[];
  /** Action names in emitted order -- the indices recordings refer to. */
  actionNames: string[];
  diagnostics: Diagnostic[];
}

export class CompileError extends Error {
  constructor(
    message: string,
    readonly diagnostics: Diagnostic[],
  ) {
    super(message);
    this.name = "CompileError";
  }
}

/**
 * Compile a behavior.
 *
 * Errors from the validator stop the build, which is the point: section 5.4
 * puts validation here precisely so a dangling task reference fails the cook
 * rather than the player's session. Warnings and hints are returned for the
 * editor to display.
 */
export function compile(behavior: Behavior): CompileResult {
  const diagnostics = validate(behavior);
  const errors = diagnostics.filter((d) => d.severity === "error");
  if (errors.length > 0) {
    throw new CompileError(
      `${errors.length} error${errors.length === 1 ? "" : "s"} in ` +
        `"${behavior.name}":\n` +
        errors.map((e) => `  ${e.path}: ${e.message}`).join("\n"),
      diagnostics,
    );
  }

  // Indices come from the authored order of `inputs`, not from a sort. They
  // appear in every recording, so re-ordering them would silently invalidate
  // recorded sessions -- see the note in serialize.ts.
  const inputNames = behavior.inputs.map((i) => i.name);
  const inputIndex = new Map(inputNames.map((n, i) => [n, i]));

  // Task names are deduplicated and sorted: unlike inputs, nothing external
  // refers to a task index, so a stable sort is free and keeps the table from
  // depending on which action happened to mention a task first.
  const taskNames = [...new Set(behavior.actions.map((a) => a.task.type))].sort();
  const taskIndex = new Map(taskNames.map((n, i) => [n, i]));

  const actions = sortActions(behavior.actions);
  const actionNames = actions.map((a) => a.name);

  // --- content version, computed exactly as aib::ComputeVersion does ---
  let h = 2166136261 >>> 0;
  h = hashString(h, behavior.name);
  for (const n of inputNames) h = hashString(h, n);
  for (const n of taskNames) h = hashString(h, n);
  for (const a of actions) {
    h = hashString(h, a.name);
    h = hashU8(h, a.bucket);
    h = hashF32(h, a.weight ?? 1);
    h = hashU8(h, a.interruptible === false ? 0 : 1);
    h = hashU16(h, taskIndex.get(a.task.type) ?? 0);
    for (const c of a.considerations) {
      h = hashU16(h, inputIndex.get(c.input) ?? 0);
      h = hashF32(h, c.range[0]);
      h = hashF32(h, c.range[1]);
      h = hashU8(h, CURVE_TYPE_ID[c.curve.type]);
      h = hashF32(h, c.curve.m);
      h = hashF32(h, c.curve.k);
      h = hashF32(h, c.curve.b);
      h = hashF32(h, c.curve.c);
    }
  }
  h = hashF32(h, behavior.policy.evaluationHz);
  h = hashF32(h, behavior.policy.currentActionBonus);
  h = hashF32(h, behavior.policy.minCommitmentSeconds);
  h = hashF32(h, behavior.policy.switchThreshold);
  h = hashF32(h, behavior.policy.bucketSatisfiedThreshold);
  const version = h >>> 0;

  // --- emit ---
  const w = new Writer();
  w.u32(MAGIC);
  w.u32(FORMAT_VERSION);
  w.u32(version);
  w.str(behavior.name);

  w.u16(inputNames.length);
  for (const n of inputNames) w.str(n);

  w.u16(taskNames.length);
  for (const n of taskNames) w.str(n);

  w.f32(behavior.policy.evaluationHz);
  w.f32(behavior.policy.currentActionBonus);
  w.f32(behavior.policy.minCommitmentSeconds);
  w.f32(behavior.policy.switchThreshold);
  w.f32(behavior.policy.bucketSatisfiedThreshold);

  w.u16(actions.length);
  for (const a of actions) {
    w.str(a.name);
    w.u8(a.bucket);
    w.u8(a.interruptible === false ? 0 : 1);
    w.f32(a.weight ?? 1);
    w.u16(taskIndex.get(a.task.type) ?? 0);
    w.u16(a.considerations.length);
    for (const c of a.considerations) {
      w.u16(inputIndex.get(c.input) ?? 0);
      w.f32(c.range[0]);
      w.f32(c.range[1]);
      w.u8(CURVE_TYPE_ID[c.curve.type]);
      w.f32(c.curve.m);
      w.f32(c.curve.k);
      w.f32(c.curve.b);
      w.f32(c.curve.c);
    }
  }

  return {
    bytes: w.bytes(),
    version,
    inputNames,
    taskNames,
    actionNames,
    diagnostics,
  };
}
