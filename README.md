# rbot

**A simple builder for you.**

rbot is a simple build tool for C projects, driven by a declarative
`Buildfile`. No Makefile or CMake project is required — define the project
once and let rbot handle compilation, dependencies, libraries, workspace
projects, embedded assets, and packaging.

Cross-platform: Linux, macOS, and Windows (MSVC + MinGW).

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
rbot clean      # bersihkan hasil build
rbot version    # tampilkan versi rbot
rbot help       # tampilkan bantuan
```

Tanpa argumen, rbot mencari `Buildfile`. Di root workspace, `Buildfile.ws`
dapat digunakan untuk mengelola beberapa project.

## Versi

Versi rbot disimpan di `.rbot-version` dan di-embed ke binary saat build.
Perubahan versi akan memicu build ulang yang diperlukan.

```bash
printf 'v0.1.8\n' > .rbot-version
rbot
rbot version
```

Kode C juga dapat memakai versi yang di-embed melalui `build/version.h`:

```c
#include "version.h"
printf("%s\n", RBOT_VERSION_EMBEDDED);
```

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

c.build = false
c.compdb = true

o.binaryName = app
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto
```

Format Buildfile lama tetap didukung.

### Konfigurasi umum

- `sources` — direktori yang dipindai untuk source `.c`.
- `exclude` — source yang dikeluarkan dari target, misalnya `main.c`.
- `headers` — direktori include.
- `flags` — flag compiler, misalnya `Wall, Wextra, MMD, MP`.
- `std` — standar bahasa, misalnya `gnu11`.
- `compiler` — compiler yang tersedia, misalnya `gcc, clang`.
- `library` / `library.<platform>` — library yang ditambahkan saat link.
- `foreground` — perilaku process group/SIGINT.
- `output.*` — konfigurasi binary dan library.

Contoh library:

```text
library.default = ssl, crypto
library.linux = m, pthread
library.macos = m
library.windows = ws2_32
```

## Build paralel

Build paralel aktif secara default sesuai jumlah core CPU.

```bash
rbot
rbot -j
rbot -j4
rbot -j1
rbot --jobs=4
```

`-j1` memaksa build serial. Fase kompilasi dapat diparalelkan; link dan
beberapa fase artifact tetap terkoordinasi oleh rbot.

## Header tracking dan incremental build

rbot melacak dependency header secara otomatis. Source akan dianggap stale
ketika source atau dependency header-nya berubah.

Dengan `MMD, MP`, compiler GNU/Clang menghasilkan `.d` files yang dapat
digunakan rbot untuk mendapatkan dependency header. Bila `.d` belum tersedia,
rbot memiliki fallback scanner untuk `#include`.

rbot juga memiliki cache dependency/fingerprint sehingga perubahan mtime
saja tidak selalu menyebabkan kompilasi ulang ketika isi sebenarnya tidak
berubah.

Contoh output no-op:

```text
> Headers   : 4 source(s) skipped (touched, content unchanged)
Compiled : 0
Skipped  : 5
```

## compile_commands.json

rbot dapat menghasilkan dan mempertahankan `compile_commands.json` untuk
editor dan tooling seperti clangd.

```text
o.compileCommands = auto
```

Cache configuration dan compile database tidak ditulis ulang ketika tidak
ada perubahan yang relevan.

## Workspace multi-project

Gunakan `Buildfile.ws` untuk beberapa project:

```text
use workspace

projects.rupamod as mod
projects.rupa as rupa
projects = rupamod, rupa, ruka

compiler = gcc, clang
flags = Wall, Wextra, MMD, MP
std = gnu11

mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar, gz
mod.archive.exclude = build, dist, LICENSE, README.md, self_archive
mod.archive.dir = ../ruka/modules

rupa.sources = src
rupa.exclude = main.c
rupa.output as rupaout
rupa.library as rupalib
rupaout.binaryName = rupa
rupaout.libraryName = rupa
rupaout.libDir = lib
rupaout.libraryShared = true

ruka.sources = manager, src
ruka.exclude = manager/main.c
ruka.output as rukaout
ruka.library as rukalib
rukaout.binaryName = ruka
rukaout.libraryName = ruka
rukaout.libDir = lib
rukaout.libraryShared = true
ruka.depends_on = rupamod
```

Perintah:

```bash
rbot init -w
rbot -w
rbot -w rupa
rbot -w clean
```

`rbot -w NAME` membangun project yang diminta beserta dependency-nya.
Workspace mensintesis Buildfile project ke `.rbot/workspace/` dan tetap
menggunakan mekanisme build normal rbot, termasuk cache, dependency tracking,
parallel build, dan compdb.

### Library dan binary dua fase

Workspace dapat memiliki dependency library silang. rbot menangani kondisi
seperti `rupa <-> ruka` dalam dua fase:

1. library pass — membuat static/shared library yang diperlukan;
2. binary pass — setelah seluruh library tersedia, link binary final.

Contoh:

```text
rukalib.linux = rupa
rupalib.linux = ruka
```

## Archive

Project workspace juga dapat menghasilkan archive sendiri:

