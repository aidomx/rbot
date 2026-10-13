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
  /* defines mempengaruhi hasil kompilasi sama seperti flags (design/flags.md). */
  fp->compile = fpList(fp->compile, &c->defines);
  fp->compile = fpList(fp->compile, &c->headerPublic);
  /* headers.internal: dipantau tanpa -I; perubahan daftarnya ikut memicu build. */
  if (c->headerInternal.count) fp->compile = fpList(fp->compile, &c->headerInternal);
  /* Shared library objects need -fPIC, so this changes compile output. */
  fp->compile = fpString(fp->compile, c->libShared ? "pic" : "no-pic");

  fp->sources = 14695981039346656037ULL;
  for (int i = 0; i < srcs->count; i++) {
    char obj[MAX_PATH];
    fp->sources = fpString(fp->sources, srcs->items[i]);
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj)))
      fp->sources = fpString(fp->sources, obj);
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

void cmdsFingerprintSave(const CmdsBuildFingerprint *fp, bool libOnly) {
  CmdsBuildFingerprint out = *fp;
  if (libOnly) {
    /* Fase library (pass 1 workspace) TIDAK meng-commit hash sources/link:
       pass binary dalam run yang sama membaca file ini dan harus tetap
       mendeteksi source set / link config berubah (mis. source dihapus ->
       binary wajib di-link ulang; dulu pass men Cetak "up-to-date" dan
       binary tetap basi). Hash compile tetap current agar pass binary tidak
       mengulang kompilasi yang baru selesai pass library. Bila belum ada
       catatan (proyek segar), hash current dipakai sebagai baseline. */
    CmdsBuildFingerprint prev;
    if (cmdsFingerprintLoad(&prev)) {
      out.sources = prev.sources;
      out.link = prev.link;
    }
  }
  mkdirs(".rbot");
  FILE *f = fopen(BUILD_FP_FILE ".tmp", "w");
  if (!f) return;
  fprintf(f, "%016llx %016llx %016llx\n", (unsigned long long)out.compile,
          (unsigned long long)out.link, (unsigned long long)out.sources);
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

void cmdsFingerprintInvalidate(void) {
  fsRemoveFile(BUILD_FP_FILE);
}

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
  listReserve(&dirs, 1024);
  listReserve(&files, 1024);
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

/*
 * Pemindai state berbasis buffer (dipakai baik oleh jalur mmap maupun
 * fallback fopen): fscanf-file -> scanf-buffer. Format persis sama dengan
 * fastStateLoad lama; hanya sumbernya yang berbeda.
 *
 * Return true bila seluruh field valid. *endOut = pointer MISALNYA ke
 * karakter setelah last token yang dikonsumsi (dipakai utk sanity check).
 */
static bool fastStateScan(const char *buf, size_t size, FastState *s) {
  memset(s, 0, sizeof(*s));
  /* scanf buffer: buat buffer NUL-terminated. State txt kecil (<~16MB). */
  char *tmp = malloc(size + 1);
  if (!tmp) return false;
  memcpy(tmp, buf, size);
  tmp[size] = '\0';

  char magic[32];
  long long bfm = 0, tm = 0;
  int libReq = 0, libSt = 0, libSh = 0, phase = 0;
  const char *cur = tmp;
  int consumed = 0;
  bool ok =
      sscanf(cur, "%31s", magic) == 1 && strcmp(magic, BUILD_STATE_MAGIC) == 0;
  consumed += magic[0] ? (int)strlen(magic) : 0;
  if (ok) {
    consumed += 1;
    cur += consumed;
    ok = sscanf(
            cur,
            "%1023s %1023s %lld %lld %2047s %lld %lld %d %d %d %d %d %d %d %2047s %2047s %lld %lld%n",
            s->cwd, s->buildfile, &bfm, &s->buildfileSize, s->target, &tm, &s->targetSize,
            &s->compiled, &s->skipped, &s->count, &phase, &libReq, &libSt, &libSh,
            s->libStaticPath, s->libSharedPath, &s->libStaticSize, &s->libSharedSize,
            &consumed) == 18 &&
        s->count >= 0 && s->count <= 200000;
  }
  if (ok) {
    cur += consumed;
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
    if (s->count && !s->stamps) ok = false;
  }
  for (int i = 0; ok && i < s->count; i++) {
    long long mt = 0, sz = 0;
    if (sscanf(cur, " %c %lld %lld %4095s%n", &s->stamps[i].kind, &mt, &sz,
               s->stamps[i].path, &consumed) != 4) {
      fastStateFree(s);
      ok = false;
      break;
    }
    cur += consumed;
    s->stamps[i].mtime = (int64_t)mt;
    s->stamps[i].size = sz;
  }
  free(tmp);
  return ok;
}

static bool fastStateLoad(FastState *s, bool libOnly) {
  /* design/bootstrap.md fase 2: map state sebagai READ-ONLY lewat mmap
     (POSIX) / CreateFileMapping (Windows) — evaluasi noop tidak menyalin
     file lewat stdio. Fallback tradisional (aturan 4) tetap ada bila map
     gagal (file kosong/OS terbatas): body identik dengan fastStateScan. */
  void *map = NULL;
  size_t mapSize = 0;
  bool mapped = stateFileFor(libOnly) != NULL && fsMapRead(stateFileFor(libOnly), &map, &mapSize);
  if (mapped) {
    bool ok = fastStateScan(map, mapSize, s);
    fsMapClose(map, mapSize);
    return ok;
  }
  /* Fallback: baca file tradisional ke memori, lalu pakai scanner sama. */
  FILE *f = fopen(stateFileFor(libOnly), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0 || n > 64 * 1024 * 1024) {
    fclose(f);
    return false;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return false;
  }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  bool ok = fastStateScan(buf, rd, s);
  free(buf);
  return ok;
}

/* Placeholder "-" = target/varian library tidak aktif (path kosong tidak
   bisa dibaca %s). Target "-" sah untuk state fase library proyek tanpa
   library: validasi murni dari stamp input + object. */
static bool fastTargetRecorded(const char *target) {
  return target[0] && strcmp(target, "-") != 0;
}

/* ==================== memo stat per proses ====================
   Validasi fast-state dipanggil per fase (library lalu binary) dalam satu
   proses dan memeriksa daftar stamp yang nyaris identik. Hasil stat disimpan
   di sini supaya fase kedua tidak men-stat ulang path yang sama. Memo hanya
   sah selama tidak ada build di antaranya: cmdBuildEx memanggil
   cmdsStatMemoReset tepat sebelum build dimulai. */
typedef struct {
  char *path;
  bool exists;
  int64_t mtime;
  long long size;
} StatMemoEntry;
static StatMemoEntry *statMemo = NULL;
static size_t statMemoCap = 0, statMemoCount = 0;
/* cwd saat validasi. Path relatif (mis. "src") berarti direktori berbeda di
   tiap proyek, jadi kunci memo harus menyertakan cwd agar tidak tertukar. */
static char statMemoCwd[MAX_PATH * 2];

static uint64_t statMemoHash(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

static void statMemoRehash(size_t newCap) {
  StatMemoEntry *fresh = calloc(newCap, sizeof(StatMemoEntry));
  if (!fresh) return; /* gagal alokasi: lewati memo, pemanggil men-stat langsung */
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = fresh;
  statMemoCap = newCap;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    size_t j = statMemoHash(old[i].path) & (newCap - 1);
    while (statMemo[j].path) j = (j + 1) & (newCap - 1);
    statMemo[j] = old[i];
  }
  free(old);
}

/* Kunci memo = path absolut ternormalisasi secara leksikal (buang "." dan
   "..", tanpa menyentuh symlink). Dengan begitu "../library/a.h" dari proyek
   core dan dari proyek bade menjadi satu entri, sehingga tidak di-stat dua kali. */
static void memoKeyFor(const char *path, char *out, size_t n) {
  char joined[MAX_PATH * 4];
  if (path[0] == '/' || statMemoCwd[0] == '\0')
    snprintf(joined, sizeof(joined), "%s", path);
  else
    snprintf(joined, sizeof(joined), "%s/%s", statMemoCwd, path);

  char work[MAX_PATH * 4];
  snprintf(work, sizeof(work), "%s", joined);
  const char *seg[256];
  int ns = 0;
  char *savePtr = NULL;
  for (char *tok = strtok_r(work, "/", &savePtr); tok; tok = strtok_r(NULL, "/", &savePtr)) {
    if (strcmp(tok, ".") == 0 || tok[0] == '\0') continue;
    if (strcmp(tok, "..") == 0) {
      if (ns > 0) ns--;
      continue;
    }
    if (ns >= 256) {
      snprintf(out, n, "%s", joined); /* terlalu dalam: pakai path apa adanya */
      return;
    }
    seg[ns++] = tok;
  }
  size_t used = 0;
  out[used++] = '/';
  out[used] = '\0';
  for (int i = 0; i < ns; i++) {
    size_t len = strlen(seg[i]);
    if (used + len + 2 > n) {
      snprintf(out, n, "%s", joined);
      return;
    }
    memcpy(out + used, seg[i], len);
    used += len;
    out[used++] = '/';
    out[used] = '\0';
  }
  if (used > 1) out[used - 1] = '\0'; /* buang '/' terakhir */
}

static bool memoStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  char key[MAX_PATH * 4];
  memoKeyFor(path, key, sizeof(key));

  if (statMemoCap == 0 || (statMemoCount + 1) * 2 > statMemoCap)
    statMemoRehash(statMemoCap ? statMemoCap * 2 : 4096);
  if (statMemoCap == 0) return fsStampNsSize(path, mtimeNs, size);

  size_t j = statMemoHash(key) & (statMemoCap - 1);
  while (statMemo[j].path && strcmp(statMemo[j].path, key) != 0)
    j = (j + 1) & (statMemoCap - 1);
  StatMemoEntry *e = &statMemo[j];
  if (e->path) {
    if (!e->exists) return false;
    *mtimeNs = e->mtime;
    *size = e->size;
    return true;
  }
  bool ok = fsStampNsSize(path, mtimeNs, size);
  /* Hanya hasil positif yang disimpan: file yang belum ada bisa dibuat build,
     jadi negatif selalu di-stat ulang. */
  if (ok) {
    char *dup = strdup(key);
    if (dup) {
      e->path = dup;
      e->exists = true;
      e->mtime = *mtimeNs;
      e->size = *size;
      statMemoCount++;
    }
  }
  return ok;
}

