# Desain: Platform & Compiler Specific Properties (Flags, Library, Headers)

## 1. Latar Belakang

Saat ini, konfigurasi di `Buildfile` bersifat global. Jika sebuah proyek membutuhkan _flag_ compiler, _library_, atau _header_ yang berbeda tergantung pada sistem operasi (OS) atau compiler yang digunakan, developer terpaksa menulis logika skrip eksternal atau membuat file `Buildfile` terpisah.

Desain ini memperkenalkan **Suffix Spesifik** (`.platform` dan `.compiler`) pada properti di `Buildfile`. Ini memungkinkan `rbot` secara otomatis memilih dan menggabungkan konfigurasi yang tepat berdasarkan lingkungan _host_ saat ini, tanpa memerlukan blok kondisional (seperti `ifeq` di Makefile atau `if (WIN32)` di CMake).

---

## 2. Sintaks & Penamaan

Properti yang mendukung sufiks spesifik meliputi: `flags`, `library`, `headers`, `defines`, dan `std`.

Format penulisan:

```text
<properti> = <nilai_global>
<properti>.<target> = <nilai_spesifik>
```

### Target yang Didukung

`rbot` akan mengenali target berikut secara _native_:

**Berdasarkan Platform (OS):**

- `linux` (termasuk Android/Termux)
- `macos` (Darwin)
- `windows` (MSVC/MinGW)
- `freebsd`
- `unix` (Fallback untuk semua OS berbasis POSIX jika tidak ada definisi spesifik)

**Berdasarkan Compiler:**

- `gcc`
- `clang`
- `cl` (MSVC)

---

## 3. Aturan Resolusi & Merging (Penggabungan)

Ketika `rbot` memproses `Buildfile`, dia akan menerapkan aturan **Append & Merge** (Global ditambahkan dengan Spesifik).

1. **Prioritas:**
   Nilai Global selalu dimuat pertama kali.
2. **Penggabungan (Merging):**
   Jika target spesifik (misal `flags.linux`) didefinisikan, nilainya akan **ditambahkan (append)** ke nilai global.
3. **Deduplikasi:**
   Untuk properti berbasis array (seperti `flags` dan `library`), `rbot` akan otomatis menghapus nilai duplikat untuk menjaga baris perintah compiler tetap bersih.

**Contoh Logika Resolusi:**
Jika `Buildfile` berisi:

```text
flags = Wall, Wextra
flags.linux = D_REENTRANT
flags.gcc = O2
```

Dan user menjalankan `rbot` di **Linux menggunakan GCC**, maka array `flags` final yang dikirim ke compiler adalah:
`-Wall -Wextra -D_REENTRANT -O2`

---

## 4. Contoh Implementasi Nyata

Berikut adalah bagaimana desain ini menyederhanakan proyek _cross-platform_ (seperti Node.js atau proyek C/C++ kompleks) menjadi sangat bersih:

```text
use project

compiler = gcc, clang, cl
std = c11

# 1. Flags Global (Berlaku di mana saja)
flags = Wall, Wextra, MMD, MP

# 2. Flags Spesifik Platform
# Linux/POSIX
flags.linux = D_POSIX_C_SOURCE=200809L
library.linux = pthread, m, dl

# macOS
flags.macos = D_DARWIN_C_SOURCE
library.macos = pthread, m

# Windows (MSVC/MinGW)
flags.windows = D_CRT_SECURE_NO_WARNINGS, DWIN32_LEAN_AND_MEAN
library.windows = ws2_32, advapi32, userenv
headers.windows = include/win32

# 3. Flags Spesifik Compiler (Jika diperlukan optimasi khusus)
flags.clang = fcolor-diagnostics
flags.gcc = fdiagnostics-color=always
```

---

## 5. Keuntungan Desain

1. **Zero Shell Scripting:** Tidak ada lagi `$(shell uname)` atau `ifeq ($(OS),Windows_NT)`. File konfigurasi murni deklaratif.
2. **Readability (Keterbacaan):** Developer dapat melihat semua kebutuhan _cross-platform_ dalam satu pandangan sekilas (single view).
3. **Maintainability:** Menambahkan dukungan untuk OS baru (misal `freebsd`) hanya membutuhkan penambahan 1-2 baris, tanpa menyentuh logika proyek yang sudah ada.

---

## 6. Catatan Implementasi (Untuk Core C `rbot`)

- Deteksi platform saat ini harus dilakukan di awal fase `Parse` atau `Apply` (menggunakan `#ifdef _WIN32`, `uname()`, dll).
- Parser `Buildfile` perlu dimodifikasi untuk memisahkan _key_ dan _suffix_ saat menemukan tanda titik (`.`).
- Jika user mendefinisikan `flags.android` tetapi `rbot` berjalan di Android, sistem harus cukup cerdas untuk memetakannya ke `flags.linux` (karena Android adalah turunan Linux), atau secara eksplisit mendukung `android` sebagai target.

---

## 2.1 Preprocessor Defines (`defines`)

Selain `flags`, `rbot` menyediakan properti khusus `defines` untuk menangani makro preprocessor (seperti `-DFOO` atau `-DVERSION="1.0"`). Memisahkan `defines` dari `flags` membuat `Buildfile` jauh lebih bersih dan mudah untuk dibaca.

**Sintaks:**

```text
defines = MACRO1, MACRO2=value
defines.<platform> = MACRO_SPESIFIK
```

**Aturan Translasi Otomatis (Compiler Abstraction):**
User tidak perlu menulis prefix `-D` atau `/D`. `rbot` akan secara otomatis menerjemahkannya berdasarkan compiler yang terdeteksi:

- **GCC / Clang:** `defines = DEBUG` akan diterjemahkan menjadi `-DDEBUG`.
- **MSVC (cl):** `defines = DEBUG` akan diterjemahkan menjadi `/DDEBUG`.

**Contoh Implementasi:**

```text
# Defines Global
defines = DEBUG, VERSION=\"1.0.0\"

# Defines Spesifik Platform
defines.linux = _POSIX_C_SOURCE=200809L
defines.windows = UNICODE, _UNICODE, WIN32_LEAN_AND_MEAN

# Defines Spesifik Compiler
defines.clang = __STDC_CONSTANT_MACROS
```

**Keuntungan Desain Ini:**

1. **Zero Boilerplate:** User tidak perlu menulis `-D` berulang-ulang.
2. **Cross-Compiler Safe:** Tidak ada lagi error `cl: Command line warning D9002: ignoring unknown option '-D'` saat build di Windows menggunakan MSVC, karena `rbot` yang mengurus konversinya.
