/*
 * cmds_state — state build lintas-run & command clean
 * (dipindah apa adanya dari commands.c).
 *
 *   - Fingerprint konfigurasi compile/link + set source
 *     (.rbot/build.fingerprint): berubah -> rebuild penuh.
 *   - Fast no-op snapshot (.rbot/build.state): mtime+size seluruh input;
 *     persis sama -> summary diulang dari cache, build dilewati total.
 *   - cmdClean: hapus artefak build (build dir, file library,
 *     compile_commands.json, deps.cache).
 */
#include "cmds_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../commands.h"
#include "../compdb.h"
#include "../compile.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"

/* ==================== Fingerprint build state ==================== */

#define BUILD_FP_FILE ".rbot/build.fingerprint"

static uint64_t fpBytes(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t fpString(uint64_t h, const char *s) {
  const unsigned char sep = 0xff;
  if (s) h = fpBytes(h, s, strlen(s));
  return fpBytes(h, &sep, 1);
}

static uint64_t fpList(uint64_t h, const List *list) {
  for (int i = 0; i < list->count; i++)
    h = fpString(h, list->items[i]);
  const unsigned char end = 0xfe;
  return fpBytes(h, &end, 1);
}

void cmdsFingerprintMake(const Config *c, const List *srcs, CmdsBuildFingerprint *fp) {
  fp->compile = 14695981039346656037ULL;
  fp->compile = fpString(fp->compile, c->cc);
  /* std selalu terisi configFinalize sesuai bahasa (gnu11/c++17), jadi
     peralihan bahasa proyek otomatis mengubah fingerprint compile. */
  fp->compile = fpString(fp->compile, c->std);
  fp->compile = fpString(fp->compile, c->target);
  fp->compile = fpString(fp->compile, c->outBuildDir);
  fp->compile = fpList(fp->compile, &c->flags);
  fp->compile = fpList(fp->compile, &c->headerPublic);
  /* Shared library objects need -fPIC, so this changes compile output. */
  fp->compile = fpString(fp->compile, c->libShared ? "pic" : "no-pic");

  fp->sources = 14695981039346656037ULL;
  for (int i = 0; i < srcs->count; i++) {
    char obj[MAX_PATH];
    fp->sources = fpString(fp->sources, srcs->items[i]);
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj))) fp->sources = fpString(fp->sources, obj);
  }

  fp->link = 14695981039346656037ULL;
  fp->link = fpString(fp->link, c->cc);
  fp->link = fpString(fp->link, c->outBinaryDir);
  fp->link = fpString(fp->link, c->outBinaryName);
  fp->link = fpList(fp->link, &c->libraries);
  fp->link = fpString(fp->link, c->libRequested ? "lib" : "no-lib");
  fp->link = fpString(fp->link, c->libStatic ? "static" : "no-static");
  fp->link = fpString(fp->link, c->libShared ? "shared" : "no-shared");
  fp->link = fpString(fp->link, c->outLibDir);
  fp->link = fpString(fp->link, c->outLibName);
  for (int i = 0; i < c->embCount; i++) {
    fp->link = fpString(fp->link, c->emb[i].enable ? "enabled" : "disabled");
    fp->link = fpString(fp->link, c->emb[i].objectPath);
  }
}

bool cmdsFingerprintLoad(CmdsBuildFingerprint *fp) {
  FILE *f = fopen(BUILD_FP_FILE, "r");
  if (!f) return false;
  unsigned long long c = 0, l = 0, s = 0;
  bool ok = fscanf(f, "%llx %llx %llx", &c, &l, &s) == 3;
  fclose(f);
  if (!ok) return false;
  fp->compile = (uint64_t)c;
  fp->link = (uint64_t)l;
  fp->sources = (uint64_t)s;
  return true;
}

