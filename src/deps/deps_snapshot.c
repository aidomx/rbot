/*
 * deps_snapshot — snapshot hash+mtime lintas-run & API publik deps
 * (dipindah apa adanya dari deps.c).
 *
 * Snapshot (.rbot/deps.cache) merekam hash konten seluruh file yang
 * terlibat build sukses terakhir: mtime dipakai sebagai shortcut (file
 * dengan mtime sama tidak dibaca ulang), hash memverifikasi `touch`
 * tanpa perubahan isi. Juga memuat section cache edges (!e/!k) dalam
 * satu buka file — lihat deps_dotd.c.
 */
#include "deps_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../compile.h"
#include "../portability.h"
#include "../util.h"

DepCache *depsNew(const Config *c) {
  DepCache *dc = calloc(1, sizeof(*dc));
  if (!dc) return NULL;
  dc->cfg = c;
  includeDirs(c, &dc->incDirs);
  return dc;
}

void depsFree(DepCache *dc) {
  if (!dc) return;
  for (int i = 0; i < dc->count; i++) {
    free(dc->nodes[i].path);
    free(dc->nodes[i].kids);
  }
  free(dc->nodes);
  free(dc->hbucket);
  free(dc->sbucket);
  free(dc->ebucket);
  for (int i = 0; i < dc->edgeCount; i++) {
    free(dc->edges[i].src);
    for (int k = 0; k < dc->edges[i].nkids; k++)
      free(dc->edges[i].kids[k]);
    free(dc->edges[i].kids);
  }
  free(dc->edges);
  listFree(&dc->incDirs);
  for (int i = 0; i < dc->snapCount; i++)
    free(dc->snap[i].path);
  free(dc->snap);
  free(dc);
}

/* ===== hash table path -> index snapshot (schemanya sama dengan node) ===== */

static void snapHashRebuild(DepCache *dc, int ncap) {
  int *nb = calloc((size_t)ncap, sizeof(int));
  if (!nb) return;
  free(dc->sbucket);
  dc->sbucket = nb;
  dc->sbcap = ncap;
  int mask = ncap - 1;
  for (int idx = 0; idx < dc->snapCount; idx++) {
    int i = (int)(depPathHash(dc->snap[idx].path) & (uint64_t)mask);
    while (nb[i]) i = (i + 1) & mask;
    nb[i] = idx + 1;
  }
}

/* Daftarkan entry idx ke tabel; grow saat load factor > 0.75. */
static void snapHashInsert(DepCache *dc, int idx) {
  if (!dc->sbcap || (idx + 1) * 4 > dc->sbcap * 3)
    snapHashRebuild(dc, dc->sbcap ? dc->sbcap * 2 : 256);
  int mask = dc->sbcap - 1;
  int i = (int)(depPathHash(dc->snap[idx].path) & (uint64_t)mask);
  while (dc->sbucket[i]) i = (i + 1) & mask;
  dc->sbucket[i] = idx + 1;
}

/* Index snapshot untuk path, atau -1. O(1) vs linear scan dulu. */
static int snapFind(const DepCache *dc, const char *path) {
  if (!dc->sbcap) return -1;
  int mask = dc->sbcap - 1;
  int i = (int)(depPathHash(path) & (uint64_t)mask);
  for (;;) {
    int v = dc->sbucket[i];
    if (!v) return -1;
    int idx = v - 1;
    if (strcmp(dc->snap[idx].path, path) == 0) return idx;
    i = (i + 1) & mask;
  }
}

/* Format baris cache: "<path> <mtime_ns_hex> <hash_hex>". Versi 2: mtime
   diikutkan sebagai shortcut — file dengan mtime sama tidak perlu dibaca
   ulang untuk di-hash. Cache tanpa mtime (versi 1) tetap terbaca: mtime
   dianggap 0, alias "hash wajib dihitung ulang sekali". */
