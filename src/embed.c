#include "embed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compile.h"
#include "embed_ld_cache.h"
#include "portability.h"
#include "util.h"

/* Deteksi GNU ld untuk jalur embed; verdict di-cache per proses
   (g_ldAvailable) DAN lintas-run (.rbot/ld.cache via embed_ld_cache),
   sehingga probe `ld --version` (fork sh + exec ld, 3-5 ms) hanya jalan
   pada RUN PERTAMA atau setelah ld berganti di PATH — run berikutnya
   cukup satu stat ke binary ld yang direkam.
   RBOT_NO_LD=1 memaksa jalur fallback (C array) — berguna untuk testing
   dan lingkungan yang ld-nya rusak; env ini JANGAN dicache. */
static int g_ldAvailable = -1;

/* Cek apakah 'ld' yang ada di PATH adalah GNU ld (bukan LLD/LLD-compatible).
 * GNU ld mendukung '-r -b binary', LLD tidak. */
static bool isGnuLd(void) {
  FILE *fp = popenRB("ld --version 2>&1");
  if (!fp) return false;

  char buf[512];
  bool gnu = false;
  while (fgets(buf, sizeof(buf), fp)) {
    /* LLD output: "LLD 17.0.6 ..." atau "Debian LLD ..."
         * GNU ld output: "GNU ld (GNU Binutils) 2.38"
         * Catatan: cek LLD dulu karena Debian kadang output "GNU LLD" */
    if (strstr(buf, "LLD") != NULL) {
      gnu = false;
      break;
    }
    if (strstr(buf, "GNU ld") != NULL) {
      gnu = true;
      /* Jangan break — lanjut baca untuk pastikan tidak ada "LLD" di baris berikutnya */
    }
  }
  pcloseRB(fp);
  return gnu;
}

static bool ldAvailable(void) {
  if (g_ldAvailable >= 0) return g_ldAvailable == 1;
  /* Env override TIDAK boleh menyentuh cache: RBOT_NO_LD diputus di sini
     sebelum resolve/load, jadi testing fallback tidak ikut tersimpan
     sebagai verdict "tanpa ld" (Uji 2 verify-fpic.sh harus tetap jalan). */
  char ldPath[MAX_PATH];
  if (!getenv("RBOT_NO_LD")) {
    bool resolved = ldCacheResolve("ld", ldPath, sizeof(ldPath));
    bool cached = false;
    if (resolved && cacheCheckLD(ldPath, &cached)) {
      /* Cache valid: path + mtime ns + size binary ld tidak berubah. */
      g_ldAvailable = cached ? 1 : 0;
      return g_ldAvailable == 1;
    }
    bool gnu = isGnuLd(); /* probe fisik, sekali per perubahan ld */
    if (resolved) cacheUpdateLD(ldPath, gnu);
    g_ldAvailable = gnu ? 1 : 0;
    return g_ldAvailable == 1;
  }
  /* Tidak ada RBOT_NO_LD dan ld tidak ditemukan di PATH (murah, stat-only):
     tidak direkam — bisa berubah kapan saja lewat PATH, dan procRun ld
     pasti gagal juga. */
  g_ldAvailable = 0;
  return false;
}

/* Probe tanpa cache untuk jalur non-mutating — lihat embed.h. */
bool embedProbeGnuLd(void) {
  if (!probeAvailable("ld")) return false;
  return isGnuLd();
}

