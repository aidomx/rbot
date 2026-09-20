#ifndef RBOT_V0_1_0_EMBED_H
#define RBOT_V0_1_0_EMBED_H

#include <stdbool.h>

#include "config.h"

/*
 * Embedding bersifat generik: tiap entri embedded mengarsipkan direktori
 * src-nya, mengonversi arsip jadi object via `ld -r -b binary`, dan
 * mendapat sekumpulan macro EMBED_<n>_* yang diterbitkan ke build/embedded.h.
 * Tidak ada bagian di sini yang tahu soal project tertentu — konsumen
 * (mis. loader rupa) include header itu dan pakai macro untuk entri yang
 * mereka pedulikan. Aktif kembali begitu Buildfile: embedded diisi.
 */

/*
 * Terbitkan build/embedded.h agar kode runtime pakai simbol yang sama
 * persis. `ld -r -b binary` menamai simbol dari path arsip relatif thd cwd:
 * modules/rupa_modules.tar.gz -> _binary_modules_rupa_modules_tar_gz_<start|end>.
 * File hanya ditulis ulang saat isinya berubah; *changed memberi tahu
 * pemanggil bahwa source yang mereferensikan simbol itu perlu dikompilasi ulang.
 */
bool emitEmbeddedHeader(const Config *c, bool *changed);

bool buildEmbedded(const Config *c);

#endif /* RBOT_V0_1_0_EMBED_H */
