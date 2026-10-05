# Releases

> Scope: implemented on the main branch after the v0.2.0 documentation
> baseline. See `docs/README.md` for the versioning convention.

A workspace separates two concepts:

```text
workspace
├── projects   = build units
└── releases   = distribution units
```

Not every project has to be released. A project that exists only as a build
dependency can stay private while other projects produce the distributable
artifacts.

## Declaring releases

Releases refer to projects already defined by the workspace:

```text
releases = rupa, ruka

releases.rupa as rrupa
releases.ruka as rruka

rrupa.name = rupa
rrupa.target = deb

rruka.name = ruka
```

- `releases = ...` — list of releases (project names; the alias defaults to the name).
- `releases.<n> as <alias>` — optional; lets release configuration live separately from project configuration.
- `<alias>.name` — source project of the artifact.
- `<alias>.target` — optional packaging format for the release: `deb` or `tar` (default: tarball).

## Release from the CLI

Releases can also be triggered entirely from the CLI, without declaring them
in `Buildfile.ws`:

```bash
rbot -w release -- name=rupa
rbot -w release -- name=rupamod target=deb
```

- `name=<project>` — required. The project must exist; when it exists but has
  no declared release, an implicit release is created for this run.
- `target=<tar|deb>` — optional packaging-format override for this run.

Errors are reserved for real problems: the project does not exist, or the
project has no `pack.files` and therefore nothing to package.

## Output

Release artifacts go to the workspace-level `dist/release/`, not to each
project's `dist/`:

```text
rbot -w                        -> dist/release/
rbot -w release -- name=rupa   -> dist/release/rupa/
```

The selective form writes to a per-name folder so its artifacts never mix
with full-workspace release output. Each artifact gets a `.sha256` checksum
when the release produces one.

## Pipeline

Building a workspace runs three phases:

```text
1. library pass   — compile sources, package lib<name>.a/.so (no binary link)
2. binary pass    — link binaries once all libraries exist
3. release pass   — package each release into dist/release
```

`rbot -w` builds and releases everything. `rbot -w NAME` builds the project
closure but only releases the releases owned by NAME — projects pulled in as
dependencies are not released on their own.

## Relationship with pack

`pack.*` stays the packaging engine; release only orchestrates it at the
workspace level. Project-level `pack.*` output keeps going to the project's
own `dist/`. The release phase repackages the same inputs with release
configuration (`pack.name`, `pack.output`, `pack.format`), so archive, Debian
packaging, merge, and checksums all work for releases without a second
packaging implementation.

Projects without `pack.*` cannot be released; declare `pack.files` on the
project first or pick a project that already packages.