static uint64_t embArchiveConfigHash(const EmbeddedEntry *e) {
  uint64_t h = UINT64_C(1469598103934665603);
#define HASH_BYTES(p, n)                                                                           \
  do {                                                                                             \
    const unsigned char *_b = (const unsigned char *)(p);                                          \
    for (size_t _i = 0; _i < (n); _i++) {                                                          \
      h ^= _b[_i];                                                                                 \
      h *= UINT64_C(1099511628211);                                                                \
    }                                                                                              \
  } while (0)
#define HASH_STR(x)                                                                                \
  do {                                                                                             \
    const char *_s = (x);                                                                          \
    HASH_BYTES(_s, strlen(_s) + 1);                                                                \
  } while (0)
  HASH_STR(e->src);
  HASH_STR(e->pattern);
  HASH_STR(e->archiveDir);
  HASH_STR(e->archiveName);
  HASH_STR(e->ext);
  HASH_BYTES(&e->tar, sizeof(e->tar));
  for (int i = 0; i < e->excludes.count; i++)
    HASH_STR(e->excludes.items[i]);
#undef HASH_STR
#undef HASH_BYTES
  return h;
}

static bool embArchiveStateMatches(const EmbeddedEntry *e) {
  char path[MAX_PATH + 96];
  snprintf(path, sizeof(path), "%s.archive", e->objectPath);
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;
  uint64_t stored = 0;
  bool ok = fread(&stored, 1, sizeof(stored), fp) == sizeof(stored);
  fclose(fp);
  return ok && stored == embArchiveConfigHash(e);
}

static void embArchiveStateSave(const EmbeddedEntry *e) {
  char path[MAX_PATH + 96];
  snprintf(path, sizeof(path), "%s.archive", e->objectPath);
  mkparent(path);
  FILE *fp = fopen(path, "wb");
  if (!fp) return;
  uint64_t h = embArchiveConfigHash(e);
  fwrite(&h, 1, sizeof(h), fp);
  fclose(fp);
}

/* Entri exclude arsip (archive.exclude / embedded.<n>.exclude): nama file,
   path relatif, atau direktori. Pencocokan konsisten dengan excludedSource
   (compile.c), plus prefix direktori untuk entri tanpa ekstensi:
   - path relatif sama persis ("LICENSE"), atau
   - basename sama persis ("main.c" vs "src/main.c"), atau
   - path di dalam direktori yang dinamai entri ("build" -> "build/x").
   Dipakai oleh DAFTAR FILE tar dan SCAN FRESHNESS — tanpa ini, artefak
   build di dalam tree sumber (mis. build/<n>.o.archive) ikut terhitung
   sebagai input dan membuat arsip di-rebuild tiap run. */
static bool embExcluded(const EmbeddedEntry *e, const char *rel) {
  for (int i = 0; i < e->excludes.count; i++) {
    const char *ex = e->excludes.items[i];
    if (!ex || !*ex) continue;
    if (strcmp(rel, ex) == 0) return true;
    const char *base = strrchr(rel, '/');
    if (base && strcmp(base + 1, ex) == 0) return true;
    size_t el = strlen(ex);
    if (strncmp(rel, ex, el) == 0 && rel[el] == '/') return true;
  }
  return false;
}

/* Path relatif terhadap e->src untuk pencocokan excludes. */
static void embRelPath(const EmbeddedEntry *e, const char *path, char *rel, size_t n) {
  size_t srcLen = strlen(e->src);
  if (strncmp(path, e->src, srcLen) == 0 &&
      (path[srcLen] == '/' || path[srcLen] == '\\' || path[srcLen] == '\0')) {
    const char *r = path + srcLen;
    while (*r == '/' || *r == '\\')
      r++;
    snprintf(rel, n, "%s", r);
    return;
  }
  snprintf(rel, n, "%s", path);
}

static bool embArchiveFresh(const EmbeddedEntry *e) {
  /* Arsip jadi (embedded.<n>.file): cukup bandingkan mtime file — tidak
     ada direktori sumber untuk discan. */
  if (e->usePrebuilt) return fsFileExists(e->archivePath);
  int64_t at = fsMTimeNs(e->archivePath);
  if (at < 0 || !embArchiveStateMatches(e)) return false;

  /* Scan freshness dengan resolusi nanodetik: perubahan source dan archive
     dapat terjadi dalam detik yang sama, sehingga time_t/fsMTime() terlalu
     kasar untuk invalidasi build. Entri yang di-exclude tidak dihitung —
     sama seperti saat arsip dibangun. */
  List files = {0};
  if (e->pattern[0])
    walkDir(e->src, e->pattern, &files);
  else
    walkDir(e->src, "", &files);
  int64_t newest = 0;
  char rel[MAX_PATH * 2];
  for (int i = 0; i < files.count; i++) {
    embRelPath(e, files.items[i], rel, sizeof(rel));
    if (!*rel || embExcluded(e, rel)) continue;
    int64_t mt = fsMTimeNs(files.items[i]);
    if (mt >= 0 && mt > newest) newest = mt;
  }
  listFree(&files);
  return newest <= at;
}

