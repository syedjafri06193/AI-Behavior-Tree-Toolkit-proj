/**
 * The authoring format (design.md sections 5.1-5.3, 16.2).
 *
 * Section 16.2's whole test is round-trip byte equality, and it is there
 * because ordering instability is what produces spurious diffs on every save
 * (5.2). If a designer opening a file and saving it without changing anything
 * produces a diff, code review of AI changes stops being possible.
 */

import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";

import { autoLayout, parse, parseLayout, FormatError } from "../src/format/parse.js";
import { serialize, serializeLayout, sortActions } from "../src/format/serialize.js";
import type { Behavior } from "../src/format/types.js";

const SAMPLES = new URL("../../samples", import.meta.url).pathname;

function sampleFiles(): string[] {
  return readdirSync(SAMPLES)
    .filter((f) => f.endsWith(".behavior.yaml"))
    .map((f) => join(SAMPLES, f));
}

function guard(): Behavior {
  return parse(readFileSync(join(SAMPLES, "guard.behavior.yaml"), "utf8"));
}

describe("round-trip", () => {
  it("finds sample files to test", () => {
    expect(sampleFiles().length).toBeGreaterThan(0);
  });

  it.each(sampleFiles())("round-trips byte-identically: %s", (path) => {
    // Section 16.2, verbatim. The samples are in canonical form because the
    // emitter owns the formatting -- parse leniently, emit canonically, the
    // way gofmt does.
    const original = readFileSync(path, "utf8");
    expect(serialize(parse(original))).toBe(original);
  });

  it("is idempotent from a non-canonical starting point", () => {
    // The stronger property: whatever a human types, one save reaches a fixed
    // point and further saves change nothing.
    const messy = `
type:   utility
name: Messy
version: 1
inputs:
  - {name: A, source: attribute, key: KeyA}
actions:
  - task: {type: Idle}
    name: Second
    bucket: 1
    considerations:
      - input: A
        curve: {type: linear, m: 1, k: 1, b: 0, c: 0}
        range: [0, 1]
  - name: First
    bucket: 0
    task: {type: Patrol}
    considerations: []
policy: {}
`;
    const once = serialize(parse(messy));
    const twice = serialize(parse(once));
    expect(twice).toBe(once);
  });

  it("sorts actions by bucket then name", () => {
    const behavior = guard();
    const sorted = sortActions(behavior.actions);
    const keys = sorted.map((a) => `${a.bucket}:${a.name}`);
    expect(keys).toEqual([...keys].sort());
  });

  it("does not reorder considerations", () => {
    // Their order is semantically meaningful: early-out evaluates them front
    // to back, so sorting them would be a silent performance regression with
    // identical behavior and different cost (section 3.4).
    const behavior = guard();
    const attack = behavior.actions.find((a) => a.name === "Attack")!;
    expect(attack.considerations.map((c) => c.input)).toEqual([
      "AmmoRatio",
      "DistanceToTarget",
    ]);

    const reserialized = parse(serialize(behavior));
    const attackAgain = reserialized.actions.find((a) => a.name === "Attack")!;
    expect(attackAgain.considerations.map((c) => c.input)).toEqual([
      "AmmoRatio",
      "DistanceToTarget",
    ]);
  });

  it("does not reorder inputs", () => {
    // Input order becomes the index table, and those indices appear in every
    // recording. Re-sorting for cosmetic reasons would invalidate recorded
    // sessions.
    const behavior = guard();
    const before = behavior.inputs.map((i) => i.name);
    const after = parse(serialize(behavior)).inputs.map((i) => i.name);
    expect(after).toEqual(before);
  });

  it("omits fields at their default so a diff shows only decisions", () => {
    const behavior = guard();
    const text = serialize(behavior);

    // Attack has weight 1 and is interruptible, so neither should appear.
    const attackBlock = text.split("- name: Attack")[1]!.split("- name:")[0]!;
    expect(attackBlock).not.toContain("weight:");
    expect(attackBlock).not.toContain("interruptible:");

    // Reload sets both, so both should.
    const reloadBlock = text.split("- name: Reload")[1]!.split("- name:")[0]!;
    expect(reloadBlock).toContain("weight: 0.9");
    expect(reloadBlock).toContain("interruptible: false");
  });

  it("writes curve parameters as floats", () => {
    // `m: -1.0` rather than `m: -1`. Purely a readability decision about how
    // the diff reads, which is the point of the format.
    const text = serialize(guard());
    expect(text).toContain("m: -1.0, k: 2.0, b: 1.0, c: 0.0");
  });
});

describe("a real change produces a small diff", () => {
  it("changing one curve exponent changes exactly one line", () => {
    // This is the claim section 5.1 makes: "Changing a curve exponent is a
    // one-line change a reviewer can read and reason about." It is the entire
    // argument for the text format, so it is worth asserting rather than
    // assuming.
    const before = serialize(guard());

    const edited = guard();
    const cover = edited.actions.find((a) => a.name === "TakeCover")!;
    cover.considerations[1]!.curve.k = 3;
    const after = serialize(edited);

    const beforeLines = before.split("\n");
    const afterLines = after.split("\n");
    expect(afterLines.length).toBe(beforeLines.length);

    const changed = beforeLines.filter((line, i) => line !== afterLines[i]);
    expect(changed).toHaveLength(1);
    expect(changed[0]).toContain("k: 2.0");
  });

  it("renaming an action moves it without touching anything else", () => {
    const edited = guard();
    const patrol = edited.actions.find((a) => a.name === "Patrol")!;
    patrol.name = "Wander";
    const after = serialize(edited);

    expect(after).toContain("- name: Wander");
    expect(after).not.toContain("- name: Patrol");
    // Still parses, still round-trips.
    expect(serialize(parse(after))).toBe(after);
  });
});

