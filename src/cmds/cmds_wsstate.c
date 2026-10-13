/*
 * cmds_wsstate — fast-state khusus mode workspace. Terpisah dari
 * cmdsFastStateSave/cmdsFastStateValid (yang tetap untuk single project).
 *
 * File: .rbot/ws.state (fase binary) dan .rbot/ws.lib.state (fase library).
 * Format (whitespace-delimited, path tanpa spasi):
 *   RBOTWS2 <cwd>
 *   META <skipped> <target|-> <buildfile>
 *   LIB <path>                 library yang diminta (dicetak saat no-op)
 *   F <mtime_ns> <size> <path> input/output yang dipakai (source, object, closure header,
 *                              target, library, archive embed, ld.cache, Buildfile)
 *   D <mtime_ns> 0 <path>      folder header: penambahan/penghapusan mengubah mtime-nya
 *   X 0 0 <path>               path yang tidak ada saat disimpan (harus tetap tidak ada)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compile.h"
#include "../deps.h"
#include "../embed_ld_cache.h"
#include "../portability.h"
#include "../util.h"
#include "cmds_internal.h"

#define WS_STATE_MAGIC "RBOTWS2"

static bool wsModeOn = false;

static void srcCacheReset(void); /* didefinisikan di bagian langkah 1 */

/* Setiap proyek punya cwd dan .rbot/ws.src sendiri. Cache daftar sumber
   harus dikosongkan di awal setiap proyek; jika tidak, record proyek
   sebelumnya terbawa dan ditulis ke file proyek berikutnya. */
void wsModeSet(bool on) {
  wsModeOn = on;
  if (on) srcCacheReset();
}
bool wsModeActive(void) { return wsModeOn; }

static void addKind(List *l, char kind, const char *path) {
  if (!path || !*path) return;
  char entry[MAX_PATH * 4];
  snprintf(entry, sizeof(entry), "%c %s", kind, path);
  listAdd(l, entry);
}

/* Samakan dengan headerEntryDir (compile.c): "I." = ".", "-..." dilewati. */
static void wsAddHeaderDir(List *l, const char *entry) {
  if (!entry || !*entry || entry[0] == '-') return;
  const char *d = (entry[0] == 'I' && entry[1] == '.') ? entry + 1 : entry;
  if (strcmp(d, ".") == 0 || !*d) return;
  addKind(l, 'D', d);
}

typedef struct {
  List *items;
} ClosureCtx;

static void closureCollect(const char *path, void *ctx) {
  addKind(((ClosureCtx *)ctx)->items, 'F', path);
}

