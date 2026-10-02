/*
 * pack_tar — tarball pack.output via shell tar.
 *
 * Entri pack.files diteruskan ke tar apa adanya (file maupun folder,
 * rekursif oleh tar sendiri); path tersimpan di arsip persis seperti yang
 * ditulis (bin/app tetap bin/app). Kompresi mengikuti PackArtifact:
 *   gzip -> cz, xz -> cJ, bz2 -> cj, none -> c.
 * Hasil ditulis ke a->tmp lalu direname ke a->path (output lama tidak
 * pernah setengah jadi).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portability.h"
#include "../util.h"
#include "pack_internal.h"

static const char *tarFlag(const char *compress) {
  if (strcmp(compress, "gzip") == 0) return "cz";
  if (strcmp(compress, "xz") == 0) return "cJ";
  if (strcmp(compress, "bz2") == 0) return "cj";
  return "c";
}

bool packBuildTar(const Config *c, const PackArtifact *a) {
  (void)c;
  fsRemoveFile(a->tmp);

  size_t n = strlen(a->tmp) + 64;
  for (int i = 0; i < c->pack.files.count; i++)
    n += strlen(c->pack.files.items[i]) + 2;
  char *cmd = malloc(n);
  if (!cmd) {
    fprintf(stderr, "rbot: pack: kehabisan memori\n");
    return false;
  }

  snprintf(cmd, n, "tar %sf %s", tarFlag(a->compress), a->tmp);
  for (int i = 0; i < c->pack.files.count; i++) {
    strcat(cmd, " ");
    strcat(cmd, c->pack.files.items[i]);
  }

  mkparent(a->tmp);
  bool ok = runCmd(cmd);
  free(cmd);
  if (!ok) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "rbot: pack: tar gagal untuk %s\n", a->path);
    return false;
  }

  mkparent(a->path);
  fsRemoveFile(a->path);
  if (rename(a->tmp, a->path) != 0) {
    fsRemoveFile(a->tmp);
    fprintf(stderr, "rbot: pack: tidak bisa menempatkan %s\n", a->path);
    return false;
  }
  return true;
}
