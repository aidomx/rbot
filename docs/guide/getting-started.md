# Getting Started

## The basic idea

rbot is a declarative build tool for C projects. A project is described by a `Buildfile`; rbot handles compilation, dependency tracking, incremental builds, libraries, workspaces, embedded assets, compile commands, and packaging.

The intended user experience is simple: a conventional project should require very little configuration, while unusual projects can opt into explicit settings.

## Minimal project

A standard project — `src/` for sources, optionally `include/` for public
headers — needs only one line in its Buildfile:

```text
use project
```

Everything else follows rbot conventions: `sources` defaults to `src`,
`headers` to `include` when that folder exists, the binary is named after
the project folder, and `std = gnu11`, `bin/`, `build/` fill in the rest.
C++ works the same way with zero declarations — `src/main.cpp` instead of
`src/main.c` gives you `std = c++17` and the `g++`/`clang++` toolchain
automatically.
A quick test:

```bash
mkdir -p hello/src
printf 'int main(void){return 0;}\n' > hello/src/main.c
cd hello && rbot init && rbot   # produces bin/hello
```

Non-standard layouts stay free: any field declared explicitly wins over
the convention (see [Buildfile](buildfile.md)).

## Initialize

```bash
rbot init
```

Creates the minimal Buildfile above — just `use project`.

For a guided walkthrough — project name, optional `include/`, workspace
with member projects and `depends_on` — use the interactive scaffold:

```bash
rbot init -p
```

Enter accepts the default at every question (a random name for an empty
answer). It scaffolds a new project folder that builds and runs
immediately; see [CLI](../reference/cli.md) for the full question list.

For a workspace:

```bash
rbot init -w
```

creates a workspace Buildfile listing projects:

```text
use workspace

projects = app
```

Each project follows the same standard convention: a folder `app/`
containing `src/` (and optionally `include/`), built with `rbot -w`
from the workspace root.

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

## Version

The project version lives in `.rbot-version` at the project root and is
embedded into the binary at build time:

```bash
printf 'v0.2.0\n' > .rbot-version
rbot
rbot version
```

Version changes trigger the rebuilds that need them. C code can read the
embedded version through `build/version.h`:

```c
#include "version.h"
printf("%s\n", RBOT_VERSION_EMBEDDED);
```
