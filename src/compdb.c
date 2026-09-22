#include "compdb.h"

#include <stdio.h>
#include <string.h>

#include "compile.h"
#include "portability.h"

bool compdbEnabled(const Config *c) {
  /* Nonaktif hanya kalau eksplisit "false"; "true" maupun "auto" (default)
     sama-sama berarti compile_commands.json dibuat/diperbarui otomatis. */
  return strcmp(c->outCompileCommands, "false") != 0;
}

static void jsonQuote(FILE *fp, const char *s) {
  fputc('"', fp);
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') fputc('\\', fp);
    fputc(*p, fp);
  }
  fputc('"', fp);
}

static uint64_t hashBytes(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t hashString(uint64_t h, const char *s) {
  h = hashBytes(h, s, strlen(s));
  const unsigned char sep = 0xff;
  return hashBytes(h, &sep, 1);
}

static uint64_t compdbFingerprint(const Config *c, List *srcs, const char *cwd) {
  uint64_t h = 1469598103934665603ULL;
  h = hashString(h, cwd);
  h = hashString(h, c->cc);
  h = hashString(h, c->std);
  h = hashString(h, c->outBuildDir);
  h = hashString(h, c->outBinaryDir);
  h = hashString(h, c->outBinaryName);

  for (int i = 0; i < c->headerPublic.count; i++)
    h = hashString(h, c->headerPublic.items[i]);
  for (int i = 0; i < c->flags.count; i++)
    h = hashString(h, c->flags.items[i]);

  /* compile_commands.json hanya bergantung pada daftar source dan konfigurasi
     compiler. Timestamp source tidak ikut fingerprint: mengubah isi source
     tidak mengubah command compile yang direkam. */
  for (int i = 0; i < srcs->count; i++) {
    char obj[MAX_PATH];
    h = hashString(h, srcs->items[i]);
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj)))
      h = hashString(h, obj);
  }
  return h;
}

static bool compdbCacheHit(uint64_t fingerprint) {
  if (!fsFileExists("compile_commands.json") || !fsFileExists(".rbot/compile_commands.cache"))
    return false;

  FILE *fp = fopen(".rbot/compile_commands.cache", "r");
  if (!fp) return false;

  unsigned long long cached = 0;
  bool ok = fscanf(fp, "%llx", &cached) == 1;
  fclose(fp);
  return ok && (uint64_t)cached == fingerprint;
}

static void saveCompdbCache(uint64_t fingerprint) {
  mkdirs(".rbot");
  FILE *fp = fopen(".rbot/compile_commands.cache.tmp", "w");
  if (!fp) return;
  fprintf(fp, "%016llx\n", (unsigned long long)fingerprint);
  fclose(fp);
  /* Rename is intentionally done through the shell-independent stdio/OS
     fallback already used by rbot's portability layer only where available.
     If the atomic replacement is unavailable, the old cache is harmless. */
  remove(".rbot/compile_commands.cache");
  rename(".rbot/compile_commands.cache.tmp", ".rbot/compile_commands.cache");
}

void writeCompdb(const Config *c, List *srcs) {
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;

  uint64_t fingerprint = compdbFingerprint(c, srcs, cwd);
  if (compdbCacheHit(fingerprint)) {
    printf("> CompDB    : compile_commands.json unchanged (cache hit)\n");
    return;
  }

  FILE *fp = fopen("compile_commands.json", "w");
  if (!fp) return;
  fprintf(fp, "[\n");

  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    char obj[MAX_PATH];
    if (!objectPathFor(c, src, obj, sizeof(obj))) continue;

    fprintf(fp, "  {\n    \"arguments\": [\n      ");
    jsonQuote(fp, c->cc);
    fprintf(fp, ",\n      ");
    for (int h = 0; h < c->headerPublic.count; h++) {
      const char *entry = c->headerPublic.items[h];
      char flag[MAX_PATH + 8];
      if (entry[0] == '-')
        snprintf(flag, sizeof(flag), "%s", entry);
      else
        snprintf(flag, sizeof(flag), "-I%s", entry[0] == 'I' && entry[1] == '.' ? entry + 1 : entry);
      jsonQuote(fp, flag);
      fprintf(fp, ",\n      ");
    }
    for (int f = 0; f < c->flags.count; f++) {
      const char *fl = c->flags.items[f];
      if (fl[0] == '-')
        jsonQuote(fp, fl);
      else {
        char withDash[128];
        snprintf(withDash, sizeof(withDash), "-%s", fl);
        jsonQuote(fp, withDash);
      }
      fprintf(fp, ",\n      ");
    }
    fprintf(fp, "\"-std=");
    fputs(c->std, fp);
    fprintf(fp, "\",\n      \"-c\",\n      \"-o\",\n      ");
    jsonQuote(fp, obj);
    fprintf(fp, ",\n      ");
    jsonQuote(fp, src);
    fprintf(fp, "\n    ],\n    \"directory\": ");
    jsonQuote(fp, cwd);
    fprintf(fp, ",\n    \"file\": ");
    char abs[MAX_PATH * 2];
    snprintf(abs, sizeof(abs), "%s/%s", cwd, src);
    jsonQuote(fp, abs);
    fprintf(fp, ",\n    \"output\": ");
    snprintf(abs, sizeof(abs), "%s/%s", cwd, obj);
    jsonQuote(fp, abs);
    fprintf(fp, "\n  }%s\n", i + 1 < srcs->count ? "," : "");
  }
  fprintf(fp, "]\n");
  fclose(fp);
  saveCompdbCache(fingerprint);
  printf("> CompDB    : compile_commands.json refreshed\n");
}