void cmdsFingerprintSave(const CmdsBuildFingerprint *fp) {
  mkdirs(".rbot");
  FILE *f = fopen(BUILD_FP_FILE ".tmp", "w");
  if (!f) return;
  fprintf(f, "%016llx %016llx %016llx\n", (unsigned long long)fp->compile,
          (unsigned long long)fp->link, (unsigned long long)fp->sources);
  bool ok = fclose(f) == 0;
  if (ok) {
#ifdef _WIN32
    fsRemoveFile(BUILD_FP_FILE);
#endif
    if (rename(BUILD_FP_FILE ".tmp", BUILD_FP_FILE) != 0) fsRemoveFile(BUILD_FP_FILE ".tmp");
  } else {
    fsRemoveFile(BUILD_FP_FILE ".tmp");
  }
}

void cmdsFingerprintInvalidate(void) { fsRemoveFile(BUILD_FP_FILE); }

/* ==================== Fast no-op snapshot ==================== */
/* Dua file state per fase build:
   - build.state     : fase binary (rbot / -w fase 2) — target = binary.
   - build.lib.state : fase library workspace (cmdBuildEx libOnly) —
     target = library proyek (atau tanpa target bila proyek tak meminta
     library; state-nya murni "semua input & object masih sama").
   Dengan state fase library, no-op `rbot -w` tidak membayar loadConfig +
   scan fingerprint + decide penuh per proyek (miniws: jalur ringan). */
#define BUILD_STATE_FILE ".rbot/build.state"
#define BUILD_STATE_FILE_LIB ".rbot/build.lib.state"
#define BUILD_STATE_MAGIC "RBOTFAST3"

typedef struct {
  char kind;
  char path[MAX_PATH];
  int64_t mtime;
  long long size;
} FastStamp;

typedef struct {
  char cwd[MAX_PATH];
  char buildfile[MAX_PATH];
  int64_t buildfileMtime;
  long long buildfileSize;
  char target[MAX_PATH * 2];
  int64_t targetMtime;
  long long targetSize;
  int compiled;
  int skipped;
  int count;
  int phase; /* 0 = binary pass, 1 = library pass */
  FastStamp *stamps;
  /* Ringkasan library: no-op berikutnya tetap menampilkan status lib. */
  bool libRequested, libStaticUp, libSharedUp;
  char libStaticPath[MAX_PATH * 2];
  char libSharedPath[MAX_PATH * 2];
  long long libStaticSize, libSharedSize;
} FastState;

static void fastStateFree(FastState *s) {
  free(s->stamps);
  memset(s, 0, sizeof(*s));
}

static bool fastStampAdd(List *paths, const char *path) {
  for (int i = 0; i < paths->count; i++)
    if (strcmp(paths->items[i], path) == 0) return true;
  return listAdd(paths, path);
}

static void fastCollectTree(const char *dir, List *paths) {
  if (!fsDirExists(dir)) return;
  fastStampAdd(paths, dir);
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  for (int i = 0; i < files.count; i++)
    fastStampAdd(paths, files.items[i]);
  for (int i = 0; i < dirs.count; i++)
    fastCollectTree(dirs.items[i], paths);
  listFree(&dirs);
  listFree(&files);
}

static const char *stateFileFor(bool libOnly) {
  return libOnly ? BUILD_STATE_FILE_LIB : BUILD_STATE_FILE;
}