void depSnapshotLoad(DepCache *dc) {
  if (dc->snapLoaded) return;
  dc->snapLoaded = true;

  FILE *fp = fopen(DEPS_SNAPSHOT_FILE, "r");
  if (!fp) {
    dc->snapIncomplete = true; /* belum ada snapshot — wajib direkam */
    return;
  }

  /* Satu pass untuk SEMUA section: snapshot hash + cache edges (!e/!k).
     Sebelumnya edgesLoad membuka & mem-parsing file ini lagi — dua kali
     baca+parsesetiap run, terasa di proot maupun termux. */
  bool wantEdges = !dc->edgesLoaded;
  dc->edgesLoaded = true;
  int curEdge = -1;

  char line[MAX_PATH + 128];
  while (fgets(line, sizeof(line), fp)) {
    if (line[0] == '!') {
      if (!wantEdges) continue;
      if (line[1] == 'e' && line[2] == ' ') {
        char src[MAX_PATH];
        unsigned long long dm = 0;
        int cnt = -1;
        int got = sscanf(line + 3, "%1023s %16llx %d", src, &dm, &cnt);
        if (got < 2) continue;
        if (got < 3) cnt = -1;
        if (dc->edgeCount == dc->edgeCap) {
          int ncap = dc->edgeCap ? dc->edgeCap * 2 : 128;
          void *nn = realloc(dc->edges, (size_t)ncap * sizeof(*dc->edges));
          if (!nn) continue;
          dc->edges = nn;
          dc->edgeCap = ncap;
        }
        int idx = dc->edgeCount;
        dc->edges[idx].src = strdup(src);
        if (!dc->edges[idx].src) continue;
        dc->edges[idx].dm = (int64_t)dm;
        dc->edges[idx].kids = NULL;
        dc->edges[idx].nkids = 0;
        dc->edges[idx].expect = cnt;
        dc->edges[idx].used = false;
        dc->edgeCount++;
        curEdge = idx;
        /* hash table dibangun SEKALI setelah semua baris dibaca (di bawah).
           Dulu tabel hanya dibangun ulang saat load > 0.75, sehingga entri
           yang dimuat setelah rebuild terakhir tidak pernah masuk tabel:
           edgeFind meleset, file .d dibuka ulang dan deps.cache ditulis
           ulang pada SETIAP build, termasuk no-op. */
      } else if (line[1] == 'k' && line[2] == ' ') {
        if (curEdge < 0 || curEdge >= dc->edgeCount) continue;
        /* Baris "!k <path>": ambil sisa baris apa adanya (tanpa sscanf —
           ~1800 baris pada project 200 source). Path dengan spasi ikut utuh. */
        char *path = line + 3;
        size_t pl = strlen(path);
        while (pl && (path[pl - 1] == '\n' || path[pl - 1] == '\r')) path[--pl] = '\0';
        if (!pl) continue;
        /* kapasitas kids tumbuh geometris: 4, 8, 16, ... (bukan realloc per
           kid). Kapasitas tersirat dari nkids: 4 untuk nkids 0..3, lalu
           digandakan tiap nkids mencapai pangkat dua >= 4. */
        int nk0 = dc->edges[curEdge].nkids;
        if (nk0 == 0 || (nk0 >= 4 && (nk0 & (nk0 - 1)) == 0)) {
          size_t ncap = nk0 == 0 ? 4 : (size_t)nk0 * 2;
          char **nk = realloc(dc->edges[curEdge].kids, ncap * sizeof(char *));
          if (!nk) continue;
          dc->edges[curEdge].kids = nk;
        }
        char *dup = strdup(path);
        if (!dup) continue;
        dc->edges[curEdge].kids[nk0] = dup;
        dc->edges[curEdge].nkids = nk0 + 1;
      }
      continue;
    }
    char path[MAX_PATH];
    unsigned long long h = 0;
    unsigned long long m = 0;
    int got = sscanf(line, "%1023s %16llx %16llx", path, &m, &h);
    if (got == 2) { h = m; m = 0; } /* versi 1: path hash */
    if (got >= 2 && depIsObjectPath(path)) {
      dc->snapDirty = true; /* entri object peninggalan bug lama: buang */
      continue;
    }
    if (got >= 2) {
      if (dc->snapCount == dc->snapCap) {
        int ncap = dc->snapCap ? dc->snapCap * 2 : 256;
        void *nn = realloc(dc->snap, (size_t)ncap * sizeof(*dc->snap));
        if (!nn) break;
        dc->snap = nn;
        dc->snapCap = ncap;
      }
      dc->snap[dc->snapCount].path = strdup(path);
      dc->snap[dc->snapCount].mtime = (int64_t)m;
      dc->snap[dc->snapCount].hash = (uint64_t)h;
      snapHashInsert(dc, dc->snapCount);
      dc->snapCount++;
    }
  }
  fclose(fp);
  depEdgesHashBuild(dc);
}

static uint64_t snapshotHashOf(DepCache *dc, const char *path, int64_t *mtimeOut) {
  depSnapshotLoad(dc);
  int idx = snapFind(dc, path);
  if (idx < 0) return 0; /* 0 = tidak ada di snapshot */
  if (mtimeOut) *mtimeOut = dc->snap[idx].mtime;
  return dc->snap[idx].hash;
}