/* "modules/rupa.tar.gz" -> "_binary_modules_rupa_tar_gz" (basis simbol) */
static void embedSymBase(const char *archivePath, char *out, size_t n) {
  snprintf(out, n, "_binary_");
  size_t k = strlen(out);
  for (const char *p = archivePath; *p && k < n - 1; p++, k++) {
    char ch = *p;
    bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    out[k] = alnum ? ch : '_';
  }
  out[k] = '\0';
}

/*
 * Terbitkan build/embedded.h. Isi menyesuaikan jalur embed yang aktif:
 *
 *   GNU ld  : simbol nyata _binary_<path>_start/_end (alamat), LEN = diff.
 *   Fallback: simbol _binary_<path>_start[] + _binary_<path>_len (unsigned
 *             long), END = (start + len) — semantik "one past the end"
 *             sama dengan ld.
 *
 * Konsumen cukup memakai macro EMBED_<N>_SYMBOL / _END / _LEN; deklarasi
 * extern simbol ikut diterbitkan di header sehingga tidak perlu ditulis
 * manual. File hanya ditulis ulang saat isinya berubah; *changed memberi
 * tahu pemanggil bahwa source yang mereferensikan simbol perlu dikompilasi
 * ulang.
 */
bool emitEmbeddedHeader(const Config *c, bool *changed) {
  static char body[16384];
  size_t used = 0;
  bool useLd = ldAvailable();

  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "/* Auto-generated by rbot from Buildfile: embedded. Do not edit. */\n"
                           "/* Embed path: %s */\n"
                           "#ifndef RUPA_EMBEDDED_H\n#define RUPA_EMBEDDED_H\n\n",
                           useLd ? "ld -r -b binary" : "c-array fallback");

  for (int i = 0; i < c->embCount; i++) {
    const EmbeddedEntry *e = &c->emb[i];
    if (!e->enable) continue;

    const char *slash = strrchr(e->archivePath, '/');
    const char *archiveFileName = slash ? slash + 1 : e->archivePath;

    /* prefix macro dari embedded.<n>.variable bila diset, kalau tidak dari
       nama entri; di-uppercase */
    const char *baseName = e->variable[0] ? e->variable : e->name;
    char prefix[EMBED_NAME_LEN + 8] = {0};
    size_t k = 0;
    for (const char *p = baseName; *p && k < sizeof(prefix) - 1; p++, k++) {
      char ch = *p;
      if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
      bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
      prefix[k] = alnum ? ch : '_';
    }

    char sym[512];
    embedSymBase(e->archivePath, sym, sizeof(sym)); /* sym sudah termasuk _binary_ */

    char line[2048];

    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "/* ===== embedded entry: %s ===== */\n", e->name);

    snprintf(line, sizeof(line), "#define EMBED_%s_ARCHIVE_NAME \"%s\"\n", prefix, archiveFileName);
    used += (size_t)strlen(line);
    strcat(body, line);

    snprintf(line, sizeof(line), "#define EMBED_%s_EXTRACT_DIR \"%s\"\n", prefix, e->extract);
    used += (size_t)strlen(line);
    strcat(body, line);

    snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL %s_start\n", prefix, sym);
    used += (size_t)strlen(line);
    strcat(body, line);

    if (useLd) {
      /* GNU ld: end adalah alamat nyata satu-byte-setelah-blok */
      snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL_END %s_end\n", prefix, sym);
      used += (size_t)strlen(line);
      strcat(body, line);

      snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL_LEN (%s_end - %s_start)\n", prefix, sym,
               sym);
      used += (size_t)strlen(line);
      strcat(body, line);

      snprintf(line, sizeof(line),
               "extern const unsigned char %s_start[];\n"
               "extern const unsigned char %s_end[];\n\n",
               sym, sym);
      used += (size_t)strlen(line);
      strcat(body, line);
    } else {
      /* fallback C array: END adalah array dummy, LEN dari simbol _len */
      snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL_END %s_end\n", prefix, sym);
      used += (size_t)strlen(line);
      strcat(body, line);

      snprintf(line, sizeof(line), "#define EMBED_%s_SYMBOL_LEN %s_len\n", prefix, sym);
      used += (size_t)strlen(line);
      strcat(body, line);

      snprintf(line, sizeof(line),
               "extern const unsigned char %s_start[];\n"
               "extern const unsigned char %s_end[];\n"
               "extern const unsigned long %s_len;\n\n",
               sym, sym, sym);
      used += (size_t)strlen(line);
      strcat(body, line);
    }
  }

  used += (size_t)snprintf(body + used, sizeof(body) - used, "#endif /* RUPA_EMBEDDED_H */\n");

  char embeddedHeader[MAX_PATH + 32];
  snprintf(embeddedHeader, sizeof(embeddedHeader), "%s/embedded.h", c->outBuildDir);
  mkparent(embeddedHeader);

  /* lewati penulisan ulang bila tidak berubah agar mtime tetap stabil */
  static char existing[16384];
  FILE *rf = fopen(embeddedHeader, "rb");
  if (rf) {
    size_t n = fread(existing, 1, sizeof(existing) - 1, rf);
    fclose(rf);
    if (n == used && memcmp(existing, body, n) == 0) return true;
  }

  FILE *hf = fopen(embeddedHeader, "w");
  if (!hf) {
    fprintf(stderr, "rbot: cannot write %s\n", embeddedHeader);
    return false;
  }
  fwrite(body, 1, used, hf);
  fclose(hf);
  if (changed) *changed = true;
  return true;
}

