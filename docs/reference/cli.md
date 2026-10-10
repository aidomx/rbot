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
rbot -w
rbot -w release -- key=value[,key=value...]
```

Build the workspace and package ONE release artifact into the workspace
`dist/` (when a `release.*` section exists in `Buildfile.ws`). With `--`,
each `key=value` pair overrides a release setting for this run (`name`,
`version`, `format`, `target` — accepted as a legacy alias of `format`).
See [Releases](../guide/releases.md).

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

## Profile

```bash
rbot profile <context> [options]
```

Measure the cost of rbot operations without performing a normal build. Does
not touch build state (`.rbot/`, `Buildfile`, artifacts); temporary data is
isolated and removed afterwards. All arguments after `profile` belong to the
context — global rbot options are not parsed there.

```bash
rbot profile archive=DIR [with=tar|none|tar,gz|tar,xz|tar,bz2]
rbot profile embed=DIR|FILE
rbot profile library=DIR [with=static|shared|static,shared]
rbot profile binary=DIR [jobs=N] [--report FILE]
rbot profile project=DIR [jobs=N]
rbot profile workspace=DIR
```

- `with=` is context-specific; an option that a context does not support is
  rejected with `rbot: profile option 'with' is not valid for context '<ctx>'`.
- Terminal output is a short summary; piped output is the detailed report —
  `rbot profile archive=DIR > profile.md` is enough.
- `--report FILE` writes the detailed report to a file and keeps the summary
  on the terminal.
- `--repeat N` (1–100) repeats the run; the sandbox is kept between runs
  (caches live inside it), so later runs measure the warm path; timings
  aggregate over all runs.
- Paths accept `~/...` (expanded by rbot — shells only expand `~` at word
  positions).

See [Profiling](../guide/profile.md) for examples and measured phases.

## Version and help

```bash
rbot version
rbot help
```


## Self-upgrade

```bash
rbot self-upgrade
```

Check the latest release of rbot through the official GitHub Releases API
and use the official installer to upgrade rbot itself. This does not update
project dependencies or system packages. If rbot is running from a development
checkout or an unrecognized custom installation path, automatic replacement
is refused and manual instructions are shown. If the version check fails, the
local installation is left unchanged. On Windows, automatic upgrade is limited
to the official per-user installation at `%LOCALAPPDATA%\rbot\rbot.exe`.
Because Windows locks the running executable, rbot launches a temporary
PowerShell helper in a separate console; the helper waits for rbot to exit,
checks GitHub Releases, and invokes `install.ps1` from the validated release tag.

## Self-uninstall

```bash
rbot self-uninstall
rbot self-uninstall --yes
```

Remove rbot's own installation and globally managed state (`$RBOT_HOME`,
default `~/.rbot/`). On an interactive terminal, rbot asks for confirmation.
In non-interactive mode, `--yes` (or `-y`) is required. The command must not
remove project build state (`.rbot/`), project source dependencies (such as
`third/`), system packages, or files whose ownership by rbot cannot be
verified. A binary is removed only when its installation path is recognized
and safe to remove.

## Ninja conversion

The v0.2.0 tree also contains Ninja Buildfile conversion support. The exact conversion flags are implementation-specific; `rbot -h` is authoritative for the installed binary.
