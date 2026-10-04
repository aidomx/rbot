# Benchmark Methodology

The benchmark bundled with rbot v0.2.0 is intended primarily to detect regressions in rbot, not to establish a universal ranking of build systems.

## Default workload

`bench/compare.sh` defaults to:

```text
150 source files
30 headers
5 repetitions
```

The generated project uses a chain of headers and a collection of C sources so that dependency propagation can be exercised.

## Compared tools

The script can compare:

- Ninja
- Make
- Bear + Make (when Bear exists)
- rbot
- CMake + Ninja (when CMake exists)
- Meson + Ninja (when Meson exists)
- XMake (when available)
- Tup (when available)

Required tools are Ninja, Make, and GCC. Optional tools are detected automatically.

## Scenarios

### `cold`

Build from a clean state. For CMake/Meson the configure step is included.

### `noop`

Build again without changes. This primarily measures build-tool overhead.

### `touch`

Touch one header without changing its content. rbot is designed to recognize this as unchanged content and avoid recompiling affected sources.

### `edit`

Change one source file. Exactly one object should need recompilation, followed by relinking. The benchmark also measures a direct GCC compile+link floor to estimate how much time is build-tool overhead.

### `compdb`

Generate `compile_commands.json`.

### `strace`

Count system calls during no-op builds when `strace` is installed.

## Measurement rules

Each tool is primed first so that its own state files are not polluted by another tool. Results use **median** and **min-max**, not a single lucky run or arithmetic mean.

The CSV records:

```text
label,scenario,ms,rc,rebuilt,version
```

A separate syscall CSV is produced when syscall tracing is available.

## Interpreting results

Do not conclude "rbot is faster" from one run. A useful evaluation asks:

1. Is the result reproducible?
2. How much work was actually performed?
3. How much does filesystem/cache state affect the result?
4. Does the result hold in another environment?
5. Is a slower result still within an acceptable range?

The most interesting rbot cases are often incremental and no-op builds, especially when content-aware dependency tracking avoids unnecessary compilation.