/* Memo stat untuk validator workspace (cmds_wsstate.c): kunci dan semantik sama
   dengan validasi single project, sehingga fase library dan binary berbagi hasil. */
/* Kunci memo memakai cwd; pemanggil di luar cmdsFastStateValid (validator ws)
   wajib menyetelnya dulu, jika tidak entri proyek lain bisa terpakai. */
void cmdsStatMemoSetCwd(void) {
  if (!fsGetCwd(statMemoCwd, sizeof(statMemoCwd))) statMemoCwd[0] = '\0';
}

bool cmdsStatCached(const char *path, int64_t *mtimeNs, long long *size) {
  return memoStampNsSize(path, mtimeNs, size);
}

/* Parser baris format state F/D/X (format bersama ws.state, ws.lib.state dan
   workspace.state — lihat header cmds_wsstate.c). Dipakai validator di
   cmds_wsstate.c dan gate workspace di uses/workspace.c agar format hidup
   di SATU tempat. */
bool cmdsParseStateEntry(const char *line, char *kind, long long *mt, long long *sz, char *path) {
  char k = line[0];
  path[0] = '\0';
  *mt = 0;
  *sz = 0;
  if (k == 'X') {
    if (sscanf(line, "X %*d %*d %" WS_SCAN "s", path) != 1) return false;
  } else if (k == 'D') {
    if (sscanf(line, "D %lld %*d %" WS_SCAN "s", mt, path) != 2) return false;
  } else if (k == 'F') {
    if (sscanf(line, "F %lld %lld %" WS_SCAN "s", mt, sz, path) != 3) return false;
  } else {
    return false;
  }
  *kind = k;
  return true;
}

