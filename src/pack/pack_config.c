/*
 * pack_config — parsing section pack.* (Buildfile) dan key bare (hasil
 * sintesis Buildfile.ws). Dipisah dari config.c agar ukuran file
 * tetap di bawah 500 baris; satu-satunya pemakai adalah configApply() —
 * deklarasi ada di pack_internal.h.
 */
#include <string.h>

#include "../util.h"
#include "pack_internal.h"

/* Setel string pack; sepasang kutip pembuka/penutup dilepas (nilai    `pack.deb.maintainer = "Nama <email>"` di Buildfile.ws). */
void packSetStr(char *dst, size_t n, const char *value) {
  size_t len = strlen(value);
  if (len >= 2 && value[0] == '\"' && value[len - 1] == '\"') {
    copyStr(dst, n, value + 1);
    dst[len - 2] = '\0';
  } else {
    copyStr(dst, n, value);
  }
}

/* pack.files = a, b, c — daftar dipisah koma; a:b memetakan source ke destination. */
void packAddFiles(PackConfig *p, const char *value) {
  char buf[2048];
  copyStr(buf, sizeof(buf), value);
  for (char *save = buf;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *item = trim(save);
    if (*item) listAdd(&p->files, item);
    if (!comma) break;
    save = comma + 1;
  }
}

/* Key deb.* — dipakai section `pack` + `pack.deb.*` maupun key bare    `deb.*` hasil sintesis Buildfile.ws. */
void packApplyDeb(Config *c, const char *key, const char *value) {
  if (strcmp(key, "maintainer") == 0)
    packSetStr(c->pack.debMaintainer, sizeof(c->pack.debMaintainer), value);
  else if (strcmp(key, "description") == 0)
    packSetStr(c->pack.debDescription, sizeof(c->pack.debDescription), value);
  else if (strcmp(key, "install_prefix") == 0 || strcmp(key, "installPrefix") == 0)
    packSetStr(c->pack.debInstallPrefix, sizeof(c->pack.debInstallPrefix), value);
  else if (strcmp(key, "architecture") == 0)
    packSetStr(c->pack.debArchitecture, sizeof(c->pack.debArchitecture), value);
}

/* Key pack.* tanpa sub — dipakai section `pack` maupun key bare di
   top-level (name, version, files, output, compress, checksum, format). */
void packApply(Config *c, const char *key, const char *value) {
  if (strcmp(key, "name") == 0)
    packSetStr(c->pack.name, sizeof(c->pack.name), value);
  else if (strcmp(key, "version") == 0)
    packSetStr(c->pack.version, sizeof(c->pack.version), value);
  else if (strcmp(key, "output") == 0)
    packSetStr(c->pack.output, sizeof(c->pack.output), value);
  else if (strcmp(key, "compress") == 0)
    packSetStr(c->pack.compress, sizeof(c->pack.compress), value);
  else if (strcmp(key, "checksum") == 0)
    packSetStr(c->pack.checksum, sizeof(c->pack.checksum), value);
  else if (strcmp(key, "format") == 0)
    packSetStr(c->pack.format, sizeof(c->pack.format), value);
  else if (strcmp(key, "files") == 0)
    packAddFiles(&c->pack, value);
}
