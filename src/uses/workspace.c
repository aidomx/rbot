#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../commands.h"
#include "../config.h"
#include "../embed.h"
#include "../pack/pack.h"
#include "../pack/pack_internal.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "alias.h"

/*
 * workspace — implementasi mode `use workspace` (Buildfile.ws; nama lama
 * Buildfile.workspace masih diterima bila file baru tidak ada).
 *
 * Workspace adalah LAYER TIPIS di atas jalur build satu project yang sudah
 * ada: file workspace di-parse untuk daftar proyek + setting per proyek +
 * depends_on; tiap proyek disintesis jadi satu Buildfile (disimpan STABIL
 * di .rbot/workspace/<nama>.Buildfile agar cache config & fast path tidak
 * miss tiap run), lalu dieksekusi lewat cmdBuild/cmdClean dengan cwd = root
 * proyeknya. FastState, fingerprint, library, compdb — semuanya ikut
 * otomatis tanpa diubah.
 *
 * Rute key di Buildfile.ws:
 *   projects = a, b           -> daftar proyek (validasi eksplisit)
 *   projects.<nama>.*         -> setting per proyek (alias `projects.<n> as
 *                                <singkatan>` diperbolehkan; batas titik
 *                                resolver menjaga `rupa` tak menelan
 *                                `rupamod`)
 *   key lain (compiler, flags, std, dst) -> default bersama semua proyek;
 *                                barisnya ikut ditulis apa adanya ke
 *                                Buildfile tiap proyek.
 *
 * Root proyek default = <root workspace>/<nama>; override: <n>.root.
 * Path relatif terhadap lokasi file workspace (aturan rbot yang sudah
 * ada) — berarti relatif root workspace. Urutan build dari depends_on
 * (topological sort), bukan urutan `projects =`.
 */

#define WS_NAME_LEN 64
#define WS_LINE_LEN 1024

typedef struct {
  char name[WS_NAME_LEN];
  char root[MAX_PATH];
  List lines; /* baris setting per proyek TANPA prefix projects.<nama>. */
} WsProject;

/*
 * ReleaseConfig — konfigurasi release TUNGGAL workspace (design/
 * release.md revisi 2). Section release.* di Buildfile.ws; key-nya sama
 * dengan pack.* karena fase release menjalankan pack engine yang sama
 * pada root workspace. Isi paket bebas: file/folder proyek mana pun.
 */
typedef struct {
  bool present;              /* section release.* disebut di Buildfile.ws */
  List lines;                /* baris "release.<key> = <value>" apa adanya */
} WsReleaseCfg;

typedef struct {
  WsProject projects[WS_MAX_PROJECTS];
  int projectCount;
  WsReleaseCfg release;
  List commonLines; /* key selain projects/projects.<nama> — default bersama */
  List order;       /* urutan build hasil topo sort (nama) */
  List depends[WS_MAX_PROJECTS];
  /* Proyek lain yang dirujuk lewat library.<os> = <nama proyek> (mis.
     rupalib.linux = ruka). TIDAK masuk topo sort — siklus library antar
     proyek (rupa <-> ruka) sah; dua fase build yang menyelesaikannya.
     Dipakai untuk memperluas build selektif `-w <nama>`. */
  List libDeps[WS_MAX_PROJECTS];
  /* Stage fase release: klarifikasi run CLI di sini bila ada. */
  char releaseStage[MAX_PATH];
} WsModel;

/* ==================== util kecil ==================== */

static bool wsValidName(const char *s) {
  if (!s || !*s) return false;
  for (const char *p = s; *p; p++) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static int wsFindProject(const WsModel *m, const char *name) {
  for (int i = 0; i < m->projectCount; i++)
    if (strcmp(m->projects[i].name, name) == 0) return i;
  return -1;
}

static WsProject *wsEnsureProject(WsModel *m, const char *name);

/* Section engine yang dikenali Config — segmen pertama key dengan salah
   satu nama ini adalah default bersama, bukan nama proyek. Nama proyek
   tidak boleh memakai nama-nama ini. */
static bool wsIsEngineSection(const char *seg) {
  static const char *names[] = {"root",   "std",      "foreground", "target",
                                "clean",  "progress", "output",     "embedded",
                                "archive", "sources",  "flags",      "compiler",
                                "exclude", "headers",  "library",    "depends_on",
                                "release", NULL};
  for (int i = 0; names[i]; i++)
    if (strcmp(seg, names[i]) == 0) return true;
  return false;
}

static WsProject *wsEnsureProject(WsModel *m, const char *name) {
  int idx = wsFindProject(m, name);
  if (idx >= 0) return &m->projects[idx];
  if (m->projectCount >= WS_MAX_PROJECTS) return NULL;
  if (!wsValidName(name) || wsIsEngineSection(name)) return NULL;
  idx = m->projectCount++;
  memset(&m->projects[idx], 0, sizeof(WsProject));
  copyStr(m->projects[idx].name, sizeof(m->projects[idx].name), name);
  snprintf(m->projects[idx].root, sizeof(m->projects[idx].root), "%s", name);
  return &m->projects[idx];
}

static void wsAddCommon(WsModel *m, const char *key, const char *value) {
  char buf[WS_LINE_LEN];
  snprintf(buf, sizeof(buf), "%s = %s", key, value);
  listAdd(&m->commonLines, buf);
}

/* ==================== parse Buildfile.ws ==================== */

/* Satu baris setting. Tiga bentuk diterima (alias di-resolve lebih dulu):
     projects.<nama>.<setting> = v   (bentuk kanonik)
     <nama>.<setting> = v            (use workspace mewakili namespace —
                                      segmen pertama yang bukan section
                                      engine = nama proyek; sama persis
                                      dengan use project)
     <section> = v                   (default bersama semua proyek) */
static void wsRecordLine(WsModel *m, const char *key, const char *value) {
  char name[WS_NAME_LEN];
  const char *setting = NULL;

  if (strcmp(key, "projects") == 0) return; /* daftar proyek, bukan setting */

  /* releases = rupa, ruka — sintaks revisi 1 (daftar release terikat
     proyek) SUDAH DIHAPUS (design/release.md revisi 2). Pesan migrasi
     di sini supaya Buildfile.ws lama gagal JELAS, bukan diam-diam
     salah kemas. */
  if (strcmp(key, "releases") == 0 || strncmp(key, "releases.", 9) == 0) {
    fprintf(stderr,
            "rbot: workspace: sintaks 'releases' tidak lagi didukung\n"
            "        gunakan section release.* — SATU paket workspace\n"
            "        (release.name/files/exclude/format; lihat design/release.md)\n");
    return;
  }

  /* release.* — konfigurasi release tunggal (baris disimpan apa adanya,
     tanpa prefix, karena fase release memakai parser pack standar). */
  if (strcmp(key, "release") == 0) return; /* section marker saja */
  if (strncmp(key, "release.", 8) == 0) {
    m->release.present = true;
    char buf[WS_LINE_LEN];
    snprintf(buf, sizeof(buf), "%s = %s", key + 8, value);
    listAdd(&m->release.lines, buf);
    return;
  }

  if (strncmp(key, "projects.", 9) == 0) {
    const char *rest = key + 9;
    const char *dot = strchr(rest, '.');
    if (!dot || dot == rest) return;
    size_t nl = (size_t)(dot - rest);
    if (nl >= sizeof(name)) return;
    memcpy(name, rest, nl);
    name[nl] = '\0';
    setting = dot + 1;
  } else {
    const char *dot = strchr(key, '.');
    if (!dot) {
      /* root di level workspace DIBAULKAN (warning + abaikan): build
         selalu berjalan dengan cwd = folder proyek, jadi root berlaku
         per proyek — nilai workspace-level tidak mengagregasi output
         ke root workspace. Agregasi lewat path per proyek (../bin). */
      if (strcmp(key, "root") == 0) {
        fprintf(stderr,
                "rbot: workspace: 'root' diabaikan (root berlaku per proyek — "
                "cwd build = folder proyek; agregasi output ke root workspace "
                "pakai output.binaryDir = ../bin dsb.)\n");
        return;
      }
      if (wsIsEngineSection(key)) wsAddCommon(m, key, value);
      return;
    }
    size_t nl = (size_t)(dot - key);
    if (nl == 0 || nl >= sizeof(name)) return;
    memcpy(name, key, nl);
    name[nl] = '\0';
    if (wsIsEngineSection(name)) {
      wsAddCommon(m, key, value);
      return;
    }
    setting = dot + 1;
  }

  WsProject *p = wsEnsureProject(m, name);
  if (!p) return;
  /* <n>.root: dieksekusi oleh wsExecOne (chdir dari root workspace) dan
     TIDAK ditulis ke Buildfile sintesis — file sintesis selalu root = "."
     terhadap cwd proyek, jadi root di situ diterapkan dua kali. */
  if (strcmp(setting, "root") == 0) {
    copyStr(p->root, sizeof(p->root), value);
    return;
  }
  char buf[WS_LINE_LEN];
  snprintf(buf, sizeof(buf), "%s = %s", setting, value);
  listAdd(&p->lines, buf);
}

/* Parse file workspace di cwd. Alias di-resolve dengan mesin alias
   yang sama dengan use project (projects.rupamod as mod). */
static bool wsParse(WsModel *m, const char *path) {
  memset(m, 0, sizeof(*m));

  FILE *fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "rbot: cannot open %s\n", path);
    return false;
  }

  List aliases = {0};
  char line[WS_LINE_LEN];
  while (fgets(line, sizeof(line), fp)) {
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';
    char *text = trim(line);
    if (!*text) continue;

    /* Komentar // (gaya use alias) dibuang sebelum apa pun. */
    char *slash2 = strstr(text, "//");
    if (slash2) *slash2 = '\0';
    if (!*trim(text)) continue;

    char *eq = strchr(text, '=');
    if (!eq) {
      aliasLineAdd(&aliases, text); /* baris alias "X as Y" */
      continue;
    }
    *eq = '\0';
    char *k = trim(text);
    char *v = trim(eq + 1);
    if (!*k) continue;

    char key[ALIAS_KEY_MAX];
    if (strlen(k) >= sizeof(key)) continue;
    copyStr(key, sizeof(key), k);
    if (strcmp(key, "projects") == 0) {
      for (char *save = v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item && !wsEnsureProject(m, item)) {
          fprintf(stderr, "rbot: workspace: invalid project name '%s'\n", item);
          fclose(fp);
          listFree(&aliases);
          return false;
        }
        if (!comma) break;
        save = comma + 1;
      }
      continue;
    }
    aliasResolve(&aliases, key, sizeof(key));
    wsRecordLine(m, key, v);
  }
  fclose(fp);
  listFree(&aliases);
  return true;
}