static int cmpEntry(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void wsAddSourceDirs(List *items);

static bool hasSpace(const char *s) { return s && strpbrk(s, " \t\r\n") != NULL; }

static const char *wsStateFile(bool libOnly) {
  return libOnly ? ".rbot/ws.lib.state" : ".rbot/ws.state";
}

void wsFastStateSave(struct DepCache *dc, const Config *c, const List *srcs, const char *target,
                     const char *buildfilePath, bool libOnly) {
  if (!dc || !c || !srcs) return;
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  cmdsStatMemoSetCwd();
  const char *bf = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  const char *tgt = (!libOnly && target && *target) ? target : "-";
  if (hasSpace(cwd) || hasSpace(bf) || hasSpace(tgt)) return; /* format whitespace-delimited */

  List items = {0};
  addKind(&items, 'F', bf);
  if (strcmp(tgt, "-") != 0) addKind(&items, 'O', tgt);

  /* Library yang diminta: dicatat sebagai input (berubah -> rebuild) dan
     sebagai LIB agar status up-to-date dicetak saat no-op. */
  List libs = {0};
  char staticLib[WS_PATH_LEN] = "", sharedLib[WS_PATH_LEN] = "";
  if (c->libRequested) {
    if (c->libStatic) cmdsLibStaticPath(c, staticLib, sizeof(staticLib));
    if (c->libShared) cmdsLibSharedPath(c, sharedLib, sizeof(sharedLib));
  }
  if (staticLib[0]) {
    addKind(&items, 'O', staticLib);
    addKind(&libs, 'L', staticLib);
  }
  if (sharedLib[0]) {
    addKind(&items, 'O', sharedLib);
    addKind(&libs, 'L', sharedLib);
  }

  ClosureCtx ctx = {&items};
  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    addKind(&items, 'F', src);
    /* Closure = header yang benar-benar dijangkau source ini (dari .d / snapshot). */
    depsClosureVisit(dc, src, closureCollect, &ctx);
    char obj[WS_PATH_LEN];
    if (objectPathFor(c, src, obj, sizeof(obj))) addKind(&items, 'O', obj);
  }

  /* Embedded: object, archive hasil generate, dan verdict ld (sama dengan single). */
  bool hasEmb = false;
  for (int i = 0; i < c->embCount; i++) {
    if (!c->emb[i].enable) continue;
    hasEmb = true;
    addKind(&items, 'O', c->emb[i].objectPath);
    if (c->emb[i].archivePath[0]) addKind(&items, 'O', c->emb[i].archivePath);
  }
  if (hasEmb) addKind(&items, 'O', ldCacheFile());

  /* Folder sumber: setiap folder yang ditelusuri daftar sumber (termasuk
     subfolder) dicatat sebagai stamp direktori. Penambahan/penghapusan file
     .c/.cpp mengubah mtime folder, sehingga fast path tidak melewatkannya. */
  wsAddSourceDirs(&items);

  /* Folder header: stamp direktori saja (penambahan/penghapusan header terdeteksi). */
  for (int i = 0; i < c->headerPublic.count; i++) wsAddHeaderDir(&items, c->headerPublic.items[i]);
  for (int i = 0; i < c->headerInternal.count; i++) wsAddHeaderDir(&items, c->headerInternal.items[i]);

  if (items.count > 1) qsort(items.items, (size_t)items.count, sizeof(char *), cmpEntry);

  /* Pre-pass: output dan folder build/ dibuang dari memo sebelum ada pembacaan.
     Dengan begitu input yang dibaca dari memo tidak memuat nilai pra-build. */
  cmdsStatMemoForgetTree("build");
  for (int i = 0; i < items.count; i++)
    if (items.items[i][0] == 'O') cmdsStatMemoForget(items.items[i] + 2);

  const char *path = wsStateFile(libOnly);
  mkdirs(".rbot");
  char tmp[WS_PATH_LEN];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *fp = fopen(tmp, "w");
  if (!fp) {
    listFree(&items);
    listFree(&libs);
    return;
  }
  fprintf(fp, "%s %s\n", WS_STATE_MAGIC, cwd);
  fprintf(fp, "META %d %s %s\n", srcs->count, tgt, bf);
  for (int i = 0; i < libs.count; i++) fprintf(fp, "LIB %s\n", libs.items[i] + 2);

  char prev[MAX_PATH * 4] = "";
  for (int i = 0; i < items.count; i++) {
    if (strcmp(items.items[i], prev) == 0) continue; /* hapus duplikat */
    snprintf(prev, sizeof(prev), "%s", items.items[i]);
    char kind = prev[0];
    const char *p = prev + 2;
    int64_t m = 0;
    long long sz = 0;
    /* Output baru dibangun: stat langsung. Input (source, header, folder)
       tidak berubah selama build, jadi boleh dari memo. */
    /* Output: stat langsung. Input (source, header, folder): dari memo (nilai
       dari validasi/decide yang sudah didapat); pra-pass di atas menjamin tidak ada
       output atau folder build/ yang basi di memo. */
    bool exists = kind == 'O' ? fsStampNsSize(p, &m, &sz) : cmdsStatCached(p, &m, &sz);
    if (!exists) {
      fprintf(fp, "X 0 0 %s\n", p);
    } else if (kind == 'D') {
      fprintf(fp, "D %lld 0 %s\n", (long long)m, p);
    } else {
      fprintf(fp, "F %lld %lld %s\n", (long long)m, sz, p);
    }
  }
  bool ok = fclose(fp) == 0;
  if (ok) rename(tmp, path);
  else fsRemoveFile(tmp);
  listFree(&items);
  listFree(&libs);

}

