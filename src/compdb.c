#include "compdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compile.h"
#include "portability.h"

#ifdef _WIN32
#define strtok_r strtok_s
#endif

bool compdbEnabled(const Config *c) {
  /* Nonaktif hanya kalau eksplisit "false"; "true" maupun "auto" (default)
     sama-sama berarti compile_commands.json dibuat/diperbarui otomatis. */
  return strcmp(c->outCompileCommands, "false") != 0;
}

// ============================================================================
// 1. Macro Pembantu untuk Append String Terformat ke Buffer
// ============================================================================
#define APPEND(buf, len, cap, fmt, ...)                                                            \
  do {                                                                                             \
    int n = snprintf((buf) + (len), (cap) - (len), fmt, ##__VA_ARGS__);                            \
    if (n > 0 && (len) + n < (cap)) (len) += n;                                                    \
  } while (0)

// ============================================================================
// 2. Helper untuk JSON Quote tanpa Alokasi Memori (Zero Allocation)
// ============================================================================
static void jsonQuoteAppend(char *buf, size_t *len, size_t cap, const char *s) {
  size_t slen = strlen(s);
  // Cek kapasitas: worst case setiap karakter di-escape (x2), plus 2 untuk tanda kutip
  if (*len + slen * 2 + 2 >= cap) return;

  buf[(*len)++] = '"'; // Buka kutip
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      buf[(*len)++] = '\\'; // Tambah backslash escape
    }
    buf[(*len)++] = *p; // Tulis karakter asli
  }
  buf[(*len)++] = '"'; // Tutup kutip
}

/*static void jsonQuote(FILE *fp, const char *s) {*/
/*fputc('"', fp);*/
/*for (const char *p = s; *p; p++) {*/
/*if (*p == '"' || *p == '\\') fputc('\\', fp);*/
/*fputc(*p, fp);*/
/*}*/
/*fputc('"', fp);*/
/*}*/

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
  h = hashString(h, "compdb-v2");
  h = hashString(h, c->cc);
  h = hashString(h, c->std);
  h = hashString(h, c->target);
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
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj))) h = hashString(h, obj);
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

/* ============ restore dari blob (optimasi jalur restore) ============
 * compile_commands.json yang hilang (terhapus manual / antar bench run)
 * dulu selalu dirender ulang penuh. Padahal fingerprint yang sama
 * menghasilkan byte JSON yang sama persis (deterministik). Konten json
 * terakhir ikut disimpan sebagai blob .rbot/compile_commands.blob —
 * restore = fingerprint cocok + salin file, tanpa render ulang. */

static bool compdbCacheFingerprint(uint64_t fingerprint) {
  FILE *fp = fopen(".rbot/compile_commands.cache", "r");
  if (!fp) return false;
  unsigned long long cached = 0;
  bool ok = fscanf(fp, "%llx", &cached) == 1;
  fclose(fp);
  return ok && (uint64_t)cached == fingerprint;
}

static bool compdbCopyFile(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in) return false;
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buf[65536];
  size_t n;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  if (ferror(in)) ok = false;
  if (fclose(out) != 0) ok = false;
  fclose(in);
  return ok;
}

static bool compdbRestoreFromBlob(uint64_t fingerprint) {
  if (!fsFileExists(".rbot/compile_commands.blob")) return false;
  if (!compdbCacheFingerprint(fingerprint)) return false;
  if (!compdbCopyFile(".rbot/compile_commands.blob", ".rbot/compile_commands.json.tmp"))
    return false;
  remove("compile_commands.json");
  if (rename(".rbot/compile_commands.json.tmp", "compile_commands.json") != 0) {
    fsRemoveFile(".rbot/compile_commands.json.tmp");
    return false;
  }
  return true;
}

static void saveCompdbBlob(const char *buf, size_t len) {
  mkdirs(".rbot");
  FILE *fp = fopen(".rbot/compile_commands.blob.tmp", "wb");
  if (!fp) return;
  size_t wrote = fwrite(buf, 1, len, fp);
  if (wrote != len) {
    fclose(fp);
    fsRemoveFile(".rbot/compile_commands.blob.tmp");
    return;
  }
  if (fclose(fp) != 0) {
    fsRemoveFile(".rbot/compile_commands.blob.tmp");
    return;
  }
#ifdef _WIN32
  fsRemoveFile(".rbot/compile_commands.blob");
#endif
  if (rename(".rbot/compile_commands.blob.tmp", ".rbot/compile_commands.blob") != 0)
    fsRemoveFile(".rbot/compile_commands.blob.tmp");
}

