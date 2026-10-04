# Archives and Embedded Assets

rbot can build archives as workspace artifacts and can embed generated files into another binary/library.

## Archive source

An archive project can use:

```text
mod.archive.src = .
```

or `./` as its source root.

This is useful when the archive represents a module tree rather than a conventional `src/` directory.

## Archive example

```text
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar, gz
mod.archive.exclude = build, dist, LICENSE, README.md, self_archive
mod.archive.dir = ../ruka/modules
```

The source root defines what is considered for archiving. Exclusion rules determine what is omitted.

`.git` is excluded from source archives by default.

## Embedded files

A generated archive can be embedded into a binary/library:

```text
ruka.embedded.modules as rukamod

rukamod.file = ../rupamod/dist/rupa_modules.tar.gz
rukamod.variable = MODULES
rukamod.extract = /tmp/rupa-system
rukamod.pattern = .rp
```

Generated embedded headers expose archive name, extraction directory, symbol, end symbol, and symbol length through `EMBED_*` macros.

Static-library use requires the generated object to remain reachable by the linker when the consuming library/binary references its symbols.