/* ==================== pack.merge ==================== */

static bool wsProjectPackFiles(const WsProject *p, List *out) {
  bool found = false;
  for (int i = 0; i < p->lines.count; i++) {
    const char *ln = p->lines.items[i];
    if (strncmp(ln, "pack.files = ", 13) != 0) continue;
    const char *v = ln + 13;
    char buf[WS_LINE_LEN * 2];
    if (strlen(v) >= sizeof(buf)) return false;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item && !listAdd(out, item)) return false;
      if (!comma) break;
      save = comma + 1;
    }
    found = true;
  }
  return found;
}

static bool wsPackMergeInfo(const WsModel *m, const WsProject *owner,
                            char *out, size_t n) {
  out[0] = '\0';
  for (int i = 0; i < owner->lines.count; i++) {
    const char *ln = owner->lines.items[i];
    if (strncmp(ln, "pack.merge = ", 13) != 0) continue;
    const char *v = ln + 13;
    char buf[WS_LINE_LEN * 2];
    if (strlen(v) >= sizeof(buf)) return false;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        int dep = wsFindProject(m, item);
        if (dep < 0) {
          fprintf(stderr, "rbot: workspace: '%s' pack.merge unknown project '%s'\n",
                  owner->name, item);
          return false;
        }
        List files = {0};
        if (!wsProjectPackFiles(&m->projects[dep], &files)) {
          fprintf(stderr, "rbot: workspace: '%s' pack.merge project '%s' has no pack.files\n",
                  owner->name, item);
          listFree(&files);
          return false;
        }
        for (int j = 0; j < files.count; j++) {
          const char *entry = files.items[j];
          const char *colon = strchr(entry, ':');
          char src[WS_LINE_LEN], dst[WS_LINE_LEN], dstBuf[WS_LINE_LEN];
          if (colon) {
            size_t sl = (size_t)(colon - entry);
            if (sl == 0 || sl >= sizeof(src)) { listFree(&files); return false; }
            memcpy(src, entry, sl); src[sl] = '\0';
            copyStr(dstBuf, sizeof(dstBuf), colon + 1);
            copyStr(dst, sizeof(dst), trim(dstBuf));
          } else {
            copyStr(src, sizeof(src), entry);
            copyStr(dst, sizeof(dst), entry);
          }
          char mapped[WS_LINE_LEN * 2];
          int written = snprintf(mapped, sizeof(mapped), "../%s/%s:%s",
                                 m->projects[dep].root, src, dst);
          if (written < 0 || (size_t)written >= sizeof(mapped)) {
            listFree(&files);
            return false;
          }
          if (out[0] && strlen(out) + 2 >= n) { listFree(&files); return false; }
          if (out[0]) strcat(out, ", ");
          if (strlen(out) + strlen(mapped) + 1 >= n) { listFree(&files); return false; }
          strcat(out, mapped);
        }
        listFree(&files);
      }
      if (!comma) break;
      save = comma + 1;
    }
  }
  return true;
}

static bool wsValidatePackMerges(WsModel *m) {
  for (int i = 0; i < m->projectCount; i++) {
    WsProject *owner = &m->projects[i];
    for (int j = 0; j < owner->lines.count; j++) {
      const char *ln = owner->lines.items[j];
      if (strncmp(ln, "pack.merge = ", 13) != 0) continue;
      const char *v = ln + 13;
      char buf[WS_LINE_LEN * 2];
      if (strlen(v) >= sizeof(buf)) return false;
      copyStr(buf, sizeof(buf), v);
      for (char *save = buf;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item) {
          int dep = wsFindProject(m, item);
          if (dep < 0) {
            fprintf(stderr, "rbot: workspace: '%s' pack.merge unknown project '%s'\n",
                    owner->name, item);
            return false;
          }
          List files = {0};
          bool ok = wsProjectPackFiles(&m->projects[dep], &files);
          listFree(&files);
          if (!ok) {
            fprintf(stderr, "rbot: workspace: '%s' pack.merge project '%s' has no pack.files\n",
                    owner->name, item);
            return false;
          }
          /* merge is also a build dependency: the merged package's files
             must exist before the owner's pack phase runs. */
          bool exists = false;
          for (int k = 0; k < m->depends[i].count; k++)
            if (strcmp(m->depends[i].items[k], item) == 0) exists = true;
          if (!exists && !listAdd(&m->depends[i], item)) return false;
        }
        if (!comma) break;
        save = comma + 1;
      }
    }
  }
  return true;
}