/* true bila baris F/D/X masih cocok stamp disk saat ini: F = mtime+size sama,
   D = mtime sama, X = path masih tidak ada. False bila parse gagal atau stamp
   beda. Memakai memo stat (cmdsStatCached) — pemanggil validator wajib sudah
   menyetel cwd memo (cmdsStatMemoSetCwd) bila lintas proyek. */
bool cmdsStateEntryFresh(const char *line) {
  char kind = 0;
  long long mt = 0, sz = 0;
  char p[WS_PATH_LEN];
  int64_t cm = -1;
  long long cs = -1;
  if (!cmdsParseStateEntry(line, &kind, &mt, &sz, p)) return false;
  bool exists = cmdsStatCached(p, &cm, &cs);
  if (kind == 'X') return !exists;
  if (!exists) return false;
  if (kind == 'D') return cm == (int64_t)mt;
  return cm == (int64_t)mt && cs == sz;
}

void cmdsStatMemoReset(void) {
  for (size_t i = 0; i < statMemoCap; i++) free(statMemo[i].path);
  free(statMemo);
  statMemo = NULL;
  statMemoCap = 0;
  statMemoCount = 0;
}

bool cmdsFastStateValid(const char *buildfilePath, bool libOnly) {
  if (!fsGetCwd(statMemoCwd, sizeof(statMemoCwd))) statMemoCwd[0] = '\0';
  FastState s;
  if (!fastStateLoad(&s, libOnly)) return false;
  if (s.phase != (libOnly ? 1 : 0)) {
    fastStateFree(&s);
    return false;
  }
  char cwd[MAX_PATH];
  int64_t buildfileMtime = -1, targetMtime = -1;
  long long buildfileSize = -1, targetSize = -1;
  bool buildfileStamp = memoStampNsSize(s.buildfile, &buildfileMtime, &buildfileSize);
  bool targetStamp =
      fastTargetRecorded(s.target) ? memoStampNsSize(s.target, &targetMtime, &targetSize) : true;
  bool targetOk = !fastTargetRecorded(s.target) ||
                  (targetStamp && targetMtime == s.targetMtime && targetSize == s.targetSize);
  bool ok =
      fsGetCwd(cwd, sizeof(cwd)) && strcmp(cwd, s.cwd) == 0 &&
      strcmp(buildfilePath && *buildfilePath ? buildfilePath : "Buildfile", s.buildfile) == 0 &&
      buildfileStamp && buildfileMtime == s.buildfileMtime && buildfileSize == s.buildfileSize &&
      targetOk;
  for (int i = 0; ok && i < s.count; i++) {
    FastStamp *st = &s.stamps[i];
    int64_t currentMtime = -1;
    long long currentSize = -1;
    if (st->kind == 'L') {
      /* RBOT_NO_LD sengaja memaksa jalur normal/fallback; jangan biarkan
         fastState menelan override itu. */
      char ldPath[MAX_PATH];
      bool cached = false;
      if (getenv("RBOT_NO_LD") || !ldCacheResolve("ld", ldPath, sizeof(ldPath)) ||
          !cacheCheckLD(ldPath, &cached)) {
        ok = false;
        break;
      }
      continue;
    }
    bool exists = memoStampNsSize(st->path, &currentMtime, &currentSize);
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
          printf("%s Library : %s%s\n", repPrefix(), s.libStaticPath,
                 s.libStaticUp ? " (up-to-date)" : "");
        if (s.libSharedPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libSharedPath,
                 s.libSharedUp ? " (up-to-date)" : "");
      }
    } else {
      printf("%s Build with cached state (fingerprint unchanged)\n", repPrefix());
      printf("%s Linking : %s (up-to-date)\n", repPrefix(), s.target);
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libStaticPath,
                 s.libStaticUp ? " (up-to-date)" : "");
        if (s.libSharedPath[0])
          printf("%s Library : %s%s\n", repPrefix(), s.libSharedPath,
                 s.libSharedUp ? " (up-to-date)" : "");
      }
    }
    printf("\n%s Summary :\n", repPrefix());
    if (s.phase == 1) {
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libStaticPath,
                 s.libStaticSize > 0 ? (double)s.libStaticSize / 1024.0 : 0);
        if (s.libSharedPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libSharedPath,
                 s.libSharedSize > 0 ? (double)s.libSharedSize / 1024.0 : 0);
      }
    } else {
      printf("    Target   : %s\n", s.target);
      printf("    Size     : %.1fKB\n", s.targetSize > 0 ? (double)s.targetSize / 1024.0 : 0);
      if (s.libRequested) {
        if (s.libStaticPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libStaticPath,
                 s.libStaticSize > 0 ? (double)s.libStaticSize / 1024.0 : 0);
        if (s.libSharedPath[0])
          printf("    Library  : %s (%.1fKB)\n", s.libSharedPath,
                 s.libSharedSize > 0 ? (double)s.libSharedSize / 1024.0 : 0);
      }
    }
    printf("    Compiled : %d\n", s.compiled);
    printf("    Skipped  : %d\n", s.skipped);
    printf("    Status   : Success\n");
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
  /* headers.internal: folder header dipantau sebagai input jalur cepat, tapi
     TIDAK masuk includeDirs (bukan -I), sehingga tidak membayangi header sistem. */
  for (int i = 0; i < c->headerInternal.count; i++) {
    const char *d = c->headerInternal.items[i];
    if (d && *d && d[0] != '-' && strcmp(d, ".") != 0) fastCollectTree(d, &paths);
  }
  /* Include file inputs that are not necessarily inside source/header roots. */
  fastStampAdd(&paths, bf);
  fastStampAdd(&paths, ".rbot-version");
  if (compdbEnabled(c) && fsFileExists("compile_commands.json"))
    fastStampAdd(&paths, "compile_commands.json");
  for (int i = 0; i < srcs->count; i++) {
    char obj[MAX_PATH];
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj))) fastStampAdd(&paths, obj);
  }
  bool hasEmbedded = false;
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    hasEmbedded = true;
    fastStampAdd(&paths, c->emb[i].objectPath);
    /* embedded.<n>.file adalah input eksternal proyek ini. Tanpa dicatat
       di fast-state, perubahan archive dapat terlewat sebelum cmdBuild()
       sempat menjalankan fase embed/link normal. Arsip yang DIHASILKAN
       (generate) juga dicatat: walkDir koleksi source proyek (jalur
       commands.c) tidak menyentuh folder archiveDir, jadi tanpa ini
       perubahan archive tidak membatalkan fast path. */
    if (c->emb[i].archivePath[0]) fastStampAdd(&paths, c->emb[i].archivePath);
  }
  /* Embedded bergantung pada verdict ld lintas-run. Tandai cache dengan
     kind khusus agar fastState memvalidasi ld.cache -> ld, bukan sekadar
     mtime file cache. Bila cache tidak valid/hilang, fast path gugur dan
     build normal akan mem-probe lalu memperbarui cache. */
  if (hasEmbedded) fastStampAdd(&paths, ldCacheFile());
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
    stamps[count].kind = strcmp(path, ldCacheFile()) == 0 && hasEmbedded
                             ? 'L'
                             : (mt < 0 ? 'M' : (fsDirExists(path) ? 'D' : 'F'));
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

