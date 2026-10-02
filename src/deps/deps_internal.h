#ifndef RBOT_V0_1_0_DEPS_INTERNAL_H
#define RBOT_V0_1_0_DEPS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../deps.h" /* Config, List, opaque DepCache */

/*
 * deps_internal — tipe & helper bersama modul deps/.
 *
 * Dipakai antar-file deps_core.c (pemindai #include & node cache),
 * deps_dotd.c (dependensi dari file .d compiler + cache edges), dan
 * deps_snapshot.c (snapshot hash lintas-run + API publik). BUKAN API
 * publik — publik tetap deps.h. Helper dulunya static di deps.c dan
 * di-prefix dep* supaya unik lintas translation unit.
 */

#define DEPS_MAX_NODES 8192
#define DOTD_MAX_LINE 16384
#define DEPS_SNAPSHOT_FILE ".rbot/deps.cache"

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

/* ==================== deps_core.c ==================== */

/* FNV-1a 64 atas isi file (streaming 4KB). 0 bila file tak terbaca. */
uint64_t depComputeFileHash(const char *path);

/* Rapikan "a/./b/../c" -> "a/c" (in-place) agar satu file = satu node. */
void depNormalizePath(char *p);

/* dir + '/' + name lalu dinormalisasi. */
void depJoinPath(char *out, size_t n, const char *dir, const char *name);

/* FNV-1a 64 untuk path (hash table). */
uint64_t depPathHash(const char *s);

int depNodeFind(const DepCache *dc, const char *path);
int depNodeAddM(DepCache *dc, const char *path, int64_t mtime);
int depNodeAdd(DepCache *dc, const char *path);
int depNodeGet(DepCache *dc, const char *path);

/* Tambah index kid ke node tanpa duplikat. */
void depKidAdd(DepNode *n, int kid);

/* Baca seluruh file ke buffer malloc (NUL-terminated); pemanggil yang free. */
char *depReadWhole(const char *path, size_t *outLen);

/* Pastikan node sudah punya edges (dari .d bila layak, else pemindai). */
void depParseNodeIfNeeded(DepCache *dc, int idx);

/* DFS: mtime terbaru di subtree node; INT64_MAX bila ada dependensi hilang. */
int64_t depNewestUnder(DepCache *dc, int idx);

/* ==================== deps_dotd.c ==================== */

/* true bila path berakhiran .o / .obj (file object bukan dependensi source). */
bool depIsObjectPath(const char *p);

/* Bangun tabel hash edges dari seluruh entri yang ada. Dipanggil sekali
   setelah deps.cache selesai dimuat. */
void depEdgesHashBuild(DepCache *dc);

/* Tulis section cache edges (!e/!k) ke file snapshot yang sedang dibuka. */
void depEdgesSave(FILE *fp, const DepCache *dc);

/* ==================== deps_snapshot.c ==================== */

/* Muat .rbot/deps.cache sekali per run: snapshot hash + section edges. */
void depSnapshotLoad(DepCache *dc);

#endif /* RBOT_V0_1_0_DEPS_INTERNAL_H */