static bool fastStateLoad(FastState *s, bool libOnly) {
  memset(s, 0, sizeof(*s));
  FILE *f = fopen(stateFileFor(libOnly), "r");
  if (!f) return false;
  char magic[32];
  long long bfm = 0, tm = 0;
  int libReq = 0, libSt = 0, libSh = 0, phase = 0;
  if (fscanf(f, "%31s", magic) != 1 || strcmp(magic, BUILD_STATE_MAGIC) != 0 ||
      fscanf(f,
             "%1023s %1023s %lld %lld %2047s %lld %lld %d %d %d %d %d %d %d %2047s %2047s %lld %lld",
             s->cwd, s->buildfile, &bfm, &s->buildfileSize, s->target, &tm, &s->targetSize,
             &s->compiled, &s->skipped, &s->count, &phase, &libReq, &libSt, &libSh,
             s->libStaticPath, s->libSharedPath, &s->libStaticSize, &s->libSharedSize) != 18 ||
      s->count < 0 || s->count > 200000) {
    fclose(f);
    return false;
  }
  s->buildfileMtime = (int64_t)bfm;
  s->targetMtime = (int64_t)tm;
  s->phase = phase;
  s->libRequested = libReq != 0;
  s->libStaticUp = libSt != 0;
  s->libSharedUp = libSh != 0;
  /* placeholder "-" = varian library tidak aktif (path kosong tidak bisa
     dibaca %s) */
  if (strcmp(s->libStaticPath, "-") == 0) s->libStaticPath[0] = '\0';
  if (strcmp(s->libSharedPath, "-") == 0) s->libSharedPath[0] = '\0';
  s->stamps = calloc((size_t)s->count, sizeof(FastStamp));
  if (s->count && !s->stamps) {
    fclose(f);
    return false;
  }
  for (int i = 0; i < s->count; i++) {
    long long mt = 0, sz = 0;
    if (fscanf(f, " %c %lld %lld %4095s", &s->stamps[i].kind, &mt, &sz, s->stamps[i].path) != 4) {
      fclose(f);
      fastStateFree(s);
      return false;
    }
    s->stamps[i].mtime = (int64_t)mt;
    s->stamps[i].size = sz;
  }
  fclose(f);
  return true;
}

/* Placeholder "-" = target/varian library tidak aktif (path kosong tidak
   bisa dibaca %s). Target "-" sah untuk state fase library proyek tanpa
   library: validasi murni dari stamp input + object. */
static bool fastTargetRecorded(const char *target) {
  return target[0] && strcmp(target, "-") != 0;
}

bool cmdsFastStateValid(const char *buildfilePath, bool libOnly) {
  FastState s;
  if (!fastStateLoad(&s, libOnly)) return false;
  if (s.phase != (libOnly ? 1 : 0)) {
    fastStateFree(&s);
    return false;
  }
  char cwd[MAX_PATH];
  int64_t buildfileMtime = -1, targetMtime = -1;
  long long buildfileSize = -1, targetSize = -1;
  bool buildfileStamp = fsStampNsSize(s.buildfile, &buildfileMtime, &buildfileSize);
  bool targetStamp = fastTargetRecorded(s.target)
                         ? fsStampNsSize(s.target, &targetMtime, &targetSize)
                         : true;
  bool targetOk =
      !fastTargetRecorded(s.target) || (targetStamp && targetMtime == s.targetMtime &&
                                        targetSize == s.targetSize);
  bool ok =
      fsGetCwd(cwd, sizeof(cwd)) && strcmp(cwd, s.cwd) == 0 &&
      strcmp(buildfilePath && *buildfilePath ? buildfilePath : "Buildfile", s.buildfile) == 0 &&
      buildfileStamp && buildfileMtime == s.buildfileMtime && buildfileSize == s.buildfileSize &&
      targetOk;
  for (int i = 0; ok && i < s.count; i++) {
    FastStamp *st = &s.stamps[i];
    int64_t currentMtime = -1;
    long long currentSize = -1;
    bool exists = fsStampNsSize(st->path, &currentMtime, &currentSize);
    if (st->kind == 'M') {
      if (exists) {
        ok = false;
        break;
      }
      continue;
    }
    if (!exists || currentMtime != st->mtime) {
      ok = false;
      break;
    }
    if (st->kind == 'F' && currentSize != st->size) {
      ok = false;
      break;
    }
  }
  if (ok) {
    if (s.phase == 1) {
      /* Fase library: tidak ada link binary — lapor status library saja. */
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("> Library   : %s%s\n", s.libStaticPath, s.libStaticUp ? " (up-to-date)" : "");
        if (s.libSharedPath[0])
          printf("> Library   : %s%s\n", s.libSharedPath, s.libSharedUp ? " (up-to-date)" : "");
      }
    } else {
      printf("> Build with cached state (fingerprint unchanged)\n");
      printf("> Linking   : %s (up-to-date)\n", s.target);
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("> Library   : %s%s\n", s.libStaticPath, s.libStaticUp ? " (up-to-date)" : "");
        if (s.libSharedPath[0])
          printf("> Library   : %s%s\n", s.libSharedPath, s.libSharedUp ? " (up-to-date)" : "");
      }
    }
    printf("\n> Summary\n");
    if (s.phase == 1) {
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("Library  : %s (%.1fKB)\n", s.libStaticPath,
                 s.libStaticSize > 0 ? (double)s.libStaticSize / 1024.0 : 0);
        if (s.libSharedPath[0])
          printf("Library  : %s (%.1fKB)\n", s.libSharedPath,
                 s.libSharedSize > 0 ? (double)s.libSharedSize / 1024.0 : 0);
      }
    } else {
      printf("Target   : %s\n", s.target);
      printf("Size     : %.1fKB\n", s.targetSize > 0 ? (double)s.targetSize / 1024.0 : 0);
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("Library  : %s (%.1fKB)\n", s.libStaticPath,
                 s.libStaticSize > 0 ? (double)s.libStaticSize / 1024.0 : 0);
        if (s.libSharedPath[0])
          printf("Library  : %s (%.1fKB)\n", s.libSharedPath,
                 s.libSharedSize > 0 ? (double)s.libSharedSize / 1024.0 : 0);
      }
    }
    printf("Compiled : %d\n", s.compiled);
    printf("Skipped  : %d\n", s.skipped);
    printf("Status   : Success\n");
  }
  fastStateFree(&s);
  return ok;
}