/*
 * embedResourceCompile — fallback embed untuk platform tanpa
 * `ld -r -b binary` (MSVC, atau MinGW tanpa GNU ld). Arsip dibaca lalu
 * ditulis ulang sebagai file C berisi array byte:
 *
 *   const unsigned char <sym>_start[] = { 0x.., ... };
 *   const unsigned long <sym>_len = <n>;
 *
 * <sym> = "_binary_" + path arsip ternormalisasi — sama dengan emitEmbeddedHeader
 * sehingga macro EMBED_<n>_* di build/embedded.h valid di kedua jalur.
 * File C itu dikompilasi toolchain aktif (object-nya di-link ke binary) —
 * tidak butuh ld sama sekali.
 */
bool embedResourceCompile(const EmbeddedEntry *e, const Config *c) {
  FILE *in = fopen(e->archivePath, "rb");
  if (!in) return false;

  char cfile[MAX_PATH + 96];
  snprintf(cfile, sizeof(cfile), "%s.embed.c", e->objectPath);
  mkparent(cfile);

  FILE *out = fopen(cfile, "w");
  if (!out) {
    fclose(in);
    return false;
  }

  char sym[512];
  embedSymBase(e->archivePath, sym, sizeof(sym));

  fprintf(out, "/* Auto-generated by rbot (embed fallback). Do not edit. */\n\n");
  fprintf(out, "const unsigned char %s_start[] = {", sym);

  unsigned char buf[4096];
  size_t n;
  long long total = 0;
  int perLine = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    for (size_t i = 0; i < n; i++) {
      if (perLine == 0) fprintf(out, "\n");
      fprintf(out, "0x%02x,", buf[i]);
      if (++perLine == 20) perLine = 0;
      total++;
    }
  }
  fclose(in);

  fprintf(out, "\n};\n");

  // TAMBAHKAN INI: Dummy _end array (1 byte)
  fprintf(out, "const unsigned char %s_end[1] = { 0 };\n", sym);

  fprintf(out, "const unsigned long %s_len = %lld;\n", sym, total);
  fclose(out);

  char *inc = includeFlags(c);
  /* Objek embed ikut dikemas ke library: bila varian shared diminta, kompilasi
     dengan -fPIC (GNU/Clang) — sama seperti object source di
     compileLibraryOne/cmdsRunParallelJobs. Tanpa ini link .so gagal dengan
     "recompile with -fPIC" di lingkungan tanpa GNU ld (jalur fallback ini).
     MSVC tidak butuh flag PIC. */
