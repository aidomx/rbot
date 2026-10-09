# Direction Notes

This document records design ideas discussed after the v0.2.0 source snapshot. These are **not guaranteed v0.2.0 features**.

## Minimal initialization

Desired output:

```text
rbot init
```

```text
use project
```

and:

```text
rbot init -w
```

```text
use workspace
```

The goal is that a conventional project needs no generated boilerplate.

## Automatic conventions

The tool should infer conventional:

- source directory
- include directory
- compiler
- flags
- build directory
- binary name
- compile database behavior
- dependency tracking
- clean behavior

Users override only what differs from the convention.

## Compact binary scope

Potential syntax:

```text
output.binary = true
output.binary.scope = name:app, dir:bin, build:build, compdb:auto
```

The boolean remains backward-compatible while `.scope` carries compact details.

## Workspace and ecosystem

The workspace model is intended to support ecosystems such as the Rupa compiler, `ruka` module manager, and `rupamod` module archive project without requiring users to manually orchestrate library and archive dependencies.

The build tool should remain independent of those projects even when it is used to build them.

## Profiling

Implemented (see the design at `design/profile.md` and the guide at
`guide/profile.md`): `rbot profile <context>` measures operations without a
normal build and never mutates build state. Shipped contexts are `archive=`,
`embed=`, `library=`, and `binary=`; `project=` and `workspace=` remain
planned until the individual contexts they would aggregate are reliable.
Candidate options not yet implemented: `--repeat <N>`.
