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
rbot version    # tampilkan versi rbot yang aktif (ter-embed dari .rbot-version)
rbot help       # tampilkan bantuan
```

### Versi (.rbot-version)

Versi tidak lagi ditulis manual di source. rbot membaca file `.rbot-version`
di root saat build, lalu menerbitkan `build/version.h` berisi
`RBOT_VERSION_EMBEDDED` — versi ikut ter-embed ke binary sehingga
`rbot version` tetap benar di mana pun binary dijalankan, tidak tergantung
path lokal atau repo. Mengubah isi `.rbot-version` otomatis memicu
kompilasi ulang saat build berikutnya.

```bash
printf 'v0.2.0\n' > .rbot-version
rbot && rbot version   # -> rbot v0.2.0
```

Kode konsumen bisa memakai macro yang sama:

```c
#include "version.h" /* tambahkan build ke headers di Buildfile */
printf("%s\n", RBOT_VERSION_EMBEDDED);
```

Cara ini juga menjawab kondisi release: binary yang di-build dari commit
tersebut selalu membawa versi yang benar, tanpa perlu membaca file versi
atau API release saat runtime.

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

| Build system    |   Run 1 |   Run 2 |   Rata-rata |
| --------------- | ------: | ------: | ----------: |
| **rbot**        | 0.858 s | 0.814 s | **0.836 s** |
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

| Build system    |  Run 1 |  Run 2 |   Rata-rata |
| --------------- | -----: | -----: | ----------: |
| **rbot**        | 0.14 s | 0.13 s | **0.135 s** |
| **bear + make** | 1.41 s | 1.41 s |  **1.41 s** |

Benchmark ini merupakan pengukuran pada environment pengujian yang sama dan
bukan klaim performa universal. Hasil dapat berbeda tergantung hardware,
filesystem, toolchain, jumlah source, dan environment runtime.

## Buildfile

`rbot init` akan membuat `Buildfile` default berikut di project kamu:

```yaml
root: .

clean:
  - build: false
  - compdb: false # compile_commands.json (dulu: compileCommands)

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
  - compileCommands: auto # compile_commands.json (beda dari clean.compdb)
```

Bagian yang sering disesuaikan:

- **sources** — daftar direktori yang di-scan rekursif untuk file `.c`.
- **target** _(opsional)_ — arsitektur tujuan build, mis. `x86_64`, `arm64`,
  atau `riscv64`. Kosong berarti host (perilaku lama). Flag arsitektur
  ditambahkan otomatis sesuai toolchain (GNU/Clang: `-m64`,
  `-march=armv8-a`, `-march=rv64gc -mabi=lp64d`; Apple Clang: `-arch`;
  MSVC: `/ARCH` bila perlu) — juga tercatat di `compile_commands.json`.

  ```yaml
  target: arm64
  ```
- **headers** — tiap entri menjadi `-I<dir>`; `I.` shorthand untuk `-I.`
  (di MSVC otomatis menjadi `/I<dir>`).
- **library** _(opsional, tidak ada di default)_ — tiap entri menjadi
  `-l<nama>`, contoh `ssl` → `-lssl` (di MSVC otomatis menjadi `ssl.lib`).
- **output.binaryName / binaryDir** — nama & lokasi binary hasil build.
- **foreground** _(opsional, default `true`)_ — kendali Ctrl+C:

  ```yaml
  foreground: false
  ```

  Saat `true` (default), child build berbagi process group dengan rbot —
  Ctrl+C dari terminal menghentikan rbot dan compiler sekaligus (perilaku
  klasik). Saat `false`, child berjalan di process group terpisah: rbot
  menerima SIGINT sendiri, meneruskannya ke compiler, lalu membatalkan
  build dengan rapi. Ini membuat SIGINT bisa diterima lebih fleksibel
  (mis. untuk mencatat status sebelum keluar) tanpa menyisakan proses
  compiler yang masih berjalan.

## Build paralel (-jN)

```bash
rbot -j         # paralel, default sejumlah core CPU
rbot -j4        # paralel, 4 job
rbot --jobs=4
rbot clean -j2  # opsi boleh sebelum/sesudah command
```

Mirip `make -j`: hanya fase kompilasi yang diparalelkan; link, embedded,
dan fase library tetap berurutan. Setiap job mendapat process group
sendiri sehingga Ctrl+C diteruskan ke semua compiler yang sedang berjalan
dan build berhenti dengan rapi (exit code 130).

## Interrupt (Ctrl+C)

Dengan `foreground: false` (atau saat `-jN` aktif), rbot menangani SIGINT
sendiri: compiler yang sedang berjalan menerima sinyal, job baru tidak
dimulai, dan rbot keluar dengan status `interrupted` alih-alih error build.

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

| Macro                       | Arti                           |
| --------------------------- | ------------------------------ |
| `EMBED_<NAMA>_ARCHIVE_NAME` | nama file arsip                |
| `EMBED_<NAMA>_EXTRACT_DIR`  | isi `extract:` di Buildfile    |
| `EMBED_<NAMA>_SYMBOL`       | pointer ke byte pertama data   |
| `EMBED_<NAMA>_SYMBOL_END`   | pointer satu-byte-setelah-blok |
| `EMBED_<NAMA>_SYMBOL_LEN`   | panjang data dalam byte        |

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

| Jalur             | Kapan dipakai                          | Bentuk simbol                     |
| ----------------- | -------------------------------------- | --------------------------------- |
| `ld -r -b binary` | GNU ld tersedia (Linux/MinGW)          | `_binary_<path>_start/_end` nyata |
| Fallback C array  | MSVC, atau tanpa `ld` (`RBOT_NO_LD=1`) | `start[]` + `unsigned long _len`  |

Macro `EMBED_<NAMA>_*` di `build/embedded.h` sama persis di kedua jalur.
Freshness arsip dicek otomatis: perubahan pada file di `src:` akan memicu
build ulang arsip dan object.

## Library (Buildfile: output.library*)

Selain binary, rbot bisa memproduksi library statis dan/atau dinamis dari
object yang sama:

```yaml
output:
  - binaryName: rupa
  - binaryDir: bin
  - buildDir: build

  - libraryName: rupa # aktifkan library: lib/librupa.a
  - libDir: lib # direktori hasil (default "lib")
  - libraryShared: true # + lib/librupa.so (perlu -fPIC saat kompilasi)
```

- `libraryName` mengaktifkan **statis**: `lib<name>.a` (GNU/Clang) atau
  `<name>.lib` (MSVC).
- `libraryShared: true` menambah varian **dinamis**: `lib<name>.so`
  (Linux), `.dylib` (macOS), `<name>.dll` (Windows). Saat aktif, semua
  source otomatis dikompilasi dengan `-fPIC` sehingga object tetap bisa
  dipakai link binary.
- **exclude** — melepas source dari pengemasan library tanpa memengaruhi
  binary (mis. `main.c` milik executable):

  ```yaml
  exclude:
    - main.c
  ```

  Cocok berdasarkan nama file (`main.c`), path (`src/main.c`), atau
  prefix direktori (`src/tools/`).

Keputusan build library incremental: fase library dilewati bila semua
varian yang diminta sudah lebih baru daripada seluruh object inputnya;
object perantara tidak dihapus sehingga build berikutnya tetap murah.
`rbot clean` (dengan `clean.build: true`) ikut menghapus `libDir`.

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
