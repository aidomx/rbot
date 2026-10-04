#include "pack_stage.h"

#include <stdio.h>
#include <string.h>

#include "../portability.h"
#include "../util.h"

bool packEntryParts(const char *entry, char *src, size_t srcN,
                    char *dst, size_t dstN) {
  const char *sep = strchr(entry, ':');
  if (!sep || sep == entry || !sep[1]) {
    copyStr(src, srcN, entry);
    copyStr(dst, dstN, entry);
    return true;
  }
  size_t n = (size_t)(sep - entry);
  if (n >= srcN) return false;
  memcpy(src, entry, n);
  src[n] = '\0';
  copyStr(dst, dstN, sep + 1);
  return dst[0] != '\0';
}

int64_t packEntryMTimeNs(const char *entry) {
  char src[MAX_PATH * 2], dst[MAX_PATH * 2];
  if (!packEntryParts(entry, src, sizeof(src), dst, sizeof(dst))) return -1;
  return fsMTimeNs(src);
}


static bool packCopyFile(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in) return false;
  FILE *out = fopen(dst, "wb");
  if (!out) { fclose(in); return false; }
  unsigned char buf[65536];
  size_t got;
  bool ok = true;
  while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, got, out) != got) { ok = false; break; }
  }
  ok = ok && feof(in) != 0;
  fclose(in);
  if (fclose(out) != 0) ok = false;
  if (ok) fsSetMode(dst, fsGetMode(src));
  return ok;
}

bool packStageEntries(const Config *c, const char *root) {
  fsRemoveTree(root);
  mkdirs(root);
  for (int i = 0; i < c->pack.files.count; i++) {
    char src[MAX_PATH * 2], dstRoot[MAX_PATH * 2];
    if (!packEntryParts(c->pack.files.items[i], src, sizeof(src),
                        dstRoot, sizeof(dstRoot))) return false;
    if (fsFileExists(src)) {
      char dst[MAX_PATH * 2];
      snprintf(dst, sizeof(dst), "%s/%s", root, dstRoot);
      mkparent(dst);
      if (!packCopyFile(src, dst)) {
        fprintf(stderr, "rbot: pack: gagal menyalin %s\n", src);
        return false;
      }
      continue;
    }
    List files = {0};
    walkDir(src, "", &files);
    size_t srcLen = strlen(src);
    for (int j = 0; j < files.count; j++) {
      const char *file = files.items[j];
      const char *suffix = file + srcLen;
      if (*suffix == '/') suffix++;
      char dst[MAX_PATH * 2];
      if (*suffix)
        snprintf(dst, sizeof(dst), "%s/%s/%s", root, dstRoot, suffix);
      else
        snprintf(dst, sizeof(dst), "%s/%s", root, dstRoot);
      mkparent(dst);
      if (!packCopyFile(file, dst)) {
        fprintf(stderr, "rbot: pack: gagal menyalin %s\n", file);
        listFree(&files);
        return false;
      }
    }
    listFree(&files);
  }
  return true;
}
