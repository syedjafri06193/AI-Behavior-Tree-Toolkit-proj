/**
 * Canonical serialization (design.md section 5.2).
 *
 * The emitter is hand-written rather than js-yaml's `dump`. That is a
 * deliberate trade: a general YAML emitter gives you no control over key
 * order, number formatting or flow style, and all three are exactly what
 * determines whether a save produces a diff.
 *
 * The rule is the one `gofmt` uses: the tool owns the formatting. Parse
 * leniently, emit canonically, and round-tripping is then byte-identical by
 * construction rather than by luck. `roundTripsByteIdentically` in the tests
 * is the check, and section 5.2 is right that ordering instability is what
 * produces spurious diffs on every save.
 */

import type {
  Action,
  Behavior,
  Consideration,
  Curve,
  InputDef,
  Layout,
} from "./types.js";
import { DEFAULT_POLICY } from "./types.js";

/**
 * Shortest representation that parses back to the same value.
 *
 * `String(n)` is already the shortest round-tripping form in JavaScript, so
 * this mostly exists to reject the values that cannot survive a round trip at
 * all. A NaN or an Infinity in a behavior file is not a formatting question --
 * it is a corrupt file, and writing it out would produce something that parses
 * into an agent that never acts.
 */
function num(n: number): string {
  if (!Number.isFinite(n)) {
    throw new Error(`cannot serialize non-finite number: ${n}`);
  }
  if (Object.is(n, -0)) return "0";
  return String(n);
}

/**
 * Curve parameters are written with a decimal point even when integral.
 *
 * `m: -1.0` rather than `m: -1`. These are continuous tuning knobs and a
 * designer reading a diff should see them as such; it also keeps a one-line
 * curve change looking like a curve change. The value is identical either way,
 * so this is purely about how the diff reads -- which is the whole point of
 * the format.
 */
function curveNum(n: number): string {
  if (!Number.isFinite(n)) {
    throw new Error(`cannot serialize non-finite curve parameter: ${n}`);
  }
  if (Object.is(n, -0)) return "0.0";
  return Number.isInteger(n) ? `${n}.0` : String(n);
}

function quoteKey(key: string): string {
  // Plain scalars are fine for the identifiers this format uses; anything
  // else gets quoted so a stray colon in a task name cannot produce a file
  // that parses into something different from what was written.
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test(key) ? key : JSON.stringify(key);
}

function scalar(value: string): string {
  if (value === "") return '""';
  if (/^[A-Za-z_][A-Za-z0-9_.-]*$/.test(value)) return value;
  return JSON.stringify(value);
}

function curveFlow(curve: Curve): string {
  // Fixed key order: type first, then the four parameters in the order the
  // formula uses them, y = m(x - c)^k + b. Alphabetical would put b before c
  // before k before m, which reads as noise.
  return (
    `{ type: ${curve.type}, m: ${curveNum(curve.m)}, k: ${curveNum(curve.k)}, ` +
    `b: ${curveNum(curve.b)}, c: ${curveNum(curve.c)} }`
  );
}

function inputLine(input: InputDef): string {
  const parts = [
    `name: ${scalar(input.name)}`,
    `source: ${input.source}`,
    `key: ${scalar(input.key)}`,
  ];
  if (input.transform) parts.push(`transform: ${scalar(input.transform)}`);
  return `  - { ${parts.join(", ")} }`;
}

function considerationLines(c: Consideration): string[] {
  return [
    `      - input: ${scalar(c.input)}`,
    `        range: [${num(c.range[0])}, ${num(c.range[1])}]`,
    `        curve: ${curveFlow(c.curve)}`,
  ];
}

