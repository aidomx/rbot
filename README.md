# rbot

Build tool sederhana untuk project C, didorong oleh satu file konfigurasi
deklaratif bernama `Buildfile`. Tidak perlu Makefile — cukup `rbot`.

## Instalasi

```bash
curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh
```

Script di atas mengunduh binary `rbot` yang sudah di-build (`bin/rbot` di
repo ini) dan memasangnya ke `/usr/bin/rbot`.

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
- **library** _(opsional, tidak ada di default)_ — tiap entri jadi `-l<nama>`,
  contoh `ssl` → `-lssl`.
- **output.binaryName / binaryDir** — nama & lokasi binary hasil build.
- **embedded** _(opsional)_ — arsipkan sebuah direktori lalu embed jadi
  object file ke binary via `ld -r -b binary`; berguna untuk menyertakan
  aset/module tanpa file eksternal saat runtime.

## `.version`

File `.version` di root project menandai versi implementasi rbot yang
aktif (mis. `v0.1.0`). Ini dipakai oleh dispatcher (`src/main.c` pada
source rbot sendiri) untuk mendelegasikan ke implementasi yang sesuai, dan
oleh `rbot version` untuk ditampilkan ke pengguna.

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