static void snapshotSet(DepCache *dc, const char *path, int64_t mtime, uint64_t hash) {
  depSnapshotLoad(dc);
  int idx = snapFind(dc, path);
  if (idx >= 0) {
    if (dc->snap[idx].hash != hash || dc->snap[idx].mtime != mtime) {
      dc->snap[idx].hash = hash;
      dc->snap[idx].mtime = mtime;
      dc->snapDirty = true;
    }
    return;
  }
  if (dc->snapCount == dc->snapCap) {
    int ncap = dc->snapCap ? dc->snapCap * 2 : 256;
    void *nn = realloc(dc->snap, (size_t)ncap * sizeof(*dc->snap));
    if (!nn) return;
    dc->snap = nn;
    dc->snapCap = ncap;
  }
  dc->snap[dc->snapCount].path = strdup(path);
  dc->snap[dc->snapCount].mtime = mtime;
  dc->snap[dc->snapCount].hash = hash;
  snapHashInsert(dc, dc->snapCount);
  dc->snapCount++;
  dc->snapDirty = true;
}

/*
 * Verifikasi satu node tanpa edges (daun dari edges .d, atau node apa pun):
 * false bila file hilang, belum pernah direkam, atau isinya berubah.
 * TIDAK membaca isi selama mtime masih sama dengan snapshot — dan TIDAK
 * membuka/memindai file untuk edges: daun .d tidak butuh edges.
 */
static bool nodeUpToDateShallow(DepCache *dc, int idx) {
  if (dc->nodes[idx].mtime < 0) return false; /* file hilang */

  int64_t recordedM = 0;
  uint64_t recorded = snapshotHashOf(dc, dc->nodes[idx].path, &recordedM);
  if (recorded == 0) {
    dc->snapIncomplete = true; /* file tak tertutup snapshot */
    return false;              /* belum pernah direkam */
  }

  if (dc->nodes[idx].mtime != recordedM) {
    /* mtime beda: `touch` atau konten benar-benar berubah — cek hash */
    if (dc->nodes[idx].hash == 0) dc->nodes[idx].hash = depComputeFileHash(dc->nodes[idx].path);
    if (dc->nodes[idx].hash != recorded) return false;
    snapshotSet(dc, dc->nodes[idx].path, dc->nodes[idx].mtime, dc->nodes[idx].hash);
  }
  return true;
}

/* Hash konten node untuk pass rekam — mtime shortcut dua arah: bila mtime
   sama dengan snapshot, pakai hash yang terekam tanpa membaca file. Ini
   membuat pass rekam setelah build no-op tidak membaca file sama sekali
   (tiap open+read mahal di proot). */
static uint64_t nodeRecordHash(DepCache *dc, int idx) {
  int64_t recordedM = 0;
  uint64_t recorded = snapshotHashOf(dc, dc->nodes[idx].path, &recordedM);
  if (recorded != 0 && recordedM == dc->nodes[idx].mtime) {
    dc->nodes[idx].hash = recorded;
    return recorded;
  }
  if (dc->nodes[idx].hash == 0) dc->nodes[idx].hash = depComputeFileHash(dc->nodes[idx].path);
  return dc->nodes[idx].hash;
}

/*
 * DFS: bandingkan hash konten subtree dengan snapshot build sukses.
 * false = ada file yang isinya benar-benar berubah.
 *
 * Node ber-flag dloaded (edges dari .d, flat-lengkap) tidak direkursi:
 * anak-anaknya diverifikasi shallow — mtime + snapshot, isi dibaca hanya
 * bila mtime berbeda. Node selalu di-resolve ulang via dc->nodes[idx]
 * (jangan simpan pointer): depNodeGet/parse bisa merealloc array node.
 */
static bool subtreeUpToDate(DepCache *dc, int idx) {
  depParseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  bool flat = dc->nodes[idx].dloaded;

  if (!nodeUpToDateShallow(dc, idx)) return false;

  /* salin kids dulu: depParseNodeIfNeeded anak bisa merealloc dc->nodes */
  int nkids = dc->nodes[idx].nkids;
  int *kids = malloc((size_t)(nkids > 0 ? nkids : 1) * sizeof(int));
  if (!kids) return false;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nkids * sizeof(int));

  bool ok = true;
  for (int i = 0; i < nkids; i++) {
    int kid = kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    dc->nodes[kid].mark = dc->gen;
    if (flat ? !nodeUpToDateShallow(dc, kid) : !subtreeUpToDate(dc, kid)) {
      ok = false;
      break;
    }
  }
  free(kids);
  return ok;
}

/* DFS: rekam hash konten subtree sebagai snapshot terkompilasi. Anak dari
   node flat direkam shallow (hash dari snapshot bila mtime sama — build
   no-op tidak membaca file); node non-flat direkursi seperti biasa. */
