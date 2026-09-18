/**
 * Parsing (design.md section 5).
 *
 * Lenient on the way in, canonical on the way out. A designer hand-editing a
 * file should not have to match the emitter's whitespace, and a file that
 * omits a default should load with that default rather than be rejected.
 *
 * What is NOT lenient: anything that would change behavior silently. An
 * unknown curve type, a consideration pointing at an input that does not
 * exist, a missing required field -- those throw, because the alternative is
 * an agent that runs and is wrong. Section 10.4 puts the same checks at edit
 * time as errors rather than warnings for the same reason.
 */

import { load } from "js-yaml";

import type {
  Action,
  Behavior,
  Consideration,
  Curve,
  CurveType,
  InputDef,
  InputSource,
  Layout,
  Policy,
  TaskRef,
} from "./types.js";
import { CURVE_TYPES, DEFAULT_CURVE, DEFAULT_POLICY } from "./types.js";

export class FormatError extends Error {
  constructor(
    message: string,
    readonly path: string,
  ) {
    super(`${path}: ${message}`);
    this.name = "FormatError";
  }
}

type Json = Record<string, unknown>;

function obj(value: unknown, path: string): Json {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new FormatError(`expected a mapping, got ${describe(value)}`, path);
  }
  return value as Json;
}

function arr(value: unknown, path: string): unknown[] {
  if (!Array.isArray(value)) {
    throw new FormatError(`expected a list, got ${describe(value)}`, path);
  }
  return value;
}

function describe(value: unknown): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "a list";
  return typeof value;
}

function str(value: unknown, path: string): string {
  if (typeof value !== "string" || value === "") {
    throw new FormatError(
      `expected a non-empty string, got ${describe(value)}`,
      path,
    );
  }
  return value;
}

function finite(value: unknown, path: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    // A NaN here is how an unclamped curve parameter reaches the runtime and
    // silently zeroes an action forever (section 3.2). Reject it at the door.
    throw new FormatError(
      `expected a finite number, got ${describe(value)}`,
      path,
    );
  }
  return value;
}

function optionalNumber(value: unknown, fallback: number, path: string): number {
  return value === undefined || value === null ? fallback : finite(value, path);
}

function parseCurve(value: unknown, path: string): Curve {
  if (value === undefined || value === null) return { ...DEFAULT_CURVE };
  const raw = obj(value, path);

  const type = str(raw.type, `${path}.type`);
  if (!CURVE_TYPES.includes(type as CurveType)) {
    throw new FormatError(
      `unknown curve type "${type}"; expected one of ${CURVE_TYPES.join(", ")}`,
      `${path}.type`,
    );
  }

  return {
    type: type as CurveType,
    m: optionalNumber(raw.m, DEFAULT_CURVE.m, `${path}.m`),
    k: optionalNumber(raw.k, DEFAULT_CURVE.k, `${path}.k`),
    b: optionalNumber(raw.b, DEFAULT_CURVE.b, `${path}.b`),
    c: optionalNumber(raw.c, DEFAULT_CURVE.c, `${path}.c`),
  };
}

function parseConsideration(value: unknown, path: string): Consideration {
  const raw = obj(value, path);
  const range = arr(raw.range ?? [0, 1], `${path}.range`);
  if (range.length !== 2) {
    throw new FormatError(
      `range must have exactly two entries, got ${range.length}`,
      `${path}.range`,
    );
  }
  return {
    input: str(raw.input, `${path}.input`),
    range: [
      finite(range[0], `${path}.range[0]`),
      finite(range[1], `${path}.range[1]`),
    ],
    curve: parseCurve(raw.curve, `${path}.curve`),
  };
}

function parseTask(value: unknown, path: string): TaskRef {
  const raw = obj(value, path);
  const task: TaskRef = { type: str(raw.type, `${path}.type`) };
  if (raw.params !== undefined && raw.params !== null) {
    task.params = obj(raw.params, `${path}.params`);
  }
  return task;
}

function parseAction(value: unknown, path: string): Action {
  const raw = obj(value, path);
  const considerations = arr(
    raw.considerations ?? [],
    `${path}.considerations`,
  ).map((c, i) => parseConsideration(c, `${path}.considerations[${i}]`));

  const action: Action = {
    name: str(raw.name, `${path}.name`),
    bucket: optionalNumber(raw.bucket, 0, `${path}.bucket`),
    considerations,
    task: parseTask(raw.task, `${path}.task`),
  };

  if (!Number.isInteger(action.bucket) || action.bucket < 0 || action.bucket > 255) {
    throw new FormatError(
      `bucket must be an integer in 0..255, got ${action.bucket}`,
      `${path}.bucket`,
    );
  }
  if (raw.weight !== undefined && raw.weight !== null) {
    action.weight = finite(raw.weight, `${path}.weight`);
  }
  if (raw.interruptible !== undefined && raw.interruptible !== null) {
    if (typeof raw.interruptible !== "boolean") {
      throw new FormatError(
        `interruptible must be true or false`,
        `${path}.interruptible`,
      );
    }
    action.interruptible = raw.interruptible;
  }
  return action;
}

const INPUT_SOURCES: readonly InputSource[] = [
  "blackboard",
  "attribute",
  "constant",
  "custom",
];

