/**
 * Scoring and selection in TypeScript.
 *
 * This is a second implementation of core/src/scoring.cpp and
 * core/src/selection.cpp, and it is not duplication for its own sake -- the
 * editor needs to score without a running game:
 *
 *   - section 9.4, replaying a recorded session against an EDITED behavior and
 *     diffing the decisions. The edit only exists in the editor, so there is
 *     nothing to ask the game;
 *   - section 10.4's "always wins" and "never wins" validation, which samples
 *     the input space at edit time;
 *   - section 10.3's curve editor showing what a change does before it ships.
 *
 * Two implementations of the same decision procedure is a liability unless
 * they are checked against each other, so tests/crosscheck.test.ts compiles a
 * behavior here, loads it in the C++ core, and requires identical decisions
 * across thousands of input vectors. If the editor tells a designer their
 * curve change makes the guard take cover 40% more often, and the runtime
 * disagrees, the tool is worse than no tool.
 *
 * THE C++ SIDE IS NORMATIVE. Where the two could differ, this matches it.
 */

import type { CompiledBehavior } from "./compiled.js";
import { compensate, evaluateCurve, normalize } from "../curves/evaluate.js";

export type ScoreOutcome = "complete" | "early-out" | "vetoed";

export interface ScoreTrace {
  score: number;
  outcome: ScoreOutcome;
  stoppedAt: number;
  considerationsEvaluated: number;
}

export interface SelectionState {
  currentAction: number;
  currentScore: number;
  timeInCurrentAction: number;
  forceReevaluate: boolean;
}

export function newSelectionState(): SelectionState {
  return {
    currentAction: -1,
    currentScore: 0,
    timeInCurrentAction: 0,
    forceReevaluate: false,
  };
}

export interface SelectionResult {
  action: number;
  score: number;
  switched: boolean;
  committed: boolean;
  bucketsEvaluated: number;
  actionsScored: number;
  earlyOuts: number;
  considerationsEvaluated: number;
}

/** Round to float32, because the runtime scores in float32 and we must agree. */
const f32 = Math.fround;

export function scoreActionTraced(
  behavior: CompiledBehavior,
  actionIndex: number,
  inputs: readonly number[],
  bestSoFar: number,
): ScoreTrace {
  const action = behavior.actions[actionIndex]!;
  let score = f32(action.weight);
  const n = action.considerations.length;

  for (let i = 0; i < n; i++) {
    if (f32(compensate(score, n)) < bestSoFar) {
      return {
        score: 0,
        outcome: "early-out",
        stoppedAt: i,
        considerationsEvaluated: i,
      };
    }
    const c = action.considerations[i]!;
    const raw = inputs[c.inputIndex] ?? 0;
    const norm = f32(normalize(raw, c.inputMin, c.inputMax));
    score = f32(score * f32(evaluateCurve(c.curve, norm)));

    if (score <= 0) {
      return {
        score: 0,
        outcome: "vetoed",
        stoppedAt: i,
        considerationsEvaluated: i + 1,
      };
    }
  }

  return {
    score: f32(compensate(score, n)),
    outcome: "complete",
    stoppedAt: -1,
    considerationsEvaluated: n,
  };
}

export function scoreAction(
  behavior: CompiledBehavior,
  actionIndex: number,
  inputs: readonly number[],
  bestSoFar = 0,
): number {
  return scoreActionTraced(behavior, actionIndex, inputs, bestSoFar).score;
}

export interface SelectOptions {
  earlyOut?: boolean;
}

