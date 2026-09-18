/**
 * Edit-time validation (design.md section 10.4).
 *
 * | Check                                          | Severity |
 * |------------------------------------------------|----------|
 * | Unknown input name                             | Error    |
 * | Task type not registered                       | Error    |
 * | Curve parameters produce NaN or constant output| Error    |
 * | Consideration range is inverted or zero-width  | Error    |
 * | Action can never score above zero              | Warning  |
 * | Action always scores highest in its bucket     | Warning  |
 * | Expensive consideration ordered first          | Hint     |
 * | Bucket empty                                   | Warning  |
 *
 * The "always wins" and "never wins" checks are the interesting ones: the
 * document notes they are cheap to compute by sampling the input space and
 * "surprisingly effective at finding authoring mistakes". They are the only
 * two here that need to actually run the scorer.
 *
 * One deliberate departure, flagged in docs/notes-on-the-spec.md: the table
 * lists an inverted range as an error, but an inverted range is the natural
 * way to author "closer is better" and the core supports it. Only a
 * ZERO-WIDTH range is an error here; inverted is accepted silently.
 */

import type { Action, Behavior, Consideration } from "../format/types.js";
import { compensate, curveRange, evaluateCurve, normalize } from "../curves/evaluate.js";

export type Severity = "error" | "warning" | "hint";

export interface Diagnostic {
  severity: Severity;
  path: string;
  message: string;
  rule: string;
}

/**
 * Task types the runtime knows about. A real project registers these from the
 * Unreal side; the default set is what the samples use.
 *
 * Section 5.4: a dangling task reference should fail the build, not fail at
 * runtime in front of a player.
 */
export const DEFAULT_TASK_REGISTRY: readonly string[] = [
  "MoveToCover",
  "FireAtTarget",
  "Reload",
  "Patrol",
  "Investigate",
  "Idle",
  "Flee",
  "UseObject",
  "Chat",
];

/**
 * Considerations known to be expensive to evaluate.
 *
 * Section 3.4: "Order considerations cheapest-and-most-discriminating first.
 * A 'is the target alive' check that returns 0 costs one branch and eliminates
 * the action; a 'path length to target' consideration that requires a navmesh
 * query costs microseconds." Getting this order right is often a larger
 * performance win than any micro-optimization.
 */
const EXPENSIVE_TRANSFORMS = new Set(["pathlength", "navdistance", "linetrace", "eqs"]);

export interface ValidateOptions {
  taskRegistry?: readonly string[];
  /** Samples per input when probing the "always/never wins" rules. */
  samples?: number;
}

function isExpensive(
  consideration: Consideration,
  behavior: Behavior,
): boolean {
  const input = behavior.inputs.find((i) => i.name === consideration.input);
  if (!input?.transform) return false;
  return EXPENSIVE_TRANSFORMS.has(input.transform.toLowerCase());
}

function scoreAction(
  action: Action,
  behavior: Behavior,
  inputs: Map<string, number>,
): number {
  let score = action.weight ?? 1;
  const n = action.considerations.length;
  for (const c of action.considerations) {
    const raw = inputs.get(c.input) ?? 0;
    score *= evaluateCurve(c.curve, normalize(raw, c.range[0], c.range[1]));
    if (score <= 0) return 0;
  }
  return compensate(score, n);
}

/**
 * Probe the input space on a deterministic lattice.
 *
 * Deterministic rather than random, so a warning that appears on one machine
 * appears on every machine and in CI. A validator that flags an action
 * intermittently is a validator people learn to ignore.
 */
function probeInputs(behavior: Behavior, samples: number): Map<string, number>[] {
  const out: Map<string, number>[] = [];
  const inputCount = behavior.inputs.length;
  if (inputCount === 0) return [new Map()];

  // A full grid is samples^inputs, which explodes. A low-discrepancy sequence
  // covers the space far better than a grid of the same size and costs the
  // same, so the "never wins" check stays cheap on a behavior with 20 inputs.
  const total = Math.max(samples * inputCount, 64);
  const PHI = 1.6180339887498949;

  for (let i = 0; i < total; i++) {
    const point = new Map<string, number>();
    behavior.inputs.forEach((input, d) => {
      const alpha = 1 / Math.pow(PHI, d + 1);
      const t = (0.5 + alpha * (i + 1)) % 1;
      // Sample across the UNION of every range declared for this input, so
      // the probe covers values any consideration actually sees.
      //
      // Taking the first declared range instead would mean that when two
      // actions declare different ranges for the same input -- which is
      // normal, "close" and "very close" are different spans of the same
      // distance -- the second one gets probed entirely outside its own range
      // and reported as never winning. That is a false positive on a correct
      // behavior, which is how a validator gets switched off.
      let lo = Infinity;
      let hi = -Infinity;
      for (const action of behavior.actions) {
        for (const c of action.considerations) {
          if (c.input !== input.name) continue;
          lo = Math.min(lo, c.range[0], c.range[1]);
          hi = Math.max(hi, c.range[0], c.range[1]);
        }
      }
      if (!Number.isFinite(lo) || !Number.isFinite(hi) || lo === hi) {
        lo = 0;
        hi = 1;
      }
      point.set(input.name, lo + t * (hi - lo));
    });
    out.push(point);
  }
  return out;
}