bool wsFastStateValid(const char *buildfilePath, bool libOnly) {
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return false;
  cmdsStatMemoSetCwd(); /* memo harus memakai cwd proyek ini, bukan proyek sebelumnya */
  FILE *fp = fopen(wsStateFile(libOnly), "r");
  if (!fp) return false;

  const char *want = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  char line[MAX_PATH * 6];
  bool ok = fgets(line, sizeof(line), fp) != NULL;
  if (ok) {
    char magic[32], scwd[WS_PATH_LEN];
    ok = sscanf(line, "%31s %" WS_SCAN "s", magic, scwd) == 2 && strcmp(magic, WS_STATE_MAGIC) == 0 &&
         strcmp(scwd, cwd) == 0;
  }
  int skipped = 0;
  char tgt[WS_PATH_LEN] = "", bf[WS_PATH_LEN] = "";
  if (ok) {
    ok = fgets(line, sizeof(line), fp) != NULL &&
         sscanf(line, "META %d %" WS_SCAN "s %" WS_SCAN "s", &skipped, tgt, bf) == 3 && strcmp(bf, want) == 0;
  }

  List libs = {0};
  while (ok && fgets(line, sizeof(line), fp)) {
    char kind = line[0];
    if (kind == 'L') {
      char p[WS_PATH_LEN];
      if (sscanf(line, "LIB %" WS_SCAN "s", p) == 1) addKind(&libs, 'L', p);
      continue;
    }
    /* F/D/X: parse + cocokkan stamp lewat helper bersama (cmds_state.c). */
    if (kind == 'F' || kind == 'D' || kind == 'X') ok = cmdsStateEntryFresh(line);
  }
  fclose(fp);
  if (!ok) {
    listFree(&libs);
    return false;
  }

  /* Output no-op: sama dengan jalur single project. */
  if (!libOnly) {
    printf("%s Build with cached state (fingerprint unchanged)\n", repPrefix());
    printf("%s Linking : %s (up-to-date)\n", repPrefix(), tgt);
  }
  for (int i = 0; i < libs.count; i++)
    printf("%s Library : %s (up-to-date)\n", repPrefix(), libs.items[i] + 2);
  printf("\n%s Summary :\n", repPrefix());
  if (!libOnly) {
    int64_t tm = -1;
    long long sizeBytes = -1;
    cmdsStatCached(tgt, &tm, &sizeBytes); /* ukuran dari memo (sudah dicek validasi) */
    printf("    Target   : %s\n", tgt);
    printf("    Size     : %.1fKB\n", sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0);
  }
  for (int i = 0; i < libs.count; i++) {
    int64_t lm = -1;
    long long sizeBytes = -1;
    cmdsStatCached(libs.items[i] + 2, &lm, &sizeBytes);
    printf("    Library  : %s (%.1fKB)\n", libs.items[i] + 2,
           sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0);
  }
  printf("    Compiled : 0\n");
  printf("    Skipped  : %d\n", skipped);
  printf("    Status   : Success\n");
  listFree(&libs);
  return true;
}

/* ==================== langkah 1: cache daftar sumber ====================
 * Penelusuran folder sumber (walkDir) dilakukan setiap fase. Di mode workspace
 * hasilnya disimpan di .rbot/ws.src bersama stamp mtime setiap folder yang
 * ditelusuri. Mtime dicatat SEBELUM folder dibaca, jadi bila ada perubahan
 * selama walk, run berikutnya melihat stamp berbeda dan menelusuri ulang.
 * Format: RBOTSRC1 <cwd>, lalu per entri: R <dir> <ext>, D <mtime_ns> <path>
 * (-1 bila folder tidak ada), S <file>. */

typedef struct {
  char *dir;
  char *ext;
  List stamps; /* "D <mtime> <path>" */
  List files;  /* file cocok ext, urutan walk */
} SrcRec;

static SrcRec *srcRecs = NULL;
static int srcRecCount = 0, srcRecCap = 0;
static bool srcLoaded = false;
static char srcCwd[WS_PATH_LEN];

static SrcRec *srcRecFind(const char *dir, const char *ext) {
  for (int i = 0; i < srcRecCount; i++)
    if (strcmp(srcRecs[i].dir, dir) == 0 && strcmp(srcRecs[i].ext, ext) == 0) return &srcRecs[i];
  return NULL;
}

