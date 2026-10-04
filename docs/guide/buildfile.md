# Buildfile

rbot uses a declarative `Buildfile` with `key = value` assignments and `use` declarations.

A representative v0.2.0 Buildfile is:

```text
use project

clean as c
library as lib
output as o

root = .
sources = src
headers = include, I., build
flags = Wall, Wextra, O2, MMD, MP
std = gnu11
compiler = cl, gcc, clang

c.build = false
c.compdb = false

o.binaryName = rbot
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```


## Core fields

### `use project`

Selects project mode.

### `root`

Project root used as the base for project paths.

### `sources`

Source directories scanned for C sources.

### `headers`

Include/header directories. Multiple paths can be comma-separated.

### `flags`

Compiler flags. For example:

```text
flags = Wall, Wextra, O2, MMD, MP
```

`MMD`/`MP` are useful with GNU/Clang dependency files.

### `std`

C language standard, for example:

```text
std = gnu11
```

### `compiler`

Compiler candidates, for example:

```text
compiler = cl, gcc, clang
```

The platform/compiler implementation chooses an available compiler.

## Aliases

The v0.2.0 configuration supports aliases:

```text
clean as c
library as lib
output as o
```

Then the corresponding settings can be addressed through the alias:

```text
o.binaryName = app
o.binaryDir = bin
o.buildDir = build
```

## Output

A typical executable configuration:

```text
output as o

o.binaryName = app
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```

Library output can be configured through the output/library fields when the project needs static or shared artifacts.

## Boolean binary switch

`output.binary` is used as a boolean switch in the current configuration model. In particular:

```text
output.binary = false
```

means that the binary artifact is skipped while library/artifact work can still occur.

This distinction is important for workspace projects that exist primarily to create a library or archive.

## Libraries

Platform-specific libraries can be expressed as:

```text
library.default = ssl, crypto
library.linux = m, pthread
library.macos = m
library.windows = ws2_32
```

## Process behavior

`foreground` controls process-group/SIGINT behavior. rbot can own SIGINT handling and forward interruption to child build processes.

## Progress

The build UI can be configured with progress settings such as:

```text
progress.bar = true
progress.error = always
```