/* ==================== validasi release ==================== */

/* release.* konfigurasi tunggal — validasi ringan: format (bila diset)
   harus tar/deb. Paket tanpa files -> fase release pakai default artefak
   build proyek (packNewestInput waktu itu juga dilaporkan). */
static bool wsValidateRelease(WsModel *m) {
  for (int i = 0; i < m->release.lines.count; i++) {
    const char *ln = m->release.lines.items[i];
    if (strncmp(ln, "format = ", 9) != 0) continue;
    const char *fmt = ln + 9;
    if (*fmt && strcmp(fmt, "tar") != 0 && strcmp(fmt, "deb") != 0 &&
        strcmp(fmt, "none") != 0) {
      fprintf(stderr, "rbot: workspace: release.format '%s' tidak dikenal (tar, deb)\n", fmt);
      return false;
    }
    if (strcmp(fmt, "none") == 0) {
      fprintf(stderr, "rbot: workspace: release.format 'none' tidak bermakna di workspace\n");
      return false;
    }
  }
  return true;
}

/* ==================== dependensi pack lintas proyek ==================== */

/* Pack build selektif (-w <proyek>): entri pack.files yang menunjuk
   artefak proyek lain (src berprefix ../<nama>/... ) bermakna "hasil
   build proyek lain". Tanpa derekaman ini, build selektif tidak
   membangun proyek pemilik artefak itu, padahal entri berpath menuntut
   file jadi. Hasil: depends[] proyek pemilik pack.files bertambah
   (dedup — entri yang sudah ada tidak ditulis ulang). */
static void wsCollectPackDeps(WsModel *m) {
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "pack.files = ", 13) != 0) continue;
      const char *v = ln + 13;
      char buf[WS_LINE_LEN * 2];
      if (strlen(v) >= sizeof(buf)) continue;
      copyStr(buf, sizeof(buf), v);
      for (char *save = buf;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        /* src:dst — src bagian kiri; hanya src berprefix ../ yang lintas
           proyek (mis. arsip hasil pack proyek lain). */
        char *colon = strchr(save, ':');
        if (colon) *colon = '\0';
        if (strncmp(save, "../", 3) == 0) {
          char *seg = save + 3;
          char *slash = strchr(seg, '/');
          if (slash && slash != seg) {
            *slash = '\0';
            int dep = wsFindProject(m, seg);
            if (dep >= 0 && dep != i) {
              bool exists = false;
              for (int k = 0; k < m->depends[i].count; k++)
                if (strcmp(m->depends[i].items[k], seg) == 0) exists = true;
              if (!exists) listAdd(&m->depends[i], seg);
            }
          }
        }
        if (!comma) break;
        save = comma + 1;
      }
    }
  }
}

/* ==================== topo sort depends_on ==================== */

/* DFS berwarna (0 putih, 1 abu, 2 hitam): siklus terdeteksi pasti, dan
   urutan post-order menjamin dependency dibangun duluan. */
static bool wsVisit(WsModel *m, int idx, unsigned char *color, List *order) {
  if (color[idx] == 2) return true;
  if (color[idx] == 1) {
    fprintf(stderr, "rbot: workspace: dependency cycle involving '%s'\n", m->projects[idx].name);
    return false;
  }
  color[idx] = 1;
  for (int i = 0; i < m->depends[idx].count; i++) {
    int dep = wsFindProject(m, m->depends[idx].items[i]);
    if (dep < 0) {
      fprintf(stderr, "rbot: workspace: '%s' depends on unknown project '%s'\n",
              m->projects[idx].name, m->depends[idx].items[i]);
      return false;
    }
    if (!wsVisit(m, dep, color, order)) return false;
  }
  color[idx] = 2;
  listAdd(order, m->projects[idx].name);
  return true;
}

static bool wsTopoSort(WsModel *m) {
  /* Kumpulkan depends_on dari baris per proyek yang tersimpan
     ("depends_on = ..." setelah prefix projects.<nama>. di-strip). */
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "depends_on ", 11) != 0) continue;
      const char *v = strchr(ln, '=');
      if (!v) continue;
      v = trim((char *)v + 1);
      for (char *save = (char *)v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item) listAdd(&m->depends[i], item);
        if (!comma) break;
        save = comma + 1;
      }
    }
  }

  /* Proyek lain yang dirujuk lewat pack.files berpath ../<nama>/...
     (hasil pack proyek lain) juga build dependency. wsCollectPackDeps
     harus berjalan SEBELUM topo sort, karena wsVisit membaca depends[]. */
  wsCollectPackDeps(m);

  /* Kumpulkan juga dependensi library lintas-proyek (library.<os> =
     <nama proyek lain>). Sengaja TIDAK masuk DFS topo: siklus library
     sah (dua fase build), hanya dipakai untuk memperluas `-w <nama>`. */
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "library.", 8) != 0) continue;
      const char *v = strchr(ln, '=');
      if (!v) continue;
      v = trim((char *)v + 1);
      for (char *save = (char *)v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item && strcmp(item, m->projects[i].name) != 0 && wsFindProject(m, item) >= 0)
          listAdd(&m->libDeps[i], item);
        if (!comma) break;
        save = comma + 1;
      }
    }
  }

  /* cdeps = <proyek lain> juga memperluas build selektif — semantik sama
     dengan library.<os>: siklus sah (dua fase build), tidak masuk topo. */
  for (int i = 0; i < m->projectCount; i++) {
    for (int j = 0; j < m->projects[i].lines.count; j++) {
      const char *ln = m->projects[i].lines.items[j];
      if (strncmp(ln, "cdeps = ", 8) != 0) continue;
      const char *v = ln + 8;
      char buf[WS_LINE_LEN];
      if (strlen(v) >= sizeof(buf)) continue;
      copyStr(buf, sizeof(buf), v);
      for (char *save = buf;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item && strcmp(item, m->projects[i].name) != 0 && wsFindProject(m, item) >= 0)
          listAdd(&m->libDeps[i], item);
        if (!comma) break;
        save = comma + 1;
      }
    }
  }

  unsigned char color[WS_MAX_PROJECTS] = {0};
  List order = {0};
  for (int i = 0; i < m->projectCount; i++)
    if (!wsVisit(m, i, color, &order)) {
      listFree(&order);
      return false;
    }
  m->order = order;
  return true;
}

/* ==================== sintesis Buildfile per proyek ==================== */

/* Ubah shorthand library workspace: `library.linux = ruka` menjadi path
   artifact project (`../ruka/lib/libruka.a`). Nilai project tidak boleh
   diterjemahkan menjadi `-lruka`, karena linker berada di root project
   consumer dan tidak otomatis mengetahui libDir project lain. */
