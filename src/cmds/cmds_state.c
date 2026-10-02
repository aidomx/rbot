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
#define BUILD_STATE_FILE ".rbot/build.state"
#define BUILD_STATE_MAGIC "RBOTFAST2"

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

static bool fastStateLoad(FastState *s) {
  memset(s, 0, sizeof(*s));
  FILE *f = fopen(BUILD_STATE_FILE, "r");
  if (!f) return false;
  char magic[32];
  long long bfm = 0, tm = 0;
  int libReq = 0, libSt = 0, libSh = 0;
  if (fscanf(f, "%31s", magic) != 1 || strcmp(magic, BUILD_STATE_MAGIC) != 0 ||
      fscanf(f,
             "%1023s %1023s %lld %lld %2047s %lld %lld %d %d %d %d %d %d %2047s %2047s %lld %lld",
             s->cwd, s->buildfile, &bfm, &s->buildfileSize, s->target, &tm, &s->targetSize,
             &s->compiled, &s->skipped, &s->count, &libReq, &libSt, &libSh, s->libStaticPath,
             s->libSharedPath, &s->libStaticSize, &s->libSharedSize) != 17 ||
      s->count < 0 || s->count > 200000) {
    fclose(f);
    return false;
  }
  s->buildfileMtime = (int64_t)bfm;
  s->targetMtime = (int64_t)tm;
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

bool cmdsFastStateValid(const char *buildfilePath) {
  FastState s;
  if (!fastStateLoad(&s)) return false;
  char cwd[MAX_PATH];
  int64_t buildfileMtime = -1, targetMtime = -1;
  long long buildfileSize = -1, targetSize = -1;
  bool buildfileStamp = fsStampNsSize(s.buildfile, &buildfileMtime, &buildfileSize);
  bool targetStamp = fsStampNsSize(s.target, &targetMtime, &targetSize);
  bool ok =
      fsGetCwd(cwd, sizeof(cwd)) && strcmp(cwd, s.cwd) == 0 &&
      strcmp(buildfilePath && *buildfilePath ? buildfilePath : "Buildfile", s.buildfile) == 0 &&
      buildfileStamp && buildfileMtime == s.buildfileMtime && buildfileSize == s.buildfileSize &&
      targetStamp && targetMtime == s.targetMtime && targetSize == s.targetSize;
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
    printf("> Build with cached state (fingerprint unchanged)\n");
    printf("> Linking   : %s (up-to-date)\n", s.target);
    if (s.libRequested) {
      if (s.libStaticPath[0])
        printf("> Library   : %s%s\n", s.libStaticPath, s.libStaticUp ? " (up-to-date)" : "");
      if (s.libSharedPath[0])
        printf("> Library   : %s%s\n", s.libSharedPath, s.libSharedUp ? " (up-to-date)" : "");
    }
    printf("\n> Summary\n");
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
    printf("Compiled : %d\n", s.compiled);
    printf("Skipped  : %d\n", s.skipped);
    printf("Status   : Success\n");
  }
  fastStateFree(&s);
  return ok;
}

void cmdsFastStateSave(const Config *c, const List *srcs, const char *target,
                       const char *buildfilePath) {
  /* Fast path hanya aman bila root build sama dengan cwd. Untuk root khusus,
     gunakan jalur normal sampai tersedia normalisasi path absolut. */
  if (strcmp(c->root, ".") != 0) {
    fsRemoveFile(BUILD_STATE_FILE);
    return;
  }
  char cwd[MAX_PATH];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  const char *bf = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  int64_t bfm = fsMTimeNs(bf);
  long long bfs = fsFileSize(bf);
  int64_t tm = fsMTimeNs(target);
  long long ts = fsFileSize(target);
  if (bfm < 0 || tm < 0 || bfs < 0 || ts < 0) return;
  /* Format cache is whitespace-delimited; disable it for paths with spaces. */
  if (strpbrk(cwd, " \t\r\n") || strpbrk(bf, " \t\r\n") || strpbrk(target, " \t\r\n")) return;

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
       sempat menjalankan fase embed/link normal. */
    if (c->emb[i].usePrebuilt && c->emb[i].archivePath[0])
      fastStampAdd(&paths, c->emb[i].archivePath);
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
  FILE *f = fopen(BUILD_STATE_FILE ".tmp", "w");
  if (!f) {
    free(stamps);
    return;
  }
  fprintf(f, "%s\n%s %s %lld %lld %s %lld %lld %d %d %d %d %d %d %s %s %lld %lld\n",
          BUILD_STATE_MAGIC, cwd, bf, (long long)bfm, bfs, target, (long long)tm, ts, 0,
          srcs->count, count, c->libRequested ? 1 : 0, staticUp ? 1 : 0, sharedUp ? 1 : 0,
          staticLib[0] ? staticLib : "-", sharedLib[0] ? sharedLib : "-", staticSz, sharedSz);
  for (int i = 0; i < count; i++)
    fprintf(f, "%c %lld %lld %s\n", stamps[i].kind, (long long)stamps[i].mtime, stamps[i].size,
            stamps[i].path);
  bool ok = fclose(f) == 0;
  free(stamps);
  if (ok) {
#ifdef _WIN32
    fsRemoveFile(BUILD_STATE_FILE);
#endif
    if (rename(BUILD_STATE_FILE ".tmp", BUILD_STATE_FILE) != 0)
      fsRemoveFile(BUILD_STATE_FILE ".tmp");
  } else
    fsRemoveFile(BUILD_STATE_FILE ".tmp");
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
