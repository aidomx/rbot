#ifndef RBOT_V0_1_0_EMBED_H
#define RBOT_V0_1_0_EMBED_H

#include <stdbool.h>

#include "config.h"

/*
 * Embedding bersifat generik: tiap entri embedded mengarsipkan direktori
 * src-nya, mengonversi arsip jadi object file, dan mendapat sekumpulan macro
 * EMBED_<n>_* yang diterbitkan ke build/embedded.h. Tidak ada bagian di sini
 * yang tahu soal project tertentu — konsumen (mis. loader rupa) include
 * header itu dan pakai macro untuk entri yang mereka pedulikan.
 *
 * Dua jalur konversi arsip -> object:
 *   1. GNU ld (Linux/MinGW): `ld -r -b binary` -> simbol nyata
 *      _binary_<path>_start/_end; LEN = (end - start).
 *   2. Fallback portabel (MSVC/tanpa ld, atau RBOT_NO_LD=1): arsip diubah
 *      jadi file C berisi array byte (build/<name>.embed.c) dengan simbol
 *      _binary_<path>_start[] + _binary_<path>_len; LEN = simbol _len.
 *   Keduanya diekspos lewat macro yang sama di build/embedded.h, lengkap
 *   dengan deklarasi extern-nya, jadi kode konsumen identik di dua jalur.
 */

/*
 * Terbitkan build/embedded.h agar kode runtime pakai simbol yang sama
 * persis. File hanya ditulis ulang saat isinya berubah; *changed memberi
 * tahu pemanggil bahwa source yang mereferensikan simbol itu perlu
 * dikompilasi ulang.
 */
bool emitEmbeddedHeader(const Config *c, bool *changed);

/* Fallback tanpa ld: tulis build/<name>.embed.c (array byte dari arsip)
   lalu kompilasi dengan toolchain aktif. */
bool embedResourceCompile(const EmbeddedEntry *e, const Config *c);

bool buildEmbedded(const Config *c);

#endif /* RBOT_V0_1_0_EMBED_H */
