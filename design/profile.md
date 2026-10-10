# Desain Profil rbot

## Tujuan

`rbot profile <context>` adalah antarmuka diagnostik untuk mengukur biaya nyata dari operasi rbot. Ini bukan mode build alternatif dan tidak boleh memerlukan override Buildfile hanya untuk memprofilkan direktori yang sudah ada.

Output terminal harus tetap singkat dan berguna untuk eksperimen cepat. `profile.md` adalah desain/spesifikasi rinci untuk fitur profile dan juga dapat menjelaskan format laporan rinci yang dihasilkan oleh sebuah profile run.

## Prinsip Utama

**Profil apa yang ada; jangan membangun konfigurasi alternatif hanya untuk mengukurnya.**

Contoh:

```
rbot profile archive=rupamod
rbot profile archive=rupamod with=tar
rbot profile archive=rupamod with=tar,gz
```

Direktori target hanya perlu ada. Perintah tidak memerlukan override archive di Buildfile.

## CLI

Bentuk dasar:

```
rbot profile <context> [options]
```

Context dipilih dengan ekspresi `name=value`:

```
project=<dir>
workspace=<dir>
archive=<dir>
library=<dir>
embed=<dir>
binary=<dir>
```

Contoh:

```
rbot profile project=.
rbot profile workspace=.
rbot profile archive=rupamod
rbot profile archive=rupamod with=tar
rbot profile archive=rupamod with=tar,gz
rbot profile library=rupamod
rbot profile embed=rupamod
rbot profile binary=.
```

`with=` bersifat spesifik terhadap context. Ini hanya boleh diterima oleh context yang memiliki backend atau operasi yang dapat dipilih secara bermakna.

Contoh archive awalnya sengaja dibuat langsung:

```
archive=<directory>
```

Tidak ada keharusan untuk memodifikasi atau meng-override konfigurasi Buildfile.

## Mesin Profil (Profile Engine)

Perintah profile harus menggunakan kembali mesin profiling/timing ringan yang sudah ada di rbot, bukan memperkenalkan subsistem timing kedua.

Secara konseptual:

```
CLI

|
v
profile parser

|
+---- context

|
+---- options

|
v
profile engine

|
+---- marks / elapsed time
+---- counters
+---- phase records

|
+------------+-------------+
v                          v
terminal                  report
```

Mesin bertanggung jawab atas pengukuran. Context handler bertanggung jawab untuk menentukan operasi mana yang diukur.

## Aturan Tidak Mengubah State (Non-mutating Rule)

`rbot profile ...` tidak boleh memodifikasi state build normal.

Secara khusus, profiling tidak boleh:

- memodifikasi `.rbot/build.state`
- memodifikasi fingerprint build
- memodifikasi Buildfile
- membuat objek build normal
- membuat binary normal
- memperbarui cache build normal
- mengubah hasil incremental build berikutnya

Data profiling sementara, jika tidak dapat dihindari, harus diisolasi dari state build normal dan dihapus setelah profiling.

Jika memungkinkan, profiling harus melakukan streaming atau menggunakan output sementara agar pengukuran itu sendiri tidak menimbulkan overhead filesystem yang signifikan.

## Output Terminal

Output terminal adalah antarmuka ringkasan. Output ini harus menjawab:

> "Apa yang mahal?"

tanpa membuang setiap detail internal.

Contoh laporan archive:

```
Profile : archive=rupamod with=tar,gz

collect       12.4 ms
tar          104.7 ms
gzip         283.1 ms
total        400.8 ms
```

Contoh laporan workspace:

```
Profile : workspace=.

parse          8.2 ms
dependency    31.4 ms
library       82.7 ms
archive      311.5 ms
embed         44.2 ms
binary        19.8 ms
total        497.8 ms
```

Nama fase dan angka pastinya bergantung pada implementasi context.

## Laporan Rinci

Laporan profil rinci ditujukan untuk `profile.md` atau file laporan lainnya. Laporan harus memuat informasi yang jauh lebih banyak daripada ringkasan terminal.

Contoh:

```
rbot profile report
===================

context

---

type       : archive
target     : rupamod
directory  : /path/to/rupamod
backend    : tar,gz

environment

---

rbot       : 0.2.2
platform   : linux/aarch64
compiler   : gcc
cwd        : /path/to/workspace

input

---

files      : 1,204
directories: 87
bytes      : 18,442,193

phases

---

collect
time     : 12.4 ms

tar
time     : 104.7 ms
input    : 18,442,193 bytes
output   : 18,721,024 bytes

gzip
time     : 283.1 ms
input    : 18,721,024 bytes
output   : 4,382,912 bytes

summary

---

total      : 400.8 ms
dominant   : gzip
```

