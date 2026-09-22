# rbot

Build tool sederhana untuk project C, didorong oleh satu file konfigurasi
deklaratif bernama `Buildfile`. Tidak perlu Makefile — cukup `rbot`.

Lintas platform: Linux, macOS, dan Windows (MSVC + MinGW).

## Instalasi

### Linux/macOS

```bash
curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh
```

Script di atas mengunduh binary `rbot` yang sudah di-build (`bin/rbot` di
repo ini) dan memasangnya ke `/usr/bin/rbot`.

### Windows

rbot juga bisa dibangun langsung dari source — tersedia toolchain MSVC
(`cl.exe`, via Developer Command Prompt) maupun MinGW (`gcc`):

```bash
cmake -S . -B build
cmake --build build
# binary: bin/rbot.exe
```

## Pemakaian

```bash
rbot            # build project sesuai Buildfile (default, tanpa argumen)
rbot init       # buat Buildfile default kalau project belum punya
rbot clean      # bersihkan build dir & compile_commands.json (lihat Buildfile: clean)
rbot version    # tampilkan versi rbot yang aktif (baca dari .version)
rbot help       # tampilkan bantuan
```

## Benchmark

Benchmark berikut membandingkan **rbot self-hosted** dengan workflow
`bear -- make` pada project C dengan **174 source file**.

`CMake` tidak termasuk dalam pengukuran karena hanya digunakan untuk
bootstrap/build awal rbot. Setelah executable tersedia, kedua workflow
dijalankan menggunakan build system masing-masing.

### No-op build

Kondisi pengujian:

- 174 source file C.
- Tidak ada source yang berubah.
- Target `bin/rupa` sudah up-to-date.
- rbot menggunakan persistent Buildfile cache.
- rbot menggunakan persistent `compile_commands.json` cache.
- `compile_commands.json` tidak ditulis ulang ketika konfigurasi tidak berubah.
- Tidak ada source yang dikompilasi ulang.
- Linker tidak dijalankan ketika target sudah up-to-date.
- Pembanding menggunakan `bear -- make` agar workflow Make juga menghasilkan
  `compile_commands.json`.

| Build system | Run 1 | Run 2 | Rata-rata |
| --- | ---: | ---: | ---: |
| **rbot** | 0.858 s | 0.814 s | **0.836 s** |
| **bear + make** | 2.204 s | 2.169 s | **2.187 s** |

Pada pengujian tersebut:

```text
rbot:
  Compiled : 0
  Skipped  : 174
  CompDB   : cache hit
  Linking  : bin/rupa (up-to-date)

bear + make:
  make: 'bin/rupa' is up to date.
```

Rata-rata wall-clock time `rbot` sekitar **2.6× lebih rendah** dibandingkan
`bear -- make` pada pengujian no-op ini.

CPU time yang tercatat:

| Build system | Run 1 | Run 2 | Rata-rata |
| --- | ---: | ---: | ---: |
| **rbot** | 0.14 s | 0.13 s | **0.135 s** |
| **bear + make** | 1.41 s | 1.41 s | **1.41 s** |

Benchmark ini merupakan pengukuran pada environment pengujian yang sama dan
bukan klaim performa universal. Hasil dapat berbeda tergantung hardware,
filesystem, toolchain, jumlah source, dan environment runtime.

## Buildfile

`rbot init` akan membuat `Buildfile` default berikut di project kamu:

```yaml
root: .

clean:
  - buildDir: false
  - compileCommands: false

version: "0.1.0"

sources:
  - src

flags:
  - Wall
  - Wextra

std: gnu11

headers:
  - include
  - I.

compiler:
  - gcc
  - clang

progress:
  bar: true
  error: always

output:
  - binaryName: rbot
  - binaryDir: bin
  - buildDir: build
  - compileCommands: auto # compile_commands.json
```

Bagian yang sering disesuaikan:

- **sources** — daftar direktori yang di-scan rekursif untuk file `.c`.
- **headers** — tiap entri menjadi `-I<dir>`; `I.` shorthand untuk `-I.`
  (di MSVC otomatis menjadi `/I<dir>`).
- **library** *(opsional, tidak ada di default)* — tiap entri menjadi
  `-l<nama>`, contoh `ssl` → `-lssl` (di MSVC otomatis menjadi `ssl.lib`).
- **output.binaryName / binaryDir** — nama & lokasi binary hasil build.

## Embedded (Buildfile: embedded)

Fitur embedded mengarsipkan sebuah direktori lalu menempelkannya ke binary
— hasil build tetap satu file executable tanpa aset eksternal.

### Menulis di Buildfile

```yaml
library:
  - ssl
  - crypto

  - linux:
      - m
      - pthread

  - macos:
      - m

  - windows:
      - ws2_32

embedded:
  - assets:
      - src: assets
      - pattern: .txt
      - extract: /tmp/rupa-assets
      - archive:
          - dir: modules
          - name: demo_assets
          - with:
              - tar: true
              - ext: gz
```

Hasil di direktori project:

```text
modules/demo_assets.tar.gz
build/demo_assets.o
build/embedded.h
```

### Memakai di kode

Include `build/embedded.h` (tambahkan `build` ke `headers` di Buildfile),
lalu pakai macro per entri `<NAMA>` (uppercase nama entri):

| Macro | Arti |
| --- | --- |
| `EMBED_<NAMA>_ARCHIVE_NAME` | nama file arsip |
| `EMBED_<NAMA>_EXTRACT_DIR` | isi `extract:` di Buildfile |
| `EMBED_<NAMA>_SYMBOL` | pointer ke byte pertama data |
| `EMBED_<NAMA>_SYMBOL_END` | pointer satu-byte-setelah-blok |
| `EMBED_<NAMA>_SYMBOL_LEN` | panjang data dalam byte |

```c
#include <stdio.h>
#include "embedded.h"

int main(void) {
  printf("arsip : %s\n", EMBED_ASSETS_ARCHIVE_NAME);
  printf("ukuran: %lu byte\n", (unsigned long)EMBED_ASSETS_SYMBOL_LEN);

  FILE *f = fopen("/tmp/restore.tar.gz", "wb");
  fwrite(EMBED_ASSETS_SYMBOL, 1, EMBED_ASSETS_SYMBOL_LEN, f);
  fclose(f);
  return 0;
}
```

### Dua jalur embed (otomatis)

| Jalur | Kapan dipakai | Bentuk simbol |
| --- | --- | --- |
| `ld -r -b binary` | GNU ld tersedia (Linux/MinGW) | `_binary_<path>_start/_end` nyata |
| Fallback C array | MSVC, atau tanpa `ld` (`RBOT_NO_LD=1`) | `start[]` + `unsigned long _len` |

Macro `EMBED_<NAMA>_*` di `build/embedded.h` sama persis di kedua jalur.
Freshness arsip dicek otomatis: perubahan pada file di `src:` akan memicu
build ulang arsip dan object.

## `.version`

File `.version` di root project menandai versi implementasi rbot yang
aktif (mis. `v0.1.0`). Ini dipakai oleh dispatcher (`src/main.c` pada
source rbot sendiri) dan oleh `rbot version`.

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