describe("layout sidecar", () => {
  it("auto-layout makes the sidecar genuinely optional", () => {
    // Section 5.3: a team that gitignores layout entirely and relies on
    // auto-layout gets conflict-free AI authoring. That only works if the
    // editor can lay out a behavior it has never seen a layout for.
    const behavior = guard();
    const layout = autoLayout(behavior);

    for (const action of behavior.actions) {
      expect(layout.nodes[action.name]).toBeDefined();
    }
    // Deterministic, so two people auto-laying out the same file agree.
    expect(serializeLayout(autoLayout(behavior))).toBe(serializeLayout(layout));
  });

  it("groups by bucket so the priority tiers are visible", () => {
    const behavior = guard();
    const layout = autoLayout(behavior);
    const cover = layout.nodes.TakeCover!;
    const attack = layout.nodes.Attack!;
    const patrol = layout.nodes.Patrol!;
    expect(cover.x).toBeLessThan(attack.x);
    expect(attack.x).toBeLessThan(patrol.x);
  });

  it("layout round-trips and sorts its keys", () => {
    const layout = autoLayout(guard());
    const text = serializeLayout(layout);
    expect(serializeLayout(parseLayout(text))).toBe(text);

    const keys = Object.keys(JSON.parse(text).nodes);
    expect(keys).toEqual([...keys].sort());
  });

  it("no layout data leaks into the logic file", () => {
    // If it did, moving a node would produce a diff and the signal-to-noise
    // ratio of AI code review would collapse (section 5.3).
    //
    // Matching whole keys rather than substrings: an earlier version of this
    // test looked for "y:" and matched the "y:" inside "key:", which is the
    // kind of false positive that teaches people to delete a test.
    const text = serialize(guard());
    const layoutKeys = ["x", "y", "position", "color", "collapsed", "comments"];
    for (const key of layoutKeys) {
      const asKey = new RegExp(`(^|[\\s{,])${key}\\s*:`, "m");
      expect(asKey.test(text), `"${key}:" must not appear as a key`).toBe(false);
    }
  });
});

describe("parsing rejects what would run and be wrong", () => {
  const bad: [string, string][] = [
    [
      "unknown curve type",
      `version: 1
name: X
inputs: [{name: A, source: attribute, key: K}]
actions:
  - name: Act
    bucket: 0
    considerations: [{input: A, range: [0,1], curve: {type: sigmoid, m: 1, k: 1, b: 0, c: 0}}]
    task: {type: Idle}`,
    ],
    [
      "consideration referencing a missing input",
      `version: 1
name: X
inputs: [{name: A, source: attribute, key: K}]
actions:
  - name: Act
    bucket: 0
    considerations: [{input: Nope, range: [0,1], curve: {type: linear, m: 1, k: 1, b: 0, c: 0}}]
    task: {type: Idle}`,
    ],
    [
      "duplicate action names",
      `version: 1
name: X
inputs: []
actions:
  - {name: Act, bucket: 0, considerations: [], task: {type: Idle}}
  - {name: Act, bucket: 1, considerations: [], task: {type: Idle}}`,
    ],
    [
      "duplicate input names",
      `version: 1
name: X
inputs:
  - {name: A, source: attribute, key: K1}
  - {name: A, source: attribute, key: K2}
actions: []`,
    ],
    [
      "a future format version",
      `version: 99
name: X
inputs: []
actions: []`,
    ],
    [
      "a non-finite curve parameter",
      `version: 1
name: X
inputs: [{name: A, source: attribute, key: K}]
actions:
  - name: Act
    bucket: 0
    considerations: [{input: A, range: [0,1], curve: {type: linear, m: .nan, k: 1, b: 0, c: 0}}]
    task: {type: Idle}`,
    ],
    [
      "a fractional bucket",
      `version: 1
name: X
inputs: []
actions:
  - {name: Act, bucket: 1.5, considerations: [], task: {type: Idle}}`,
    ],
  ];

  it.each(bad)("rejects %s", (_label, text) => {
    expect(() => parse(text)).toThrow(FormatError);
  });

  it("names the path in the error", () => {
    // A parse error that says "invalid" and nothing else is one a designer
    // cannot act on.
    try {
      parse(`version: 1
name: X
inputs: [{name: A, source: attribute, key: K}]
actions:
  - name: Act
    bucket: 0
    considerations: [{input: A, range: [0,1], curve: {type: nope, m: 1, k: 1, b: 0, c: 0}}]
    task: {type: Idle}`);
      expect.unreachable();
    } catch (e) {
      expect((e as FormatError).message).toContain("considerations[0].curve.type");
      expect((e as FormatError).message).toContain("nope");
    }
  });

  it("accepts a file that omits every optional field", () => {
    const minimal = parse(`version: 1
name: Minimal
inputs: []
actions: []`);
    expect(minimal.policy.evaluationHz).toBe(5);
    expect(minimal.policy.currentActionBonus).toBe(1.1);
    expect(serialize(parse(serialize(minimal)))).toBe(serialize(minimal));
  });
});
