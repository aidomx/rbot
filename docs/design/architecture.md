# Architecture Notes

rbot's implementation is organized around several concerns rather than a monolithic compiler loop.

## Configuration

Buildfiles are parsed into project/workspace configuration. Aliases allow common namespaces such as `clean`, `library`, and `output` to be addressed compactly.

## Dependency tracking

The dependency subsystem reads `.d` files and can scan includes when necessary. Snapshot/fingerprint logic supplies content-aware state so unchanged content does not cause unnecessary work.

## Compilation

The compiler layer selects the platform/compiler command and builds individual objects. Parallel execution is coordinated by rbot.

## Workspace

Workspace handling maps a multi-project configuration into project builds while preserving dependency order and shared build behavior.

## Compile database

The compdb layer maintains `compile_commands.json` independently of the executable build artifact.

## Packaging

Packaging is treated as another artifact stage: source files, binaries, libraries, archives, embedded files, and package metadata can be combined without requiring an external build generator.

## Portability

Platform-specific process and filesystem operations are isolated behind portability code. This is important for Linux/macOS/Windows support and for environments such as Termux/proot.
