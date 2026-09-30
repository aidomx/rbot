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
rbot -f lain    # build memakai file konfigurasi lain (bila diperlukan)
rbot init       # buat Buildfile default kalau project belum punya
rbot clean      # bersihkan build dir, file library (.a/.so), compile_commands.json & .rbot/deps.cache (folder lib/ tetap)
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

## Header tracking

rbot melacak dependensi header secara otomatis: source dikompilasi ulang
bila `.c`-nya **atau header apa pun yang di-include-nya (transitif)** lebih
baru daripada object-nya. Tidak perlu konfigurasi tambahan.

- `#include "x.h"` dicari di direktori file pengguna dulu, lalu di direktori
  `headers` Buildfile; `#include <x.h>` hanya di direktori `headers`.
- Header yang tidak ada di direktori project (mis. `<stdio.h>`) dianggap
  header sistem dan diabaikan.
- Pemindai mengabaikan komentar dan `#if`/`#ifdef`, jadi hasilnya bisa
  sedikit berlebih (kompilasi ulang yang tak perlu), tidak pernah kurang.
  `#include` yang memakai macro tidak terdeteksi.
- Header hanya dipindai untuk source yang lolos cek mtime `.c`, dan setiap
  header dibaca sekali per build — no-op build tetap murah.
- Saat ada yang usang karena header, rbot mencetak
  `> Headers   : N source(s) stale due to header change`.

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

### Benchmark otomatis (bench/compare.sh)

Script `bench/compare.sh` membuat project sintetis (N source + header
berantai), menuliskan `build.ninja` + `Makefile` + `Buildfile` untuk project
yang sama, lalu mengukur ninja, make, `bear -- make`, dan rbot pada skenario
setara:

```bash
./bench/compare.sh                  # default: 150 source, 30 header, 2 run
./bench/compare.sh -n 40 -H 10      # project kecil, cepat
./bench/compare.sh -s noop,touch    # subset skenario
./bench/compare.sh -k               # simpan artefak di sys.tmp/
```

Skenario: `cold` (build dari nol), `noop` (tanpa perubahan), `touch`
(header disentuh, konten sama — rbot: 0 recompile), `edit` (ubah 1 source),
`compdb` (generate `compile_commands.json`), `strace` (hitung syscall no-op).
Hasil: tabel rerata + `sys.tmp/results.csv`.

Contoh hasil (20 source / 6 header, proot-distro, 1 run — angka untuk
melacak regresi rbot, bukan klaim universal):

| Skenario | ninja | make | bear+make | rbot |
|---|---:|---:|---:|---:|
| cold (ms) | 2533 | 2240 | 9353 | **1590** |
| noop (ms) | 212 | 139 | 1103 | 144 |
| touch (ms) | 1690 | 1253 | 6210 | **102** |
| edit (ms) | 1851 | 992 | 3369 | **796** |
| syscall noop | 11363 | 352 | 6197 | **310** |

### Optimasi environment proot

Pada proot-distro (Termux) tiap syscall melewati ptrace sehingga biayanya
puluhan kali lebih mahal dari Linux biasa. Optimasi berbasis pengurangan
syscall pada project uji 151 source / 30 header (kernel
`6.17.0-PRoot-Distro`):

| Aspek | Sebelum | Sesudah |
|---|---|---|
| No-op build | 0.80–0.94 s | **0.33–0.72 s** |
| Total syscall | 4800 | **~1500** |
| openat (file dibuka) | 670 | **50–80** |
| read (byte dibaca) | 1339 | **~100** |

Penghematan utama:

- `.rbot/deps.cache` dimuat **satu pass** (snapshot hash + cache edges
  sekaligus, tidak dibuka dua kali);
- cache edges `!e/!k` — file `.d` tidak dibuka ulang selama mtime sama
  (satu stat menggantikan open+read per source);