static SrcRec *srcRecNew(const char *dir, const char *ext) {
  if (srcRecCount == srcRecCap) {
    int next = srcRecCap ? srcRecCap * 2 : 16;
    SrcRec *grown = realloc(srcRecs, (size_t)next * sizeof(SrcRec));
    if (!grown) return NULL;
    srcRecs = grown;
    srcRecCap = next;
  }
  SrcRec *r = &srcRecs[srcRecCount++];
  memset(r, 0, sizeof(*r));
  r->dir = strdup(dir);
  r->ext = strdup(ext);
  return r;
}

static void srcLoad(void) {
  if (srcLoaded) return;
  srcLoaded = true;
  if (!fsGetCwd(srcCwd, sizeof(srcCwd))) return;
  FILE *fp = fopen(".rbot/ws.src", "r");
  if (!fp) return;
  char line[MAX_PATH * 6];
  if (fgets(line, sizeof(line), fp)) {
    char magic[32], cwd[WS_PATH_LEN];
    if (sscanf(line, "%31s %" WS_SCAN "s", magic, cwd) != 2 || strcmp(magic, "RBOTSRC1") != 0 ||
        strcmp(cwd, srcCwd) != 0) {
      fclose(fp);
      return; /* cache milik folder lain: abaikan */
    }
  }
  SrcRec *cur = NULL;
  while (fgets(line, sizeof(line), fp)) {
    if (line[0] == 'R') {
      char d[WS_PATH_LEN], e[64];
      /* ekstensi kosong ditulis "-" (token sscanf tak boleh kosong) */
      if (sscanf(line, "R %" WS_SCAN "s %63s", d, e) == 2) {
        if (strcmp(e, "-") == 0) e[0] = '\0';
        cur = srcRecNew(d, e);
      }
      continue;
    }
    if (!cur) continue;
    if (line[0] == 'D') {
      size_t n = strcspn(line, "\n");
      line[n] = '\0';
      listAdd(&cur->stamps, line);
    } else if (line[0] == 'S') {
      char f[WS_PATH_LEN];
      if (sscanf(line, "S %" WS_SCAN "s", f) == 1) listAdd(&cur->files, f);
    }
  }
  fclose(fp);
}

static bool srcRecValid(const SrcRec *r) {
  for (int i = 0; i < r->stamps.count; i++) {
    long long mt = 0;
    char p[WS_PATH_LEN];
    if (sscanf(r->stamps.items[i], "D %lld %" WS_SCAN "s", &mt, p) != 2) return false;
    int64_t curM = -1;
    long long curS = -1;
    bool exists = cmdsStatCached(p, &curM, &curS);
    if (mt < 0) {               /* saat disimpan folder tidak ada: valid selama tetap tidak ada */
      if (exists) return false;
      continue;
    }
    if (!exists || curM != (int64_t)mt) return false;
  }
  return true;
}

static void srcWalk(SrcRec *r, const char *dir, List *out) {
  int64_t mt = -1;
  long long sz = 0;
  bool ok = fsStampNsSize(dir, &mt, &sz); /* stamp SEBELUM dibaca */
  char stamp[MAX_PATH * 3];
  snprintf(stamp, sizeof(stamp), "D %lld %s", ok ? (long long)mt : -1LL, dir);
  listAdd(&r->stamps, stamp);

  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  size_t elen = strlen(r->ext);
  for (int i = 0; i < files.count; i++) {
    const char *path = files.items[i];
    size_t len = strlen(path);
    if (len > elen && strcmp(path + len - elen, r->ext) == 0) {
      listAdd(out, path);
      listAdd(&r->files, path);
    }
  }
  for (int i = 0; i < dirs.count; i++) {
    /* .rbot adalah state: tidak pernah jadi input. Mencatatnya membuat cache
       basi di setiap run, karena rbot sendiri menulis isi .rbot. */
    const char *b = strrchr(dirs.items[i], '/');
    b = b ? b + 1 : dirs.items[i];
    if (strcmp(b, ".rbot") == 0) continue;
    srcWalk(r, dirs.items[i], out);
  }
  listFree(&dirs);
  listFree(&files);
}

/* Bandingkan isi .rbot/ws.src dengan daftar baris yang akan ditulis. */
static bool srcFileSame(const List *lines) {
  FILE *fp = fopen(".rbot/ws.src", "r");
  if (!fp) return false;
  char line[WS_PATH_LEN * 2];
  bool same = true;
  int i = 0;
  while (same && fgets(line, sizeof(line), fp)) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = '\0';
    if (i >= lines->count || strcmp(line, lines->items[i]) != 0) same = false;
    i++;
  }
  if (same && i != lines->count) same = false;
  fclose(fp);
  return same;
}