#ifndef _WIN32
  char *wf = NULL;
  if (c->libShared && compilerIsMSVC(c) == false)
    wf = picWarningFlags(c);
  else
    wf = warningFlags(c);
#else
  char *wf = warningFlags(c);
#endif
  bool ok = compileOne(c, inc, wf, cfile, e->objectPath);
  free(inc);
  free(wf);
  if (!ok) fprintf(stderr, "rbot: %s: failed to compile embed resource\n", e->name);
  return ok;
}

static bool buildEmbeddedArchiveEntry(const EmbeddedEntry *e) {
  if (e->usePrebuilt) {
    if (fsFileExists(e->archivePath)) return true;
    /* Embedded.<n>.file hilang HARUS gagal jelas (design: silent-swallow
       audit) — tanpa pesan ini, build "berhenti" tanpa arahan dan fase
       berikutnya memakai object embed lama. */
    fprintf(stderr,
            "rbot: %s: embedded file '%s' tidak ditemukan\n",
            e->name, e->archivePath);
    return false;
  }
  if (!fsDirExists(e->src)) {
    fprintf(stderr, "rbot: %s: src '%s' not found\n", e->name, e->src);
    return false;
  }
  if (embArchiveFresh(e)) return true;

  char tmpArchive[MAX_PATH * 2 + 32];
  snprintf(tmpArchive, sizeof(tmpArchive), ".rbot-embed-%s.tmp", e->name);
  fsRemoveFile(tmpArchive);
  bool ok;
  if (e->tar) {
    const char *z = (strcmp(e->ext, "gz") == 0) ? "z" : "";

    /* Snapshot the files before invoking tar.  This is required when
       archive.src is "."/"./": the archive output directory and rbot's        temporary files can live inside the source tree, so `tar ... .`
       otherwise observes the tree changing while it is being read. */
    List files = {0};
    if (e->pattern[0])
      walkDir(e->src, e->pattern, &files);
    else
      walkDir(e->src, "", &files);
    /* Listfile tar menentukan urutan entri arsip: sortir sekali pada data
       lengkap agar arsip reprodusible antar mesin (urutan readdir tidak
       lagi melekat pada hasil). */
    listSort(&files);

    char listFile[MAX_PATH * 2 + 32];
    snprintf(listFile, sizeof(listFile), ".rbot-embed-%s.list", e->name);
    FILE *lf = fopen(listFile, "w");
    if (!lf) {
      listFree(&files);
      return false;
    }

    char rel[MAX_PATH * 2];
    for (int i = 0; i < files.count; i++) {
      const char *path = files.items[i];
      embRelPath(e, path, rel, sizeof(rel));
      if (*rel && !embExcluded(e, rel)) fprintf(lf, "%s\n", rel);
    }
    fclose(lf);
    listFree(&files);

    size_t cap = strlen(tmpArchive) + strlen(e->src) + strlen(listFile) + 96;
    char *cmd = malloc(cap);
    if (!cmd) {
      fsRemoveFile(listFile);
      return false;
    }
    snprintf(cmd, cap, "tar c%sf %s -C %s -T %s", z, tmpArchive, e->src, listFile);
    ok = runCmd(cmd);
    free(cmd);
    fsRemoveFile(listFile);
  } else {
    char *cmd = malloc(strlen(tmpArchive) + strlen(e->src) + 64);
    if (!cmd) return false;
    sprintf(cmd, "tar czf %s -C %s .", tmpArchive, e->src);
    ok = runCmd(cmd);
    free(cmd);
  }
  if (!ok) {
    fsRemoveFile(tmpArchive);
    fprintf(stderr, "rbot: %s: failed to archive %s\n", e->name, e->src);
    return false;
  }
  mkparent(e->archivePath);
  fsRemoveFile(e->archivePath);
  if (rename(tmpArchive, e->archivePath) != 0) {
    fsRemoveFile(tmpArchive);
    fprintf(stderr, "rbot: %s: cannot move archive into place (%s)\n", e->name, e->archivePath);
    return false;
  }
  embArchiveStateSave(e);
  return true;
}