- header anak edges `.d` diverifikasi shallow tanpa dibuka/dipindai;
- pass rekam setelah build sukses **dilewati pada no-op murni** (tidak ada
  yang berubah sejak rekam terakhir) — membuang DFS + ribuan pembaruan
  snapshot yang murni CPU; dan pass rekam memakai hash terekam selama
  mtime sama (build no-op = nol baca isi).

### Optimasi kinerja (clean & no-op)

- **Clean build paralel secara default** — kompilasi memakai semua core CPU
  tanpa perlu `-j` (`-j1` untuk serial).
- **Tanpa `/bin/sh` per job** — command kompilasi/link yang polos (tanpa
  quote, variabel, glob, pipe) dijalankan langsung lewat `posix_spawn`: satu
  `exec` per job, bukan dua (shell + compiler), dan tanpa menyalin page table
  proses rbot. Command yang butuh shell tetap lewat `sh -c` seperti dulu.
- **No-op tidak lagi menulis apa pun** — cache edges (`!e/!k`) kini selalu
  ter-indeks penuh; sebelumnya sebagian entri terlewat sehingga file `.d`
  dibuka ulang dan `.rbot/deps.cache` ditulis ulang di setiap build.
- Source tanpa header project (hanya header sistem) sekarang tercatat sah di
  cache (`!e <src> <mtime> 0`), tidak lagi dianggap "cache meleset".
- Object (`.o`) tidak lagi tercatat sebagai dependensi source-nya sendiri
  (bug offset pada pengecekan target `.o` di parser `.d`): satu `stat` lebih
  sedikit per source, dan isi object tidak lagi di-hash ke snapshot.
- `stat` per entri direktori diganti `d_type` dari `readdir`; `mkdir` berulang
  per object dilewati; mtime object/source dari fase klasifikasi dipakai ulang
  saat keputusan link, jadi no-op tidak men-stat ulang semua object.

## Buildfile

`rbot init` akan membuat `Buildfile` default berikut di project kamu
(format baru `use alias`; format lama berbasis section `key:` juga tetap
diterima — lihat bagian di bawah):

```yaml
use alias

clean as c
output as o

root = .

c.build = false
c.compdb = false # compile_commands.json

sources = src

flags = Wall, Wextra, MMD, MP # MMD: dep file <obj>.d (GNU/Clang); MP: phony target

std = gnu11

headers = include, I.

compiler = gcc, clang

progress.bar = true
progress.error = always

o.binaryName = app
o.binaryDir = bin
o.buildDir = build
o.compileCommands = auto # compile_commands.json
```

Bagian yang sering disesuaikan:

- **sources** — daftar direktori yang di-scan rekursif untuk file `.c`.
- **target** _(opsional)_ — arsitektur tujuan build, mis. `x86_64`, `arm64`,
  atau `riscv64`. Kosong berarti host (perilaku lama). Flag arsitektur
  ditambahkan otomatis sesuai toolchain (GNU/Clang: `-m64`,
  `-march=armv8-a`, `-march=rv64gc -mabi=lp64d`; Apple Clang: `-arch`;
  MSVC: `/ARCH` bila perlu) — juga tercatat di `compile_commands.json`.

  ```yaml
  target = arm64
  ```

- **headers** — tiap entri menjadi `-I<dir>`; `I.` shorthand untuk `-I.`
  (di MSVC otomatis menjadi `/I<dir>`).
- **flags** — flag compiler. Direkomendasikan untuk pelacakan header penuh:

  ```yaml
  flags = Wall, Wextra, MMD, MP # MMD: dep file <obj>.d (GNU/Clang); MP: phony target
  ```

  Dengan `-MMD`, tiap kompilasi menghasilkan `<obj>.d` di build dir —
  berguna untuk editor/IDE dan debugger build. rbot sendiri tidak bergantung
  pada file itu untuk keputusan incremental (lihat pelacakan header di
  bawah); di MSVC flag `-M*` dilewati otomatis.
- **library** _(opsional, tidak ada di default)_ — tiap entri menjadi
  `-l<nama>`, contoh `ssl` → `-lssl` (di MSVC otomatis menjadi `ssl.lib`).
  Di format baru: `library = ssl, crypto`, atau per platform
  `library.linux = m, pthread`.
