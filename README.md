# AI Behavior Toolkit

[![CI](https://github.com/syedjafri06193/AI-Behavior-Tree-Toolkit-proj/actions/workflows/ci.yml/badge.svg)](https://github.com/syedjafri06193/AI-Behavior-Tree-Toolkit-proj/actions/workflows/ci.yml)

Utility AI for Unreal that plugs into both Behavior Tree and StateTree, authored
in a diffable, mergeable text format, with score-breakdown tooling and session
record/replay for debugging beyond PIE.

> **Scope note.** The original pitch was a visual BT editor with live debugging
> and a blackboard inspector — but Unreal already ships all three. This project
> targets what's missing: **utility AI**, **diffable authoring** (`.uasset` is
> binary), and **debugging in cooked builds and dedicated servers**. Reasoning in
> [`1—Documentation/design.md`](1—Documentation/design.md) §1.

## Repository layout

```
.
├── README.md          ← you are here
├── 1—Documentation/   ← all documentation (index in its README)
│   ├── design.md      ← full design guide (the spec code comments cite)
│   ├── design.pdf     ← same guide, PDF
│   └── v1/            tuning guide, file format, debug protocol, spec errata
└── v1/                ← first implementation
    ├── core/          C++17 scoring engine, zero engine types, CMake + tests + bench
    ├── editor/        TypeScript: YAML format, validation, compiler, debug protocol, replay
    ├── unreal/        Unreal plugin (BT + StateTree bindings, debug server)
    └── samples/       guard.behavior.yaml
```

Each `vN/` directory is a self-contained iteration. Start with
[`v1/README.md`](v1/README.md).

## Quick start

Needs CMake, a C++17 compiler, and Node.js:

```bash
cd v1
make          # 56 C++ tests, 81 TypeScript tests, and the C++ ↔ TS cross-check
make bench    # measure the performance budget
make demo     # compile the sample behavior and explain a decision
```

The Unreal plugin isn't built by `make`; see [`v1/unreal/README.md`](v1/unreal/README.md).

## Versions and feedback

| Version | Summary | Feedback |
|---|---|---|
| [v1](v1/) | C++ utility core, TS editor toolchain, Unreal BT/StateTree plugin, record/replay | — |

Add a row per version as new iterations land.

## Feedback

Feedback, bug reports and ideas are welcome.

- **Bugs or problems:** [open an issue](https://github.com/syedjafri06193/AI-Behavior-Tree-Toolkit-proj/issues/new) and include your engine or runtime version, a minimal tree that reproduces the problem, and what you expected to happen.
- **Ideas or questions:** start a thread in [Discussions](https://github.com/syedjafri06193/AI-Behavior-Tree-Toolkit-proj/discussions) (enable it under Settings → General → Features).
- **Code changes:** pull requests are appreciated; please describe what you changed and how you tested it.