void cmdsFastStateSave(const Config *c, const List *srcs, const char *target,
                       const char *buildfilePath, bool libOnly) {
  const char *stateFile = stateFileFor(libOnly);
  /* Fast path hanya aman bila root build sama dengan cwd. Untuk root khusus,
     gunakan jalur normal sampai tersedia normalisasi path absolut. */
  if (strcmp(c->root, ".") != 0) {
    fsRemoveFile(stateFile);
    return;
  }
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  const char *bf = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  int64_t bfm = fsMTimeNs(bf);
  long long bfs = fsFileSize(bf);
  /* Fase library proyek tanpa library tidak punya target: simpan "-" —
     validasi murni dari stamp input + object (lihat fastTargetRecorded). */
  char targetBuf[MAX_PATH * 2];
  const char *tgt = target;
  if (!tgt || !tgt[0]) {
    copyStr(targetBuf, sizeof(targetBuf), "-");
    tgt = targetBuf;
  }
  int64_t tm = 0;
  long long ts = 0;
  if (fastTargetRecorded(tgt)) {
    tm = fsMTimeNs(tgt);
    ts = fsFileSize(tgt);
    if (tm < 0 || ts < 0) return;
  }
  if (bfm < 0 || bfs < 0) return;
  /* Format cache is whitespace-delimited; disable it for paths with spaces. */
  if (strpbrk(cwd, " \t\r\n") || strpbrk(bf, " \t\r\n") || strpbrk(tgt, " \t\r\n")) return;

  /* Library: status up-to-date saat build sukses selesai — ditampilkan
     lagi pada no-op berikutnya (dulu informasi ini hilang di fast path). */
  char staticLib[MAX_PATH * 2] = {0}, sharedLib[MAX_PATH * 2] = {0};
  bool staticUp = true, sharedUp = true;
  long long staticSz = 0, sharedSz = 0;
  if (c->libRequested) {
    if (c->libStatic) {
      cmdsLibStaticPath(c, staticLib, sizeof(staticLib));
      staticUp = cmdsLibVariantUpToDate(c, true, srcs);
      long long s1 = fsFileSize(staticLib);
      if (s1 < 0) return; /* lib diwajibkan tapi belum ada — jangan simpan */
      staticSz = s1;
    }
    if (c->libShared) {
      cmdsLibSharedPath(c, sharedLib, sizeof(sharedLib));
      sharedUp = cmdsLibVariantUpToDate(c, false, srcs);
      long long s2 = fsFileSize(sharedLib);
      if (s2 < 0) return;
      sharedSz = s2;
    }
  }

  List paths = {0};
  for (int i = 0; i < c->sources.count; i++)
    fastCollectTree(c->sources.items[i], &paths);
  List incDirs = {0};
  includeDirs(c, &incDirs);
  for (int i = 0; i < incDirs.count; i++) {
    const char *d = incDirs.items[i];
    if (d && *d && strcmp(d, ".") != 0) fastCollectTree(d, &paths);
  }
  listFree(&incDirs);
  /* Include file inputs that are not necessarily inside source/header roots. */
  fastStampAdd(&paths, bf);
  fastStampAdd(&paths, ".rbot-version");
  if (compdbEnabled(c) && fsFileExists("compile_commands.json"))
    fastStampAdd(&paths, "compile_commands.json");
  for (int i = 0; i < srcs->count; i++) {
    char obj[MAX_PATH];
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj))) fastStampAdd(&paths, obj);
  }
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    fastStampAdd(&paths, c->emb[i].objectPath);
    /* embedded.<n>.file adalah input eksternal proyek ini. Tanpa dicatat
       di fast-state, perubahan archive dapat terlewat sebelum cmdBuild()
       sempat menjalankan fase embed/link normal. Arsip yang DIHASILKAN
       (generate) juga dicatat: walkDir koleksi source proyek (jalur
       commands.c) tidak menyentuh folder archiveDir, jadi tanpa ini
       perubahan archive tidak membatalkan fast path. */
    if (c->emb[i].archivePath[0]) fastStampAdd(&paths, c->emb[i].archivePath);
  }
  /* Library masuk stamp seperti file biasa ('F'): berubah -> fast path
     batal dan jalur normal meng-rebuild varian yang perlu (per-varian). */
  if (staticLib[0]) fastStampAdd(&paths, staticLib);
  if (sharedLib[0]) fastStampAdd(&paths, sharedLib);

  FastStamp *stamps = calloc((size_t)paths.count, sizeof(FastStamp));
  if (paths.count && !stamps) {
    listFree(&paths);
    return;
  }
  int count = 0;
  bool safePaths = true;
  for (int i = 0; i < paths.count; i++) {
    const char *path = paths.items[i];
    if (strlen(path) >= sizeof(stamps[count].path) || strpbrk(path, " \t\r\n")) {
      safePaths = false;
      break;
    }
    /* Satu stat per path: mtime + ukuran sekaligus (fsStampNsSize). */
    int64_t mt = 0;
    long long sz = 0;
    if (!fsStampNsSize(path, &mt, &sz)) {
      mt = -1;
      sz = 0;
    }
    stamps[count].kind = mt < 0 ? 'M' : (fsDirExists(path) ? 'D' : 'F');
    snprintf(stamps[count].path, sizeof(stamps[count].path), "%s", path);
    stamps[count].mtime = mt;
    stamps[count].size = stamps[count].kind == 'F' ? sz : 0;
    count++;
  }
  if (!safePaths) {
    free(stamps);
    listFree(&paths);
    return;
  }
  listFree(&paths);
  mkdirs(".rbot");
  char stateTmp[MAX_PATH + 16];
  snprintf(stateTmp, sizeof(stateTmp), "%s.tmp", stateFile);
  FILE *f = fopen(stateTmp, "w");
  if (!f) {
    free(stamps);
    return;
  }
  fprintf(f, "%s\n%s %s %lld %lld %s %lld %lld %d %d %d %d %d %d %d %s %s %lld %lld\n",
          BUILD_STATE_MAGIC, cwd, bf, (long long)bfm, bfs, tgt, (long long)tm, ts, 0, srcs->count,
          count, libOnly ? 1 : 0, c->libRequested ? 1 : 0, staticUp ? 1 : 0, sharedUp ? 1 : 0,
          staticLib[0] ? staticLib : "-", sharedLib[0] ? sharedLib : "-", staticSz, sharedSz);
  for (int i = 0; i < count; i++)
    fprintf(f, "%c %lld %lld %s\n", stamps[i].kind, (long long)stamps[i].mtime, stamps[i].size,
            stamps[i].path);
  bool ok = fclose(f) == 0;
  free(stamps);
  if (ok) {
#ifdef _WIN32
    fsRemoveFile(stateFile);
#endif
    if (rename(stateTmp, stateFile) != 0) fsRemoveFile(stateTmp);
  } else
    fsRemoveFile(stateTmp);
}

