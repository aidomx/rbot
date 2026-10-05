# Packaging

rbot has built-in packaging support through the `pack.*` namespace.

## Source archive

```text
pack.name = rbot
pack.version = 0.2.0
pack.output = dist/{name}-v{version}.tar.gz
pack.files = bin/rbot:bin/rbot, README.md:share/rbot/README.md
```

A mapping uses:

```text
source:path/in/package
```

Without a mapping, the source path is preserved.

Binary entries may omit the `.exe` suffix: when the exact source is absent,
the packer falls back to `<source>.exe` (a Windows-linked binary is
`bin/rbot.exe` while the Buildfile says `bin/rbot`). The packaged path keeps
the destination as configured.

## Package formats

A package can select a format:

```text
pack.format = deb
```

and optionally a checksum:

```text
pack.checksum = sha256
```

## Debian

Example:

```text
pack.deb.install_prefix = /usr/local
pack.deb.maintainer = "Aidomx <aidomxdev@gmail.com>"
pack.deb.description = "A simple builder for you"
pack.deb.architecture = arm64
```

The install prefix is combined with package destinations when constructing the Debian filesystem tree.

## Package merge

Workspace package definitions can merge files from another project while keeping the target project's package metadata and output identity.

```text
rupa.pack.merge = ruka
```

This is useful when a release artifact contains the outputs of several workspace projects.
