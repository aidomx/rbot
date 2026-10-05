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
printf 'v0.1.8\n' > .rbot-version
rbot
rbot version
```

Version changes trigger the rebuilds that need them. C code can read the
embedded version through `build/version.h`:

```c
#include "version.h"
printf("%s\n", RBOT_VERSION_EMBEDDED);
```