static void srcSave(void) {
  List lines = {0};
  char buf[WS_PATH_LEN * 2];
  snprintf(buf, sizeof(buf), "RBOTSRC1 %s", srcCwd);
  listAdd(&lines, buf);
  for (int i = 0; i < srcRecCount; i++) {
    SrcRec *r = &srcRecs[i];
    snprintf(buf, sizeof(buf), "R %s %s", r->dir, r->ext[0] ? r->ext : "-");
    listAdd(&lines, buf);
    for (int j = 0; j < r->stamps.count; j++) listAdd(&lines, r->stamps.items[j]);
    for (int j = 0; j < r->files.count; j++) {
      snprintf(buf, sizeof(buf), "S %s", r->files.items[j]);
      listAdd(&lines, buf);
    }
  }
  if (!srcFileSame(&lines)) {
    mkdirs(".rbot");
    FILE *fp = fopen(".rbot/ws.src.tmp", "w");
    if (fp) {
      for (int i = 0; i < lines.count; i++) fprintf(fp, "%s\n", lines.items[i]);
      if (fclose(fp) == 0) rename(".rbot/ws.src.tmp", ".rbot/ws.src");
      else fsRemoveFile(".rbot/ws.src.tmp");
    }
  }
  listFree(&lines);
}

/* Pengganti walkDir di jalur workspace. Di luar mode workspace: walkDir biasa. */
/* Salin stamp folder ("D <mtime> <path>") milik record cache (dir, ext). Dipakai
   freshness arsip: mtime folder berubah bila file ditambah/dihapus. Panggil
   setelah wsWalkDirCached agar record sudah valid/terbaru. */
void wsWalkDirStamps(const char *dir, const char *ext, List *out) {
  if (!wsModeOn || !dir || !ext) return;
  srcLoad();
  SrcRec *r = srcRecFind(dir, ext);
  if (!r) return;
  for (int i = 0; i < r->stamps.count; i++) listAdd(out, r->stamps.items[i]);
}

void wsWalkDirCached(const char *dir, const char *ext, List *out) {
  if (!wsModeOn || !dir || !ext || hasSpace(dir) || hasSpace(ext)) {
    walkDir(dir, ext, out);
    return;
  }
  srcLoad();
  SrcRec *r = srcRecFind(dir, ext);
  if (r && srcRecValid(r)) {
    for (int i = 0; i < r->files.count; i++) listAdd(out, r->files.items[i]);
    return;
  }
  if (r) {
    listFree(&r->stamps);
    listFree(&r->files);
  } else {
    r = srcRecNew(dir, ext);
    if (!r) {
      walkDir(dir, ext, out);
      return;
    }
  }
  srcWalk(r, dir, out);
  srcSave();
}

static void srcCacheReset(void) {
  for (int i = 0; i < srcRecCount; i++) {
    free(srcRecs[i].dir);
    free(srcRecs[i].ext);
    listFree(&srcRecs[i].stamps);
    listFree(&srcRecs[i].files);
  }
  srcRecCount = 0;
  srcLoaded = false;
}

/* Folder sumber yang ditelusuri (termasuk subfolder) dicatat sebagai D. */
static void wsAddSourceDirs(List *items) {
  for (int i = 0; i < srcRecCount; i++) {
    for (int j = 0; j < srcRecs[i].stamps.count; j++) {
      long long mt = 0;
      char p2[WS_PATH_LEN];
      if (sscanf(srcRecs[i].stamps.items[j], "D %lld %" WS_SCAN "s", &mt, p2) == 2)
        addKind(items, 'D', p2);
    }
  }
}

/* mtime untuk keputusan decide. Mode workspace: lewat memo (output yang berubah
   dibuang dari memo saat state disimpan). Mode single: stat langsung, perilaku lama. */
int64_t cmdsMTimeDecide(const char *path) {
  if (!wsModeOn) return fsMTimeNs(path);
  int64_t m = -1;
  long long sz = 0;
  if (cmdsStatCached(path, &m, &sz)) return m;
  return -1;
}
