# CLI Reference

## Build

```bash
rbot
```

Build the current project from `Buildfile`.

## Alternate Buildfile

```bash
rbot -f FILE
```

Use another Buildfile.

## Initialize project

```bash
rbot init
```

Create a project Buildfile.

## Initialize workspace

```bash
rbot init -w
```

Create a workspace Buildfile.

## Workspace build

```bash
rbot -w
rbot -w NAME
```

Build all projects or one named project and its dependencies.

## Workspace release

```bash
rbot -w release -- name=PROJECT[,key=value...]
```

Package a release from the CLI. `name=<project>` is required and the project
must exist; `target=deb|tar` overrides the packaging format for this run.
Artifacts go to `dist/release/` (or `dist/release/<name>/` for a selective
release). Without `--`, `rbot -w` also builds and releases every declared
release. See [Releases](../guide/releases.md).

## Clean

```bash
rbot clean
```

Remove generated build artifacts according to the active project/workspace configuration.

## Jobs

```bash
rbot -j
rbot -j4
rbot -j1
rbot --jobs=4
```

Control parallel build jobs.

## Version and help

```bash
rbot version
rbot help
```

## Ninja conversion

The v0.2.0 tree also contains Ninja Buildfile conversion support. The exact conversion flags are implementation-specific; `rbot -h` is authoritative for the installed binary.