static bool wsLibraryValue(const WsModel *m, const WsProject *owner,
                           const char *value, char *out, size_t n) {
  (void)owner;
  out[0] = '\0';
  const char *p = value;
  bool first = true;
  while (*p) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    while (len && (p[0] == ' ' || p[0] == '\t')) { p++; len--; }
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    char item[WS_LINE_LEN];
    if (len >= sizeof(item)) return false;
    memcpy(item, p, len); item[len] = '\0';
    const WsProject *dep = NULL;
    for (int i = 0; i < m->projectCount; i++) {
      if (strcmp(m->projects[i].name, item) == 0) { dep = &m->projects[i]; break; }
    }
    char mapped[WS_LINE_LEN];
    if (dep) {
      char libName[WS_NAME_LEN];
      char libDir[WS_LINE_LEN];
      snprintf(libName, sizeof(libName), "%s", dep->name);
      snprintf(libDir, sizeof(libDir), "lib");
      for (int j = 0; j < dep->lines.count; j++) {
        const char *ln = dep->lines.items[j];
        if (strncmp(ln, "output.libraryName = ", 22) == 0)
          snprintf(libName, sizeof(libName), "%s", ln + 22);
        else if (strncmp(ln, "output.libDir = ", 16) == 0)
          snprintf(libDir, sizeof(libDir), "%s", ln + 16);
      }
      /* Paths in synthesized Buildfile are relative to consumer root.
         Build the string only after checking its exact size so long workspace
         project/library paths cannot trigger snprintf truncation. */
      {
        const char *prefix = "../";
        const char *middle = "/";
        const char *libPrefix = "/lib";
        const char *suffix = ".a";
        size_t need = strlen(prefix) + strlen(dep->root) +
                      strlen(middle) + strlen(libDir) +
                      strlen(libPrefix) + strlen(libName) + strlen(suffix) + 1;
        if (need > sizeof(mapped)) return false;
        char *dst = mapped;
        const char *parts[] = { prefix, dep->root, middle, libDir,
                                libPrefix, libName, suffix };
        for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
          size_t partLen = strlen(parts[k]);
          memcpy(dst, parts[k], partLen);
          dst += partLen;
        }
        *dst = '\0';
      }
    } else {
      snprintf(mapped, sizeof(mapped), "%s", item);
    }
    if (!first) strncat(out, ", ", n - strlen(out) - 1);
    strncat(out, mapped, n - strlen(out) - 1);
    first = false;
    if (!comma) break;
    p = comma + 1;
  }
  return true;
}

/* ==================== cdeps ==================== */
/*
 * cdeps = <proyek lain>[, ...] — ketergantungan compile-time lintas proyek
 * dalam satu key (todos/7_10_2026.txt). Menggantikan penulisan manual:
 *     <n>.headers = ../<dep>/include      (folder header publik proyek)
 *     <n>.library.<os> = <dep>            (artifact library per-OS)
 * Dengan cdeps, sintesis workspace menerbitkan `library = ../<dep>/...
 * dan `headers = ../<dep>/<dir>` secara internal. Siklus antar proyek
 * sah — sama seperti library.<os>: dua fase build workspace yang
 * menyelesaikannya (fase library melewatkan artifact path proyek lain).
 */

/* Folder header publik proyek `dep`: semua nilai `headers = ...` proyek
   itu (diprefix ../<root>/), default "include" bila proyek tidak
   menulis headers sama sekali. */
static void wsProjectHeaderDirs(const WsProject *dep, List *out) {
  bool any = false;
  for (int j = 0; j < dep->lines.count; j++) {
    const char *ln = dep->lines.items[j];
    if (strncmp(ln, "headers = ", 10) != 0) continue;
    const char *v = ln + 10;
    char buf[WS_LINE_LEN];
    if (strlen(v) >= sizeof(buf)) continue;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        /* "../" + root + "/" + item — dibangun manual (memcpy) agar
           gcc -Wformat-truncation tidak menolak path panjang. */
        char joined[MAX_PATH];
        size_t need = 3 + strlen(dep->root) + 1 + strlen(item) + 1;
        if (need > sizeof(joined)) continue;
        char *dst = joined;
        memcpy(dst, "../", 3); dst += 3;
        size_t rl = strlen(dep->root);
        memcpy(dst, dep->root, rl); dst += rl;
        *dst++ = '/';
        memcpy(dst, item, strlen(item)); dst += strlen(item);
        *dst = '\0';
        listAdd(out, joined);
        any = true;
      }
      if (!comma) break;
      save = comma + 1;
    }
  }
  if (!any) {
    char joined[MAX_PATH];
    size_t need = 3 + strlen(dep->root) + strlen("/include") + 1;
    if (need <= sizeof(joined)) {
      char *dst = joined;
      memcpy(dst, "../", 3); dst += 3;
      size_t rl = strlen(dep->root);
      memcpy(dst, dep->root, rl); dst += rl;
      memcpy(dst, "/include", 8); dst += 8;
      *dst = '\0';
      listAdd(out, joined);
    }
  }
}

/* Sudah ada di list? (dedup persis string — cukup untuk path folder). */
static bool wsListHas(const List *l, const char *s) {
  for (int i = 0; i < l->count; i++)
    if (strcmp(l->items[i], s) == 0) return true;
  return false;
}

/* Ekspansi blok cdeps proyek `p` menjadi baris Buildfile sintesis:
   satu `library = ...` per proyek dep + satu `headers = ...` gabungan.
   Baris `cdeps = ...` asli TIDAK ikut ditulis (sudah dikonsumsi). */
