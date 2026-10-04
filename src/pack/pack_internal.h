#ifndef RBOT_V0_1_0_PACK_INTERNAL_H
#define RBOT_V0_1_0_PACK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../config.h"

/*
 * pack_internal — kontrak antar-file modul pack/ (publik: pack.h).
 *
 *   pack.c       : template, freshness, checksum, dispatch artefak.
 *   pack_tar.c   : tarball via shell tar (cz/c/cJ/cj).
 *   pack_deb.c   : staging control+data, tar cz, ar archive (debian-binary,
 *                  control.tar.gz, data.tar.gz) ditulis sendiri di C.
 *   pack_sha256.c: SHA-256 murni C + penulis file .sha256.
 */

/* Satu artefak hasil resolusi template pack.output. */
typedef struct {
  char path[MAX_PATH * 2]; /* output final, relatif root proyek */
  char tmp[MAX_PATH * 2];  /* file sementara, direname setelah jadi */
  bool isDeb;
  char compress[16]; /* gzip|none|xz|bz2 — tarball saja */
} PackArtifact;

/* Ganti {name} {version} {os} {arch} di template `tpl` dengan nilai host. */
void packTemplate(const Config *c, const char *tpl, char *out, size_t n);

/* Nama .deb dari path tarball: ekstensi tar dikenal (.tar.gz/.tgz/.tar.xz/
   .tar.bz2/.tar) diganti .deb; tanpa ekstensi dikenal -> <path>.deb. */
void packDebName(const char *tarPath, char *out, size_t n);

/* Direktori staging bersama: ".rbot/pack". */
void packStageDir(char *out, size_t n);

/* checksum sha256 -> "<path>.sha256" berisi "<hex>  <basename>" (dua spasi,
   kompatibel `sha256sum -c`). */
bool packWriteSha256(const char *path);

/* Pembangun artefak: tulis file final di a->path, true bila sukses. */
bool packBuildTar(const Config *c, const PackArtifact *a);
bool packBuildDeb(const Config *c, const PackArtifact *a);

/* mtime ns terbesar di antara source entri pack.files (folder di-walk rekursif);
   -1 bila tidak ada entri valid. */
int64_t packNewestInput(const Config *c);

/* mtime ns file, -1 bila tidak ada. */
int64_t packPathMTimeNs(const char *path);

/* ============ parsing (pack_config.c; dipakai config.c) ============ */

/* Setel string pack; sepasang kutip pembuka/penutup dilepas. */
void packSetStr(char *dst, size_t n, const char *value);
/* pack.files = a, b, c — daftar dipisah koma. */
void packAddFiles(PackConfig *p, const char *value);
/* Key deb.* (pack.deb.* dan key bare deb.*). */
void packApplyDeb(Config *c, const char *key, const char *value);
/* Key pack.* tanpa sub (section pack maupun key bare top-level). */
void packApply(Config *c, const char *key, const char *value);

#endif /* RBOT_V0_1_0_PACK_INTERNAL_H */