function parseInput(value: unknown, path: string): InputDef {
  const raw = obj(value, path);
  const source = str(raw.source, `${path}.source`);
  if (!INPUT_SOURCES.includes(source as InputSource)) {
    throw new FormatError(
      `unknown input source "${source}"; expected one of ${INPUT_SOURCES.join(", ")}`,
      `${path}.source`,
    );
  }
  const input: InputDef = {
    name: str(raw.name, `${path}.name`),
    source: source as InputSource,
    key: str(raw.key, `${path}.key`),
  };
  if (raw.transform !== undefined && raw.transform !== null) {
    input.transform = str(raw.transform, `${path}.transform`);
  }
  return input;
}

function parsePolicy(value: unknown, path: string): Policy {
  if (value === undefined || value === null) return { ...DEFAULT_POLICY };
  const raw = obj(value, path);
  return {
    evaluationHz: optionalNumber(
      raw.evaluationHz,
      DEFAULT_POLICY.evaluationHz,
      `${path}.evaluationHz`,
    ),
    currentActionBonus: optionalNumber(
      raw.currentActionBonus,
      DEFAULT_POLICY.currentActionBonus,
      `${path}.currentActionBonus`,
    ),
    minCommitmentSeconds: optionalNumber(
      raw.minCommitmentSeconds,
      DEFAULT_POLICY.minCommitmentSeconds,
      `${path}.minCommitmentSeconds`,
    ),
    switchThreshold: optionalNumber(
      raw.switchThreshold,
      DEFAULT_POLICY.switchThreshold,
      `${path}.switchThreshold`,
    ),
    bucketSatisfiedThreshold: optionalNumber(
      raw.bucketSatisfiedThreshold,
      DEFAULT_POLICY.bucketSatisfiedThreshold,
      `${path}.bucketSatisfiedThreshold`,
    ),
  };
}

export function parse(text: string): Behavior {
  const raw = obj(load(text), "<root>");

  const version = optionalNumber(raw.version, 1, "version");
  if (version !== 1) {
    // Same reasoning as the binary format: refuse rather than misread.
    throw new FormatError(
      `unsupported format version ${version}; this build understands version 1`,
      "version",
    );
  }

  const type = str(raw.type ?? "utility", "type");
  if (type !== "utility") {
    throw new FormatError(
      `unsupported behavior type "${type}"`,
      "type",
    );
  }

  const behavior: Behavior = {
    version,
    type: "utility",
    name: str(raw.name, "name"),
    inputs: arr(raw.inputs ?? [], "inputs").map((v, i) =>
      parseInput(v, `inputs[${i}]`),
    ),
    actions: arr(raw.actions ?? [], "actions").map((v, i) =>
      parseAction(v, `actions[${i}]`),
    ),
    policy: parsePolicy(raw.policy, "policy"),
  };

  // Structural checks that must hold before anything else can run. These are
  // errors rather than warnings because each one produces an agent that runs
  // and is wrong, which is worse than one that fails to load.
  const seenInputs = new Set<string>();
  for (const input of behavior.inputs) {
    if (seenInputs.has(input.name)) {
      throw new FormatError(`duplicate input name "${input.name}"`, "inputs");
    }
    seenInputs.add(input.name);
  }

  const seenActions = new Set<string>();
  for (const action of behavior.actions) {
    if (seenActions.has(action.name)) {
      throw new FormatError(`duplicate action name "${action.name}"`, "actions");
    }
    seenActions.add(action.name);

    for (const [i, c] of action.considerations.entries()) {
      if (!seenInputs.has(c.input)) {
        throw new FormatError(
          `consideration references unknown input "${c.input}"`,
          `actions[${action.name}].considerations[${i}]`,
        );
      }
    }
  }

  return behavior;
}

export function parseLayout(text: string): Layout {
  const raw = obj(JSON.parse(text), "<layout>");
  return {
    version: optionalNumber(raw.version, 1, "version"),
    nodes: (raw.nodes ?? {}) as Layout["nodes"],
    comments: raw.comments as Layout["comments"],
    collapsed: raw.collapsed as string[] | undefined,
  };
}

/**
 * A layout generated from scratch (section 5.3).
 *
 * The sidecar is only genuinely optional if the editor can lay out a behavior
 * without one. A team that gitignores layout entirely then gets conflict-free
 * AI authoring, which is the unusual property worth having -- so this is not
 * a convenience, it is what makes the decision in 5.3 stick.
 */
export function autoLayout(behavior: Behavior): Layout {
  const nodes: Layout["nodes"] = {};
  const byBucket = new Map<number, Action[]>();
  for (const action of behavior.actions) {
    const list = byBucket.get(action.bucket) ?? [];
    list.push(action);
    byBucket.set(action.bucket, list);
  }

  const buckets = [...byBucket.keys()].sort((a, b) => a - b);
  buckets.forEach((bucket, column) => {
    const actions = [...(byBucket.get(bucket) ?? [])].sort((a, b) =>
      a.name < b.name ? -1 : 1,
    );
    actions.forEach((action, row) => {
      nodes[action.name] = { x: column * 320, y: row * 180 };
    });
  });

  return { version: 1, nodes };
}
