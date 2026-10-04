# Incremental Builds and Dependency Tracking

Incremental correctness is one of rbot's important design areas.

## `.d` dependency tracking

GNU/Clang builds can emit dependency files with:

```text
MMD
MP
```

A dependency file associates an object with the headers that affected it. rbot uses this information to decide which source files are stale.

If a `.d` file is unavailable, rbot can fall back to scanning `#include` relationships.

## Content-aware freshness

rbot also tracks content/fingerprint state. A timestamp change does not automatically imply that the compiler must run when the relevant content is unchanged.

This makes cases such as:

```bash
touch include/foo.h
rbot
```

potentially become a no-op instead of recompiling every source that includes `foo.h`.

Typical output can distinguish a touched-but-unchanged dependency from a genuinely changed one.

## Why this matters

The objective is not to claim that rbot is always faster than Ninja. The objective is predictable incremental behavior with low overhead across environments.

The benchmark methodology should therefore look at both elapsed time and actual object recompilation count.
