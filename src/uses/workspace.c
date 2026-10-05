#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../commands.h"
#include "../config.h"
#include "../embed.h"
#include "../pack/pack.h"
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
#define WS_MAX_PROJECTS 16
#define WS_MAX_RELEASES 8
#define WS_LINE_LEN 1024

typedef struct {
  char name[WS_NAME_LEN];
  char root[MAX_PATH];
  List lines; /* baris setting per proyek TANPA prefix projects.<nama>. */
} WsProject;

/*
 * Release — unit distribusi workspace (design/release.md):
 *   releases = rupa, ruka      (daftar; berisi NAMA PROYEK, bukan nama baru)
 *   releases.rupa as rrupa     (opsional; alias memungkinkan konfigurasi
 *                               release terpisah dari konfigurasi project)
 *   rrupa.name = rupa          (proyek sumber artefak; default = nama)
 *   rrupa.target = deb         (opsional; format kemasan release — override
 *                               pack.format proyek untuk artefak release)
 * Selektor CLI `-- key=value` (mis. `rbot -w release -- name=rupa`)
 * memilih satu release; build selektif menarik seluruh closure proyek,
 * termasuk proyek lain yang dirujuk pack.merge proyek sumber.
 */
typedef struct {
  char alias[WS_NAME_LEN]; /* key alias pada Buildfile.ws ("rrupa") */
  char name[WS_NAME_LEN];  /* proyek sumber artefak (default = alias) */
  char target[16];         /* "" = pack.format proyek; selain itu override */
} WsRelease;

