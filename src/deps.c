#include "deps.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compile.h"
#include "portability.h"
#include "util.h"

#define DEPS_MAX_NODES 8192
#define DOTD_MAX_LINE 16384

typedef struct {
  char *path;    /* path ternormalisasi relatif root */
  int64_t mtime; /* mtime ns; -1 bila tidak terbaca */
  uint64_t hash;
  int *kids; /* index node hasil resolusi #include */
  int nkids;
  unsigned mark; /* generasi kunjungan terakhir (deteksi siklus) */
  bool parsed;
  bool dloaded; /* edges dari file .d compiler: flat & lengkap — header anak
                   tidak perlu dibuka/dipindai saat verifikasi */
} DepNode;

struct DepCache {
  DepNode *nodes;
  int count, cap;
  unsigned gen;
  const Config *cfg;
  List incDirs;

  /* Hash table path -> index node (open addressing, kapasitas pangkat dua).
     Menggantikan pencarian linear O(n) per resolusi #include (dulu O(n^2)
     untuk ratusan file). Isi bucket = index node + 1; 0 = kosong. */
  int *hbucket;
  int hbcap;

  /* Snapshot hash+mtime lintas-run. Mtime dipakai sebagai shortcut:
     file dengan mtime sama seperti saat direkam TIDAK perlu dibaca ulang
     untuk di-hash — hanya mtime (1x stat) yang dibandingkan. */
  struct {
    char *path;
    int64_t mtime;
    uint64_t hash;
  } *snap;
  int snapCount, snapCap;
  bool snapLoaded;
  bool snapDirty;

  /* Hash table path -> index snapshot (isi = idx+1, 0 = kosong), memakai
     pathHash yang sama. Snapshot lookup/set per file per run tanpa ini
     adalah linear scan O(n) — untuk ratusan source berarti ribuan strcmp
     sia-sia tiap run, mahal di lingkungan proot. */
  int *sbucket;
  int sbcap;

  /* Cache edges dari run sebelumnya (bagian "!e" deps.cache): bila mtime
     file .d masih sama, edges langsung dipakai tanpa membuka file .d —
     satu stat menggantikan open+read+fstat per source per run. */
  struct {
    char *src;
    int64_t dm;
    char **kids;
    int nkids;
    int expect; /* jumlah kids yang dijanjikan baris "!e" (-1 = format lama,
                   tak diketahui) — mendeteksi cache terpotong */
    bool used;  /* edge ini sudah dipasang ke node run ini */
  } *edges;
  int edgeCount, edgeCap;
  int *ebucket;
  int ebcap;
  bool edgeDirty;
  bool edgesLoaded;     /* section !e/!k sudah dicoba dimuat (sekali per run) */
  bool snapIncomplete;  /* ada file yang tak tertutup snapshot (deps.cache
                           hilang / file baru terlihat saat verifikasi) —
                           pass rekam wajib jalan setelah build sukses */
};

// hash
static uint64_t computeFileHash(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;

  uint64_t hash = 14695981039346656037ULL; // FNV offset basis
  unsigned char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    for (size_t i = 0; i < n; i++) {
      hash ^= buf[i];
      hash *= 1099511628211ULL; // FNV prime
    }
  }

  fclose(f);
  return hash;
}

/* ==================== path ==================== */

/* Rapikan "a/./b/../c" -> "a/c" (in-place) agar satu file = satu node. */
static void normalizePath(char *p) {
  char *seg[128];
  int ns = 0;
  char *save = p;
  size_t len = strlen(p);
  char *tmp = malloc(len + 1);
  memcpy(tmp, p, len + 1);
  bool abs = tmp[0] == '/';

  for (char *tok = tmp; tok && *tok;) {
    char *slash = strchr(tok, '/');
    if (slash) *slash = '\0';
    if (strcmp(tok, ".") == 0 || *tok == '\0') {
      /* lewati */
    } else if (strcmp(tok, "..") == 0 && ns > 0 && strcmp(seg[ns - 1], "..") != 0) {
      ns--;
    } else if (ns < 128) {
      seg[ns++] = tok;
    }
    tok = slash ? slash + 1 : NULL;
  }

  char *w = save;
  if (abs) *w++ = '/';
  for (int i = 0; i < ns; i++) {
    size_t sl = strlen(seg[i]);
    memcpy(w, seg[i], sl);
    w += sl;
    if (i + 1 < ns) *w++ = '/';
  }
  *w = '\0';
  if (w == save) strcpy(save, ".");
  free(tmp);
}

