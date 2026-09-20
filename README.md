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
(cl.exe, via Developer Command Prompt) maupun MinGW (gcc):

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
- **headers** — tiap entri jadi `-I<dir>`; `I.` shorthand untuk `-I.`.
- **headers** — tiap entri jadi `-I<dir>`; `I.` shorthand untuk `-I.`
  (di MSVC otomatis jadi `/I<dir>`).
- **library** _(opsional, tidak ada di default)_ — tiap entri jadi `-l<nama>`,
  contoh `ssl` → `-lssl` (di MSVC otomatis jadi `ssl.lib`).
- **output.binaryName / binaryDir** — nama & lokasi binary hasil build.
## Embedded (Buildfile: embedded)

Fitur embedded mengarsipkan sebuah direktori lalu menempelkannya ke binary
— hasil build tetap satu file executable tanpa aset eksternal.

### Menulis di Buildfile

```yaml
embedded:
  - assets:              # nama entri (bebas; jadi prefix macro EMBED_ASSETS_*)
    - src: assets        # direktori yang diarsipkan
    - pattern: .txt      # (opsional) pola scan freshness; kosong = semua file
    - extract: /tmp/rupa-assets  # (opsional) catatan direktori ekstraksi runtime
    - archive:
      - dir: modules     # lokasi arsip (default: modules)
      - name: demo_assets # nama arsip (default: nama entri)
      - with:
        - tar: true      # true = tar[.gz], false = gzip langsung
        - ext: gz        # ekstensi tambahan (gz)
```

Hasil di direktori project:

```
modules/demo_assets.tar.gz  # arsip aset
build/demo_assets.o         # object hasil embed (di-link ke binary)
build/embedded.h            # macro & deklarasi simbol — di-include kode kamu
```

### Memakai di kode

Include `build/embedded.h` (tambahkan `build` ke `headers` di Buildfile),
lalu pakai macro per entri `<NAMA>` (uppercase nama entri):

| Macro | Arti |
|---|---|
| `EMBED_<NAMA>_ARCHIVE_NAME` | nama file arsip (`"demo_assets.tar.gz"`) |
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

  /* data juga bisa langsung ditulis kembali ke disk saat runtime */
  FILE *f = fopen("/tmp/restore.tar.gz", "wb");
  fwrite(EMBED_ASSETS_SYMBOL, 1, EMBED_ASSETS_SYMBOL_LEN, f);
  fclose(f);
  return 0;
}
```

### Dua jalur embed (otomatis)

| Jalur | Kapan dipakai | Bentuk simbol |
|---|---|---|
| `ld -r -b binary` | GNU ld tersedia (Linux/MinGW) | `_binary_<path>_start/_end` nyata |
| Fallback C array | MSVC, atau tanpa `ld` (paksa: env `RBOT_NO_LD=1`) | `start[]` + `unsigned long _len` |

Macro `EMBED_<NAMA>_*` di `build/embedded.h` sama persis di kedua jalur —
kode konsumen tidak perlu tahu jalur mana yang aktif (header bahkan
menerbitkan deklarasi `extern` simbolnya). Freshness arsip dicek otomatis:
berubah satu file di `src:` saja, arsip & object di-build ulang; macro
`EMBED_<NAMA>_SYMBOL_LEN` di jalur fallback dihitung saat build, jadi ukuran
selalu cocok.

## `.version`

File `.version` di root project menandai versi implementasi rbot yang
aktif (mis. `v0.1.0`). Ini dipakai oleh dispatcher (`src/main.c` pada
source rbot sendiri) untuk mendelegasikan ke implementasi yang sesuai, dan
oleh `rbot version` untuk ditampilkan ke pengguna.

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