- **o.binaryName / o.binaryDir** (alias kanonik `output.*`) — nama & lokasi
  binary hasil build.
- **foreground** _(opsional, default `true`)_ — kendali Ctrl+C:

  ```yaml
  foreground = false
  ```

  Saat `true` (default), child build berbagi process group dengan rbot —
  Ctrl+C dari terminal menghentikan rbot dan compiler sekaligus (perilaku
  klasik). Saat `false`, child berjalan di process group terpisah: rbot
  menerima SIGINT sendiri, meneruskannya ke compiler, lalu membatalkan
  build dengan rapi. Ini membuat SIGINT bisa diterima lebih fleksibel
  (mis. untuk mencatat status sebelum keluar) tanpa menyisakan proses
  compiler yang masih berjalan.

## Build paralel (-jN)

Tanpa opsi `-j`, rbot langsung paralel sebanyak core CPU (seperti `ninja`).
Pakai `-j1` untuk build serial.

```bash
rbot            # paralel, sejumlah core CPU (default)
rbot -j         # sama seperti di atas
rbot -j4        # paralel, 4 job
rbot -j1        # serial
rbot --jobs=4
rbot clean -j2  # opsi boleh sebelum/sesudah command
```

Mirip `make -j`: hanya fase kompilasi yang diparalelkan; link, embedded,
dan fase library tetap berurutan. Setiap job mendapat process group
sendiri sehingga Ctrl+C diteruskan ke semua compiler yang sedang berjalan
dan build berhenti dengan rapi (exit code 130).

## Buildfile format baru (`use alias`)

Buildfile format baru tetap bernama `Buildfile` — cukup diawali baris
`use alias`, dan rbot membacanya otomatis tanpa opsi apa pun. Opsi
`-f <file>` hanya diperlukan bila memakai nama file konfigurasi lain
(bentuk rapat `-f<file>` juga bisa):

```bash
rbot                # Buildfile (format baru maupun lama) terbaca otomatis
rbot -f lain        # pakai nama file lain, bila memang diperlukan
rbot -flain clean   # bentuk rapat; command apa pun menghormati -f
```

File konfigurasi diawali baris `use alias` memakai format flat
`key = value` (kualifikasi dengan titik) plus definisi alias `X as Y`.
Semantiknya seperti alias pada umumnya: **nama kanonik tetap berlaku**,
alias hanya nama kedua — dan bisa berantai:

```yaml
use alias

clean as c                # c.build = ... juga berarti clean.build
embedded.modules as mod   # mod.src = ... berarti embedded.modules.src
mod.archive as archive    # chain: archive.name = ... -> e.modules.archive

root = .
c.build = false
sources = src, tools
flags = Wall, O2, MMD, MP

o.binaryName = app
o.binaryDir = bin
```

Kunci `key` tanpa titik pada baris menjorok mewarisi prefix section yang
terbaru; komentar `//` didukung di format ini (komentar `#` didukung di
semua format). `Buildfile` di repo ini sendiri memakai format baru —
lihat isinya untuk contoh nyata.

Tanpa baris `use alias`, parser lama berbasis section/indentasi tetap
dipakai persis seperti biasa — kedua format tidak saling mengganggu.
`-f` dengan path yang memuat direktori membuat rbot masuk ke direktori
tersebut dulu (gaya `make -C`), jadi sources/headers/output tetap
relatif terhadap lokasi Buildfile.

## Lintas konfigurasi (`-xf` / `-xcf`)

rbot bisa meniru project yang sudah punya `build.ninja` (hasil CMake
generator Ninja, atau tulisan tangan): file itu dikonversi menjadi
Buildfile format baru, lalu command dijalankan dengan Buildfile hasil
konversi.

```bash
rbot -xf nbuild/build.ninja       # konversi -> build -> hapus Buildfile.xf.tmp
rbot -xcf nbuild/build.ninja      # konversi -> build, Buildfile tetap ada
rbot -xf nbuild/build.ninja clean # bekerja untuk command apa pun
```

