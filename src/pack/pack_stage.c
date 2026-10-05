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

bool packResolveSource(const char *src, char *out, size_t n) {
  if (fsFileExists(src) || fsDirExists(src)) {
    copyStr(out, n, src);
    return true;
  }
  size_t l = strlen(src);
  if (l + 5 <= n) {
    memcpy(out, src, l);
    memcpy(out + l, ".exe", 5);
    if (fsFileExists(out)) return true; /* hanya file — direktori .exe tidak masuk akal */
  }
  copyStr(out, n, src);
  return false;
}

int64_t packEntryMTimeNs(const char *entry) {
  char src[MAX_PATH * 2], dst[MAX_PATH * 2];
  if (!packEntryParts(entry, src, sizeof(src), dst, sizeof(dst))) return -1;
  char eff[MAX_PATH * 2];
  if (!packResolveSource(src, eff, sizeof(eff))) return -1;
  return fsMTimeNs(eff);
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
    /* Fallback .exe: bin/rbot -> bin/rbot.exe di host Windows. */
    char eff[MAX_PATH * 2];
    packResolveSource(src, eff, sizeof(eff));
    if (fsFileExists(eff)) {
      char dst[MAX_PATH * 2];
      snprintf(dst, sizeof(dst), "%s/%s", root, dstRoot);
      mkparent(dst);
      if (!packCopyFile(eff, dst)) {
        fprintf(stderr, "rbot: pack: gagal menyalin %s\n", eff);
        return false;
      }
      continue;
    }
    List files = {0};
    walkDir(eff, "", &files);
    size_t srcLen = strlen(eff);
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