static bool wsEmitCdeps(const WsModel *m, const WsProject *p, char *body, size_t cap) {
  List deps = {0}, hdirs = {0}, ownHeaders = {0};
  bool found = false;

  /* Folder header milik proyek sendiri — supaya emit tidak menduplikasi
     entri yang sudah eksplisit di Buildfile user. */
  for (int j = 0; j < p->lines.count; j++) {
    const char *ln = p->lines.items[j];
    if (strncmp(ln, "headers = ", 10) != 0) continue;
    const char *v = ln + 10;
    char buf[WS_LINE_LEN];
    if (strlen(v) >= sizeof(buf)) continue;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) listAdd(&ownHeaders, item);
      if (!comma) break;
      save = comma + 1;
    }
  }

  for (int j = 0; j < p->lines.count; j++) {
    const char *ln = p->lines.items[j];
    if (strncmp(ln, "cdeps = ", 8) != 0) continue;
    found = true;
    const char *v = ln + 8;
    char buf[WS_LINE_LEN];
    if (strlen(v) >= sizeof(buf)) continue;
    copyStr(buf, sizeof(buf), v);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        if (strcmp(item, p->name) == 0) {
          fprintf(stderr, "rbot: workspace: '%s' cdeps berisi dirinya sendiri\n", p->name);
        } else if (wsFindProject(m, item) < 0) {
          fprintf(stderr, "rbot: workspace: '%s' cdeps unknown project '%s'\n", p->name, item);
        } else if (!wsListHas(&deps, item)) {
          listAdd(&deps, item);
        }
      }
      if (!comma) break;
      save = comma + 1;
    }
  }
  if (!found) {
    listFree(&ownHeaders);
    return true;
  }

  for (int j = 0; j < deps.count; j++) {
    int di = wsFindProject(m, deps.items[j]);
    const WsProject *dep = &m->projects[di];
    char mapped[WS_LINE_LEN];
    if (!wsLibraryValue(m, p, dep->name, mapped, sizeof(mapped))) continue;
    char line[WS_LINE_LEN * 2];
    int written = snprintf(line, sizeof(line), "library = %s", mapped);
    if (written < 0 || (size_t)written >= sizeof(line)) continue;
    if (strlen(body) + strlen(line) + 2 > cap) {
      listFree(&deps); listFree(&hdirs); listFree(&ownHeaders);
      return false;
    }
    strcat(body, line);
    strcat(body, "\n");
    /* Header publik proyek dep — jangan duplikasi dengan own/terlanjur. */
    List depHeaders = {0};
    wsProjectHeaderDirs(dep, &depHeaders);
    for (int k = 0; k < depHeaders.count; k++) {
      const char *hd = depHeaders.items[k];
      if (wsListHas(&hdirs, hd) || wsListHas(&ownHeaders, hd)) continue;
      listAdd(&hdirs, hd);
    }
    listFree(&depHeaders);
  }

  if (hdirs.count > 0) {
    char line[WS_LINE_LEN * 4];
    size_t off = 0;
    off += (size_t)snprintf(line + off, sizeof(line) - off, "headers = ");
    for (int k = 0; k < hdirs.count; k++) {
      if (k > 0) off += (size_t)snprintf(line + off, sizeof(line) - off, ", ");
      off += (size_t)snprintf(line + off, sizeof(line) - off, "%s", hdirs.items[k]);
      if (off >= sizeof(line) - 2) break;
    }
    if (strlen(body) + off + 2 > cap) {
      listFree(&deps); listFree(&hdirs); listFree(&ownHeaders);
      return false;
    }
    strcat(body, line);
    strcat(body, "\n");
  }
  listFree(&deps);
  listFree(&hdirs);
  listFree(&ownHeaders);
  return true;
}

/* Tulis file hanya bila isinya berubah — mtime stabil agar cache config
   & fast path tidak miss sia-sia. */
static bool wsWriteIfChanged(const char *path, const char *body) {
  bool same = false;
  FILE *rf = fopen(path, "rb");
  if (rf) {
    char old[16384];
    size_t got = fread(old, 1, sizeof(old), rf);
    fclose(rf);
    same = (got == strlen(body)) && memcmp(old, body, got) == 0;
  }
  if (same) return true;
  FILE *wf = fopen(path, "w");
  if (!wf) {
    fprintf(stderr, "rbot: cannot write %s\n", path);
    return false;
  }
  fputs(body, wf);
  fclose(wf);
  return true;
}

