/**
 * The TypeScript implementations against the C++ core.
 *
 * There are two scorers in this repository. The C++ one in core/ is what ships
 * inside the game. The TypeScript one in editor/src/runtime/ is what the
 * editor uses to answer questions with no game running -- replaying a recorded
 * session against an edited behavior (design.md section 9.4), the
 * always-wins/never-wins validation (10.4), and drawing what a curve change
 * will do (10.3).
 *
 * That duplication is justified only if the two agree. If the editor tells a
 * designer "your curve change makes the guard take cover 40% more often in
 * this recorded encounter" and the runtime disagrees, the tool is worse than
 * no tool: it is a confident wrong answer, and the design review it feeds is
 * built on it.
 *
 * So: compile here, load there, score both, compare. The C++ side is
 * normative.
 *
 * These tests need the core built. `make core` at the repository root does it,
 * and they skip with a clear message rather than failing if it is missing --
 * a contributor working only on the editor should not be blocked by a
 * toolchain they are not using.
 */

import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";

import { compile } from "../src/format/compile.js";
import { parse } from "../src/format/parse.js";
import { decodeBehavior } from "../src/runtime/compiled.js";
import { newSelectionState, scoreAll, select } from "../src/runtime/score.js";
import { evaluateCurve } from "../src/curves/evaluate.js";
import { CURVE_TYPES } from "../src/format/types.js";

const PROBE = new URL("../../core/build/aib_probe", import.meta.url).pathname;
const SAMPLES = new URL("../../samples", import.meta.url).pathname;

const haveProbe = existsSync(PROBE);
const describeIfBuilt = haveProbe ? describe : describe.skip;

if (!haveProbe) {
  // eslint-disable-next-line no-console
  console.warn(
    `\n  cross-check skipped: ${PROBE} not found.\n` +
      `  Build it with \`make core\` from the repository root.\n`,
  );
}

function probe(args: string[], stdin?: string): string {
  return execFileSync(PROBE, args, {
    input: stdin ?? "",
    encoding: "utf8",
    maxBuffer: 256 * 1024 * 1024,
  });
}

function guardBytes(): Uint8Array {
  const text = readFileSync(join(SAMPLES, "guard.behavior.yaml"), "utf8");
  return compile(parse(text)).bytes;
}

function writeTemp(bytes: Uint8Array): string {
  const dir = mkdtempSync(join(tmpdir(), "aib-"));
  const path = join(dir, "behavior.aib");
  writeFileSync(path, bytes);
  return path;
}

/** Deterministic inputs, so a failure reproduces from the seed alone. */
function* inputVectors(count: number, dims: number, seed = 1): Generator<number[]> {
  let state = seed >>> 0 || 1;
  const next = (): number => {
    state ^= state << 13;
    state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5;
    state >>>= 0;
    return state / 0x100000000;
  };
  // Scales chosen to cover each input's declared range and to overshoot it,
  // because out-of-range inputs are normal at runtime and the clamp has to
  // agree too.
  const scales = [3600, 1.2, 1.2, 36, 6];
  for (let i = 0; i < count; i++) {
    const v: number[] = [];
    for (let d = 0; d < dims; d++) {
      v.push(Math.fround(next() * (scales[d] ?? 1)));
    }
    yield v;
  }
}

describeIfBuilt("curve evaluation agrees with the C++ core", () => {
  it("matches across the whole parameter grid", () => {
    // 94,500 points: four curve families x nine slopes x five exponents x
    // five vertical shifts x five horizontal shifts x 21 samples.
    const tsv = probe(["curves"]).trim().split("\n");
    const header = tsv.shift()!;
    expect(header.split("\t")).toEqual(["type", "m", "k", "b", "c", "x", "y"]);
    expect(tsv.length).toBeGreaterThan(90000);

    let worst = 0;
    let worstRow = "";
    for (const line of tsv) {
      const [t, m, k, b, c, x, y] = line.split("\t").map(Number);
      const curve = {
        type: CURVE_TYPES[t!]!,
        m: m!,
        k: k!,
        b: b!,
        c: c!,
      };
      const ours = evaluateCurve(curve, x!);
      const diff = Math.abs(ours - y!);
      if (diff > worst) {
        worst = diff;
        worstRow = `${line}  -> ts ${ours}`;
      }
    }

    // Float32 in C++ against float64 in JavaScript, so exact equality is not
    // the right bar; 1e-6 is far tighter than any difference that could change
    // a decision, and loose enough not to be a precision-theatre failure.
    expect(worst, `worst disagreement:\n  ${worstRow}`).toBeLessThan(1e-6);
  });

  it("agrees that nothing in the grid is NaN or out of range", () => {
    // The C++ suite asserts this on its side. Asserting it here too means a
    // clamp that gets removed from one implementation cannot pass unnoticed
    // because the other still has it.
    const tsv = probe(["curves"]).trim().split("\n").slice(1);
    for (const line of tsv) {
      const y = Number(line.split("\t")[6]);
      expect(Number.isFinite(y)).toBe(true);
      expect(y).toBeGreaterThanOrEqual(0);
      expect(y).toBeLessThanOrEqual(1);
    }
  });
});