// ============================================================================
// 3. Fungsi Utama: writeCompdb (Memory-First Serialization)
// ============================================================================
void writeCompdb(const Config *c, List *srcs) {
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;

  uint64_t fingerprint = compdbFingerprint(c, srcs, cwd);
  if (compdbCacheHit(fingerprint)) {
    printf("> CompDB    : compile_commands.json unchanged (cache hit)\n");
    return;
  }
  /* Json hilang tapi blob + fingerprint masih cocok: restore tanpa render. */
  if (!fsFileExists("compile_commands.json") && compdbRestoreFromBlob(fingerprint)) {
    printf("> CompDB    : compile_commands.json restored (blob)\n");
    return;
  }

  // 1. Alokasi buffer di memori (2MB aman untuk ribuan file)
  size_t bufCap = 2 * 1024 * 1024;
  char *buf = malloc(bufCap);
  if (!buf) return;
  size_t bufLen = 0;

  APPEND(buf, bufLen, bufCap, "[\n");

  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    char obj[MAX_PATH];
    if (!objectPathFor(c, src, obj, sizeof(obj))) continue;

    APPEND(buf, bufLen, bufCap, "  {\n    \"arguments\": [\n      ");

    // Compiler
    jsonQuoteAppend(buf, &bufLen, bufCap, c->cc);
    APPEND(buf, bufLen, bufCap, ",\n      ");

    // Target Flags
    char *tf = targetFlags(c);
    if (tf && tf[0]) {
      char *savePtr = NULL;
      char *tok = strtok_r(tf, " ", &savePtr);
      while (tok) {
        jsonQuoteAppend(buf, &bufLen, bufCap, tok);
        APPEND(buf, bufLen, bufCap, ",\n      ");
        tok = strtok_r(NULL, " ", &savePtr);
      }
    }
    free(tf);

    // Header Public
    for (int h = 0; h < c->headerPublic.count; h++) {
      const char *entry = c->headerPublic.items[h];
      char flag[MAX_PATH + 8];
      if (entry[0] == '-')
        snprintf(flag, sizeof(flag), "%s", entry);
      else
        snprintf(flag, sizeof(flag), "-I%s",
                 entry[0] == 'I' && entry[1] == '.' ? entry + 1 : entry);

      jsonQuoteAppend(buf, &bufLen, bufCap, flag);
      APPEND(buf, bufLen, bufCap, ",\n      ");
    }

    // Flags
    for (int f = 0; f < c->flags.count; f++) {
      const char *fl = c->flags.items[f];
      char withDash[MAX_PATH + 8];
      if (fl[0] == '-')
        snprintf(withDash, sizeof(withDash), "%s", fl);
      else
        snprintf(withDash, sizeof(withDash), "-%s", fl);

      jsonQuoteAppend(buf, &bufLen, bufCap, withDash);
      APPEND(buf, bufLen, bufCap, ",\n      ");
    }

    // Std, -c, -o, obj, src
    APPEND(buf, bufLen, bufCap, "\"-std=%s\",\n      \"-c\",\n      \"-o\",\n      ", c->std);
    jsonQuoteAppend(buf, &bufLen, bufCap, obj);
    APPEND(buf, bufLen, bufCap, ",\n      ");
    jsonQuoteAppend(buf, &bufLen, bufCap, src);
    APPEND(buf, bufLen, bufCap, "\n    ],\n");

    // Directory, File, Output
    char absPath[MAX_PATH * 2];

    APPEND(buf, bufLen, bufCap, "    \"directory\": ");
    jsonQuoteAppend(buf, &bufLen, bufCap, cwd);

    APPEND(buf, bufLen, bufCap, ",\n    \"file\": ");
    snprintf(absPath, sizeof(absPath), "%s/%s", cwd, src);
    jsonQuoteAppend(buf, &bufLen, bufCap, absPath);

    APPEND(buf, bufLen, bufCap, ",\n    \"output\": ");
    snprintf(absPath, sizeof(absPath), "%s/%s", cwd, obj);
    jsonQuoteAppend(buf, &bufLen, bufCap, absPath);

    APPEND(buf, bufLen, bufCap, "\n  }%s\n", i + 1 < srcs->count ? "," : "");
  }

  APPEND(buf, bufLen, bufCap, "]\n");

  // 2. SATU KALI TULIS KE DISK (The Magic Happens Here)
  FILE *fp = fopen("compile_commands.json", "w");
  if (fp) {
    fwrite(buf, 1, bufLen, fp);
    fclose(fp);
  }

  saveCompdbBlob(buf, bufLen); /* konten json untuk restore tanpa render */
  free(buf);
  saveCompdbCache(fingerprint);
  printf("> CompDB    : compile_commands.json refreshed\n");
}
