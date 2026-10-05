# Workspace

A workspace groups multiple rbot projects under one top-level Buildfile.

## Initialization

```bash
rbot init -w
```

## Basic form

```text
use workspace

projects.rupamod as mod
projects.rupa as rupa
projects.ruka as ruka
```

A workspace can define common compiler settings and project-specific settings.

## Build commands

```bash
rbot -w
```

Build all projects in the workspace.

```bash
rbot -w rupa
```

Build a named project and the dependencies needed by it.

```bash
rbot -w release -- name=rupa
cd ../ruka && rbot -w clean
```

Release a single project (see [Releases](releases.md)); `clean` works per
project root like build.

## Dependencies

A project can declare another workspace project as a dependency:

```text
ruka.depends_on = rupamod
```

This is intended to make project ordering explicit while keeping each project independently buildable.

## Library-first builds

Workspace projects can be configured to produce libraries before final binaries. This is useful when multiple projects depend on each other's libraries or when the workspace has a two-stage relationship between libraries and executables.

The important model is:

1. establish required libraries;
2. link the final binaries once their library dependencies exist.

This avoids forcing users to manually orchestrate intermediate library builds.

Cross-project library references (`<n>.library.<os> = <project>`) resolve to
the other project's library artifact. Cyclic references (`rupa <-> ruka`) are
allowed: the two-phase build resolves them, and selective builds pull the
referenced projects transitively.

Packaging a project's files can reference another project's build outputs;
such path references become build dependencies automatically.

## Releases

A workspace can also declare distribution units — see
[Releases](releases.md). In short: `releases = ...` names the projects whose
artifacts get packaged into the workspace-level `dist/release/`, and
`rbot -w release -- name=<project>` packages a release straight from the CLI.
