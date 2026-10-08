# Configuration Design

## Explicit only when necessary

The configuration language should avoid turning defaults into mandatory boilerplate.

Instead of requiring:

```text
sources = src
headers = include
output.binaryName = app
output.binaryDir = bin
output.buildDir = build
output.compileCommands = auto
```

for every standard project, rbot can infer conventional values and let the Buildfile override only exceptions.

## `output.binary` and scope

One design discussion considered keeping:

```text
output.binary = true
```

as the compatibility-safe boolean switch, while placing detailed binary configuration under:

```text
output.binary.scope = name:app, dir:bin, build:build, compdb:auto
```

This avoids changing the meaning/type of the existing `output.binary` field while still allowing a compact configuration surface.

This is a proposed design direction, not a statement that v0.2.0 accepts the `.scope` syntax.

## Compiler selection

Another simplification direction is:

```text
use project
compiler = cl, gcc, clang
```

with compiler settings inferred automatically. A separate:

```text
compiler.settings = auto
```

would be redundant if automatic settings are the default behavior.

The general rule is: **do not expose an option whose only useful value is the default**.
