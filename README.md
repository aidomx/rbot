<div align="center">
  <img src="rbot_logo.png" alt="rbot logo" width="600">
</div>

# rbot

**A simple builder for you.**

rbot is a simple build tool for C projects, driven by a declarative
`Buildfile`. No Makefile or CMake project is required — define the project
once and let rbot handle compilation, dependencies, libraries, workspace
projects, embedded assets, packaging, and releases.

Cross-platform: Linux, macOS, and Windows (MSVC + MinGW).

Full documentation lives in [docs/](./docs/README.md): guides
([getting started](./docs/guide/getting-started.md),
[Buildfile](./docs/guide/buildfile.md),
[workspace](./docs/guide/workspace.md),
[releases](./docs/guide/releases.md),
[packaging](./docs/guide/packaging.md)) and references
([CLI](./docs/reference/cli.md),
[Buildfile fields](./docs/reference/buildfile-fields.md)).

## Instalasi

### Linux/macOS

```bash
curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh
```

The script installs the prebuilt `rbot` binary from the repository.

### Bootstrap from source

```bash
./install.sh --dev
```

Requires a C compiler such as `gcc` or `clang`. The resulting binary is
`build/bin/rbot`.

### Windows

From a source tree, use MSVC (`cl.exe`) or MinGW (`gcc`) without CMake:

```powershell
.\install.ps1 --dev
```

The resulting binary is `build\bin\rbot.exe`.

## Pemakaian

```bash
rbot            # build project dari Buildfile
rbot -f FILE    # gunakan Buildfile lain
rbot init       # buat Buildfile baru
rbot init -w    # buat Buildfile.ws baru
rbot -w         # build semua project dalam workspace
rbot -w NAME    # build project NAME dan dependency-nya
rbot -w release -- name=NAME    # release satu project via CLI
rbot clean      # bersihkan hasil build
rbot version    # tampilkan versi rbot
rbot help       # tampilkan bantuan
```

Tanpa argumen, rbot mencari `Buildfile`. Di root workspace, `Buildfile.ws`
dapat digunakan untuk mengelola beberapa project.

## Buildfile

Buildfile modern menggunakan `use alias` dan format `key = value`:

```text
use alias

clean as c
output as o

root = .
sources = src
headers = include, I.
flags = Wall, Wextra, O2, MMD, MP
std = gnu11
compiler = gcc, clang

o.binaryName = app
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```

Format Buildfile lama tetap didukung. Daftar lengkap field ada di
[docs/reference/buildfile-fields.md](./docs/reference/buildfile-fields.md).

## Workspace multi-project

`Buildfile.ws` mengelola beberapa project sekaligus:

```text
use workspace

projects.rupamod as mod
projects.rupa as rupa
projects = rupamod, rupa, ruka

ruka.depends_on = rupamod
rukalib.linux = rupa
rupalib.linux = ruka
```

Perintah:

```bash
rbot -w          # build + release semua project
rbot -w rupa     # build rupa dan dependency-nya
rbot -w clean    # bersihkan seluruh workspace
```

rbot membangun workspace dalam dua fase — library pass membuat
`lib<name>.a`/`.so` tanpa link, binary pass link binary final setelah semua
library tersedia — sehingga dependency library silang (`rupa <-> ruka`)
tetap bisa dibangun. Detail di
[docs/guide/workspace.md](./docs/guide/workspace.md).

## Release

Workspace memisahkan `projects` (unit build) dan `releases` (unit
distribusi):

```text
releases = rupa, ruka

releases.rupa as rrupa
rrupa.name = rupa
rrupa.target = deb
```

```bash
rbot -w                          # build + release semua -> dist/release/
rbot -w release -- name=rupa     # release satu project via CLI
```

Release bisa dipicu penuh dari CLI tanpa deklarasi di `Buildfile.ws`.
Detail di [docs/guide/releases.md](./docs/guide/releases.md).

## Fitur lain

- **Incremental build** — header tracking dua lapis (mtime + hash konten),
  cache dependency/fingerprint; lihat
  [docs/guide/incremental.md](./docs/guide/incremental.md).
- **Build paralel** — `-j[N]`, default sebanyak core CPU.
- **compile_commands.json** — `o.compileCommands = auto` untuk clangd;
  lihat [docs/reference/compdb.md](./docs/reference/compdb.md).
- **Archive** — `archive.*` untuk project kemasan arsip; lihat
  [docs/guide/embedded-and-archives.md](./docs/guide/embedded-and-archives.md).
- **Embedded files** — archive menjadi object binary dan di-embed ke
  binary/library; lihat
  [docs/guide/embedded-and-archives.md](./docs/guide/embedded-and-archives.md).
- **Packaging** — `pack.*`: tarball, `.deb` (mapping file, merge antar
  project, checksum sha256); lihat
  [docs/guide/packaging.md](./docs/guide/packaging.md).
- **Library output** — static + shared dari object yang sama
  (`output.libraryName`, `output.libraryShared`).
- **Konversi build.ninja** — `rbot -xf build.ninja` (sementara) atau
  `-xcf` (dipertahankan); lihat
  [docs/reference/rbot-ninja-import.md](./docs/reference/rbot-ninja-import.md).
- **Interrupt (Ctrl+C)** — SIGINT diteruskan ke job compiler; build yang
  dibatalkan tidak merekam snapshot sukses. Detail di
  [docs/reference/errors-and-interrupts.md](./docs/reference/errors-and-interrupts.md).

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