static bool wsSynthesize(const WsModel *m, const char *outDir, const char *wsName) {
  mkdirs(outDir);
  for (int i = 0; i < m->projectCount; i++) {
    const WsProject *p = &m->projects[i];

    /* Bandingkan isi lama vs baru: hanya tulis ulang bila berubah agar
       mtime stabil (cache config & fast path tidak miss sia-sia). */
    size_t n = 4096;
    for (int j = 0; j < m->commonLines.count; j++) n += strlen(m->commonLines.items[j]) + 1;
    for (int j = 0; j < p->lines.count; j++) n += strlen(p->lines.items[j]) + 1;
    n += (size_t)WS_MAX_PROJECTS * WS_LINE_LEN * 2;
    char *body = malloc(n);
    if (!body) return false;
    body[0] = '\0';

    char hdr[192];
    snprintf(hdr, sizeof(hdr),
             "/* Generated by rbot from %s — project '%s'. Do not edit. */\n",
             wsName, p->name);
    strcat(body, hdr);
    strcat(body, "use project\n\noutput as o\nclean as c\n\n");
    /* root = ".": tiap proyek dieksekusi dengan cwd = root proyeknya
       (wsExecOne), jadi root tetap "kualifikasi" tanpa menonaktifkan
       fastState (yang menuntut root == "."). */
    strcat(body, "root = .\n\n");
    for (int j = 0; j < m->commonLines.count; j++) {
      strcat(body, m->commonLines.items[j]);
      strcat(body, "\n");
    }
    char mergedFiles[WS_MAX_PROJECTS * WS_LINE_LEN * 2];
    if (!wsPackMergeInfo(m, p, mergedFiles, sizeof(mergedFiles))) {
      free(body);
      return false;
    }
    for (int j = 0; j < p->lines.count; j++) {
      const char *ln = p->lines.items[j];
      if (strncmp(ln, "cdeps = ", 8) == 0) continue; /* dikonsumsi wsEmitCdeps */
      if (strncmp(ln, "pack.merge = ", 13) == 0) continue;
      if (strncmp(ln, "pack.files = ", 13) == 0 && mergedFiles[0]) {
        char mergedLine[WS_LINE_LEN * 2 + 32];
        int written = snprintf(mergedLine, sizeof(mergedLine), "%s, %s", ln, mergedFiles);
        if (written < 0 || (size_t)written >= sizeof(mergedLine)) {
          free(body);
          return false;
        }
        strcat(body, mergedLine);
        strcat(body, "\n");
        continue;
      }
      if (strncmp(ln, "library.", 8) == 0) {
        const char *eq = strchr(ln, '=');
        if (eq) {
          char key[WS_LINE_LEN], value[WS_LINE_LEN], mapped[WS_LINE_LEN * 2];
          size_t kl = (size_t)(eq - ln);
          if (kl < sizeof(key)) {
            memcpy(key, ln, kl); key[kl] = '\0';
            snprintf(value, sizeof(value), "%s", eq + 1);
            char *k = trim(key); char *v = trim(value);
            if (wsLibraryValue(m, p, v, mapped, sizeof(mapped))) {
              char rewritten[WS_LINE_LEN * 3];
              snprintf(rewritten, sizeof(rewritten), "%s = %s", k, mapped);
              strcat(body, rewritten); strcat(body, "\n");
              continue;
            }
          }
        }
      }
      strcat(body, ln);
      strcat(body, "\n");
    }

    /* cdeps: library artifact + folder header proyek lain — satu key
       (lihat wsEmitCdeps). Dipanggil SETELAH baris proyek supaya pengaturan
       eksplisit user tetap menang di parser (last-wins tidak relevan untuk
       list, tapi posisi baris yang konsisten lebih mudah dibaca). */
    if (!wsEmitCdeps(m, p, body, n)) {
      free(body);
      return false;
    }

    /* Buildfile utama (fase build: library & binary pass) — pack.* proyek
       tetap berjalan normal di kedua jalur cmdBuild (kemasan per-proyek ke
       project/dist). Release TIDAK disintesis di sini: fase release
       menjalankan pack engine langsung dengan konfigurasi release.*
       (lihat wsRelease). */
    /* outDir (2*MAX_PATH) + WS_NAME_LEN + "/"+".Buildfile" — tak pernah
       terpotong (nama proyek maks. WS_NAME_LEN-1). */
    char path[MAX_PATH * 2 + WS_NAME_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s.Buildfile", outDir, p->name);
    if (!wsWriteIfChanged(path, body)) {
      free(body);
      return false;
    }
    free(body);
  }
  return true;
}

/* ==================== eksekusi ==================== */

/* Tandai closure dependency untuk build selektif. Topological order sudah
   menjamin dependency muncul lebih dulu; yang perlu dilakukan di sini hanya
   memperluas target `-w <nama>` menjadi dependency + target. */
static bool wsMarkNeeded(const WsModel *m, int idx, unsigned char *needed,
                         unsigned char *visiting) {
  if (idx < 0 || idx >= m->projectCount) return false;
  if (needed[idx]) return true;
  if (visiting[idx]) {
    fprintf(stderr, "rbot: workspace: dependency cycle involving '%s'\n",
            m->projects[idx].name);
    return false;
  }
  visiting[idx] = 1;
  for (int i = 0; i < m->depends[idx].count; i++) {
    int dep = wsFindProject(m, m->depends[idx].items[i]);
    if (dep < 0) {
      fprintf(stderr, "rbot: workspace: '%s' depends on unknown project '%s'\n",
              m->projects[idx].name, m->depends[idx].items[i]);
      visiting[idx] = 0;
      return false;
    }
    if (!wsMarkNeeded(m, dep, needed, visiting)) {
      visiting[idx] = 0;
      return false;
    }
  }
  visiting[idx] = 0;
  needed[idx] = 1;
  return true;
}

static int wsExecOne(const WsModel *m, const char *name, const char *cmd, int jobs,
                     const char *wsDir, bool libOnly) {
  int idx = wsFindProject(m, name);
  if (idx < 0) return 1;
  if (!m->projects[idx].root[0]) return 1;

  /* Buildfile hasil sintesis: <wsDir>/.rbot/workspace/<nama>.Buildfile.
     Path di dalamnya dievaluasi terhadap CWD saat build — dan wsExecOne
     selalu masuk ke root proyek dulu — jadi path relatif = relatif root
     PROYEK, bukan root workspace. Referensi antar-proyek memakai ../
     (mis. ../rupamod/dist/x.tar.gz). */
  char bf[MAX_PATH * 2];
  snprintf(bf, sizeof(bf), "%s/%s/%s.Buildfile", wsDir, WORKSPACE_SYNTH_DIR, name);

  if (strcmp(cmd, "clean") == 0) {
    if (!fsSetCwd(m->projects[idx].root)) {
      fprintf(stderr, "rbot: cannot enter project root '%s'\n", m->projects[idx].root);
      return 1;
    }
    int rc = cmdClean(bf);
    fsSetCwd(wsDir);
    return rc;
  }

  /* build: cwd ke root proyek agar state (.rbot/, build/, bin/, lib/)
     muncul di root proyek masing-masing; cmdBuild chdir lagi ke root
     (nilai sama) dan root = "." membuat fastState tetap aktif. */
  if (!fsSetCwd(m->projects[idx].root)) {
    fprintf(stderr, "rbot: cannot enter project root '%s'\n", m->projects[idx].root);
    return 1;
  }
  /* Label fase: proyek kemasan (output.binary = false) fase 1-nya adalah
     ARSIP (buildEmbedded), bukan library — label harus mencerminkan itu. */
  const char *passLabel = "";
  if (libOnly) {
    bool isArchive = false;
    const WsProject *p = &m->projects[idx];
    for (int i = 0; i < p->lines.count; i++) {
      if (strcmp(p->lines.items[i], "output.binary = false") == 0) {
        isArchive = true;
        break;
      }
    }
    passLabel = isArchive ? "  (archive pass)" : "  (library pass)";
  }
  printf("\n> Project   : %s%s\n", name, passLabel);
  int rc = cmdBuildEx(jobs, bf, libOnly);
  fsSetCwd(wsDir);
  return rc;
}

/* Bebaskan seluruh isi model (dipakai di semua jalur keluar workspaceRun
   agar tidak ada List yang bocor saat parse/topo/needed gagal). */
static void wsModelFree(WsModel *m) {
  for (int i = 0; i < WS_MAX_PROJECTS; i++) {
    listFree(&m->depends[i]);
    listFree(&m->libDeps[i]);
  }
  listFree(&m->order);
  for (int i = 0; i < m->projectCount; i++) listFree(&m->projects[i].lines);
  listFree(&m->commonLines);
  listFree(&m->release.lines);
}

/* Perluas set proyek yang dibutuhkan dengan dependensi library (libDeps)
   DAN depends_on secara transitif (fixpoint, bukan topo — siklus library
   sah). Contoh: `-w rupa` menarik ruka (library rupa), lalu rupamod
   (depends_on ruka) agar arsip embed tersedia untuk fase library ruka. */
static void wsExpandLibDeps(const WsModel *m, unsigned char *needed) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < m->projectCount; i++) {
      if (!needed[i]) continue;
      for (int j = 0; j < m->libDeps[i].count; j++) {
        int dep = wsFindProject(m, m->libDeps[i].items[j]);
        if (dep >= 0 && !needed[dep]) {
          needed[dep] = 1;
          changed = true;
        }
      }
      for (int j = 0; j < m->depends[i].count; j++) {
        int dep = wsFindProject(m, m->depends[i].items[j]);
        if (dep >= 0 && !needed[dep]) {
          needed[dep] = 1;
          changed = true;
        }
      }
    }
  }
}

/*
 * wsRelease — fase release workspace (design/release.md revisi 2):
 * kemas SATU paket dari root workspace lewat pack engine yang sama,
 * dengan konfigurasi release.* (key identik pack.*).
 *
 * Sumber file:
 *   1. Baris "files = ..." di release.* (entri polos relatif root
 *      workspace; entri ../<n>/... di-strip ../ jadi <n>/...).
 *   2. Bila tanpa baris files: default = artefak build semua proyek —
 *      bin/<binaryName> tiap proyek binary + lib/<libraryName>.a/.so
 *      tiap proyek library. Proyek archive/tanpa output tidak menyumbang.
 *
 * Baris release lain (name/version/output/format/compress/checksum/
 * exclude/deb.*) diteruskan apa adanya ke Config — pack engine yang
 * mengartikan. prefix dikonversi ke pack.* agar parser lama mengenali.
 */