static void joinPath(char *out, size_t n, const char *dir, const char *name) {
  if (!dir || !*dir || strcmp(dir, ".") == 0)
    snprintf(out, n, "%s", name);
  else
    snprintf(out, n, "%s/%s", dir, name);
  normalizePath(out);
}

/* ==================== node cache ==================== */

/* FNV-1a 64 untuk path (hash table) */
static uint64_t pathHash(const char *s) {
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static void hashRebuild(DepCache *dc, int ncap) {
  int *nb = calloc((size_t)ncap, sizeof(int));
  if (!nb) return;
  free(dc->hbucket);
  dc->hbucket = nb;
  dc->hbcap = ncap;

  int mask = ncap - 1;
  for (int idx = 0; idx < dc->count; idx++) {
    int i = (int)(pathHash(dc->nodes[idx].path) & (uint64_t)mask);
    while (nb[i]) i = (i + 1) & mask;
    nb[i] = idx + 1;
  }
}

static int nodeFind(const DepCache *dc, const char *path) {
  if (!dc->hbcap) return -1;
  int mask = dc->hbcap - 1;
  int i = (int)(pathHash(path) & (uint64_t)mask);
  for (;;) {
    int v = dc->hbucket[i];
    if (!v) return -1;
    int idx = v - 1;
    if (strcmp(dc->nodes[idx].path, path) == 0) return idx;
    i = (i + 1) & mask;
  }
}

static int nodeAddM(DepCache *dc, const char *path, int64_t mtime) {
  if (dc->count >= DEPS_MAX_NODES) return -1;
  if (dc->count == dc->cap) {
    int ncap = dc->cap ? dc->cap * 2 : 64;
    DepNode *nn = realloc(dc->nodes, (size_t)ncap * sizeof(DepNode));
    if (!nn) return -1;
    dc->nodes = nn;
    dc->cap = ncap;
  }
  DepNode *n = &dc->nodes[dc->count];
  memset(n, 0, sizeof(*n));
  n->path = strdup(path);
  n->mtime = mtime;
  int idx = dc->count++;

  /* daftarkan ke hash table; tumbuh saat load factor > 0.75 */
  if (!dc->hbcap || dc->count * 4 > dc->hbcap * 3) hashRebuild(dc, dc->hbcap ? dc->hbcap * 2 : 256);
  int mask = dc->hbcap - 1;
  int i = (int)(pathHash(path) & (uint64_t)mask);
  while (dc->hbucket[i]) i = (i + 1) & mask;
  dc->hbucket[i] = idx + 1;

  return idx;
}

static int nodeAdd(DepCache *dc, const char *path) {
  return nodeAddM(dc, path, fsMTimeNs(path));
}

static int nodeGet(DepCache *dc, const char *path) {
  int i = nodeFind(dc, path);
  return i >= 0 ? i : nodeAdd(dc, path);
}

/* ==================== pemindai #include ==================== */

static char *readWhole(const char *path, size_t *outLen) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  long long sz = fsFileSize(path);
  if (sz < 0) sz = 0;
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[got] = '\0';
  *outLen = got;
  return buf;
}

/* Cari file yang di-include: "..." -> dir includer dulu, lalu -I; <...> -> -I
   saja. Return false bila bukan file project (header sistem). */
static bool resolveInclude(const DepCache *dc, const char *fromPath, const char *name, bool quoted,
                           char *out, size_t n) {
  if (quoted) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", fromPath);
    char *slash = strrchr(dir, '/');
    if (slash)
      *slash = '\0';
    else
      strcpy(dir, ".");
    joinPath(out, n, dir, name);
    if (fsFileExists(out)) return true;
  }
  for (int i = 0; i < dc->incDirs.count; i++) {
    joinPath(out, n, dc->incDirs.items[i], name);
    if (fsFileExists(out)) return true;
  }
  return false;
}

static void kidAdd(DepNode *n, int kid) {
  for (int i = 0; i < n->nkids; i++)
    if (n->kids[i] == kid) return;
  int *nk = realloc(n->kids, (size_t)(n->nkids + 1) * sizeof(int));
  if (!nk) return;
  n->kids = nk;
  n->kids[n->nkids++] = kid;
}

/* Baca satu file, catat semua #include yang teresolusi sebagai anak node.
   Komentar blok, komentar baris, dan isi string literal tidak dianggap
   direktif. */
