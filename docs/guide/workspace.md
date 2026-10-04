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