static void subtreeRecord(DepCache *dc, int idx) {
  depParseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  bool flat = dc->nodes[idx].dloaded;

  if (dc->nodes[idx].mtime < 0) return;
  snapshotSet(dc, dc->nodes[idx].path, dc->nodes[idx].mtime, nodeRecordHash(dc, idx));

  int nkids = dc->nodes[idx].nkids;
  int *kids = malloc((size_t)(nkids > 0 ? nkids : 1) * sizeof(int));
  if (!kids) return;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nkids * sizeof(int));

  for (int i = 0; i < nkids; i++) {
    int kid = kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    dc->nodes[kid].mark = dc->gen;
    if (flat) {
      if (dc->nodes[kid].mtime < 0) continue;
      snapshotSet(dc, dc->nodes[kid].path, dc->nodes[kid].mtime, nodeRecordHash(dc, kid));
    } else {
      subtreeRecord(dc, kid);
    }
  }
  free(kids);
}

bool depsContentUpToDate(DepCache *dc, const char *src) {
  if (!dc || !src) return false;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);

  int root = depNodeGet(dc, path);
  if (root < 0) return false;

  dc->gen++;
  return subtreeUpToDate(dc, root);
}

/*
 * true bila snapshot hash belum menutupi seluruh state saat ini (deps.cache
 * tidak ada, ada file yang belum pernah direkam, atau cache edges meleset).
 * Bila false DAN tidak ada yang dikompilasi, pass rekam setelah build boleh
 * dilewati — tidak ada informasi baru yang bisa direkam (no-op murni).
 */
bool depsSnapshotIncomplete(const DepCache *dc) {
  return !dc || dc->snapIncomplete;
}

void depsRecordUpdate(DepCache *dc, const char *src) {
  if (!dc || !src) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);

  int root = depNodeGet(dc, path);
  if (root < 0) return;

  dc->gen++;
  subtreeRecord(dc, root);
}

void depsSave(DepCache *dc) {
  if (!dc || (!dc->snapDirty && !dc->edgeDirty)) return;

  mkdirs(".rbot");
  char tmp[MAX_PATH + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", DEPS_SNAPSHOT_FILE);
  FILE *fp = fopen(tmp, "w");
  if (!fp) return;

  for (int i = 0; i < dc->snapCount; i++)
    fprintf(fp, "%s %016llx %016llx\n", dc->snap[i].path, (unsigned long long)dc->snap[i].mtime,
            (unsigned long long)dc->snap[i].hash);
  depEdgesSave(fp, dc);

  bool ok = fclose(fp) == 0;
  if (ok) {
    fsRemoveFile(DEPS_SNAPSHOT_FILE);
    ok = rename(tmp, DEPS_SNAPSHOT_FILE) == 0;
  }
  if (!ok) fsRemoveFile(tmp);
  else {
    dc->snapDirty = false;
    dc->edgeDirty = false;
  }
}

int64_t depsNewestHeaderMTime(DepCache *dc, const char *src) {
  return depsNewestHeaderMTimeAt(dc, src, -2);
}

int64_t depsNewestHeaderMTimeAt(DepCache *dc, const char *src, int64_t srcMTimeNs) {
  if (!dc || !src) return 0;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  depNormalizePath(path);
  int root = depNodeFind(dc, path);
  if (root < 0) root = srcMTimeNs >= -1 ? depNodeAddM(dc, path, srcMTimeNs) : depNodeAdd(dc, path);
  if (root < 0) return 0;

  dc->gen++;
  dc->nodes[root].mark = dc->gen;
  if (!dc->nodes[root].parsed) depParseNodeIfNeeded(dc, root);

  /* Edges dari .d: flat-lengkap — cukup mtime anak langsung (sudah di-stat
     saat node dibuat). Tanpa ini jalur ini memindai & membuka semua header
     transitif padahal listnya sudah jadi; mahal di proot. */
  if (dc->nodes[root].dloaded) {
    int64_t best = 0;
    for (int i = 0; i < dc->nodes[root].nkids; i++) {
      int64_t m = dc->nodes[dc->nodes[root].kids[i]].mtime;
      if (m < 0) return INT64_MAX; /* dependensi hilang -> stale (lihat depNewestUnder) */
      if (m > best) best = m;
    }
    return best;
  }

  int64_t best = 0;
  for (int i = 0; i < dc->nodes[root].nkids; i++) {
    int kid = dc->nodes[root].kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = depNewestUnder(dc, kid);
    if (m > best) best = m;
  }
  return best;
}