static bool buildEmbeddedEntry(const Config *c, const EmbeddedEntry *e) {
  if (!buildEmbeddedArchiveEntry(e)) return false;

  if (e->usePrebuilt) {
    printf("> Embedded  : %s (file: %s)\n", e->name, e->archivePath);
  } else if (ldAvailable() || fsFileExists(e->archivePath)) {
    printf("> Embedded  : %s (archive: %s)\n", e->name, e->archivePath);
  }

  if (!fsNewerThan(e->archivePath, e->objectPath)) return true;

  if (ldAvailable()) {
    char *cmd = malloc(strlen(e->archivePath) + strlen(e->objectPath) + 64);
    if (!cmd) return false;
    sprintf(cmd, "ld -r -z noexecstack -b binary -o %s %s", e->objectPath, e->archivePath);
    bool ok = runCmd(cmd);
    free(cmd);
    if (!ok) fprintf(stderr, "rbot: %s: failed to embed %s\n", e->name, e->archivePath);
    return ok;
  }

  return embedResourceCompile(e, c);
}

bool buildEmbeddedArchives(const Config *c) {
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    if (!buildEmbeddedArchiveEntry(&c->emb[i])) return false;
  }
  return true;
}

bool buildEmbedded(const Config *c) {
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    if (!buildEmbeddedEntry(c, &c->emb[i])) return false;
  }
  return true;
}

/* ==================== finalisasi entri ==================== */

/*
 * configFinalizeEntry — turunkan archivePath & objectPath dari setting
 * arsip entri. Dipindah apa adanya dari config.c: pengetahuan penamaan
 * arsip (<dir>/<name>[.tar.<ext>]) lebih cocok tinggal di modul embed;
 * config.c tinggal memanggilnya dari configFinalize() per entri.
 */
void configFinalizeEntry(EmbeddedEntry *e, const char *buildDir) {
  if (e->usePrebuilt && e->prebuiltPath[0]) {
    /* Arsip jadi (embedded.<n>.file): jalurnya apa adanya; object tetap di
       build dir dengan nama entri. */
    copyStr(e->archivePath, sizeof(e->archivePath), e->prebuiltPath);
    snprintf(e->objectPath, sizeof(e->objectPath), "%s/%s.o", buildDir, e->name);
    return;
  }

  /* archive path: <archiveDir>/<n>[.tar.<ext>] tergantung with.tar/with.ext */
  if (e->tar) {
    if (e->ext[0])
      snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.tar.%s", e->archiveDir,
               e->archiveName, e->ext);
    else
      snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.tar", e->archiveDir, e->archiveName);
  } else if (e->ext[0]) {
    snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.%s", e->archiveDir, e->archiveName,
             e->ext);
  } else {
    snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s", e->archiveDir, e->archiveName);
  }

  /* object ada di bawah build dir, mis. build/rupa_modules.o */
  snprintf(e->objectPath, sizeof(e->objectPath), "%s/%s.o", buildDir, e->archiveName);
}