Format laporan rinci dapat berkembang seiring bertambahnya context. Field harus tetap mengutamakan keterbacaan manusia; output yang dapat dibaca mesin dapat ditambahkan sebagai format terpisah nanti, bukan dengan membuat laporan normal sulit dibaca.

## Output Laporan

Perintah profile tidak boleh secara otomatis membuat file `profile.md` yang persisten pada setiap pemanggilan. File laporan otomatis akan menambah I/O filesystem ke benchmark dan dapat mengkontaminasi pengukuran.

Untuk laporan rinci, opsi eksplisit di masa depan dapat digunakan:

```
rbot profile archive=rupamod --report profile.md
```

Sampai opsi tersebut ada, redirection shell normal sudah cukup:

```
rbot profile archive=rupamod > profile.md
```

Implementasi harus membedakan output profil normal dari file internal sementara apa pun sehingga pembuatan laporan tidak menjadi bagian dari operasi yang diukur.

## Model Context

Setiap context memiliki sekumpulan fase.

**Archive:**

```
archive
+-- discover
+-- collect
+-- tar
+-- gzip
```

**Project:**

```
project
+-- config
+-- fingerprint
+-- decide
+-- compile
+-- link
```

**Workspace:**

```
workspace
+-- parse
+-- dependency
+-- library
+-- archive
+-- embed
+-- binary
```

Ini adalah grup fase konseptual. Sebuah context hanya boleh mengekspos fase yang benar-benar diimplementasikan dan dapat diukur.

Tujuannya adalah memungkinkan peta biaya workspace, misalnya:

```
workspace cold
|
+-- compile       66.7 s
+-- archive        0.4 s
+-- library        0.8 s
+-- embed          0.2 s
+-- binary         0.1 s
```
atau, jika kompresi mendominasi:

```
archive             4.0 s
+-- tar            0.8 s
+-- gzip           3.2 s   <-- kandidat optimasi
```

`profile` karena itu adalah alat pengukuran, bukan pengoptimal. Optimasi sebaiknya hanya dilakukan setelah profiling mengidentifikasi biaya yang bermakna.

## Opsi Spesifik Context

Opsi tidak harus valid secara global.

Misalnya:

```
rbot profile archive=rupamod with=tar,gz
```

bermakna, sedangkan:

```
rbot profile project=. with=tar
```

harus ditolak jika `project` tidak mendefinisikan backend `with`.

Parser harus melaporkan kombinasi context/opsi yang tidak valid dengan jelas, misalnya:

```
rbot: profile option 'with' is not valid for context 'project'
```

## Opsi Masa Depan

Opsi berikut adalah kandidat, bukan persyaratan untuk implementasi pertama:

```
--report <file>
--repeat <N>
```

`--repeat` harus dirancang dengan hati-hati agar pengukuran berulang tidak secara tidak sengaja mengubah perilaku cache/state atau mengubah satu pemanggilan profile menjadi workload yang berbeda.

## Kemungkinan Context Masa Depan

Context agregat di masa depan dapat menyediakan:

```
rbot profile workspace=.
```

dengan laporan yang memuat fase workspace yang relevan. Perintah `profile all` terpisah tidak diperlukan pada awalnya dan tidak boleh ditambahkan sampai context individual dapat diandalkan.

## Batasan Desain

1. Jaga agar perintah cukup murah untuk digunakan selama investigasi performa.
2. Gunakan kembali mesin profile/timing yang sudah ada.
3. Jangan duplikasi logika build-state hanya untuk profiling.
4. Jangan memerlukan perubahan Buildfile untuk profiling direktori secara langsung.
5. Jangan pernah secara diam-diam memutasi state build normal.
6. Jaga output terminal tetap ringkas.
7. Letakkan informasi diagnostik rinci pada output laporan eksplisit.
8. Tambahkan context secara bertahap; jangan mengarang pengukuran yang tidak nyata.
9. Buat batas fase eksplisit agar biaya workspace dapat diatribusikan.
10. Utamakan pengukuran daripada asumsi saat memutuskan apa yang akan dioptimalkan.

## Prioritas Awal

Context pertama yang berguna seharusnya adalah yang membantu menjelaskan biaya workspace:

```
archive=<dir>
library=<dir>
embed=<dir>
binary=<dir>
workspace=<dir>
```

Archive adalah kasus konkret pertama karena memungkinkan perbandingan langsung operasi individual seperti tar dan gzip tanpa mengubah Buildfile.

Alur kerja yang dimaksudkan adalah:

1. Profil satu fase secara langsung.
2. Identifikasi sub-fase dominannya.
3. Bandingkan biaya terukur dengan benchmark workspace lengkap.
4. Baru setelah itu putuskan apakah optimasi implementasi dibenarkan.

Ini menjaga `rbot profile` tetap berguna sebagai laboratorium untuk performa workspace tanpa mengubah profiling itu sendiri menjadi jalur build lain.