Aturan translasi:

- tiap edge compile (`-c ... -o <obj>.o`) jadi source rbot; flag
  `-D/-O/-W/-f` diteruskan, `-std=...` jadi `std`, `-I` jadi `headers`;
- edge link dipakai menebak `o.binaryName`;
- output `lib/lib<name>.a|.so` jadi `o.libraryName` (+
  `o.libraryStatic`/`o.libraryShared` sesuai varian yang ada);
- token `-l` pada rule link (termasuk lewat variabel seperti `$libs`)
  jadi `lib.default`/`lib.linux`/`lib.macos` (`m`/`pthread` diterjemahkan
  per platform; `lib.windows` disunting manual);
- chain embedded (arsip -> file `.c` hasil-generate -> object) jadi
  `embedded.<name>.src/.dir/.with.tar/.with.ext` — object embed tidak
  masuk daftar source;
- path absolut di bawah cwd di-relatif-kan; rule template CMake
  (`$FLAGS`, `$INCLUDES`, `${...}`, `include rules.ninja`) diekspansi;
- edge custom command (ar, ld, cmake -E, dsb.) diabaikan — link & library
  dikerjakan rbot sendiri.

**Buildfile tidak pernah tertimpa**: `-xf` menulis ke `Buildfile.xf.tmp`
(sementara, dihapus setelah command — Buildfile eksisting aman), sedangkan
`-xcf` menulis ke `Buildfile` dengan konfirmasi `[y/N]` dulu bila file itu
sudah ada; jawaban selain `y` membatalkan tanpa mengubah apa pun.

## Pelacakan header (incremental)

Mengubah header tidak lagi mengharuskan build penuh yang membabi buta.

Sumber dependensi header dipilih otomatis per source:

1. **file `.d` dari compiler** — dengan flag `-MMD -MP` (GNU/Clang),
   daftar header yang ditulis compiler dipakai langsung. Paling akurat:
   compiler yang meresolusi `#if`, makro, dan computed include, sehingga
   header yang tidak benar-benar dipakai (mis. di balik `#if 0`) tidak
   memicu kompilasi ulang. Bila `.d` basi (source lebih baru) atau belum
   ada, rbot jatuh ke pemindai `#include`.
2. **pemindai `#include` (fallback)** — dipakai untuk build pertama,
   MSVC (tanpa `-MMD`), atau saat `.d` belum ada. Hasilnya bisa sedikit
   berlebih (mengabaikan `#if`), tidak pernah kurang.

Keputusan build dua lapis:

1. **mtime** — object dianggap usang bila salah satu header dependensi
   lebih baru daripada object.
2. **konten** — bila mtime mengatakan stale (mis. setelah `touch include/app.h`),
   hash isi source + seluruh dependensinya dibandingkan dengan snapshot dari
   build sukses terakhir (`.rbot/deps.cache`). Isi tidak berubah → build
   dilewati, tanpa kompilasi ulang.

```text
> Headers   : 4 source(s) skipped (touched, content unchanged)
Compiled : 0
Skipped  : 5
```

Snapshot hanya direkam setelah build sukses, jadi object yang gagal
kompilasi tidak pernah dianggap segar. `rbot clean` menghapusnya juga —
verifikasi hash pada build berikutnya dimulai dari nol, lalu snapshot
direkam ulang saat build sukses.

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
- Setiap varian dicek **terpisah**: bila seluruh object inputnya lebih
  lama dari target, varian itu dilewati (`up-to-date`) tanpa menjalankan
  `ar`/linker lagi — menghapus `.so` saja tidak meng-rebuild `.a`, dan
  build no-op tidak menyentuh library sama sekali.
- `rbot clean` menghapus **file** library (`lib<name>.a`/`.so`) tanpa
  menghapus folder `libDir`-nya — dulu seluruh folder ikut terhapus.
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

## Lisensi

MIT — lihat [LICENSE](./LICENSE).