```text
mod.output.binary = false
mod.archive.src = .
mod.archive.name = rupa_modules
mod.archive.with = tar, gz
mod.archive.exclude = build, dist, LICENSE, README.md, self_archive
mod.archive.dir = ../ruka/modules
```

`archive.src` menentukan root source. `archive.pattern` (bila digunakan)
hanya digunakan untuk mendeteksi freshness archive; bukan filter isi archive.
Isi archive dikendalikan oleh `archive.exclude`.

`.git` merupakan exclusion bawaan untuk source archive.

## Embedded files

rbot dapat mengubah archive menjadi object binary dan memasukkannya ke
binary/library.

Contoh:

```text
ruka.embedded.modules as rukamod

rukamod.file = ../rupamod/dist/rupa_modules.tar.gz
rukamod.variable = MODULES
rukamod.extract = /tmp/rupa-system
rukamod.pattern = .rp
```

Header yang dihasilkan berada di `build/embedded.h` dan menyediakan macro:

```text
EMBED_<NAME>_ARCHIVE_NAME
EMBED_<NAME>_EXTRACT_DIR
EMBED_<NAME>_SYMBOL
EMBED_<NAME>_SYMBOL_END
EMBED_<NAME>_SYMBOL_LEN
```

Untuk static-library dependency, reference ke symbol embedded harus cukup kuat
agar linker menarik object embedded dari archive ketika diperlukan.

## Packaging

rbot memiliki packaging built-in melalui `pack.*`.

Contoh sederhana:

```text
pack.name = rbot
pack.version = 0.1.8
pack.output = dist/{name}-v{version}.tar.gz
pack.files = bin/rbot
```

### Source → destination mapping

`pack.files` dapat memetakan file source ke path di dalam package:

```text
pack.files = bin/rbot:bin/rbot, README.md:share/rbot/README.md
```

Hasil archive:

```text
bin/rbot
share/rbot/README.md
```

Tanpa `:destination`, path lama tetap berlaku:

```text
pack.files = bin/rbot, README.md
```

### Format, compression, checksum

```text
pack.name = rbot
pack.version = 0.1.8
pack.output = dist/{name}-v{version}.tar.gz
pack.files = bin/rbot:bin/rbot, LICENSE:share/rbot/LICENSE
pack.format = deb
pack.checksum = sha256
pack.deb.install_prefix = /usr/local
```

`pack.format` dapat digunakan untuk memilih format package yang didukung.
Compression dapat ditentukan melalui `pack.compress` atau extension output,
misalnya `.tar.gz`.

Template output yang umum:

```text
{name}
{version}
{os}
{arch}
```

### Package merge pada workspace

Satu project dapat menggabungkan **package definition** project lain:

```text
rupa.pack.files = bin/rupa:bin/rupa, src/prompt/cmd.txt:share/rupa/cmd.txt
rupa.pack.merge = ruka

ruka.pack.files = bin/ruka:bin/ruka
ruka.pack.name = ruka
ruka.pack.version = 0.2.2
ruka.pack.output = dist/{name}-v{version}.tar.gz
```

`pack.merge` berarti project target harus memiliki konfigurasi `pack.*` sendiri.
Yang digabung adalah isi package, terutama `pack.files`; metadata output tetap
milik masing-masing project.

Dengan konfigurasi tersebut package `rupa` dapat berisi:

```text
bin/rupa
bin/ruka
share/rupa/cmd.txt
```

Sementara `ruka` tetap dapat menghasilkan package-nya sendiri.

Jika project yang disebut pada `pack.merge` tidak memiliki `pack.files`, rbot
menganggap konfigurasi tersebut invalid dan berhenti dengan error.

### Debian install prefix

Untuk `.deb`, destination dari `pack.files` ditempatkan relatif terhadap
`pack.deb.install_prefix`:

```text
pack.deb.install_prefix = /usr/local
pack.files = bin/rbot:bin/rbot, README.md:share/rbot/README.md
```

menghasilkan path data package:

```text
/usr/local/bin/rbot
/usr/local/share/rbot/README.md
```

## Library output

Selain executable, rbot dapat membuat static dan shared library dari object
yang sama:

```text
output as o

o.binaryName = app
o.binaryDir = bin
o.buildDir = build

o.libraryName = app
o.libDir = lib
o.libraryShared = true
```

Hasil pada Linux:

```text
bin/app
lib/libapp.a
lib/libapp.so
```

`libraryShared = true` membuat shared library dan menggunakan object yang
sesuai untuk shared linking. Masing-masing artifact memiliki freshness check
sendiri.

## Konversi build.ninja

rbot dapat menggunakan `build.ninja` sebagai sumber konfigurasi sementara:

```bash
rbot -xf build.ninja
rbot -xcf build.ninja
rbot -xf build.ninja clean
```

`-xf` menghapus Buildfile hasil konversi setelah command selesai, sedangkan
`-xcf` mempertahankannya.

## Interrupt (Ctrl+C)

rbot menangani SIGINT dan meneruskannya ke compiler/job yang sedang berjalan.
Build yang dibatalkan akan berhenti dengan status interrupted dan tidak
menandai build yang gagal sebagai successful snapshot.

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