/* ==================== clean ==================== */

int cmdClean(const char *buildfilePath) {
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;

  bool any = false;
  if (c.cleanBuildDir && safeRelative(c.outBuildDir)) {
    if (fsRemoveTree(c.outBuildDir))
      printf("> Removed   : %s\n", c.outBuildDir);
    else
      printf("> Removed   : %s (sebagian gagal dihapus)\n", c.outBuildDir);
    any = true;
  }
  /* Hapus FILE library (.a/.so/.dll), bukan foldernya — dulu fsRemoveTree
     pada outLibDir menghapus folder lib/ sekaligus isinya. Folder tetap
     ada; build berikutnya membuat ulang isinya bila diminta. */
  if (c.cleanBuildDir && c.libRequested && safeRelative(c.outLibDir)) {
    char libPath[MAX_PATH * 2];
    if (c.libStatic) {
      cmdsLibStaticPath(&c, libPath, sizeof(libPath));
      if (fsRemoveFile(libPath)) {
        printf("> Removed   : %s\n", libPath);
        any = true;
      }
    }
    if (c.libShared) {
      cmdsLibSharedPath(&c, libPath, sizeof(libPath));
      if (fsRemoveFile(libPath)) {
        printf("> Removed   : %s\n", libPath);
        any = true;
      }
    }
  }
  if (c.cleanCompileCommands && fsFileExists("compile_commands.json")) {
    fsRemoveFile("compile_commands.json");
    printf("> Removed   : compile_commands.json\n");
    any = true;
  }
  /* Snapshot hash dependensi dihapus juga: verifikasi konten dimulai dari
     nol pada build berikutnya (build sukses merekam ulang dari awal). */
  if (fsFileExists(".rbot/deps.cache")) {
    fsRemoveFile(".rbot/deps.cache");
    printf("> Removed   : .rbot/deps.cache\n");
    any = true;
  }
  /* State fast no-op kedua fase (binary & library) ikut dibuang. */
  if (fsFileExists(BUILD_STATE_FILE)) {
    fsRemoveFile(BUILD_STATE_FILE);
    printf("> Removed   : %s\n", BUILD_STATE_FILE);
    any = true;
  }
  if (fsFileExists(BUILD_STATE_FILE_LIB)) {
    fsRemoveFile(BUILD_STATE_FILE_LIB);
    printf("> Removed   : %s\n", BUILD_STATE_FILE_LIB);
    any = true;
  }
  /* Cache verdict deteksi GNU ld (.rbot/ld.cache) ikut dibuang: clean
     berarti mulai dari nol, run berikutnya mem-probe ulang `ld --version`
     sekali lalu merekamnya lagi. */
  const char *ldCache = ldCacheFile(); /* satu sumber nama file: embed_ld_cache */
  if (fsFileExists(ldCache)) {
    fsRemoveFile(ldCache);
    printf("> Removed   : %s\n", ldCache);
    any = true;
  }
  /* Staging packaging (.rbot/pack) adalah cache — artefak final di dist/
     TIDAK disentuh (bisa jadi dirujuk embedded.* proyek lain). */
  if (fsDirExists(".rbot/pack")) {
    fsRemoveTree(".rbot/pack");
    printf("> Removed   : .rbot/pack\n");
    any = true;
  }
  if (!any) printf("> Nothing to clean (see Buildfile: clean)\n");
  return 0;
}
