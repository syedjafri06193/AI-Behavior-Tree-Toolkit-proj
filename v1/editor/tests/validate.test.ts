/**
 * Edit-time validation (design.md section 10.4).
 *
 * Every rule here exists because a behavior can violate it silently. A test
 * suite that only ever sees a valid behavior proves nothing about a validator,
 * so each rule gets a behavior that breaks it in the way a designer actually
 * would -- and the clean sample has to stay clean, which is the other half.
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";

import { compile, CompileError } from "../src/format/compile.js";
import { parse } from "../src/format/parse.js";
import { validate, type Diagnostic } from "../src/validate/rules.js";
import type { Behavior } from "../src/format/types.js";

const SAMPLES = new URL("../../samples", import.meta.url).pathname;

function guard(): Behavior {
  return parse(readFileSync(join(SAMPLES, "guard.behavior.yaml"), "utf8"));
}

function rules(diagnostics: readonly Diagnostic[]): string[] {
  return diagnostics.map((d) => d.rule);
}

describe("the shipped samples are clean", () => {
  it("guard.behavior.yaml has no errors", () => {
    const diagnostics = validate(guard());
    const errors = diagnostics.filter((d) => d.severity === "error");
    expect(
      errors,
      `unexpected errors:\n${errors.map((e) => `${e.path}: ${e.message}`).join("\n")}`,
    ).toHaveLength(0);
  });

  it("and compiles", () => {
    expect(() => compile(guard())).not.toThrow();
  });
});

describe("errors stop the build", () => {
  it("an unregistered task type", () => {
    // Section 5.4: "a dangling task reference should fail the build, not fail
    // at runtime in front of a player."
    const behavior = guard();
    behavior.actions[0]!.task.type = "MoveToCovr";
    expect(rules(validate(behavior))).toContain("unknown-task");
    expect(() => compile(behavior)).toThrow(CompileError);
  });

  it("a zero-width consideration range", () => {
    const behavior = guard();
    behavior.actions[0]!.considerations[0]!.range = [0.5, 0.5];
    expect(rules(validate(behavior))).toContain("zero-width-range");
    expect(() => compile(behavior)).toThrow(CompileError);
  });

  it("a curve that outputs a constant", () => {
    // m = 0 is legal arithmetic and always an authoring mistake: the
    // consideration contributes a constant factor and can never discriminate.
    const behavior = guard();
    behavior.actions[0]!.considerations[0]!.curve = {
      type: "linear",
      m: 0,
      k: 1,
      b: 0.5,
      c: 0,
    };
    expect(rules(validate(behavior))).toContain("curve-constant");
    expect(() => compile(behavior)).toThrow(CompileError);
  });

  it("the compile error names every problem, not just the first", () => {
    // A validator that reports one error per run turns a five-mistake file
    // into five build cycles.
    const behavior = guard();
    behavior.actions[0]!.task.type = "Nope";
    behavior.actions[1]!.task.type = "AlsoNope";
    try {
      compile(behavior);
      expect.unreachable();
    } catch (e) {
      const error = e as CompileError;
      expect(error.message).toContain("Nope");
      expect(error.message).toContain("AlsoNope");
      expect(error.message).toContain("2 errors");
    }
  });
});

describe("warnings that catch authoring mistakes", () => {
  it("an action that can never score above zero", () => {
    // Section 10.4. "Cheap to compute by sampling the input space and
    // surprisingly effective at finding authoring mistakes."
    // A zeroed weight: the way a designer disables an action while testing
    // something else, and then forgets. The action stays in the file, reads
    // as active, and can never fire.
    const behavior = guard();
    const attack = behavior.actions.find((a) => a.name === "Attack")!;
    attack.weight = 0;

    const found = validate(behavior).filter((d) => d.rule === "never-wins");
    expect(found.map((d) => d.path)).toContain("actions.Attack");

    // And only Attack. Reload shares the AmmoRatio input, and an earlier
    // version of the probe sampled each input over whichever range it saw
    // first -- so an unusual range on one action dragged its bucket-mates out
    // of their own ranges and reported them as never winning too. The probe
    // now covers the union of every declared range.
    expect(found.map((d) => d.path)).not.toContain("actions.Reload");
  });

  it("cannot see a range the game will never produce, and says so", () => {
    // An honest limit. The probe samples the space the BEHAVIOR declares, so
    // a consideration whose range sits entirely outside what the game actually
    // feeds it looks perfectly reasonable here.
    //
    // That gap is what the recording closes: replaying a real session against
    // the behavior (section 9.4) is the only thing that knows what the inputs
    // really do. Worth pinning as a known boundary rather than discovering it
    // when somebody trusts a clean validation run.
    const behavior = guard();
    const attack = behavior.actions.find((a) => a.name === "Attack")!;
    attack.considerations[0]!.range = [900, 1000]; // AmmoRatio is 0..1

    const found = validate(behavior).filter((d) => d.rule === "never-wins");
    expect(found.map((d) => d.path)).not.toContain("actions.Attack");
  });

  it("an action that always wins, making its bucket-mates dead code", () => {
    const behavior = guard();
    const attack = behavior.actions.find((a) => a.name === "Attack")!;
    // Weight it so nothing in bucket 1 can ever beat it.
    attack.weight = 1;
    attack.considerations = [];
    const reload = behavior.actions.find((a) => a.name === "Reload")!;
    reload.weight = 0.01;

    const diagnostics = validate(behavior);
    const always = diagnostics.filter((d) => d.rule === "always-wins");
    expect(always.length).toBeGreaterThan(0);
    expect(always[0]!.message).toContain("Reload");
  });

  it("a gap in the bucket numbering", () => {
    const behavior = guard();
    for (const action of behavior.actions) {
      if (action.bucket === 1) action.bucket = 2;
    }
    expect(rules(validate(behavior))).toContain("empty-bucket");
  });

  it("an action with no considerations at all", () => {
    const behavior = guard();
    behavior.actions[0]!.considerations = [];
    expect(rules(validate(behavior))).toContain("no-considerations");
  });
});

describe("hints", () => {
  it("flags an expensive consideration ordered first", () => {
    // Section 3.4: "Order considerations cheapest-and-most-discriminating
    // first... Getting this order right is often a larger performance win than
    // any micro-optimization."
    //
    // DistanceToTarget has transform: distance, which is cheap. Give it a
    // navmesh transform and it becomes the expensive one.
    const behavior = guard();
    const distance = behavior.inputs.find((i) => i.name === "DistanceToTarget")!;
    distance.transform = "pathlength";

    const attack = behavior.actions.find((a) => a.name === "Attack")!;
    // Attack currently has AmmoRatio first (cheap) then DistanceToTarget.
    // Reverse it, which is the mistake.
    attack.considerations.reverse();

    const hints = validate(behavior).filter((d) => d.rule === "expensive-first");
    expect(hints.length).toBeGreaterThan(0);
    expect(hints[0]!.message).toContain("Early-out");
  });

  it("does not flag the correct order", () => {
    const behavior = guard();
    const distance = behavior.inputs.find((i) => i.name === "DistanceToTarget")!;
    distance.transform = "pathlength";
    // Attack already has the cheap AmmoRatio first.
    const hints = validate(behavior).filter(
      (d) => d.rule === "expensive-first" && d.path.includes("Attack"),
    );
    expect(hints).toHaveLength(0);
  });

  it("a hint never blocks a build", () => {
    const behavior = guard();
    const distance = behavior.inputs.find((i) => i.name === "DistanceToTarget")!;
    distance.transform = "pathlength";
    behavior.actions.find((a) => a.name === "Attack")!.considerations.reverse();
    expect(() => compile(behavior)).not.toThrow();
  });
});

describe("the validator itself", () => {
  it("produces the same diagnostics every time", () => {
    // A validator that flags an action intermittently is one people learn to
    // ignore, which is why the input-space probe is a deterministic lattice
    // rather than random sampling.
    const behavior = guard();
    behavior.actions[0]!.task.type = "Nope";
    const first = JSON.stringify(validate(behavior));
    for (let i = 0; i < 5; i++) {
      expect(JSON.stringify(validate(behavior))).toBe(first);
    }
  });

  it("sorts errors before warnings before hints", () => {
    const behavior = guard();
    behavior.actions[0]!.task.type = "Nope";
    behavior.actions[1]!.considerations = [];
    const order = validate(behavior).map((d) => d.severity);
    const rank = { error: 0, warning: 1, hint: 2 } as const;
    for (let i = 1; i < order.length; i++) {
      expect(rank[order[i]!]).toBeGreaterThanOrEqual(rank[order[i - 1]!]);
    }
  });

  it("accepts an inverted range, which is how 'closer is better' is written", () => {
    // The document's table lists an inverted range as an error. It is not one:
    // the core supports it and it is the natural authoring for a falling
    // response. See 1—Documentation/v1/notes-on-the-spec.md.
    const behavior = guard();
    behavior.actions[0]!.considerations[1]!.range = [3000, 0];
    const errors = validate(behavior).filter((d) => d.severity === "error");
    expect(errors).toHaveLength(0);
    expect(() => compile(behavior)).not.toThrow();
  });

  it("a custom task registry replaces the default", () => {
    // A real project registers its own tasks from the Unreal side.
    const behavior = guard();
    expect(
      validate(behavior, { taskRegistry: ["MoveToCover"] }).filter(
        (d) => d.rule === "unknown-task",
      ).length,
    ).toBeGreaterThan(0);
  });
});