typedef struct {
  WsProject projects[WS_MAX_PROJECTS];
  int projectCount;
  WsRelease releases[WS_MAX_RELEASES];
  int releaseCount;
  List commonLines; /* key selain projects/projects.<nama> — default bersama */
  List order;       /* urutan build hasil topo sort (nama) */
  List depends[WS_MAX_PROJECTS];
  /* Proyek lain yang dirujuk lewat library.<os> = <nama proyek> (mis.
     rupalib.linux = ruka). TIDAK masuk topo sort — siklus library antar
     proyek (rupa <-> ruka) sah; dua fase build yang menyelesaikannya.
     Dipakai untuk memperluas build selektif `-w <nama>`. */
  List libDeps[WS_MAX_PROJECTS];
  /* Folder output fase release; kosong = dist/release. Diisi
     dist/release/<nama> bila selektor release `-- name=<x>` aktif. */
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

static WsRelease *wsFindRelease(WsModel *m, const char *alias) {
  for (int i = 0; i < m->releaseCount; i++)
    if (strcmp(m->releases[i].alias, alias) == 0) return &m->releases[i];
  return NULL;
}

static WsRelease *wsEnsureRelease(WsModel *m, const char *alias) {
  WsRelease *r = wsFindRelease(m, alias);
  if (r) return r;
  if (m->releaseCount >= WS_MAX_RELEASES) return NULL;
  if (!wsValidName(alias)) return NULL;
  r = &m->releases[m->releaseCount++];
  memset(r, 0, sizeof(*r));
  copyStr(r->alias, sizeof(r->alias), alias);
  copyStr(r->name, sizeof(r->name), alias); /* default: proyek bernama sama */
  return r;
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
                                "releases", NULL};
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

  /* releases = rupa, ruka : daftar release (nama proyek; alias = nama). */
  if (strcmp(key, "releases") == 0) {
    char buf[WS_LINE_LEN];
    if (strlen(value) >= sizeof(buf)) return;
    copyStr(buf, sizeof(buf), value);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item && !wsEnsureRelease(m, item))
        fprintf(stderr, "rbot: workspace: invalid release '%s'\n", item);
      if (!comma) break;
      save = comma + 1;
    }
    return;
  }

  /* releases.<alias>.<setting> — konfigurasi release (name, target). */
  if (strncmp(key, "releases.", 9) == 0) {
    const char *rest = key + 9;
    const char *dot = strchr(rest, '.');
    if (!dot || dot == rest) return;
    size_t nl = (size_t)(dot - rest);
    if (nl >= sizeof(name)) return;
    memcpy(name, rest, nl);
    name[nl] = '\0';
    WsRelease *r = wsEnsureRelease(m, name);
    if (!r) return;
    if (strcmp(dot + 1, "name") == 0) {
      if (wsFindProject(m, value) < 0)
        fprintf(stderr, "rbot: workspace: release '%s' refers to unknown project '%s'\n",
                r->alias, value);
      else
        copyStr(r->name, sizeof(r->name), value);
    } else if (strcmp(dot + 1, "target") == 0) {
      copyStr(r->target, sizeof(r->target), value);
    }
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

/* ==================== validasi releases ==================== */

static bool wsValidateReleases(WsModel *m) {
  for (int i = 0; i < m->releaseCount; i++) {
    WsRelease *r = &m->releases[i];
    if (wsFindProject(m, r->name) < 0) {
      fprintf(stderr, "rbot: workspace: release '%s' refers to unknown project '%s'\n",
              r->alias, r->name);
      return false;
    }
    if (r->target[0] && strcmp(r->target, "tar") != 0 && strcmp(r->target, "deb") != 0) {
      fprintf(stderr, "rbot: workspace: release '%s' unknown target '%s' (tar, deb)\n",
              r->alias, r->target);
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

    /* Buildfile utama (fase build: library & binary pass) — TANPA blok
       release, agar pack default (pack.* proyek -> project/dist) tetap
       berjalan normal pada kedua jalur cmdBuild. */
    /* outDir (2*MAX_PATH) + WS_NAME_LEN + "/"+".Buildfile" — tak pernah
       terpotong (nama proyek maks. WS_NAME_LEN-1). */
    char path[MAX_PATH * 2 + WS_NAME_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s.Buildfile", outDir, p->name);
    if (!wsWriteIfChanged(path, body)) {
      free(body);
      return false;
    }

    /* Blok release — Buildfile TERPISAH untuk fase release (design/
       release.md): isi sama + blok release di akhir yang menimpa pack.*
       proyek (parser last-wins). Dipisah supaya pack default fase build
       tidak hilang dan fase 3 tidak dobel. packRunAt menjalankan file
       ini pada root WORKSPACE, jadi path output relatif root workspace:
       - target deb : output = <stage>/<name>-<version>.deb, format deb
       - target tar : output = <stage>/<name>-<version>.tar.gz
       <stage> = dist/release; selektor `-- name=rupa` mengisinya dengan
       dist/release/<nama> (lihat workspaceRun). */
    const WsRelease *rel = NULL;
    for (int ri = 0; ri < m->releaseCount && !rel; ri++)
      if (strcmp(m->releases[ri].name, p->name) == 0) rel = &m->releases[ri];
    if (rel) {
      const char *stage = m->releaseStage[0] ? m->releaseStage : "dist/release";
      char relblock[WS_LINE_LEN * 3];
      char outTpl[WS_LINE_LEN];
      if (rel->target[0] && strcmp(rel->target, "deb") == 0) {
        snprintf(outTpl, sizeof(outTpl), "%s/{name}-{version}.deb", stage);
        snprintf(relblock, sizeof(relblock),
                 "\n# release '%s' (target %s)\n"
                 "pack.name = %s\n"
                 "pack.output = %s\n"
                 "pack.format = deb\n",
                 rel->alias, rel->target, p->name, outTpl);
      } else {
        snprintf(outTpl, sizeof(outTpl), "%s/{name}-{version}.tar.gz", stage);
        snprintf(relblock, sizeof(relblock),
                 "\n# release '%s' (target tar)\n"
                 "pack.name = %s\n"
                 "pack.output = %s\n"
                 "pack.format = tar\n"
                 "pack.checksum = sha256\n",
                 rel->alias, p->name, outTpl);
      }
      strcat(body, relblock);
      char rpath[MAX_PATH * 2 + WS_NAME_LEN + 24];
      snprintf(rpath, sizeof(rpath), "%s/%s.release.Buildfile", outDir, p->name);
      if (!wsWriteIfChanged(rpath, body)) {
        free(body);
        return false;
      }
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
 * wsReleaseOne — fase release workspace untuk satu release (design/
 * release.md): kemas artefak proyek sumber lewat packRunAt pada root
 * WORKSPACE, dengan konfigurasi pack hasil timpaan blok release di
 * Buildfile sintesis (pack.output = <stage>/<name>-<version>.<ext>,
 * pack.format = target release).
 *
 * Entri pack.files di-rewrite RELATIF ROOT WORKSPACE: relatif root
 * proyek saja (tanpa ../) menjadi ../<root>/<p>, karena packRunAt
 * berjalan di root workspace. Entri sudah berprefix ../ (hasil merge
 * antar-proyek) tidak diubah. pack.output dari blok release sudah
 * relatif root workspace. Tidak ada fast state di root workspace —
 * freshness kemasan murni dari mtime input packRun.
 */
static int wsReleaseOne(WsModel *m, int relIdx, int jobs, const char *wsDir) {
  (void)jobs;
  WsRelease *r = &m->releases[relIdx];
  int idx = wsFindProject(m, r->name);
  if (idx < 0) {
    fprintf(stderr, "rbot: workspace: release '%s' refers to unknown project '%s'\n",
            r->alias, r->name);
    return 1;
  }
  const WsProject *p = &m->projects[idx];

  /* Proyek tanpa pack.files tidak punya input kemasan — error di sini,
     SEBELUM loadConfig: blok release di Buildfile sintesis menyetel
     pack.output sendiri, sehingga c.pack.requested selalu true dan tidak
     bisa dipakai mendeteksi proyek yang memang tidak bisa direlease. */
  bool hasPackFiles = false;
  for (int i = 0; i < p->lines.count; i++)
    if (strncmp(p->lines.items[i], "pack.files = ", 13) == 0) hasPackFiles = true;
  if (!hasPackFiles) {
    fprintf(stderr,
            "rbot: workspace: release '%s': project '%s' tidak memiliki pack.files "
            "— tidak ada yang bisa direlease\n",
            r->alias, p->name);
    return 1;
  }

  /* Buildfile sintesis fase release: isi sama dengan fase binary + blok
     release yang menimpa pack.* proyek. */
  char bf[MAX_PATH * 2];
  snprintf(bf, sizeof(bf), "%s/%s/%s.release.Buildfile", wsDir, WORKSPACE_SYNTH_DIR, p->name);

  Config c = configDefaults();
  if (!loadConfig(&c, bf)) return 1;

  /* Path pack milik proyek ini relatif ROOT PROYEK -> relatif ROOT
     WORKSPACE (root workspace = induk folder proyek):
     - entri polos (bin/rupa)        -> <root>/bin/rupa
     - entri merge (../ruka/dist/x)  -> ruka/dist/x (strip ../) */
  List relFiles = {0};
  for (int i = 0; i < c.pack.files.count; i++) {
    char entry[WS_LINE_LEN * 2];
    const char *src = c.pack.files.items[i];
    int written;
    if (strncmp(src, "../", 3) == 0) {
      written = snprintf(entry, sizeof(entry), "%s", src + 3);
    } else {
      const char *colon = strchr(src, ':');
      written = colon ? snprintf(entry, sizeof(entry), "%s/%.*s%s", p->root,
                                 (int)(colon - src), src, colon)
                      : snprintf(entry, sizeof(entry), "%s/%s", p->root, src);
    }
    if (written < 0 || (size_t)written >= sizeof(entry) || !listAdd(&relFiles, entry)) {
      listFree(&relFiles);
      return 1;
    }
  }
  listFree(&c.pack.files);
  c.pack.files = relFiles;

  /* packRunAt berjalan di CWD sekarang — pastikan itu root workspace. */
  if (!fsSetCwd(wsDir)) {
    fprintf(stderr, "rbot: cannot enter workspace root\n");
    listFree(&c.pack.files);
    return 1;
  }
  bool ok = packRunAt(&c);
  fsSetCwd("."); /* nilai wsDir; diri sendiri sebagai pemulih sederhana */
  listFree(&c.pack.files);
  if (!ok) {
    fprintf(stderr, "rbot: workspace: release '%s' gagal\n", r->alias);
    return 1;
  }
  return 0;
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

  /* Tiga mode:
     - releaseSel : `rbot -w release -- name=rupa` — release yang cocok
                    (AND antar pasangan `key=value` dipisah koma).
     - only       : `rbot -w <proyek>` — build proyek + closure-nya.
     - keduanya kosong : build + release seluruh workspace. */
  bool releaseOnly = releaseSel && *releaseSel;
  unsigned char relNeeded[WS_MAX_RELEASES] = {0};

  /* Selektor release via CLI (`rbot -w release -- key=value[,k=v]`):
     DIREKTIF, bukan filter — konfigurasi release boleh datang penuh dari
     CLI tanpa deklarasi `releases` di Buildfile.ws:
       name=<proyek>    pilih release proyek itu; bila proyek ada tapi
                        belum dideklarasikan, release IMPLISIT dibuat
                        (alias = name = proyek). Proyek tidak ada ->
                        error (satu-satunya error wajar).
       target=<tar|deb> override format kemasan run ini (menimpa target
                        deklarasi; tanpa target = tarball).
  */
  if (releaseOnly) {
    bool hasName = false;
    char targetOv[16] = {0};
    for (const char *p = releaseSel; *p;) {
      char pair[WS_LINE_LEN];
      const char *comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      if (len == 0 || len >= sizeof(pair)) {
        fprintf(stderr, "rbot: workspace: release selector tidak valid\n");
        wsModelFree(&m);
        return 1;
      }
      memcpy(pair, p, len);
      pair[len] = '\0';
      char *eq = strchr(pair, '=');
      if (!eq) {
        fprintf(stderr,
                "rbot: workspace: selector '%s' tidak valid (pakai key=value, mis. name=rupa)\n",
                trim(pair));
        wsModelFree(&m);
        return 1;
      }
      *eq = '\0';
      char *k = trim(pair);
      char *v = trim(eq + 1);
      if (strcmp(k, "name") == 0) {
        if (!wsValidName(v)) {
          fprintf(stderr, "rbot: workspace: nama proyek '%s' tidak valid\n", v);
          wsModelFree(&m);
          return 1;
        }
        if (wsFindProject(&m, v) < 0) {
          fprintf(stderr,
                  "rbot: workspace: project '%s' tidak ada — tidak ada yang bisa direlease\n", v);
          wsModelFree(&m);
          return 1;
        }
        int ri = -1;
        for (int i = 0; i < m.releaseCount; i++)
          if (strcmp(m.releases[i].name, v) == 0) { ri = i; break; }
        if (ri < 0) { /* belum dideklarasikan: release implisit dari CLI */
          WsRelease *nr = wsEnsureRelease(&m, v);
          if (!nr) {
            fprintf(stderr, "rbot: workspace: terlalu banyak release\n");
            wsModelFree(&m);
            return 1;
          }
          ri = (int)(nr - m.releases);
        }
        relNeeded[ri] = 1;
        hasName = true;
      } else if (strcmp(k, "target") == 0) {
        if (strcmp(v, "tar") != 0 && strcmp(v, "deb") != 0) {
          fprintf(stderr, "rbot: workspace: target '%s' tidak dikenal (tar, deb)\n", v);
          wsModelFree(&m);
          return 1;
        }
        snprintf(targetOv, sizeof(targetOv), "%s", v);
      } else {
        fprintf(stderr, "rbot: workspace: key selektor '%s' tidak dikenal (name, target)\n", k);
        wsModelFree(&m);
        return 1;
      }
      if (!comma) break;
      p = comma + 1;
    }
    if (!hasName) {
      fprintf(stderr, "rbot: workspace: release memerlukan name=<project>\n");
      wsModelFree(&m);
      return 1;
    }
    /* Override format semua release terpilih (menimpa target deklarasi). */
    if (targetOv[0]) {
      for (int i = 0; i < m.releaseCount; i++)
        if (relNeeded[i])
          copyStr(m.releases[i].target, sizeof(m.releases[i].target), targetOv);
    }

    /* Folder output fase release: default dist/release; selektor dengan
       pasangan `name=<x>` mengarahkan ke dist/release/<x> agar artefak
       selektif tidak tercampur artefak release workspace penuh. */
    for (const char *p = releaseSel; *p;) {
      char pair[WS_LINE_LEN];
      const char *comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      if (len < sizeof(pair)) {
        memcpy(pair, p, len);
        pair[len] = '\0';
        char *eq = strchr(pair, '=');
        if (eq) {
          *eq = '\0';
          char *v = trim(eq + 1);
          if (strcmp(trim(pair), "name") == 0 && wsValidName(v))
            snprintf(m.releaseStage, sizeof(m.releaseStage), "dist/release/%s", v);
        }
      }
      if (!comma) break;
      p = comma + 1;
    }
  }

  if (!wsValidateReleases(&m)) {
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
  if (releaseOnly) {
    /* Mode release: bangun proyek sumber tiap release terpilih +
       closure-nya (depends_on + library + pack.files berpath). */
    for (int i = 0; i < m.releaseCount; i++) {
      if (!relNeeded[i]) continue;
      int target = wsFindProject(&m, m.releases[i].name);
      if (target < 0) continue;
      unsigned char visiting[WS_MAX_PROJECTS] = {0};
      if (!wsMarkNeeded(&m, target, needed, visiting)) {
        wsModelFree(&m);
        return 1;
      }
    }
    wsExpandLibDeps(&m, needed);
  } else if (only && *only) {
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
    /* Build selektif juga menarik proyek yang dirujuk lewat library
       (library.<os> = <nama proyek>) — transitif; siklus library sah dan
       tidak membuat loop ini macet (fixpoint sederhana). */
    wsExpandLibDeps(&m, needed);
  } else {
    for (int i = 0; i < m.projectCount; i++) needed[i] = 1;
  }

  int rc = 0;
  if (strcmp(cmd, "build") == 0 && !releaseOnly) {
    /* FASE 1 — library pass: tiap proyek mengkompilasi source-nya dan
       mengemas lib<name>.a/.so TANPA link binary. Proyek yang saling
       memakai library lintas-proyek (rupa <-> ruka) baru bisa link
       setelah SEMUA library tersedia. Urutan tetap topo (depends_on):
       archive rupamod harus jadi sebelum fase library ruka meng-embed. */
    for (int i = 0; i < m.order.count && rc == 0; i++) {
      int idx = wsFindProject(&m, m.order.items[i]);
      if (idx < 0 || !needed[idx]) continue;
      rc = wsExecOne(&m, m.order.items[i], cmd, jobs, wsDir, true);
    }
  }

  if (strcmp(cmd, "build") == 0) {
    /* FASE 2 — binary pass: link binary dengan seluruh library lintas-
       proyek yang kini sudah ada (command selain build tidak lewat sini). */
    for (int i = 0; rc == 0 && i < m.order.count; i++) {
      int idx = wsFindProject(&m, m.order.items[i]);
      if (idx < 0 || !needed[idx]) continue;
      rc = wsExecOne(&m, m.order.items[i], cmd, jobs, wsDir, false);
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

  /* FASE 3 — release pass: pack tiap release pada root workspace, output
     ke dist/release (workspace penuh) atau dist/release/<name> (selektif).
     packRunAt memakai Buildfile sintesis yang sama — blok release di
     dalamnya menimpa pack.* proyek. */
  if (rc == 0 && strcmp(cmd, "build") == 0) {
    for (int i = 0; i < m.releaseCount && rc == 0; i++) {
      const WsRelease *r = &m.releases[i];
      if (releaseOnly) {
        if (!relNeeded[i]) continue;
      } else if (only && *only) {
        /* Build selektif: hanya release milik proyek yang diminta — proyek
           lain dalam closure hanyalah dependensi build, bukan target
           release (design/release.md §9). `-w rupa` me-release rupa saja,
           meski ruka ikut dibangun sebagai peer library. */
        if (strcmp(r->name, only) != 0) continue;
      } else {
        int owner = wsFindProject(&m, r->name);
        if (owner < 0 || !needed[owner]) continue;
      }
      printf("\n> Release    : %s (project %s%s%s)\n", r->alias, r->name,
             r->target[0] ? ", target " : "", r->target);
      rc = wsReleaseOne(&m, i, jobs, wsDir);
    }
  }

  wsModelFree(&m);
  return rc;
}

const char *workspaceFileName(void) {
  if (fsFileExists(WORKSPACE_FILENAME)) return WORKSPACE_FILENAME;
  if (fsFileExists(WORKSPACE_FILENAME_LEGACY)) return WORKSPACE_FILENAME_LEGACY;
  return NULL;
}

bool workspaceFileExists(void) { return workspaceFileName() != NULL; }