export function validate(
  behavior: Behavior,
  options: ValidateOptions = {},
): Diagnostic[] {
  const diagnostics: Diagnostic[] = [];
  const registry = new Set(options.taskRegistry ?? DEFAULT_TASK_REGISTRY);
  const inputNames = new Set(behavior.inputs.map((i) => i.name));

  const add = (
    severity: Severity,
    rule: string,
    path: string,
    message: string,
  ): void => {
    diagnostics.push({ severity, rule, path, message });
  };

  // ------------------------------------------------------------- structure

  for (const action of behavior.actions) {
    const base = `actions.${action.name}`;

    if (!registry.has(action.task.type)) {
      add(
        "error",
        "unknown-task",
        `${base}.task`,
        `task type "${action.task.type}" is not registered. ` +
          `Known: ${[...registry].sort().join(", ")}`,
      );
    }

    if (action.considerations.length === 0) {
      add(
        "warning",
        "no-considerations",
        base,
        "action has no considerations, so it always scores its weight and " +
          "can never respond to anything",
      );
    }

    action.considerations.forEach((c, i) => {
      const path = `${base}.considerations[${i}]`;

      if (!inputNames.has(c.input)) {
        add(
          "error",
          "unknown-input",
          path,
          `references unknown input "${c.input}"`,
        );
      }

      // Zero-width would divide by zero. Inverted is legal and is how "closer
      // is better" is authored -- see the note at the top of this file.
      if (c.range[0] === c.range[1]) {
        add(
          "error",
          "zero-width-range",
          `${path}.range`,
          `range [${c.range[0]}, ${c.range[1]}] has zero width, so every ` +
            "input normalizes to the same value",
        );
      }

      const range = curveRange(c.curve);
      if (!Number.isFinite(range.min) || !Number.isFinite(range.max)) {
        add(
          "error",
          "curve-nan",
          `${path}.curve`,
          "curve produces a non-finite value; an unclamped NaN silently " +
            "zeroes this action forever",
        );
      } else if (range.constant) {
        add(
          "error",
          "curve-constant",
          `${path}.curve`,
          `curve output is constant at ${range.min.toFixed(3)} across the ` +
            "whole input range, so this consideration cannot discriminate",
        );
      }

      // Section 3.4's hint. Not an error -- sometimes the expensive check
      // genuinely is the discriminating one -- but worth surfacing.
      if (i > 0 && isExpensive(c, behavior)) {
        const cheaperFollow = action.considerations
          .slice(0, i)
          .every((earlier) => !isExpensive(earlier, behavior));
        if (!cheaperFollow) return;
      }
      if (i === 0 && isExpensive(c, behavior) && action.considerations.length > 1) {
        const anyCheapLater = action.considerations
          .slice(1)
          .some((later) => !isExpensive(later, behavior));
        if (anyCheapLater) {
          add(
            "hint",
            "expensive-first",
            path,
            `"${c.input}" is expensive to evaluate and is ordered first. ` +
              "Early-out works front to back, so a cheap discriminating " +
              "consideration ahead of it would skip the cost entirely " +
              "(section 3.4)",
          );
        }
      }
    });
  }

  // --------------------------------------------------------------- buckets

  const buckets = new Map<number, Action[]>();
  for (const action of behavior.actions) {
    const list = buckets.get(action.bucket) ?? [];
    list.push(action);
    buckets.set(action.bucket, list);
  }

  const maxBucket = Math.max(-1, ...behavior.actions.map((a) => a.bucket));
  for (let b = 0; b <= maxBucket; b++) {
    if (!buckets.has(b)) {
      add(
        "warning",
        "empty-bucket",
        `buckets[${b}]`,
        `bucket ${b} is empty but bucket ${maxBucket} is populated, so the ` +
          "priority tiers have a gap in them",
      );
    }
  }

  // ------------------------------------------- always-wins and never-wins
  //
  // "Cheap to compute by sampling the input space and surprisingly effective
  // at finding authoring mistakes."

  const probes = probeInputs(behavior, options.samples ?? 24);

  for (const [bucket, actions] of [...buckets.entries()].sort((a, b) => a[0] - b[0])) {
    const wins = new Map<string, number>();
    const everPositive = new Map<string, boolean>();

    for (const point of probes) {
      let best = -1;
      let winner = "";
      for (const action of actions) {
        const score = scoreAction(action, behavior, point);
        if (score > 0) everPositive.set(action.name, true);
        if (score > best) {
          best = score;
          winner = action.name;
        }
      }
      if (winner) wins.set(winner, (wins.get(winner) ?? 0) + 1);
    }

    for (const action of actions) {
      if (!everPositive.get(action.name)) {
        add(
          "warning",
          "never-wins",
          `actions.${action.name}`,
          "action never scores above zero anywhere in the sampled input " +
            "space. Usually a curve that outputs zero, or a range the input " +
            "never reaches",
        );
        continue;
      }
      if (actions.length > 1 && wins.get(action.name) === probes.length) {
        const others = actions
          .filter((a) => a.name !== action.name)
          .map((a) => a.name)
          .join(", ");
        add(
          "warning",
          "always-wins",
          `actions.${action.name}`,
          `action wins every sampled point in bucket ${bucket}, which makes ` +
            `${others} dead code`,
        );
      }
    }
  }

  // Deterministic order, so a diff of two validation runs is readable and CI
  // output does not shuffle between runs.
  const severityRank: Record<Severity, number> = { error: 0, warning: 1, hint: 2 };
  diagnostics.sort((a, b) => {
    if (a.severity !== b.severity) {
      return severityRank[a.severity] - severityRank[b.severity];
    }
    if (a.path !== b.path) return a.path < b.path ? -1 : 1;
    return a.rule < b.rule ? -1 : a.rule > b.rule ? 1 : 0;
  });

  return diagnostics;
}

export function formatDiagnostics(diagnostics: readonly Diagnostic[]): string {
  if (diagnostics.length === 0) return "no problems found";
  const icon: Record<Severity, string> = {
    error: "error",
    warning: "warn ",
    hint: "hint ",
  };
  return diagnostics
    .map((d) => `${icon[d.severity]} ${d.path}\n        ${d.message}`)
    .join("\n");
}
