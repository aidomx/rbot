# Getting Started

## The basic idea

rbot is a declarative build tool for C projects. A project is described by a `Buildfile`; rbot handles compilation, dependency tracking, incremental builds, libraries, workspaces, embedded assets, compile commands, and packaging.

The intended user experience is simple: a conventional project should require very little configuration, while unusual projects can opt into explicit settings.

## Minimal project

The minimal project form discussed for the next simplification of rbot is:

```text
use project
```

For rbot's current v0.2.0 configuration model, explicit fields are still commonly used when the project needs them.

## Initialize

```bash
rbot init
```

Creates a project Buildfile.

For a workspace:

```bash
rbot init -w
```

creates the workspace Buildfile.

## Build

```bash
rbot
```

Without arguments rbot searches for `Buildfile` and builds the project.

## Other common commands

```bash
rbot -f FILE
rbot clean
rbot version
rbot help
rbot -j4
rbot -j1
```

`-j` enables the normal parallel build behavior; `-jN` chooses an explicit job count.
