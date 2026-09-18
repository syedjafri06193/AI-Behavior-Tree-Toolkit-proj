#!/usr/bin/env node
/**
 * aibc -- the behavior compiler (design.md section 5.4).
 *
 * "Compile during the Unreal cook step so shipped builds never see YAML. The
 * compiler is also where validation happens -- a malformed curve or a dangling
 * task reference should fail the build, not fail at runtime in front of a
 * player."
 *
 * So it exits non-zero on an error, which is what makes it usable as a cook
 * step rather than a suggestion.
 *
 *   aibc compile <in.behavior.yaml> [-o out.aib]
 *   aibc check   <in.behavior.yaml>...
 *   aibc fmt     <in.behavior.yaml>...      canonical formatting, in place
 *   aibc explain <in.behavior.yaml> <input=value>...
 *   aibc diff    <before.yaml> <after.yaml> <session.arec>
 */

import { readFileSync, writeFileSync } from "node:fs";
import { basename } from "node:path";

import { compile, CompileError } from "../format/compile.js";
import { parse, FormatError } from "../format/parse.js";
import { serialize } from "../format/serialize.js";
import { formatDiagnostics, validate } from "../validate/rules.js";
import { decodeBehavior } from "../runtime/compiled.js";
import { explain } from "../runtime/score.js";
import { decodeRecording, diffBehaviors, summarizeDiff } from "../replay/replay.js";
import { bucketName } from "../format/types.js";

function fail(message: string): never {
  process.stderr.write(`aibc: ${message}\n`);
  process.exit(1);
}

function read(path: string): ReturnType<typeof parse> {
  try {
    return parse(readFileSync(path, "utf8"));
  } catch (e) {
    if (e instanceof FormatError) fail(`${path}: ${e.message}`);
    throw e;
  }
}

function cmdCompile(args: string[]): void {
  const input = args[0];
  if (!input) fail("compile needs an input file");

  const outIndex = args.indexOf("-o");
  const output =
    outIndex >= 0 && args[outIndex + 1]
      ? args[outIndex + 1]!
      : input.replace(/\.behavior\.yaml$/, "") + ".aib";

  const behavior = read(input);
  try {
    const result = compile(behavior);
    writeFileSync(output, result.bytes);

    const notes = result.diagnostics.filter((d) => d.severity !== "error");
    if (notes.length > 0) {
      process.stderr.write(formatDiagnostics(notes) + "\n");
    }
    process.stdout.write(
      `${output}  ${result.bytes.length} bytes  ` +
        `version ${result.version.toString(16).padStart(8, "0")}  ` +
        `${result.actionNames.length} actions, ` +
        `${result.inputNames.length} inputs\n`,
    );
  } catch (e) {
    if (e instanceof CompileError) fail(e.message);
    throw e;
  }
}

function cmdCheck(args: string[]): void {
  if (args.length === 0) fail("check needs at least one file");
  let errors = 0;

  for (const path of args) {
    const diagnostics = validate(read(path));
    const fileErrors = diagnostics.filter((d) => d.severity === "error").length;
    errors += fileErrors;

    if (diagnostics.length === 0) {
      process.stdout.write(`${basename(path)}: ok\n`);
    } else {
      process.stdout.write(`${basename(path)}:\n${formatDiagnostics(diagnostics)}\n`);
    }
  }
  if (errors > 0) process.exit(1);
}

function cmdFmt(args: string[]): void {
  // Canonical formatting in place. The emitter owns the layout, so this is
  // what keeps a file from drifting and producing a diff on the next save
  // (section 5.2).
  let changed = 0;
  for (const path of args) {
    const original = readFileSync(path, "utf8");
    const formatted = serialize(read(path));
    if (formatted !== original) {
      writeFileSync(path, formatted);
      process.stdout.write(`${path}: reformatted\n`);
      changed++;
    }
  }
  if (changed === 0) process.stdout.write("all files already canonical\n");
}

function cmdExplain(args: string[]): void {
  // Section 8.2's breakdown, on the command line. Useful in a code review
  // where the question is "would this actually fire?" and nobody wants to
  // launch the editor to find out.
  const path = args[0];
  if (!path) fail("explain needs a behavior file");

  const behavior = decodeBehavior(compile(read(path)).bytes);
  const inputs = new Array<number>(behavior.inputNames.length).fill(0);

  for (const assignment of args.slice(1)) {
    const [name, value] = assignment.split("=");
    const index = behavior.inputNames.indexOf(name ?? "");
    if (index < 0) {
      fail(
        `unknown input "${name}"; this behavior has ` +
          behavior.inputNames.join(", "),
      );
    }
    inputs[index] = Number(value);
  }

  process.stdout.write(`${behavior.name}  inputs:\n`);
  behavior.inputNames.forEach((name, i) => {
    process.stdout.write(`  ${name.padEnd(22)} ${inputs[i]}\n`);
  });
  process.stdout.write("\n");

  behavior.actionsInBucket.forEach((indices, bucket) => {
    if (indices.length === 0) return;
    process.stdout.write(`Bucket ${bucket} (${bucketName(bucket)})\n`);

    const breakdowns = indices
      .map((i) => explain(behavior, i, inputs))
      .sort((a, b) => b.finalScore - a.finalScore);

    for (const b of breakdowns) {
      process.stdout.write(
        `  ${b.actionName.padEnd(20)} ${b.finalScore.toFixed(3)}\n`,
      );
      for (const c of b.considerations) {
        const low = c.curveOutput < 0.15 ? "  <- low" : "";
        process.stdout.write(
          `      ${c.inputName.padEnd(20)} ${c.raw.toFixed(2).padStart(8)}` +
            ` -> ${c.normalized.toFixed(3)} -> ${c.curveOutput.toFixed(3)}${low}\n`,
        );
      }
      if (b.considerations.length > 1) {
        process.stdout.write(
          `      compensation x${b.compensation.toFixed(3)} ` +
            `(raw ${b.rawProduct.toFixed(3)})\n`,
        );
      }
    }
    process.stdout.write("\n");
  });
}

function cmdDiff(args: string[]): void {
  // Section 9.4 on the command line, so it can run in CI on a tuning PR.
  const [beforePath, afterPath, recordingPath] = args;
  if (!beforePath || !afterPath || !recordingPath) {
    fail("diff needs <before.yaml> <after.yaml> <session.arec>");
  }

  const before = decodeBehavior(compile(read(beforePath)).bytes);
  const after = decodeBehavior(compile(read(afterPath)).bytes);
  const recording = decodeRecording(readFileSync(recordingPath));

  process.stdout.write(
    summarizeDiff(diffBehaviors(recording, before, after)) + "\n",
  );
}

function main(): void {
  const [command, ...args] = process.argv.slice(2);
  switch (command) {
    case "compile":
      return cmdCompile(args);
    case "check":
      return cmdCheck(args);
    case "fmt":
      return cmdFmt(args);
    case "explain":
      return cmdExplain(args);
    case "diff":
      return cmdDiff(args);
    default:
      process.stderr.write(
        "usage:\n" +
          "  aibc compile <in.behavior.yaml> [-o out.aib]\n" +
          "  aibc check   <in.behavior.yaml>...\n" +
          "  aibc fmt     <in.behavior.yaml>...\n" +
          "  aibc explain <in.behavior.yaml> <Input=value>...\n" +
          "  aibc diff    <before.yaml> <after.yaml> <session.arec>\n",
      );
      process.exit(2);
  }
}

main();
