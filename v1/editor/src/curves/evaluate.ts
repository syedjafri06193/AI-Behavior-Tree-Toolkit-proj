/**
 * Response curve evaluation (design.md section 3.2).
 *
 * This is a second implementation of `EvaluateCurve` in
 * core/src/curve.cpp, and it exists because the editor has to draw the curve
 * the designer is tuning and show the live input marker on it (section 10.3)
 * without a round trip to the game.
 *
 * Two implementations of the same function is a liability unless they are
 * checked against each other, so they are: tests/crosscheck.test.ts runs both
 * over a dense parameter grid and requires agreement to float precision. If
 * the editor draws a curve the runtime does not evaluate, every tuning
 * decision made in the editor is made against a lie -- and that is a subtle,
 * expensive kind of wrong.
 *
 * The C++ side is normative. Where they could differ, this matches C++.
 */

import type { Curve } from "../format/types.js";

/**
 * Clamp to [0,1], mapping NaN to 0.
 *
 * Written as `!(v > 0)` rather than `v < 0` for the same reason as the C++:
 * NaN compares false against everything, so `v < 0` lets it through and
 * `!(v > 0)` catches it. An unclamped NaN propagates through the score
 * multiply and produces an action that never wins, with no error anywhere.
 */
export function clamp01(v: number): number {
  if (!(v > 0)) return 0;
  return v > 1 ? 1 : v;
}

/**
 * Math.pow with a negative base and a non-integral exponent is NaN in both
 * languages. Designers do write c > 0 with k = 0.5, which makes (x - c)
 * negative for small x, so this is reached by ordinary authoring.
 *
 * Mirrors the positive branch and carries the sign, which is continuous
 * through zero where returning 0 would not be.
 */
function safePow(base: number, exponent: number): number {
  if (base < 0) {
    const rounded = Math.round(exponent);
    if (Math.abs(exponent - rounded) > 1e-6) {
      return -Math.pow(-base, exponent);
    }
  }
  return Math.pow(base, exponent);
}

export function evaluateCurve(curve: Curve, x: number): number {
  x = clamp01(x);
  let y = 0;

  switch (curve.type) {
    case "linear":
    case "polynomial":
      y = curve.m * safePow(x - curve.c, curve.k) + curve.b;
      break;

    case "logistic": {
      // Clamp the exponent before exp(), not the result after: a large |m|
      // with x far from centre overflows to Infinity, and Infinity/Infinity is
      // NaN. +-80 saturates the sigmoid to within a float epsilon, so this
      // changes no answer anyone can observe.
      let exponent = -10 * curve.m * (x - 0.5 - curve.c);
      if (!(exponent > -80)) exponent = -80;
      if (exponent > 80) exponent = 80;
      y = curve.k / (1 + Math.exp(exponent)) + curve.b;
      break;
    }

    case "logit": {
      // Diverges at both ends of its domain -- the case section 3.2 singles
      // out. Shifting the argument off the boundaries keeps it finite, and the
      // value there is already saturated so the clamp costs nothing real.
      // The denominator is carried alongside rather than recomputed as
      // (1 - t), to match the C++ exactly. There, `1.0f - 0.9999f` in float32
      // is catastrophic cancellation and returns 1.00017e-4 for a true 1e-4 --
      // an error the logarithm then amplifies. Both sides carry the exact
      // epsilon so the two agree to float32 precision.
      //
      // Math.fround on the epsilon and its complement so the constants are
      // the same numbers the float32 side uses, not their float64 neighbours.
      const eps = Math.fround(1e-4);
      const oneMinusEps = Math.fround(1 - eps);
      let t = x - curve.c;
      let u: number;
      if (t <= eps) {
        t = eps;
        u = oneMinusEps;
      } else if (t >= oneMinusEps) {
        t = oneMinusEps;
        u = eps;
      } else {
        u = 1 - t;
      }
      y = (curve.m * Math.log(t / u)) / 5 + 0.5 + curve.b;
      break;
    }

    default:
      y = 0;
      break;
  }

  return clamp01(y);
}

export function normalize(raw: number, min: number, max: number): number {
  const span = max - min;
  if (span === 0 || Number.isNaN(span)) return 0;
  return clamp01((raw - min) / span);
}

/**
 * Compensation (section 3.3). Mirrors aib::Compensate.
 */
export function compensate(score: number, n: number): number {
  if (n <= 1) return score;
  const modFactor = 1 - 1 / n;
  const makeUp = (1 - score) * modFactor;
  return score + makeUp * score;
}

/**
 * Sample a curve for plotting. Section 10.3 calls the curve editor "the main
 * design surface", on the grounds that designers spend far more time tuning
 * curves than restructuring trees.
 */
export function sampleCurve(curve: Curve, samples = 128): Float32Array {
  const out = new Float32Array(samples);
  for (let i = 0; i < samples; i++) {
    out[i] = evaluateCurve(curve, i / (samples - 1));
  }
  return out;
}

/**
 * Does this curve discriminate at all?
 *
 * A consideration whose output never changes contributes a constant factor and
 * can never influence a decision. It is always an authoring mistake, and
 * section 10.4 wants it flagged -- it is the "curve parameters produce NaN or
 * constant output" row.
 */
export function curveRange(curve: Curve, samples = 128): {
  min: number;
  max: number;
  constant: boolean;
} {
  let min = Infinity;
  let max = -Infinity;
  for (let i = 0; i < samples; i++) {
    const y = evaluateCurve(curve, i / (samples - 1));
    if (y < min) min = y;
    if (y > max) max = y;
  }
  return { min, max, constant: max - min < 1e-6 };
}