static void parseNode(DepCache *dc, int idx) {
  DepNode *node = &dc->nodes[idx];
  node->parsed = true;
  if (node->mtime < 0) return;

  size_t len = 0;
  char *buf = readWhole(node->path, &len);
  if (!buf) return;
  char *fromPath = strdup(node->path); /* nodes bisa realloc saat nodeGet */

  bool inBlock = false;
  char *p = buf;
  while (*p) {
    char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);

    /* salin baris dengan komentar diganti spasi */
    char line[1024];
    size_t o = 0;
    for (size_t i = 0; i < ll && o < sizeof(line) - 1;) {
      if (inBlock) {
        if (p[i] == '*' && i + 1 < ll && p[i + 1] == '/') {
          inBlock = false;
          i += 2;
        } else {
          i++;
        }
        line[o++] = ' ';
      } else if (p[i] == '/' && i + 1 < ll && p[i + 1] == '*') {
        inBlock = true;
        i += 2;
        line[o++] = ' ';
      } else if (p[i] == '/' && i + 1 < ll && p[i + 1] == '/') {
        break;
      } else if (p[i] == '"') {
        /* string literal: salin apa adanya (bisa memuat pola komentar) */
        line[o++] = p[i++];
        while (i < ll && p[i] != '"' && o < sizeof(line) - 1) {
          if (p[i] == '\\' && i + 1 < ll) line[o++] = p[i++];
          line[o++] = p[i++];
        }
        if (i < ll && o < sizeof(line) - 1) line[o++] = p[i++];
      } else {
        line[o++] = p[i++];
      }
    }
    line[o] = '\0';

    const char *q = line;
    while (*q == ' ' || *q == '\t' || *q == '\r')
      q++;
    if (*q == '#') {
      q++;
      while (*q == ' ' || *q == '\t')
        q++;
      if (strncmp(q, "include", 7) == 0) {
        q += 7;
        if (strncmp(q, "_next", 5) == 0) q += 5;
        /* "#include_foo" bukan direktif include */
        if (*q != ' ' && *q != '\t' && *q != '"' && *q != '<') q = "";
        while (*q == ' ' || *q == '\t')
          q++;
        if (*q == '"' || *q == '<') {
          bool quoted = *q == '"';
          char close = quoted ? '"' : '>';
          const char *s = ++q;
          while (*q && *q != close)
            q++;
          if (*q == close && q > s) {
            char name[MAX_PATH];
            size_t nl = (size_t)(q - s);
            if (nl < sizeof(name)) {
              memcpy(name, s, nl);
              name[nl] = '\0';
              /* dir + '/' + name bisa sampai ~2*MAX_PATH — buffer diperbesar
                 agar joinPath tidak pernah memotong (-Wformat-truncation). */
              char resolved[MAX_PATH * 2];
              if (resolveInclude(dc, fromPath, name, quoted, resolved, sizeof(resolved))) {
                int kid = nodeGet(dc, resolved);
                if (kid >= 0) kidAdd(&dc->nodes[idx], kid);
              }
            }
          }
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
  free(fromPath);
  free(buf);
}

/* Node source (bukan header): satu-satunya yang punya file .d dari -MMD.
   MSVC tidak menulis .d — langsung pemindai #include. */
static bool dotdUsableSource(const DepCache *dc, const char *path) {
  if (!dc || !dc->cfg || compilerIsMSVC(dc->cfg)) return false;
  static const char *srcExt[] = {".c", ".C", ".cc", ".cpp", ".cxx", ".m", ".mm"};
  const char *dot = strrchr(path, '.');
  if (!dot) return false;
  for (size_t i = 0; i < sizeof(srcExt) / sizeof(srcExt[0]); i++)
    if (strcmp(dot, srcExt[i]) == 0) return true;
  return false;
}

static void parseNodeIfNeeded(DepCache *dc, int idx) {
  DepNode *node = &dc->nodes[idx];

  /* mtime sudah diambil tepat sekali saat node dibuat (nodeAdd) — file
     tidak bisa berubah di tengah satu run rbot, jadi stat ulang di sini
     (dan di depsLoadDotD) hanya pemborosan syscall, mahal di proot. */
  if (node->parsed) return;
  node->parsed = true;

  if (node->mtime < 0) return; /* file hilang */

  /* Prioritas: edges dari file .d compiler (lebih akurat dari pemindai —
     menangani #if, makro, computed include). Fallback: pemindai #include
     untuk build pertama, MSVC, atau .d yang belum ada/basi. */
  if (dotdUsableSource(dc, node->path) && depsLoadDotD(dc, node->path)) return;

  parseNode(dc, idx);
}

/* DFS iteratif-rekursif: mtime terbaru di subtree, tiap node sekali per
   source. Kids disalin dulu agar aman dari realloc dc->nodes. Dependensi
   HILANG (mtime -1) dikembalikan sebagai INT64_MAX agar source dipaksa
   stale: compiler menampilkan error include yang jelas, bukan build
   "sukses" di atas state yang tidak konsisten (semantik make -MP). */
static int64_t newestUnder(DepCache *dc, int idx) {
  parseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  int64_t best = dc->nodes[idx].mtime;
  if (best < 0) return INT64_MAX; /* file hilang */

  int nkids = dc->nodes[idx].nkids;
  int *kids = malloc((size_t)(nkids > 0 ? nkids : 1) * sizeof(int));
  if (!kids) return best;
  memcpy(kids, dc->nodes[idx].kids, (size_t)nkids * sizeof(int));

  for (int i = 0; i < nkids; i++) {
    int kid = kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = newestUnder(dc, kid);
    if (m > best) best = m;
  }
  free(kids);
  return best;
}

/* ==================== dependensi dari compiler (.d, -MMD) ====================

 * Bila object sudah pernah dikompilasi dengan -MMD (GCC/Clang), compiler
 * menulis daftar header yang benar-benar dipakai — menangani #if, makro,
 * computed include, hal yang dilampaui pemindai #include. File .d segar
 * (mtime >= source) dipakai sebagai sumber edges node source: flat tapi
 * lengkap, karena compiler yang meresolusi rekursi itu sendiri.
 */

/* Sisipkan satu path dependensi (sudah di-copy ke buffer) sebagai anak node
   idx — tanpa duplikat. nodeGet bisa merealloc dc->nodes: akses selalu via
   idx, pointer diambil ulang SETELAH nodeGet. */
static void dotdAddDep(DepCache *dc, int idx, const char *depPath) {
  if (idx < 0 || !depPath || !*depPath) return;
  char norm[MAX_PATH];
  snprintf(norm, sizeof(norm), "%s", depPath);
  normalizePath(norm);

  int kid = nodeGet(dc, norm);
  if (kid < 0 || kid == idx) return;

  DepNode *n = &dc->nodes[idx];
  for (int i = 0; i < n->nkids; i++)
    if (n->kids[i] == kid) return;
  int *nk = realloc(n->kids, (size_t)(n->nkids + 1) * sizeof(int));
  if (!nk) return;
  n->kids = nk;
  n->kids[n->nkids++] = kid;
}

/* Ujung baris logis: backslash continuation digabung, newline lain tetap.
   Return menunjuk '\n' atau '\0'; backslash ada di e[-1] bila continuation. */
static const char *dotdLineEnd(const char *s) {
  const char *e = strchr(s, '\n');
  if (!e) return s + strlen(s);
  const char *q = e;
  while (q > s && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r'))
    q--;
  if (q > s && q[-1] == '\\') return q - 1;
  return e;
}

/*
 * Satu baris logis .d (fragment, backslash sudah dibuang):
 *   - baris pertama entry  : "target: dep dep ..."
 *   - baris continuation   : "dep dep ..."
 *   - phony -MP            : "path:" (entry baru, tanpa dependen)
 * Target .o (berakhiran .o/.obj) di-skip; sisanya dianggap dependensi.
 */
static void dotdProcessLine(DepCache *dc, int idx, char *line, bool *isTarget) {
  char *q = line;
  while (*q == ' ' || *q == '\t' || *q == '\r')
    q++;
  if (!*q || *q == '#') return; /* kosong / komentar */

  char *colon = strchr(q, ':');
  if (!colon) {
    if (!*isTarget) dotdAddDep(dc, idx, q); /* continuation */
    return;
  }

  /* Entry baru: teks sebelum ':' adalah target, sisanya dependensi. */
  *isTarget = true;

  /* Windows path "C:/..." atau "C:\..." — colon bagian drive, bukan
     pemisah target: geser ke colon berikutnya. */
  if ((colon - q) == 1 &&
      ((q[0] >= 'A' && q[0] <= 'Z') || (q[0] >= 'a' && q[0] <= 'z')) &&
      (colon[1] == '\\' || colon[1] == '/')) {
    char *real = strchr(colon + 1, ':');
    if (real) colon = real;
  }

  *colon = '\0';
  char *deps = colon + 1;

  size_t tl = strlen(q);
  /* NB: ".o" = 2 karakter → tl - 2. Offset lama (tl - 3) tidak pernah cocok,
     sehingga object ikut tercatat sebagai dependensi source-nya sendiri
     (stat ekstra per source + isi .o ikut di-hash ke snapshot). */
  if (tl >= 3 && q[tl - 2] == '.' && (q[tl - 1] == 'o' || q[tl - 1] == 'O')) {
    /* target object GNU/Clang: dependensi di baris yang sama tetap diproses */
    goto deps_of_line;
  }
  if (tl >= 4 && q[tl - 4] == '.' && (q[tl - 3] == 'o' || q[tl - 3] == 'O') &&
      (q[tl - 2] == 'b' || q[tl - 2] == 'B') && (q[tl - 1] == 'j' || q[tl - 1] == 'J'))
    goto deps_of_line; /* target object MSVC */

  dotdAddDep(dc, idx, q); /* target non-object: header phony hasil -MP dsb. */

deps_of_line:;
  /* dependensi di baris yang sama, setelah ':' */
  char *tok = deps;
  while (*tok) {
    while (*tok == ' ' || *tok == '\t' || *tok == '\r')
      tok++;
    if (!*tok) break;
    char *end = tok;
    while (*end && *end != ' ' && *end != '\t' && *end != '\r')
      end++;
    bool last = *end == '\0';
    *end = '\0';
    dotdAddDep(dc, idx, tok);
    if (last) break;
    tok = end + 1;
  }
}

#define DEPS_SNAPSHOT_FILE ".rbot/deps.cache"

/* ==================== cache edges (.d -> daftar path) ====================

 * Run sebelumnya menyimpan edges ("!e <src> <mtime_d_hex>" + baris path)
 * dari file .d yang dipakai. Run berikutnya memvalidasi satu stat mtime .d:
 * sama -> edges dipasang langsung, file .d tidak dibuka. Berbeda/hilang ->
 * entry diabaikan, edges dibangun ulang dari .d (atau scanner) seperti
 * biasa, lalu direkam ulang saat build sukses.
 */

/* true bila path berakhiran .o / .obj (file object bukan dependensi source). */
static bool isObjectPath(const char *p) {
  size_t n = strlen(p);
  if (n >= 3 && p[n - 2] == '.' && (p[n - 1] == 'o' || p[n - 1] == 'O')) return true;
  return n >= 5 && p[n - 4] == '.' && (p[n - 3] == 'o' || p[n - 3] == 'O') &&
         (p[n - 2] == 'b' || p[n - 2] == 'B') && (p[n - 1] == 'j' || p[n - 1] == 'J');
}

static int edgeFind(const DepCache *dc, const char *src) {
  if (!dc->ebcap) return -1;
  int mask = dc->ebcap - 1;
  int i = (int)(pathHash(src) & (uint64_t)mask);
  for (;;) {
    int v = dc->ebucket[i];
    if (!v) return -1;
    int idx = v - 1;
    if (strcmp(dc->edges[idx].src, src) == 0) return idx;
    i = (i + 1) & mask;
  }
}

/* Bangun tabel hash edges dari seluruh entri yang ada (kapasitas pangkat
   dua, load <= 0.5). Dipanggil sekali setelah deps.cache selesai dimuat. */
static void edgesHashBuild(DepCache *dc) {
  int ncap = 256;
  while (ncap < dc->edgeCount * 2)
    ncap *= 2;
  int *nb = calloc((size_t)ncap, sizeof(int));
  if (!nb) return;
  free(dc->ebucket);
  dc->ebucket = nb;
  dc->ebcap = ncap;
  int mask = ncap - 1;
  for (int k = 0; k < dc->edgeCount; k++) {
    int j = (int)(pathHash(dc->edges[k].src) & (uint64_t)mask);
    while (nb[j]) j = (j + 1) & mask;
    nb[j] = k + 1;
  }
}

static bool snapshotLoad(DepCache *dc); /* forward: edgesLoad memicu ini */

/* Section !e/!k dimuat oleh snapshotLoad dalam pass yang sama (satu buka
   file per run). Fungsi ini hanya memastikan load sudah terjadi. */
static void edgesLoad(DepCache *dc) {
  if (dc->edgesLoaded) return;
  snapshotLoad(dc);
}

/* Catat edges source idx (dari .d yang barusan diparse) untuk run berikutnya. */
static void edgesRecord(DepCache *dc, int idx, int64_t dm) {
  const char *src = dc->nodes[idx].path;
  int ei = edgeFind(dc, src);
  if (ei < 0) {
    if (dc->edgeCount == dc->edgeCap) {
      int ncap = dc->edgeCap ? dc->edgeCap * 2 : 128;
      void *nn = realloc(dc->edges, (size_t)ncap * sizeof(*dc->edges));
      if (!nn) return;
      dc->edges = nn;
      dc->edgeCap = ncap;
      /* hash table edges dibangun ulang saat grow */
    }
    /* hash table grow saat load > 0.75 */
    if (!dc->ebcap || (dc->edgeCount + 1) * 4 > dc->ebcap * 3) {
      int ncap = dc->ebcap ? dc->ebcap * 2 : 256;
      int *nb = calloc((size_t)ncap, sizeof(int));
      if (nb) {
        free(dc->ebucket);
        dc->ebucket = nb;
        dc->ebcap = ncap;
        int mask = ncap - 1;
        for (int k = 0; k < dc->edgeCount; k++) {
          int j = (int)(pathHash(dc->edges[k].src) & (uint64_t)mask);
          while (nb[j]) j = (j + 1) & mask;
          nb[j] = k + 1;
        }
      }
    }
    ei = dc->edgeCount++;
    dc->edges[ei].src = strdup(src);
    dc->edges[ei].kids = NULL;
    dc->edges[ei].nkids = 0;
    dc->edges[ei].expect = -1;
    dc->edges[ei].dm = 0;
    if (dc->ebcap) {
      int mask = dc->ebcap - 1;
      int j = (int)(pathHash(src) & (uint64_t)mask);
      while (dc->ebucket[j]) j = (j + 1) & mask;
      dc->ebucket[j] = ei + 1;
    }
  }

  /* bandingkan isi: bila sama & mtime .d sama, tak ada yang berubah */
  bool changed = dc->edges[ei].dm != dm || dc->edges[ei].nkids != dc->nodes[idx].nkids;
  if (!changed) {
    for (int k = 0; k < dc->nodes[idx].nkids && !changed; k++) {
      const char *a = dc->edges[ei].kids[k];
      const char *b = dc->nodes[dc->nodes[idx].kids[k]].path;
      if (!a || strcmp(a, b) != 0) changed = true;
    }
  }
  if (!changed) return;

  for (int k = 0; k < dc->edges[ei].nkids; k++)
    free(dc->edges[ei].kids[k]);
  dc->edges[ei].nkids = 0;
  for (int k = 0; k < dc->nodes[idx].nkids; k++) {
    char **nk = realloc(dc->edges[ei].kids, (size_t)(k + 1) * sizeof(char *));
    if (!nk) return;
    dc->edges[ei].kids = nk;
    dc->edges[ei].kids[k] = strdup(dc->nodes[dc->nodes[idx].kids[k]].path);
    if (!dc->edges[ei].kids[k]) return;
    dc->edges[ei].nkids = k + 1;
  }
  dc->edges[ei].dm = dm;
  dc->edges[ei].expect = -1;
  dc->edges[ei].used = true;
  dc->edgeDirty = true;
}

/* Entry yang tak terpakai (source hilang dari build) tidak ikut disimpan —
   cache edges ter-pangkas otomatis tiap kali ditulis. */
static void edgesSave(FILE *fp, const DepCache *dc) {
  for (int i = 0; i < dc->edgeCount; i++) {
    /* Source tanpa header project (nkids == 0) sah disimpan: jumlah kids ikut
       ditulis agar cache terpotong tidak salah dibaca sebagai "tanpa header". */
    if (!dc->edges[i].used) continue;
    fprintf(fp, "!e %s %016llx %d\n", dc->edges[i].src, (unsigned long long)dc->edges[i].dm,
            dc->edges[i].nkids);
    for (int k = 0; k < dc->edges[i].nkids; k++)
      fprintf(fp, "!k %s\n", dc->edges[i].kids[k]);
  }
}
bool depsLoadDotD(DepCache *dc, const char *src) {
  if (!dc || !src) return false;

  const Config *c = dc->cfg;
  if (!c || c->sources.count == 0) return false;

  char dpath[MAX_PATH * 2];
  if (!dotDPathFor(c, src, dpath, sizeof(dpath))) return false;

  int64_t dm = fsMTimeNs(dpath);
  /* .d basi (source lebih baru — kompilasi berikutnya akan menulis ulang)
     atau .d hilang -> return false, pemanggil jatuh ke pemindai #include.
     mtime source tidak di-stat ulang: pemanggil (parseNodeIfNeeded) sudah
     punya node-nya — ambil via hash table, satu strcmp saja. */
  int sidx = nodeFind(dc, src);
  int64_t sm = sidx >= 0 ? dc->nodes[sidx].mtime : -1;
  if (dm < 0 || sm < 0 || dm < sm) return false;

  /* Salin path source dulu: nodeGet bisa merealloc dc->nodes. */
  char srcNorm[MAX_PATH];
  snprintf(srcNorm, sizeof(srcNorm), "%s", src);
  normalizePath(srcNorm);
  int idx = nodeGet(dc, srcNorm);
  if (idx < 0) return false;

  dc->nodes[idx].parsed = true;  /* edges .d dipakai; scanner tidak perlu */
  dc->nodes[idx].dloaded = true; /* flat-lengkap: anak tak perlu dipindai */
  dc->nodes[idx].hash = 0;       /* hash source dihitung ulang bila perlu */

  /* Cache edges run sebelumnya: mtime .d sama -> edges dipasang langsung,
     file .d TIDAK dibuka (satu stat menggantikan open+read per source). */
  edgesLoad(dc);
  int ei = edgeFind(dc, srcNorm);
  if (ei >= 0 && dc->edges[ei].dm == dm &&
      (dc->edges[ei].expect >= 0 ? dc->edges[ei].nkids == dc->edges[ei].expect
                                 : dc->edges[ei].nkids > 0)) {
    /* cache versi lama menyimpan object sebagai "dependensi" (bug offset .o):
       buang di tempat; cache ditulis ulang sekali, bersih. */
    int w = 0;
    for (int k = 0; k < dc->edges[ei].nkids; k++) {
      if (isObjectPath(dc->edges[ei].kids[k])) {
        free(dc->edges[ei].kids[k]);
        dc->edgeDirty = true;
        continue;
      }
      dc->edges[ei].kids[w++] = dc->edges[ei].kids[k];
    }
    dc->edges[ei].nkids = w;
    for (int k = 0; k < dc->edges[ei].nkids; k++) {
      int kid = nodeGet(dc, dc->edges[ei].kids[k]);
      if (kid < 0 || kid == idx) continue;
      kidAdd(&dc->nodes[idx], kid);
    }
    dc->edges[ei].used = true;
    return true;
  }
  dc->snapIncomplete = true; /* cache meleset — state .d berubah sejak
                                rekaman terakhir: paksa pass rekam */

  size_t len = 0;
  char *buf = readWhole(dpath, &len);
  if (!buf) return false;

  /* Gabungkan continuation backslash, proses tiap baris logis. */
  char line[DOTD_MAX_LINE];
  size_t lo = 0;
  const char *p = buf;
  bool isTarget = false;
  for (;;) {
    const char *e = dotdLineEnd(p);
    size_t seg = (size_t)(e - p);
    if (lo + seg > sizeof(line) - 1)
      seg = lo < sizeof(line) - 1 ? sizeof(line) - 1 - lo : 0;
    memcpy(line + lo, p, seg);
    lo += seg;

    if (*e == '\0') break; /* EOF: baris terakhir tanpa '\n' ditangani di bawah */
    if (*e == '\\') {
      p = e + 1; /* buang backslash; '\n' menyusul — baris logis berlanjut */
      continue;
    }

    line[lo] = '\0';
    dotdProcessLine(dc, idx, line, &isTarget);
    lo = 0;
    p = e + 1;
  }
  if (lo > 0) {
    line[lo] = '\0';
    dotdProcessLine(dc, idx, line, &isTarget);
  }

  free(buf);
  edgesRecord(dc, idx, dm);
  return true;
}

/* ==================== API ==================== */

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
    int i = (int)(pathHash(dc->snap[idx].path) & (uint64_t)mask);
    while (nb[i]) i = (i + 1) & mask;
    nb[i] = idx + 1;
  }
}

/* Daftarkan entry idx ke tabel; grow saat load factor > 0.75. */
static void snapHashInsert(DepCache *dc, int idx) {
  if (!dc->sbcap || (idx + 1) * 4 > dc->sbcap * 3)
    snapHashRebuild(dc, dc->sbcap ? dc->sbcap * 2 : 256);
  int mask = dc->sbcap - 1;
  int i = (int)(pathHash(dc->snap[idx].path) & (uint64_t)mask);
  while (dc->sbucket[i]) i = (i + 1) & mask;
  dc->sbucket[i] = idx + 1;
}

/* Index snapshot untuk path, atau -1. O(1) vs linear scan dulu. */
static int snapFind(const DepCache *dc, const char *path) {
  if (!dc->sbcap) return -1;
  int mask = dc->sbcap - 1;
  int i = (int)(pathHash(path) & (uint64_t)mask);
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
static bool snapshotLoad(DepCache *dc) {
  if (dc->snapLoaded) return true;
  dc->snapLoaded = true;

  FILE *fp = fopen(DEPS_SNAPSHOT_FILE, "r");
  if (!fp) {
    dc->snapIncomplete = true; /* belum ada snapshot — wajib direkam */
    return false;
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
    if (got >= 2 && isObjectPath(path)) {
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
  edgesHashBuild(dc);
  return true;
}

static uint64_t snapshotHashOf(DepCache *dc, const char *path, int64_t *mtimeOut) {
  snapshotLoad(dc);
  int idx = snapFind(dc, path);
  if (idx < 0) return 0; /* 0 = tidak ada di snapshot */
  if (mtimeOut) *mtimeOut = dc->snap[idx].mtime;
  return dc->snap[idx].hash;
}

static void snapshotSet(DepCache *dc, const char *path, int64_t mtime, uint64_t hash) {
  snapshotLoad(dc);
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
    if (dc->nodes[idx].hash == 0) dc->nodes[idx].hash = computeFileHash(dc->nodes[idx].path);
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
  if (dc->nodes[idx].hash == 0) dc->nodes[idx].hash = computeFileHash(dc->nodes[idx].path);
  return dc->nodes[idx].hash;
}

/*
 * DFS: bandingkan hash konten subtree dengan snapshot build sukses.
 * false = ada file yang isinya benar-benar berubah.
 *
 * Node ber-flag dloaded (edges dari .d, flat-lengkap) tidak direkursi:
 * anak-anaknya diverifikasi shallow — mtime + snapshot, isi dibaca hanya
 * bila mtime berbeda. Node selalu di-resolve ulang via dc->nodes[idx]
 * (jangan simpan pointer): nodeGet/parse bisa merealloc array node.
 */
static bool subtreeUpToDate(DepCache *dc, int idx) {
  parseNodeIfNeeded(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  bool flat = dc->nodes[idx].dloaded;

  if (!nodeUpToDateShallow(dc, idx)) return false;

  /* salin kids dulu: parseNodeIfNeeded anak bisa merealloc dc->nodes */
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
  parseNodeIfNeeded(dc, idx);
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
  normalizePath(path);

  int root = nodeGet(dc, path);
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
  normalizePath(path);

  int root = nodeGet(dc, path);
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
  edgesSave(fp, dc);

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
  normalizePath(path);
  int root = nodeFind(dc, path);
  if (root < 0) root = srcMTimeNs >= -1 ? nodeAddM(dc, path, srcMTimeNs) : nodeAdd(dc, path);
  if (root < 0) return 0;

  dc->gen++;
  dc->nodes[root].mark = dc->gen;
  if (!dc->nodes[root].parsed) parseNodeIfNeeded(dc, root);

  /* Edges dari .d: flat-lengkap — cukup mtime anak langsung (sudah di-stat
     saat node dibuat). Tanpa ini jalur ini memindai & membuka semua header
     transitif padahal listnya sudah jadi; mahal di proot. */
  if (dc->nodes[root].dloaded) {
    int64_t best = 0;
    for (int i = 0; i < dc->nodes[root].nkids; i++) {
      int64_t m = dc->nodes[dc->nodes[root].kids[i]].mtime;
      if (m < 0) return INT64_MAX; /* dependensi hilang -> stale (lihat newestUnder) */
      if (m > best) best = m;
    }
    return best;
  }

  int64_t best = 0;
  for (int i = 0; i < dc->nodes[root].nkids; i++) {
    int kid = dc->nodes[root].kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = newestUnder(dc, kid);
    if (m > best) best = m;
  }
  return best;
}