static int wsRelease(WsModel *m, int jobs, const char *wsDir) {
  (void)jobs;

  Config c = configDefaults();

  /* Baris release -> pack.* ke Config. Baris files butuh perawatan
     path: ../<n>/x di-strip ../ (root workspace = induk folder proyek). */
  for (int i = 0; i < m->release.lines.count; i++) {
    char buf[WS_LINE_LEN * 2];
    copyStr(buf, sizeof(buf), m->release.lines.items[i]);
    char *eq = strchr(buf, '=');
    if (!eq) continue;
    *eq = '\0';
    char *k = trim(buf);
    char *v = trim(eq + 1);
    if (!*k) continue;
    if (strcmp(k, "files") == 0) {
      for (char *save = v;;) {
        char *comma = strchr(save, ',');
        if (comma) *comma = '\0';
        char *item = trim(save);
        if (*item) {
          const char *path = item;
          if (strncmp(item, "../", 3) == 0) path = item + 3;
          if (!listAdd(&c.pack.files, path)) goto oom;
        }
        if (!comma) break;
        save = comma + 1;
      }
    } else {
      if (strcmp(k, "name") == 0) packApply(&c, "name", v);
      else if (strcmp(k, "version") == 0) packApply(&c, "version", v);
      else if (strcmp(k, "output") == 0) packApply(&c, "output", v);
      else if (strcmp(k, "compress") == 0) packApply(&c, "compress", v);
      else if (strcmp(k, "checksum") == 0) packApply(&c, "checksum", v);
      else if (strcmp(k, "format") == 0) packApply(&c, "format", v);
      else if (strcmp(k, "exclude") == 0) packApply(&c, "exclude", v);
      else if (strncmp(k, "deb.", 4) == 0) packApplyDeb(&c, k + 4, v);
      else
        fprintf(stderr, "rbot: release: key '%s' tidak dikenal (padanan pack.*)\n", k);
    }
  }

  /* Default files: artefak build semua proyek (bila tidak ada files). */
  if (c.pack.files.count == 0) {
    for (int i = 0; i < m->projectCount; i++) {
      const WsProject *pp = &m->projects[i];
      bool isArchive = false;
      for (int l = 0; l < pp->lines.count; l++) {
        const char *ln = pp->lines.items[l];
        if (strcmp(ln, "output.binary = false") == 0) { isArchive = true; break; }
        if (strncmp(ln, "output.libraryName = ", 21) == 0 &&
            !isArchive) {
          char libEntry[WS_LINE_LEN * 2 ];
          const char *libName = ln + 21;
          int w1 = snprintf(libEntry, sizeof(libEntry), "%s/lib/lib%s.a", pp->root, libName);
          if (w1 > 0 && (size_t)w1 < sizeof(libEntry) && !listAdd(&c.pack.files, libEntry))
            goto oom;
          char shEntry[WS_LINE_LEN * 4];
          int w2 = snprintf(shEntry, sizeof(shEntry), "%s/lib/lib%s.so", pp->root, libName);
          if (w2 > 0 && (size_t)w2 < sizeof(shEntry) && !listAdd(&c.pack.files, shEntry))
            goto oom;
        }
      }
      if (!isArchive) {
        /* nama binary: dari baris output.binaryName, bila ada */
        for (int l = 0; l < pp->lines.count; l++) {
          const char *ln = pp->lines.items[l];
          if (strncmp(ln, "output.binaryName = ", 20) == 0) {
            char binEntry[WS_LINE_LEN * 3];
            int w = snprintf(binEntry, sizeof(binEntry), "%s/bin/%s", pp->root, ln + 20);
            if (w > 0 && (size_t)w < sizeof(binEntry) && !listAdd(&c.pack.files, binEntry))
              goto oom;
          }
        }
      }
    }
  }

  c.pack.requested = true;
  if (!c.pack.name[0]) {
    /* produk utter: nama folder workspace */
    char cwd[MAX_PATH];
    if (fsGetCwd(cwd, sizeof(cwd))) {
      const char *base = strrchr(cwd, '/');
      base = base ? base + 1 : cwd;
      copyStr(c.pack.name, sizeof(c.pack.name), base);
    }
  }
  if (!c.pack.version[0]) copyStr(c.pack.version, sizeof(c.pack.version), "0.0.0");
  if (!c.pack.output[0])
    copyStr(c.pack.output, sizeof(c.pack.output), "dist/{name}-v{version}.tar.gz");
  if (!c.pack.debInstallPrefix[0])
    copyStr(c.pack.debInstallPrefix, sizeof(c.pack.debInstallPrefix), "/usr/local");

  printf("\n> Release    : %s (workspace%s%s)\n", c.pack.name,
         c.pack.format[0] ? ", format " : "", c.pack.format[0] ? c.pack.format : "");

  /* packRunAt berjalan di CWD sekarang — pastikan itu root workspace. */
  if (!fsSetCwd(wsDir)) {
    fprintf(stderr, "rbot: cannot enter workspace root\n");
    packFreeConfig(&c.pack);
    return 1;
  }
  bool ok = packRunAt(&c);
  packFreeConfig(&c.pack);
  if (!ok) fprintf(stderr, "rbot: workspace: release gagal\n");
  return ok ? 0 : 1;

oom:
  fprintf(stderr, "rbot: workspace: kehabisan memori (release)\n");
  packFreeConfig(&c.pack);
  return 1;
}