/* Buang satu entri memo (kunci = path ternormalisasi, sama seperti insert). */
void cmdsStatMemoForget(const char *path) {
  if (statMemoCap == 0 || !path) return;
  char key[MAX_PATH * 4];
  memoKeyFor(path, key, sizeof(key));
  /* Bangun ulang tabel tanpa kunci itu; open addressing tak punya tombstone. */
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = calloc(oldCap, sizeof(StatMemoEntry));
  if (!statMemo) {
    statMemo = old;
    statMemoCap = oldCap;
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    if (strcmp(old[i].path, key) == 0) {
      free(old[i].path);
      continue;
    }
    size_t j = statMemoHash(old[i].path) & (oldCap - 1);
    while (statMemo[j].path) j = (j + 1) & (oldCap - 1);
    statMemo[j] = old[i];
    kept++;
  }
  free(old);
  statMemoCount = kept;
}

/* Buang semua entri di bawah folder `dir` (relatif cwd), termasuk folder itu. */
void cmdsStatMemoForgetTree(const char *dir) {
  if (statMemoCap == 0 || !dir) return;
  char prefix[MAX_PATH * 4];
  memoKeyFor(dir, prefix, sizeof(prefix));
  size_t pl = strlen(prefix);
  StatMemoEntry *old = statMemo;
  size_t oldCap = statMemoCap;
  statMemo = calloc(oldCap, sizeof(StatMemoEntry));
  if (!statMemo) {
    statMemo = old;
    statMemoCap = oldCap;
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].path) continue;
    bool drop = strncmp(old[i].path, prefix, pl) == 0 &&
                (old[i].path[pl] == '\0' || old[i].path[pl] == '/');
    if (drop) {
      free(old[i].path);
      continue;
    }
    size_t j = statMemoHash(old[i].path) & (oldCap - 1);
    while (statMemo[j].path) j = (j + 1) & (oldCap - 1);
    statMemo[j] = old[i];
    kept++;
  }
  free(old);
  statMemoCount = kept;
}