export function select(
  behavior: CompiledBehavior,
  inputs: readonly number[],
  state: SelectionState,
  dt: number,
  options: SelectOptions = {},
): SelectionResult {
  const earlyOut = options.earlyOut ?? true;
  const policy = behavior.policy;

  const result: SelectionResult = {
    action: state.currentAction,
    score: state.currentScore,
    switched: false,
    committed: false,
    bucketsEvaluated: 0,
    actionsScored: 0,
    earlyOuts: 0,
    considerationsEvaluated: 0,
  };

  state.timeInCurrentAction = f32(state.timeInCurrentAction + dt);

  const canSwitch =
    state.timeInCurrentAction >= policy.minCommitmentSeconds ||
    state.forceReevaluate ||
    state.currentAction < 0;

  if (!canSwitch) {
    result.committed = true;
    return result;
  }

  if (
    state.forceReevaluate &&
    state.currentAction >= 0 &&
    state.currentAction < behavior.actions.length &&
    behavior.actions[state.currentAction]!.interruptible === false &&
    state.timeInCurrentAction < policy.minCommitmentSeconds
  ) {
    result.committed = true;
    return result;
  }

  const bonus = policy.currentActionBonus;
  let best = 0;
  let bestIndex = -1;
  let incumbentScore = 0;
  let incumbentScored = false;

  for (let bucket = 0; bucket < behavior.actionsInBucket.length; bucket++) {
    result.bucketsEvaluated = bucket + 1;

    for (const i of behavior.actionsInBucket[bucket]!) {
      const isIncumbent = i === state.currentAction;

      let raw: number;
      if (earlyOut && !isIncumbent) {
        const trace = scoreActionTraced(behavior, i, inputs, best);
        raw = trace.score;
        result.considerationsEvaluated += trace.considerationsEvaluated;
        if (trace.outcome === "early-out") result.earlyOuts++;
      } else {
        // The incumbent is never early-out: its score is the bar the
        // challenger has to clear, and a pruned incumbent reports zero.
        // See the long comment in core/src/selection.cpp.
        raw = scoreAction(behavior, i, inputs, 0);
        result.considerationsEvaluated += behavior.actions[i]!.considerations.length;
      }
      result.actionsScored++;

      const score = isIncumbent ? f32(raw * bonus) : raw;
      if (isIncumbent) {
        incumbentScore = score;
        incumbentScored = true;
      }
      if (score > best) {
        best = score;
        bestIndex = i;
      }
    }

    if (best > policy.bucketSatisfiedThreshold) break;
  }

  const bar = incumbentScored ? incumbentScore : 0;
  const shouldSwitch =
    bestIndex !== state.currentAction && best > f32(bar + policy.switchThreshold);

  if (shouldSwitch) {
    state.currentAction = bestIndex;
    state.timeInCurrentAction = 0;
    state.currentScore = best;
    result.switched = true;
  } else {
    state.currentScore = bar;
  }

  state.forceReevaluate = false;
  result.action = state.currentAction;
  result.score = state.currentScore;
  return result;
}

export function scoreAll(
  behavior: CompiledBehavior,
  inputs: readonly number[],
): number[] {
  return behavior.actions.map((_, i) => scoreAction(behavior, i, inputs, 0));
}

// ------------------------------------------------------------- breakdown

export interface ConsiderationBreakdown {
  inputIndex: number;
  inputName: string;
  raw: number;
  normalized: number;
  curveOutput: number;
  runningProduct: number;
}

export interface ActionBreakdown {
  actionIndex: number;
  actionName: string;
  bucket: number;
  weight: number;
  rawProduct: number;
  compensation: number;
  finalScore: number;
  outcome: ScoreOutcome;
  stoppedAt: number;
  considerations: ConsiderationBreakdown[];
}

/**
 * Every level of the calculation (section 8.2).
 *
 * "This is the feature that justifies the tool for utility AI. A behavior tree
 * has structure you can point at. Utility AI has numbers, and 'why did it pick
 * that?' is genuinely hard to answer without tooling."
 *
 * No early-out, and it does not stop at the first veto: a designer needs to
 * see that AllyCount was 0.10 *and* that Health was fine, not just that
 * something failed.
 */
export function explain(
  behavior: CompiledBehavior,
  actionIndex: number,
  inputs: readonly number[],
): ActionBreakdown {
  const action = behavior.actions[actionIndex]!;
  let score = f32(action.weight);
  const n = action.considerations.length;
  const considerations: ConsiderationBreakdown[] = [];
  let outcome: ScoreOutcome = "complete";
  let stoppedAt = -1;

  action.considerations.forEach((c, i) => {
    const raw = inputs[c.inputIndex] ?? 0;
    const normalized = f32(normalize(raw, c.inputMin, c.inputMax));
    const curveOutput = f32(evaluateCurve(c.curve, normalized));
    score = f32(score * curveOutput);
    considerations.push({
      inputIndex: c.inputIndex,
      inputName: behavior.inputNames[c.inputIndex] ?? `#${c.inputIndex}`,
      raw,
      normalized,
      curveOutput,
      runningProduct: score,
    });
    if (curveOutput <= 0 && stoppedAt < 0) {
      outcome = "vetoed";
      stoppedAt = i;
    }
  });

  const finalScore = f32(compensate(score, n));
  return {
    actionIndex,
    actionName: action.name,
    bucket: action.bucket,
    weight: action.weight,
    rawProduct: score,
    compensation: score > 0 ? finalScore / score : 1,
    finalScore,
    outcome,
    stoppedAt,
    considerations,
  };
}