function taskLine(action: Action): string {
  const params = action.task.params;
  if (!params || Object.keys(params).length === 0) {
    return `    task: { type: ${scalar(action.task.type)} }`;
  }
  // Params are sorted so a designer adding one does not reshuffle the rest.
  const entries = Object.keys(params)
    .sort()
    .map((key) => {
      const value = params[key];
      const rendered =
        typeof value === "number"
          ? num(value)
          : typeof value === "boolean"
            ? String(value)
            : scalar(String(value));
      return `${quoteKey(key)}: ${rendered}`;
    });
  return (
    `    task: { type: ${scalar(action.task.type)}, ` +
    `params: { ${entries.join(", ")} } }`
  );
}

function actionLines(action: Action): string[] {
  const lines: string[] = [];
  lines.push(`  - name: ${scalar(action.name)}`);
  lines.push(`    bucket: ${num(action.bucket)}`);
  // Optional fields are omitted at their default, so a file only mentions what
  // was actually decided. A format that writes every default makes a two-line
  // change look like a rewrite.
  if (action.weight !== undefined && action.weight !== 1) {
    lines.push(`    weight: ${curveNum(action.weight)}`);
  }
  if (action.interruptible === false) {
    lines.push(`    interruptible: false`);
  }
  lines.push(`    considerations:`);
  for (const c of action.considerations) {
    lines.push(...considerationLines(c));
  }
  lines.push(taskLine(action));
  return lines;
}

/**
 * Sort actions by bucket, then name (section 5.2).
 *
 * Considerations are deliberately NOT sorted: their order is semantically
 * meaningful, because early-out evaluates them in sequence and section 3.4
 * says to put the cheap, discriminating ones first. Sorting them would be a
 * silent performance regression, and a nasty one, because the behavior would
 * be identical and only the cost would change.
 */
export function sortActions(actions: readonly Action[]): Action[] {
  return [...actions].sort((a, b) => {
    if (a.bucket !== b.bucket) return a.bucket - b.bucket;
    return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
  });
}

export function serialize(behavior: Behavior): string {
  const lines: string[] = [];

  lines.push(`version: ${num(behavior.version)}`);
  lines.push(`type: ${behavior.type}`);
  lines.push(`name: ${scalar(behavior.name)}`);
  lines.push("");

  lines.push("inputs:");
  // Inputs keep their authored order too: the compiler assigns indices from
  // it, and those indices appear in recordings. Re-sorting them would
  // invalidate every recording made against the file for a cosmetic reason.
  for (const input of behavior.inputs) {
    lines.push(inputLine(input));
  }
  lines.push("");

  lines.push("actions:");
  const sorted = sortActions(behavior.actions);
  sorted.forEach((action, index) => {
    if (index > 0) lines.push("");
    lines.push(...actionLines(action));
  });
  lines.push("");

  lines.push("policy:");
  lines.push(`  evaluationHz: ${num(behavior.policy.evaluationHz)}`);
  lines.push(`  currentActionBonus: ${num(behavior.policy.currentActionBonus)}`);
  lines.push(
    `  minCommitmentSeconds: ${num(behavior.policy.minCommitmentSeconds)}`,
  );
  lines.push(`  switchThreshold: ${num(behavior.policy.switchThreshold)}`);
  lines.push(
    `  bucketSatisfiedThreshold: ${num(behavior.policy.bucketSatisfiedThreshold)}`,
  );

  return lines.join("\n") + "\n";
}

export function serializeLayout(layout: Layout): string {
  // JSON, not YAML: this file is machine-written, frequently gitignored, and
  // nobody reviews it. Sorted keys so that even when it is committed it does
  // not churn.
  const nodes: Record<string, unknown> = {};
  for (const key of Object.keys(layout.nodes).sort()) {
    nodes[key] = layout.nodes[key];
  }
  const out: Record<string, unknown> = { version: layout.version, nodes };
  if (layout.comments?.length) out.comments = layout.comments;
  if (layout.collapsed?.length) out.collapsed = [...layout.collapsed].sort();
  return JSON.stringify(out, null, 2) + "\n";
}

export { DEFAULT_POLICY };
