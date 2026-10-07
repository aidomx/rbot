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

Create a project Buildfile — for a standard project this is just `use project`.

## Interactive init

```bash
rbot init -p
```

Interactive scaffold. Enter accepts the default at every question; the
scaffold is written into a new project folder, so it can be run from
anywhere:

| Question | Default (Enter) |
|---|---|
| `Project name?` | random name (`rbot-xxxxxx`) — the name is always the scaffold folder |
| `language?` | `c` — answering `cpp` scaffolds `src/main.cpp` (iostream) and writes `std = c++17` |
| `std?` | none written — convention (`gnu11`, or `c++17` for cpp) |
| `compiler?` | none written — auto-detect |
| `flags?` | none written — defaults |
| `Create include/?` | `N` |
| `Uses workspace?` | `N` |
| `Workspace projects?` (workspace only) | `app,tool` |
| `depends_on?` (workspace only) | none — format `<project> depends <project>` |

Non-default answers are written explicitly into the Buildfile (`std = ...`,
`compiler = ...`, `flags = ...`); everything else stays convention-based.
The scaffold builds and runs out of the box:

```text
<name>/
├── Buildfile        # single-project only; workspace projects need none
├── include/<name>.h # when include/ is requested
└── src/main.c       # src/main.cpp when language = cpp
```

A workspace scaffold writes `Buildfile.ws` (`projects = ...`, optional
`<p>.depends_on = <p>` lines) plus a `src/main.c` per member — no per-project
Buildfiles; rbot synthesizes them. `init -p` never overwrites: if any target
file already exists, it aborts before writing anything. Ctrl+C or EOF on a
question cancels the scaffold.

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