int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel) {
  char wsDir[MAX_PATH];
  if (!fsGetCwd(wsDir, sizeof(wsDir))) {
    fprintf(stderr, "rbot: cannot determine current directory\n");
    return 1;
  }
  const char *wsName = workspaceFileName();
  if (!wsName) {
    fprintf(stderr, "rbot: %s not found\n", WORKSPACE_FILENAME);
    return 1;
  }
  char wsFile[MAX_PATH * 2];
  snprintf(wsFile, sizeof(wsFile), "%s/%s", wsDir, wsName);

  WsModel m;
  if (!wsParse(&m, wsFile)) {
    wsModelFree(&m);
    return 1;
  }
  if (m.projectCount == 0) {
    fprintf(stderr, "rbot: workspace: no projects declared\n");
    wsModelFree(&m);
    return 1;
  }
  if (!wsValidatePackMerges(&m)) {
    wsModelFree(&m);
    return 1;
  }
  if (!wsTopoSort(&m)) {
    wsModelFree(&m);
    return 1;
  }

  /* Dua mode (design/release.md revisi 2):
     - releaseSel : `-- key=value[,...]` — OVERRIDE setting release untuk
                    run ini (name, version, format, target->format; dsb).
                    Konfigurasi dasar tetap dari section release.*.
     - only       : `rbot -w <proyek>` — build proyek + closure-nya;
                    release tetap workspace penuh bila release.* ada.
     - keduanya kosong : build + release seluruh workspace. */
  bool releaseOnly = releaseSel && *releaseSel;

  /* Override CLI (release -- name=binyan dst.): tangani target sebagai
     sinonim format (sintaks revisi 1) supaya transisi Buildfile lama
     yang memakai CLI tetap jalan. */
  if (releaseOnly) {
    for (const char *p = releaseSel; *p;) {
      char pair[WS_LINE_LEN];
      const char *comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      if (len == 0 || len >= sizeof(pair)) {
        fprintf(stderr, "rbot: workspace: override release tidak valid\n");
        wsModelFree(&m);
        return 1;
      }
      memcpy(pair, p, len);
      pair[len] = '\0';
      char *eq = strchr(pair, '=');
      if (!eq) {
        fprintf(stderr,
                "rbot: workspace: override '%s' tidak valid (pakai key=value, mis. name=rupa)\n",
                trim(pair));
        wsModelFree(&m);
        return 1;
      }
      *eq = '\0';
      char *k = trim(pair);
      char *v = trim(eq + 1);
      const char *key = strcmp(k, "target") == 0 ? "format" : k;
      if (strcmp(key, "format") == 0 && *v &&
          strcmp(v, "tar") != 0 && strcmp(v, "deb") != 0 && strcmp(v, "none") != 0) {
        fprintf(stderr, "rbot: workspace: format '%s' tidak dikenal (tar, deb)\n", v);
        wsModelFree(&m);
        return 1;
      }
      if (strcmp(k, "name") == 0 && !wsValidName(v)) {
        fprintf(stderr, "rbot: workspace: nama release '%s' tidak valid\n", v);
        wsModelFree(&m);
        return 1;
      }
      char ln[WS_LINE_LEN];
      snprintf(ln, sizeof(ln), "%s = %s", key, v);
      if (!listAdd(&m.release.lines, ln) || (m.release.present = true, false)) {
        wsModelFree(&m);
        return 1;
      }
      if (!comma) break;
      p = comma + 1;
    }
  }

  if (!wsValidateRelease(&m)) {
    wsModelFree(&m);
    return 1;
  }

  char synthDir[MAX_PATH * 2];
  snprintf(synthDir, sizeof(synthDir), "%s/%s", wsDir, WORKSPACE_SYNTH_DIR);
  if (!wsSynthesize(&m, synthDir, wsName)) {
    wsModelFree(&m);
    return 1;
  }
  profMark("workspace-parse+synth");

  unsigned char needed[WS_MAX_PROJECTS] = {0};
  if (releaseOnly || (only && *only)) {
    if (only && *only) {
      int target = wsFindProject(&m, only);
      if (target < 0) {
        fprintf(stderr, "rbot: workspace: unknown project '%s'\n", only);
        wsModelFree(&m);
        return 1;
      }
      unsigned char visiting[WS_MAX_PROJECTS] = {0};
      if (!wsMarkNeeded(&m, target, needed, visiting)) {
        wsModelFree(&m);
        return 1;
      }
    } else {
      for (int i = 0; i < m.projectCount; i++) needed[i] = 1;
    }
    /* Build selektif juga menarik proyek yang dirujuk lewat library
       (library.<os> = <nama proyek>) — transitif; siklus library sah dan
       tidak membuat loop ini macet (fixpoint sederhana). */
    wsExpandLibDeps(&m, needed);
  } else {
    for (int i = 0; i < m.projectCount; i++) needed[i] = 1;
  }

  int rc = 0;
  profMark("ws-parse+synth");
  if (strcmp(cmd, "build") == 0) {
    /* FASE 1 — library pass: tiap proyek mengkompilasi source-nya dan
       mengemas lib<name>.a/.so TANPA link binary. Proyek yang saling
       memakai library lintas-proyek (rupa <-> ruka) baru bisa link
       setelah SEMUA library tersedia. Urutan tetap topo (depends_on):
       archive rupamod harus jadi sebelum fase library ruka meng-embed. */
    for (int i = 0; i < m.order.count && rc == 0; i++) {
      int idx = wsFindProject(&m, m.order.items[i]);
      if (idx < 0 || !needed[idx]) continue;
      profMark("ws-libpass-pre");
      rc = wsExecOne(&m, m.order.items[i], cmd, jobs, wsDir, true);
      profMark("ws-libpass-post");
    }
  }

  if (strcmp(cmd, "build") == 0) {
    /* FASE 2 — binary pass: link binary dengan seluruh library lintas-
       proyek yang kini sudah ada (command selain build tidak lewat sini). */
    for (int i = 0; rc == 0 && i < m.order.count; i++) {
      int idx = wsFindProject(&m, m.order.items[i]);
      if (idx < 0 || !needed[idx]) continue;
      /* Proyek kemasan (output.binary = false) tuntas di fase library
         (archive/pack pass) — fase binary tidak punya link binary.
         Lewati dengan status jelas, jangan jalankan ulang dua kali. */
      const WsProject *ap = &m.projects[idx];
      bool archiveOnly = false;
      for (int l = 0; l < ap->lines.count; l++)
        if (strcmp(ap->lines.items[l], "output.binary = false") == 0) archiveOnly = true;
      if (archiveOnly) {
        printf("\n> Project   : %s  (skip: selesai di fase library)\n", ap->name);
        continue;
      }
      profMark("ws-binpass-pre");
      rc = wsExecOne(&m, m.order.items[i], cmd, jobs, wsDir, false);
      profMark("ws-binpass-post");
      if (rc != 0) break; /* gagal: hentikan rantai */
    }
  } else {
    /* clean: satu pass cukup (binary pass), tanpa fase library. */
    for (int i = 0; rc == 0 && i < m.order.count; i++) {
      int idx = wsFindProject(&m, m.order.items[i]);
      if (idx < 0 || !needed[idx]) continue;
      rc = wsExecOne(&m, m.order.items[i], cmd, jobs, wsDir, false);
    }
  }

  /* FASE 3 — release pass: pack SATU paket workspace pada root workspace,
     output ke dist/ (design/release.md revisi 2). Bila tidak ada section
     release.* sama sekali (dan tidak ada override CLI), langkah ini
     dilewati — workspace tanpa release berperilaku seperti dulu. */
  if (rc == 0 && strcmp(cmd, "build") == 0 && m.release.present) {
    profMark("release-pre");
    rc = wsRelease(&m, jobs, wsDir);
    profMark("release-post");
  }

  if (profOn()) profReport(); /* delta antar fase workspace (build no-op:
                                 seluruh fase berada antara dua penanda) */
  wsModelFree(&m);
  return rc;
}

const char *workspaceFileName(void) {
  if (fsFileExists(WORKSPACE_FILENAME)) return WORKSPACE_FILENAME;
  if (fsFileExists(WORKSPACE_FILENAME_LEGACY)) return WORKSPACE_FILENAME_LEGACY;
  return NULL;
}

bool workspaceFileExists(void) { return workspaceFileName() != NULL; }

/*
 * workspaceEnumerate — parse & validasi model workspace untuk enumerasi
 * (dipakai `rbot profile workspace=`): semua validasi model yang sama
 * dengan workspaceRun, TANPA sintesis/build apa pun. nama proyek hasil
 * topo sort diisi ke projectNames[]. Buffer internal model statis —
 * pointer hasil valid sampai panggilan berikutnya.
 */
int workspaceEnumerate(const char *wsDir, int *count, const char **projectNames,
                       int maxProjects) {
  static WsModel mM;
  WsModel *m = &mM;
  *count = 0;

  const char *wsName = workspaceFileName();
  if (!wsName) {
    fprintf(stderr, "rbot: %s not found\n", WORKSPACE_FILENAME);
    return 1;
  }
  char wsFile[MAX_PATH * 2];
  snprintf(wsFile, sizeof(wsFile), "%s/%s", wsDir, wsName);
  if (!wsParse(m, wsFile)) {
    wsModelFree(m);
    return 1;
  }
  if (m->projectCount == 0) {
    fprintf(stderr, "rbot: workspace: no projects declared\n");
    wsModelFree(m);
    return 1;
  }
  if (!wsValidatePackMerges(m)) {
    wsModelFree(m);
    return 1;
  }
  if (!wsValidateRelease(m)) {
    wsModelFree(m);
    return 1;
  }
  if (!wsTopoSort(m)) {
    wsModelFree(m);
    return 1;
  }

  /* Salin nama ke buffer statis agar valid setelah wsModelFree. */
  static WsProject namesStorage[WS_MAX_PROJECTS];
  (void)namesStorage;
  static char nameBufs[WS_MAX_PROJECTS][WS_NAME_LEN];
  for (int i = 0; i < m->order.count && *count < maxProjects; i++) {
    /* Indeks dibaca SEKALI: `a[(*count)++] = b[*count]` tidak berurutan
       (UB) dan bisa menunjuk slot berikutnya yang belum terisi. */
    int idx = (*count)++;
    copyStr(nameBufs[idx], WS_NAME_LEN, m->order.items[i]);
    projectNames[idx] = nameBufs[idx];
  }
  wsModelFree(m);
  return 0;
}