describeIfBuilt("the compiled format", () => {
  it("a file written by the TypeScript compiler loads in the C++ core", () => {
    // The thing that makes the whole split work: the cook step runs the
    // TypeScript compiler and the shipped game loads the result.
    const path = writeTemp(guardBytes());
    const info = probe(["info", path]);

    expect(info).toContain("name\tGuardBehavior");
    expect(info).toContain("action\tTakeCover\t0\t");
    expect(info).toContain("action\tPatrol\t3\t");
    expect(info).toContain("input\tDistanceToTarget");
    expect(info).toContain("task\tMoveToCover");
  });

  it("both sides compute the same content version", () => {
    // Recordings carry this so replay can tell "the behavior was edited" from
    // "the core is non-deterministic" (section 9.2). If the two hashes
    // disagreed, every recording would look like a version mismatch.
    const result = compile(parse(readFileSync(join(SAMPLES, "guard.behavior.yaml"), "utf8")));
    const path = writeTemp(result.bytes);
    const info = probe(["info", path]);
    const version = Number(
      info.split("\n").find((l) => l.startsWith("version\t"))!.split("\t")[1],
    );
    expect(version).toBe(result.version);
  });

  it("the TypeScript decoder agrees with the C++ decoder", () => {
    const bytes = guardBytes();
    const path = writeTemp(bytes);
    const ours = decodeBehavior(bytes);
    const info = probe(["info", path]);

    expect(ours.inputNames).toEqual(
      info
        .split("\n")
        .filter((l) => l.startsWith("input\t"))
        .map((l) => l.split("\t")[1]),
    );
    expect(ours.taskNames).toEqual(
      info
        .split("\n")
        .filter((l) => l.startsWith("task\t"))
        .map((l) => l.split("\t")[1]),
    );

    const actionLines = info.split("\n").filter((l) => l.startsWith("action\t"));
    expect(ours.actions).toHaveLength(actionLines.length);
    actionLines.forEach((line, i) => {
      const [, name, bucket, weight, interruptible, cons] = line.split("\t");
      const a = ours.actions[i]!;
      expect(a.name).toBe(name);
      expect(a.bucket).toBe(Number(bucket));
      expect(a.weight).toBeCloseTo(Number(weight), 6);
      expect(a.interruptible).toBe(interruptible === "1");
      expect(a.considerations).toHaveLength(Number(cons));
    });
  });
});

describeIfBuilt("scoring agrees with the C++ core", () => {
  it("every action score matches across 2000 input vectors", () => {
    const bytes = guardBytes();
    const path = writeTemp(bytes);
    const behavior = decodeBehavior(bytes);

    const vectors = [...inputVectors(2000, behavior.inputNames.length, 0x5eed)];
    const stdin = vectors.map((v) => v.join("\t")).join("\n") + "\n";
    const lines = probe(["score", path], stdin).trim().split("\n");

    expect(lines).toHaveLength(vectors.length);

    let worst = 0;
    let worstDetail = "";
    lines.forEach((line, i) => {
      const theirs = line.split("\t").map(Number);
      const ours = scoreAll(behavior, vectors[i]!);
      expect(ours).toHaveLength(theirs.length);
      for (let a = 0; a < theirs.length; a++) {
        const diff = Math.abs(ours[a]! - theirs[a]!);
        if (diff > worst) {
          worst = diff;
          worstDetail =
            `vector ${i} action ${behavior.actions[a]!.name}: ` +
            `cpp ${theirs[a]}, ts ${ours[a]}, inputs [${vectors[i]!.join(", ")}]`;
        }
      }
    });

    expect(worst, worstDetail).toBeLessThan(1e-6);
  });

  it("selection agrees frame for frame over a long session", () => {
    // Scores agreeing is necessary but not sufficient. Selection carries state
    // -- commitment timers, the incumbent bonus, the switch threshold -- and a
    // disagreement there compounds: once the two pick different actions they
    // have different incumbents and diverge for good. Running 1500 frames with
    // the state carried through is the test that would catch it.
    const bytes = guardBytes();
    const path = writeTemp(bytes);
    const behavior = decodeBehavior(bytes);

    const vectors = [...inputVectors(1500, behavior.inputNames.length, 0xc0ffee)];
    const stdin = vectors.map((v) => v.join("\t")).join("\n") + "\n";
    // 0.2 s per step: 5 Hz, the policy's own evaluation rate, so the
    // commitment window is genuinely exercised rather than always satisfied.
    const lines = probe(["select", path, "0.2"], stdin).trim().split("\n");

    const state = newSelectionState();
    lines.forEach((line, i) => {
      const [action, score, switched, committed, buckets, considerations] =
        line.split("\t");
      const ours = select(behavior, vectors[i]!, state, 0.2);

      expect(
        ours.action,
        `frame ${i}: cpp chose ${action}, ts chose ${ours.action}`,
      ).toBe(Number(action));
      expect(ours.score).toBeCloseTo(Number(score), 6);
      expect(ours.switched).toBe(switched === "1");
      expect(ours.committed).toBe(committed === "1");
      expect(ours.bucketsEvaluated).toBe(Number(buckets));
      // Not just the same answer -- the same work. A disagreement here means
      // early-out or the bucket break is pruning differently, which would be
      // a real difference that happened not to change the winner yet.
      expect(
        ours.considerationsEvaluated,
        `frame ${i}: work done differs`,
      ).toBe(Number(considerations));
    });
  });

  it("the session actually changes its mind, or this proves nothing", () => {
    const bytes = guardBytes();
    const behavior = decodeBehavior(bytes);
    const state = newSelectionState();
    const chosen = new Set<number>();
    let switches = 0;

    for (const v of inputVectors(1500, behavior.inputNames.length, 0xc0ffee)) {
      const r = select(behavior, v, state, 0.2);
      chosen.add(r.action);
      if (r.switched) switches++;
    }

    expect(chosen.size).toBeGreaterThan(1);
    expect(switches).toBeGreaterThan(10);
  });
});
