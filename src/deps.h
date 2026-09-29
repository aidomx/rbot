#ifndef RBOT_V0_1_0_DEPS_H
#define RBOT_V0_1_0_DEPS_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/*
 * deps — pelacak dependensi header untuk build incremental.
 *
 * Tanpa ini, rbot hanya membandingkan mtime object dengan file .c-nya,
 * sehingga mengubah sebuah header tidak pernah memicu kompilasi ulang.
 * Modul ini memindai direktif #include ("..." dan <...>) secara transitif,
 * meresolusinya seperti preprocessor (dir file pengguna dulu untuk "...",
 * lalu direktori -I dari Buildfile: headers), dan melaporkan mtime header
 * terbaru yang dipakai sebuah source.
 *
 * Header yang tidak ditemukan di direktori project (mis. <stdio.h>)
 * dianggap header sistem dan diabaikan. Pemindaian mengabaikan #if/#ifdef,
 * jadi hasilnya bisa sedikit berlebih (kompilasi ulang yang tak perlu),
 * tidak pernah kurang.
 */

typedef struct DepCache DepCache;

/* Buat cache; `incDirs` = direktori -I hasil includeDirs(). */
DepCache *depsNew(const List *incDirs);
void depsFree(DepCache *dc);

/*
 * mtime nanodetik terbaru di antara seluruh header (transitif) yang di-include
 * `src`; 0 bila tidak ada header project. Header dibaca sekali per proses
 * dan di-cache, jadi aman dipanggil untuk ratusan source.
 */
int64_t depsNewestHeaderMTime(DepCache *dc, const char *src);

#endif /* RBOT_V0_1_0_DEPS_H */
