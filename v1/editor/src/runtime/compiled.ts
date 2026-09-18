/**
 * Reading the compiled .aib format in TypeScript.
 *
 * The mirror of aib::Deserialize. The editor needs this to load what the
 * runtime is actually running -- during a debug session the game reports its
 * behavior version, and the editor has to be looking at the same bytes or the
 * score breakdown it draws is a breakdown of something else.
 *
 * Same discipline as the C++ reader: every read is bounds-checked, an unknown
 * curve type is rejected rather than defaulted, and an out-of-range index
 * fails the load rather than producing an agent that runs and is wrong.
 */

import type { Curve, CurveType } from "../format/types.js";
import { CURVE_TYPES } from "../format/types.js";
import { FORMAT_VERSION, MAGIC } from "../format/compile.js";

export interface CompiledConsideration {
  inputIndex: number;
  inputMin: number;
  inputMax: number;
  curve: Curve;
}

export interface CompiledAction {
  name: string;
  bucket: number;
  weight: number;
  interruptible: boolean;
  taskIndex: number;
  considerations: CompiledConsideration[];
}

export interface CompiledPolicy {
  evaluationHz: number;
  currentActionBonus: number;
  minCommitmentSeconds: number;
  switchThreshold: number;
  bucketSatisfiedThreshold: number;
}

export interface CompiledBehavior {
  name: string;
  version: number;
  inputNames: string[];
  taskNames: string[];
  actions: CompiledAction[];
  actionsInBucket: number[][];
  policy: CompiledPolicy;
}

export class DecodeError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "DecodeError";
  }
}

class Reader {
  private pos = 0;
  private view: DataView;

  constructor(private bytes: Uint8Array) {
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  private need(n: number): void {
    if (this.pos + n > this.bytes.length) {
      throw new DecodeError(
        `file ends mid-record: wanted ${n} bytes at offset ${this.pos}, ` +
          `have ${this.bytes.length - this.pos}`,
      );
    }
  }
  u8(): number {
    this.need(1);
    return this.view.getUint8(this.pos++);
  }
  u16(): number {
    this.need(2);
    const v = this.view.getUint16(this.pos, true);
    this.pos += 2;
    return v;
  }
  u32(): number {
    this.need(4);
    const v = this.view.getUint32(this.pos, true);
    this.pos += 4;
    return v;
  }
  f32(): number {
    this.need(4);
    const v = this.view.getFloat32(this.pos, true);
    this.pos += 4;
    return v;
  }
  str(): string {
    const n = this.u16();
    this.need(n);
    const s = new TextDecoder().decode(this.bytes.subarray(this.pos, this.pos + n));
    this.pos += n;
    return s;
  }
}

export function decodeBehavior(bytes: Uint8Array): CompiledBehavior {
  const r = new Reader(bytes);

  if (r.u32() !== MAGIC) throw new DecodeError("not an .aib file");
  const format = r.u32();
  if (format !== FORMAT_VERSION) {
    throw new DecodeError(
      `unsupported format version ${format}; this build understands ` +
        `${FORMAT_VERSION}`,
    );
  }

  const version = r.u32();
  const name = r.str();

  const inputNames: string[] = [];
  const inputCount = r.u16();
  for (let i = 0; i < inputCount; i++) inputNames.push(r.str());

  const taskNames: string[] = [];
  const taskCount = r.u16();
  for (let i = 0; i < taskCount; i++) taskNames.push(r.str());

  const policy: CompiledPolicy = {
    evaluationHz: r.f32(),
    currentActionBonus: r.f32(),
    minCommitmentSeconds: r.f32(),
    switchThreshold: r.f32(),
    bucketSatisfiedThreshold: r.f32(),
  };

  const actions: CompiledAction[] = [];
  const actionCount = r.u16();
  let maxBucket = 0;

  for (let i = 0; i < actionCount; i++) {
    const actionName = r.str();
    const bucket = r.u8();
    const interruptible = r.u8() !== 0;
    const weight = r.f32();
    const taskIndex = r.u16();
    const considerations: CompiledConsideration[] = [];
    const consCount = r.u16();

    for (let j = 0; j < consCount; j++) {
      const inputIndex = r.u16();
      const inputMin = r.f32();
      const inputMax = r.f32();
      const curveTypeId = r.u8();
      const curveType = CURVE_TYPES[curveTypeId];
      if (curveType === undefined) {
        throw new DecodeError(
          `unknown curve type ${curveTypeId} in action "${actionName}"`,
        );
      }
      const curve: Curve = {
        type: curveType as CurveType,
        m: r.f32(),
        k: r.f32(),
        b: r.f32(),
        c: r.f32(),
      };
      if (inputIndex >= inputNames.length) {
        throw new DecodeError(
          `action "${actionName}" references input index ${inputIndex}, ` +
            `but only ${inputNames.length} inputs are declared`,
        );
      }
      considerations.push({ inputIndex, inputMin, inputMax, curve });
    }

    if (taskIndex >= taskNames.length) {
      throw new DecodeError(
        `action "${actionName}" references task index ${taskIndex}, ` +
          `but only ${taskNames.length} tasks are declared`,
      );
    }

    if (bucket > maxBucket) maxBucket = bucket;
    actions.push({
      name: actionName,
      bucket,
      weight,
      interruptible,
      taskIndex,
      considerations,
    });
  }

  // Derived, not stored -- same as the C++ reader, for the same reason.
  const actionsInBucket: number[][] = Array.from(
    { length: maxBucket + 1 },
    () => [],
  );
  actions.forEach((a, i) => actionsInBucket[a.bucket]!.push(i));

  return { name, version, inputNames, taskNames, actions, actionsInBucket, policy };
}
