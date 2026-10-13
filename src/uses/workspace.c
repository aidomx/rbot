#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "workspace.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../cmds/cmds_internal.h"
#include "../commands.h"
#include "../config.h"
#include "../embed.h"
#include "../pack/pack.h"
#include "../pack/pack_internal.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "alias.h"

#ifndef _WIN32
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

/* File state workspace di root — padanan build.state per-proyek untuk mode
   workspace, sehingga bootstrap bisa evaluasi no-op tanpa parse topologi
   (design/workspace.md). Definisi di bawah (setelah workspaceFileExists). */
#define WS_STATE_FILE ".rbot/workspace.state"

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
  bool present; /* section release.* disebut di Buildfile.ws */
  List lines;   /* baris "release.<key> = <value>" apa adanya */
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

/* Deklarasi maju: dipakai workspaceRun (gate per-closure) — definisinya ada
   di bagian workspace state jauh di bawah. */
static bool wsGateStartValid = false;
static bool wsStateValidFiltered(const WsModel *m, const unsigned char *needed);
static void wsStateSave(const WsModel *m, const char *wsDir);

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
  static const char *names[] = {"root",       "std",      "foreground", "target",  "clean",
                                "progress",   "output",   "embedded",   "archive", "sources",
                                "flags",      "compiler", "exclude",    "headers", "library",
                                "depends_on", "release",  NULL};
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
    fprintf(stderr, "rbot: workspace: sintaks 'releases' tidak lagi didukung\n"
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
        fprintf(stderr, "rbot: workspace: 'root' diabaikan (root berlaku per proyek — "
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

static bool wsPackMergeInfo(const WsModel *m, const WsProject *owner, char *out, size_t n) {
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
          fprintf(stderr, "rbot: workspace: '%s' pack.merge unknown project '%s'\n", owner->name,
                  item);
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
            if (sl == 0 || sl >= sizeof(src)) {
              listFree(&files);
              return false;
            }
            memcpy(src, entry, sl);
            src[sl] = '\0';
            copyStr(dstBuf, sizeof(dstBuf), colon + 1);
            copyStr(dst, sizeof(dst), trim(dstBuf));
          } else {
            copyStr(src, sizeof(src), entry);
            copyStr(dst, sizeof(dst), entry);
          }
          char mapped[WS_LINE_LEN * 2];
          int written =
              snprintf(mapped, sizeof(mapped), "../%s/%s:%s", m->projects[dep].root, src, dst);
          if (written < 0 || (size_t)written >= sizeof(mapped)) {
            listFree(&files);
            return false;
          }
          if (out[0] && strlen(out) + 2 >= n) {
            listFree(&files);
            return false;
          }
          if (out[0]) strcat(out, ", ");
          if (strlen(out) + strlen(mapped) + 1 >= n) {
            listFree(&files);
            return false;
          }
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
            fprintf(stderr, "rbot: workspace: '%s' pack.merge unknown project '%s'\n", owner->name,
                    item);
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
    if (*fmt && strcmp(fmt, "tar") != 0 && strcmp(fmt, "deb") != 0 && strcmp(fmt, "none") != 0) {
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
static bool wsLibraryValue(const WsModel *m, const WsProject *owner, const char *value, char *out,
                           size_t n) {
  (void)owner;
  out[0] = '\0';
  const char *p = value;
  bool first = true;
  while (*p) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    while (len && (p[0] == ' ' || p[0] == '\t')) {
      p++;
      len--;
    }
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
      len--;
    char item[WS_LINE_LEN];
    if (len >= sizeof(item)) return false;
    memcpy(item, p, len);
    item[len] = '\0';
    const WsProject *dep = NULL;
    for (int i = 0; i < m->projectCount; i++) {
      if (strcmp(m->projects[i].name, item) == 0) {
        dep = &m->projects[i];
        break;
      }
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
        size_t need = strlen(prefix) + strlen(dep->root) + strlen(middle) + strlen(libDir) +
                      strlen(libPrefix) + strlen(libName) + strlen(suffix) + 1;
        if (need > sizeof(mapped)) return false;
        char *dst = mapped;
        const char *parts[] = {prefix, dep->root, middle, libDir, libPrefix, libName, suffix};
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
        memcpy(dst, "../", 3);
        dst += 3;
        size_t rl = strlen(dep->root);
        memcpy(dst, dep->root, rl);
        dst += rl;
        *dst++ = '/';
        memcpy(dst, item, strlen(item));
        dst += strlen(item);
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
      memcpy(dst, "../", 3);
      dst += 3;
      size_t rl = strlen(dep->root);
      memcpy(dst, dep->root, rl);
      dst += rl;
      memcpy(dst, "/include", 8);
      dst += 8;
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
      listFree(&deps);
      listFree(&hdirs);
      listFree(&ownHeaders);
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
      listFree(&deps);
      listFree(&hdirs);
      listFree(&ownHeaders);
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
    for (int j = 0; j < m->commonLines.count; j++)
      n += strlen(m->commonLines.items[j]) + 1;
    for (int j = 0; j < p->lines.count; j++)
      n += strlen(p->lines.items[j]) + 1;
    n += (size_t)WS_MAX_PROJECTS * WS_LINE_LEN * 2;
    char *body = malloc(n);
    if (!body) return false;
    body[0] = '\0';

    char hdr[192];
    snprintf(hdr, sizeof(hdr), "/* Generated by rbot from %s — project '%s'. Do not edit. */\n",
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
            memcpy(key, ln, kl);
            key[kl] = '\0';
            snprintf(value, sizeof(value), "%s", eq + 1);
            char *k = trim(key);
            char *v = trim(value);
            if (wsLibraryValue(m, p, v, mapped, sizeof(mapped))) {
              char rewritten[WS_LINE_LEN * 3];
              snprintf(rewritten, sizeof(rewritten), "%s = %s", k, mapped);
              strcat(body, rewritten);
              strcat(body, "\n");
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
    char path[WS_PATH_LEN + WS_NAME_LEN + 16]; /* outDir sekarang WS_PATH_LEN */
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
    fprintf(stderr, "rbot: workspace: dependency cycle involving '%s'\n", m->projects[idx].name);
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

/* true bila proyek kemasan murni (output.binary = false di baris proyek):
   fase build-nya arsip/pack, bukan link binary. Semua pemindai baris
   `output.binary = false` di file ini memakai helper ini agar format
   hidup di satu tempat. */
static bool wsProjectArchiveOnly(const WsProject *p) {
  for (int i = 0; i < p->lines.count; i++)
    if (strcmp(p->lines.items[i], "output.binary = false") == 0) return true;
  return false;
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
  char bf[WS_PATH_LEN];
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
  const char *passLabel = "binary";
  if (libOnly)
    passLabel = wsProjectArchiveOnly(&m->projects[idx]) ? "archive" : "library";
  printf("\n%s Project : %s\n", repPrefix(), name);
  printf("%s Fase    : %s\n", repPrefix(), passLabel);
  wsModeSet(true);
  int rc = cmdBuildEx(jobs, bf, libOnly);
  if (rc != 0) cmdsStatMemoReset(); /* build gagal: output mungkin sudah berubah sebagian */
  wsModeSet(false);
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
  for (int i = 0; i < m->projectCount; i++)
    listFree(&m->projects[i].lines);
  listFree(&m->commonLines);
  listFree(&m->release.lines);
}

/* Muat model workspace + validasi dasar yang sama untuk semua mode (build,
   release, profile workspace=): file workspace ada, parse sukses, minimal
   satu proyek, dan validasi pack merge. false = pesan error sudah dicetak
   dan model sudah dibebaskan; pemanggil tidak boleh memakai model. Validasi
   release & topo-sort tetap di pemanggil: urutannya berbeda antar mode
   (workspaceRun memvalidasi topo sebelum override release; workspaceEnumerate
   sebaliknya) dan keduanya membutuhkan model yang sudah dimuat. */
static bool wsModelLoad(const char *wsDir, WsModel *m, const char **wsNameOut) {
  const char *wsName = workspaceFileName();
  if (!wsName) {
    fprintf(stderr, "rbot: %s not found\n", WORKSPACE_FILENAME);
    return false;
  }
  if (wsNameOut) *wsNameOut = wsName;
  char wsFile[WS_PATH_LEN];
  snprintf(wsFile, sizeof(wsFile), "%s/%s", wsDir, wsName);
  if (!wsParse(m, wsFile)) {
    wsModelFree(m);
    return false;
  }
  if (m->projectCount == 0) {
    fprintf(stderr, "rbot: workspace: no projects declared\n");
    wsModelFree(m);
    return false;
  }
  if (!wsValidatePackMerges(m)) {
    wsModelFree(m);
    return false;
  }
  return true;
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
      if (strcmp(k, "name") == 0)
        packApply(&c, "name", v);
      else if (strcmp(k, "version") == 0)
        packApply(&c, "version", v);
      else if (strcmp(k, "output") == 0)
        packApply(&c, "output", v);
      else if (strcmp(k, "compress") == 0)
        packApply(&c, "compress", v);
      else if (strcmp(k, "checksum") == 0)
        packApply(&c, "checksum", v);
      else if (strcmp(k, "format") == 0)
        packApply(&c, "format", v);
      else if (strcmp(k, "exclude") == 0)
        packApply(&c, "exclude", v);
      else if (strncmp(k, "deb.", 4) == 0)
        packApplyDeb(&c, k + 4, v);
      else
        fprintf(stderr, "rbot: release: key '%s' tidak dikenal (padanan pack.*)\n", k);
    }
  }

  /* Default files: artefak build semua proyek (bila tidak ada files). */
  if (c.pack.files.count == 0) {
    for (int i = 0; i < m->projectCount; i++) {
      const WsProject *pp = &m->projects[i];
      /* isArchive dihitung penuh lebih dulu (helper bersama): proyek
         kemasan tidak pernah punya entri lib default, apa pun urutan
         baris di Buildfile.ws. */
      bool isArchive = wsProjectArchiveOnly(pp);
      for (int l = 0; l < pp->lines.count; l++) {
        const char *ln = pp->lines.items[l];
        if (strncmp(ln, "output.libraryName = ", 21) == 0 && !isArchive) {
          char libEntry[WS_LINE_LEN * 2];
          const char *libName = ln + 21;
          int w1 = snprintf(libEntry, sizeof(libEntry), "%s/lib/lib%s.a", pp->root, libName);
          if (w1 > 0 && (size_t)w1 < sizeof(libEntry) && !listAdd(&c.pack.files, libEntry))
            goto oom;
          char shEntry[WS_LINE_LEN * 4];
          int w2 = snprintf(shEntry, sizeof(shEntry), "%s/lib/lib%s.so", pp->root, libName);
          if (w2 > 0 && (size_t)w2 < sizeof(shEntry) && !listAdd(&c.pack.files, shEntry)) goto oom;
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

  printf("\n%s Release : %s (workspace%s%s)\n", repPrefix(), c.pack.name,
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

/* ==================== Eksekusi anak build (workspaceRun) ====================
 *
 * Satu worklist per run: proyek needed dalam urutan topo, indeks model
 * dihitung SEKALI (loop kembar libpass/binpass tidak lagi memanggil
 * wsFindProject berulang). `level` = kedalaman depends_on; fase library
 * dijalankan per level — dua proyek pada level sama tidak saling butuh
 * hasil build (link .so fase library melewatkan artifact proyek lain),
 * jadi aman dikerjakan paralel. Fase binary TETAP pass terpisah seluruh
 * proyek: library lintas-proyek bisa saling siklik (rupa <-> ruka via
 * rupalib.cdeps/rukalib.cdeps di tests/rupaws) sehingga link hanya aman
 * setelah SEMUA library pass selesai.
 */

typedef struct {
  int idx;          /* indeks proyek di WsModel */
  const char *name; /* nama proyek (m->order.items[i], valid selama run) */
  int level;        /* 0 = tanpa dependensi needed; batch = level sama */
} WsWork;

/* Isi work[] dengan proyek needed dalam urutan topo; kembalikan jumlah. */
static int wsBuildWorklist(const WsModel *m, const unsigned char *needed, WsWork *work) {
  int n = 0;
  for (int i = 0; i < m->order.count; i++) {
    const char *name = m->order.items[i];
    int idx = wsFindProject(m, name);
    if (idx < 0 || !needed[idx]) continue;
    /* Topo menjamin dependensi sudah masuk work sebelumnya (i > dep). */
    int level = 0;
    for (int d = 0; d < m->depends[idx].count; d++) {
      int dep = wsFindProject(m, m->depends[idx].items[d]);
      if (dep < 0 || !needed[dep]) continue;
      for (int k = 0; k < n; k++)
        if (work[k].idx == dep && work[k].level >= level) level = work[k].level + 1;
    }
    work[n].idx = idx;
    work[n].name = name;
    work[n].level = level;
    n++;
  }
  return n;
}

/* Serial: persis perilaku lama — penanda fase per anak, henti saat gagal. */
static int wsRunBatchSerial(const WsModel *m, const WsWork *batch, int n, int jobs,
                            const char *cmd, const char *wsDir, bool libOnly) {
  const char *pre = libOnly ? "ws-libpass-pre" : "ws-binpass-pre";
  const char *post = libOnly ? "ws-libpass-post" : "ws-binpass-post";
  for (int i = 0; i < n; i++) {
    profMark(pre);
    int rc = wsExecOne(m, batch[i].name, cmd, jobs, wsDir, libOnly);
    profMark(post);
    if (rc != 0) return rc;
  }
  return 0;
}

#ifndef _WIN32
/* Paralel (POSIX): fork satu child per proyek — jobs dibagi rata (min 1).
   Output anak diarahkan ke file sementara .rbot/ws-par-* agar laporan
   per-proyek tidak tercampur; parent mencetak per slot (urutan worklist)
   setelah semua anak batch selesai. Anak gagal -> rc non-nol -> batch
   berikutnya berhenti (seperti serial). Untuk batch paralel penanda fase
   dicetak sekali per batch (bukan per anak). */
static int wsRunBatchFork(const WsModel *m, const WsWork *batch, int n, int jobs,
                          const char *cmd, const char *wsDir, bool libOnly) {
  const char *pre = libOnly ? "ws-libpass-pre" : "ws-binpass-pre";
  const char *post = libOnly ? "ws-libpass-post" : "ws-binpass-post";
  mkdirs(".rbot");
  pid_t *pids = malloc(sizeof(pid_t) * (size_t)n);
  int *fds = malloc(sizeof(int) * (size_t)n);
  char (*tmpl)[32] = malloc(sizeof(char[32]) * (size_t)n);
  if (!pids || !fds || !tmpl) {
    free(pids);
    free(fds);
    free(tmpl);
    return wsRunBatchSerial(m, batch, n, jobs, cmd, wsDir, libOnly);
  }
  profMark(pre);
  fflush(NULL); /* buffer stdout/stderr parent tidak boleh ikut ke child */
  int rc = 0;
  for (int i = 0; i < n; i++) {
    pids[i] = -1;
    fds[i] = -1;
    snprintf(tmpl[i], 32, ".rbot/ws-par-XXXXXX");
    fds[i] = mkstemp(tmpl[i]);
    if (fds[i] < 0) break; /* disk penuh dst: slot sisa -> serial */
    int pj = jobs / n + (i < jobs % n ? 1 : 0);
    if (pj < 1) pj = 1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
      dup2(fds[i], STDOUT_FILENO);
      dup2(fds[i], STDERR_FILENO);
      if (fds[i] > STDERR_FILENO) close(fds[i]);
      int crc = wsExecOne(m, batch[i].name, cmd, pj, wsDir, libOnly);
      fflush(NULL);
      _exit(crc); /* bukan exit(): hindari atexit ganda setelah fork */
    }
    if (pid < 0) break; /* fork gagal: slot ini -> serial */
    pids[i] = pid;
  }
  for (int i = 0; i < n; i++) {
    if (pids[i] > 0) {
      int st = 0;
      while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR) {}
      int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
      if (crc != 0 && rc == 0) rc = crc;
    } else {
      /* mkstemp/fork gagal untuk slot ini: kerjakan serial (stdout asli). */
      int crc = wsExecOne(m, batch[i].name, cmd, jobs, wsDir, libOnly);
      if (crc != 0 && rc == 0) rc = crc;
    }
    if (fds[i] >= 0) {
      char buf[4096];
      ssize_t r;
      lseek(fds[i], 0, SEEK_SET);
      while ((r = read(fds[i], buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)r, stdout);
      close(fds[i]);
      unlink(tmpl[i]);
    }
  }
  fflush(stdout);
  free(pids);
  free(fds);
  free(tmpl);
  profMark(post);
  return rc;
}
#endif

/* Jalankan batch anak build — DEFAULT SERIAL. Paralel lintas proyek (fork)
   hanya opt-in via RBOT_WS_PAR=1: pengukuran di rupaws (proot, -j4) menunjukkan
   paralel anak ~netral pada inkremental kecil dan ~48% LEBIH LAMBAT pada warm
   build besar (213 compile: 310.8s paralel vs 208.8s serial) — pembagian jobs
   antar anak menurunkan paralelisme dalam-proyek dan proot/contended gcc tidak
   diuntungkan. Keuntungan nyata butuh scheduler global lintas proyek (satu pool
   jobs untuk semua compile), bukan fork per anak. Jalur paralel dipertahankan
   untuk lingkungan native yang bisa diuntungkan — ukur dulu sebelum memakai. */
static int wsRunBatch(const WsModel *m, const WsWork *batch, int n, int jobs, const char *cmd,
                      const char *wsDir, bool libOnly) {
  if (n <= 0) return 0;
#ifndef _WIN32
  if (jobs > 1 && n > 1 && getenv("RBOT_WS_PAR"))
    return wsRunBatchFork(m, batch, n, jobs, cmd, wsDir, libOnly);
#endif
  return wsRunBatchSerial(m, batch, n, jobs, cmd, wsDir, libOnly);
}

int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel) {
  char wsDir[MAX_PATH];
  if (!fsGetCwd(wsDir, sizeof(wsDir))) {
    fprintf(stderr, "rbot: cannot determine current directory\n");
    return 1;
  }
  WsModel m;
  const char *wsName = NULL; /* dipakai wsSynthesize di bawah */
  if (!wsModelLoad(wsDir, &m, &wsName)) return 1;
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
      if (strcmp(key, "format") == 0 && *v && strcmp(v, "tar") != 0 && strcmp(v, "deb") != 0 &&
          strcmp(v, "none") != 0) {
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

  char synthDir[WS_PATH_LEN];
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
      for (int i = 0; i < m.projectCount; i++)
        needed[i] = 1;
    }
    /* Build selektif juga menarik proyek yang dirujuk lewat library
       (library.<os> = <nama proyek>) — transitif; siklus library sah dan
       tidak membuat loop ini macet (fixpoint sederhana). */
    wsExpandLibDeps(&m, needed);
  } else {
    for (int i = 0; i < m.projectCount; i++)
      needed[i] = 1;
  }

  int rc = 0;
  profMark("ws-parse+synth");

  /* Gerbang workspace per-closure: hanya entri milik proyek needed yang
     di-stat — `-w <nama>` tidak membayar validasi seluruh workspace. */
  if (strcmp(cmd, "build") == 0) {
    wsStateValidFiltered(&m, needed);
    /* Status gerbang akurat (per closure yang dibangun), dihitung di sini setelah
       parse agar tidak mengubah urutan validasi build. */
    const char *rs = getenv("RBOT_STATUS");
    if (rs && *rs && strcmp(rs, "0") != 0)
      fprintf(stderr, "rbot status: mode=workspace gate=%s\n", wsGateStartValid ? "valid" : "cold");
  }

  /* Worklist sekali: proyek needed urut topo + level dependensi. */
  WsWork work[WS_MAX_PROJECTS];
  int wcount = wsBuildWorklist(&m, needed, work);

  if (strcmp(cmd, "build") == 0) {
    /* FASE 1 — library pass: tiap proyek mengkompilasi source-nya dan
       mengemas lib<name>.a/.so TANPA link binary. Dipecah per level
       dependensi: proyek pada level sama tidak saling butuh hasil build
       — dikerjakan paralel (lihat wsRunBatch). Urutan topo tetap
       dijaga: archive rupamod jadi sebelum fase library ruka meng-embed. */
    for (int i = 0; rc == 0 && i < wcount;) {
      int j = i;
      while (j < wcount && work[j].level == work[i].level) j++;
      rc = wsRunBatch(&m, work + i, j - i, jobs, cmd, wsDir, true);
      i = j;
    }
    /* FASE 2 — binary pass: link binary setelah SEMUA library tersedia
       (cdeps bisa siklik — lihat catatan wsBuildWorklist). Proyek
       kemasan (output.binary = false) tuntas di fase library — Lewati
       SENYAP: laporan hanya menampilkan proyek yang dikerjakan fase ini
       (design/build_report.md). */
    if (rc == 0) {
      WsWork bin[WS_MAX_PROJECTS];
      int bn = 0;
      for (int i = 0; i < wcount; i++)
        if (!wsProjectArchiveOnly(&m.projects[work[i].idx])) bin[bn++] = work[i];
      rc = wsRunBatch(&m, bin, bn, jobs, cmd, wsDir, false);
    }
  } else {
    /* clean: satu pass serial (binary pass), tanpa fase library. */
    for (int i = 0; rc == 0 && i < wcount; i++)
      rc = wsExecOne(&m, work[i].name, cmd, jobs, wsDir, false);
  }

  /* FASE 3 — release pass: pack SATU paket workspace pada root workspace,
     output ke dist/ (design/release.md revisi 2). Bila tidak ada section
     release.* sama sekali (dan tidak ada override CLI), langkah ini
     dilewati — workspace tanpa release berperilaku seperti dulu. */
  /* Build selektif (`-w <nama>`) tidak membuat paket: release hanya jalan untuk
     build penuh atau bila diminta eksplisit (`release`). */
  bool selectiveBuild = only && *only && !releaseOnly;
  if (rc == 0 && strcmp(cmd, "build") == 0 && m.release.present && !selectiveBuild) {
    profMark("release-pre");
    rc = wsRelease(&m, jobs, wsDir);
    profMark("release-post");
  }

  if (profOn())
    profReport(); /* delta antar fase workspace (build no-op:
                                 seluruh fase berada antara dua penanda) */

  /* Rekam/buang workspace state (design/workspace.md): build sukses ->
     tulis .rbot/workspace.state; clean -> buang agar build berikutnya cold. */
  if (strcmp(cmd, "build") == 0 && rc == 0) {
    /* Gerbang disimpan setiap run sukses (bukan hanya setelah build), supaya
       perubahan layout yang terjadi saat run (mis. folder state) tidak basi. */
    if (!selectiveBuild) wsStateSave(&m, wsDir);
  } else if (strcmp(cmd, "clean") == 0) {
    char sp[WS_PATH_LEN];
    snprintf(sp, sizeof(sp), "%s/%s", wsDir, WS_STATE_FILE);
    fsRemoveFile(sp);
  }

  wsModelFree(&m);
  return rc;
}

const char *workspaceFileName(void) {
  if (fsFileExists(WORKSPACE_FILENAME)) return WORKSPACE_FILENAME;
  if (fsFileExists(WORKSPACE_FILENAME_LEGACY)) return WORKSPACE_FILENAME_LEGACY;
  return NULL;
}

bool workspaceFileExists(void) {
  return workspaceFileName() != NULL;
}

/* ==================== workspace state (design/workspace.md) ==================== */

/*
 * workspace.state (design/workspace.md): stamp SEMUA input seluruh workspace,
 * dalam path absolut, diambil dari state yang sudah ditulis build proyek:
 *   .rbot/ws.state, .rbot/ws.lib.state  (F/D/X per proyek)
 *   .rbot/ws.src                        (folder + file sumber yang ditelusuri)
 * Baris pertama: RBOTWSG1 <wsDir absolut>. Lalu baris F/D/X seperti ws.state.
 * `cold` = stamp tidak lagi cocok. Path absolut membuat hasil stat ini ikut
 * masuk memo proses, sehingga validator per proyek tidak men-stat ulang.
 */
/* Hasil validasi gerbang di awal run (dipakai wsStateSave untuk melewati penulisan
   saat no-op: gerbang yang sudah cocok sudah memuat keadaan yang sama). */

#define WS_GATE_PATH (MAX_PATH * 3)
/* Gabung dua bagian path dengan '/', memeriksa panjang secara eksplisit.
   false bila tidak muat (dst dikosongkan). */
static bool wsPathJoin(char *dst, size_t n, const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (n == 0 || la + 1 + lb + 1 > n) {
    if (n) dst[0] = '\0';
    return false;
  }
  memcpy(dst, a, la);
  dst[la] = '/';
  memcpy(dst + la + 1, b, lb);
  dst[la + 1 + lb] = '\0';
  return true;
}

#define WS_GATE_MAGIC "RBOTWSG1"

static int wsGateCmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void wsGateAdd(List *l, char kind, const char *abs, long long mt, long long sz) {
  char line[WS_GATE_PATH * 2];
  if (kind == 'X') snprintf(line, sizeof(line), "X 0 0 %s", abs);
  else if (kind == 'D') snprintf(line, sizeof(line), "D %lld 0 %s", mt, abs);
  else snprintf(line, sizeof(line), "F %lld %lld %s", mt, sz, abs);
  listAdd(l, line);
}

/* Baca satu file state proyek (ws.state / ws.lib.state) dan tambahkan entrinya. */
/* Gabung folder proyek dengan path relatif; path absolut dipakai apa adanya. */
static void wsGateJoin(char *out, size_t n, const char *projDir, const char *rel) {
  if (rel[0] == '/') snprintf(out, n, "%s", rel);
  else wsPathJoin(out, n, projDir, rel);
}

/* Folder .rbot adalah state, bukan input: tidak pernah masuk stamp workspace. */
static bool wsGateIsState(const char *p) {
  return strstr(p, "/.rbot") != NULL || strncmp(p, ".rbot", 5) == 0;
}

/* Entri yang di-exclude proyek (mis. build, dist) bukan input: freshness arsip
   juga mengabaikannya. Cocok per nama segmen path. */
static bool wsGateExcluded(const List *excl, const char *rel) {
  if (!excl) return false;
  char *savePtr = NULL;
  char buf[MAX_PATH * 3];
  snprintf(buf, sizeof(buf), "%s", rel);
  for (char *seg = strtok_r(buf, "/", &savePtr); seg; seg = strtok_r(NULL, "/", &savePtr)) {
    if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) continue;
    for (int i = 0; i < excl->count; i++)
      if (strcmp(excl->items[i], seg) == 0) return true;
  }
  return false;
}

/* Kumpulkan nama exclude dari baris proyek ("archive.exclude = a, b" / "exclude = a, b"). */
static void wsGateCollectExcludes(const WsProject *pr, List *excl) {
  for (int i = 0; i < pr->lines.count; i++) {
    const char *ln = pr->lines.items[i];
    const char *eq = strstr(ln, "exclude = ");
    if (!eq) continue;
    if (eq != ln && eq[-1] != '.') continue;
    char *savePtr = NULL;
    char buf[MAX_PATH * 3];
    snprintf(buf, sizeof(buf), "%s", eq + 10);
    for (char *tok = strtok_r(buf, ",", &savePtr); tok; tok = strtok_r(NULL, ",", &savePtr)) {
      while (*tok == ' ') tok++;
      char *e = tok + strlen(tok);
      while (e > tok && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
      if (*tok) listAdd(excl, tok);
    }
  }
}

static void wsGateReadState(List *out, const char *projDir, const char *file) {
  FILE *fp = fopen(file, "r");
  if (!fp) return;
  char line[WS_GATE_PATH * 2];
  if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; } /* header */
  while (fgets(line, sizeof(line), fp)) {
    /* F/D/X: parse lewat helper bersama (cmds_state.c); tujuan absolut. */
    char kind = 0;
    long long mt = 0, sz = 0;
    char rel[WS_GATE_PATH], abs[WS_GATE_PATH];
    if (!cmdsParseStateEntry(line, &kind, &mt, &sz, rel)) continue;
    wsGateJoin(abs, sizeof(abs), projDir, rel);
    if (kind == 'D') wsGateAdd(out, 'D', abs, mt, 0);
    else if (kind == 'F') wsGateAdd(out, 'F', abs, mt, sz);
    else wsGateAdd(out, 'X', abs, 0, 0);
  }
  fclose(fp);
}

/* Folder sumber & file yang ditelusuri (ws.src): D dan S. Mtime D dari walk. */
static void wsGateReadSrc(List *out, const char *projDir, const List *excl) {
  char file[WS_PATH_LEN];
  wsPathJoin(file, sizeof(file), projDir, ".rbot/ws.src");
  FILE *fp = fopen(file, "r");
  if (!fp) return;
  char line[WS_GATE_PATH * 2];
  if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
  while (fgets(line, sizeof(line), fp)) {
    char abs[WS_GATE_PATH], rel[WS_GATE_PATH];
    long long mt = 0;
    if (line[0] == 'D' && sscanf(line, "D %lld %" WS_SCAN "s", &mt, rel) == 2) {
      if (wsGateIsState(rel) || wsGateExcluded(excl, rel)) continue;
      wsPathJoin(abs, sizeof(abs), projDir, rel);
      /* Mtime folder diambil SEKARANG (bukan dari catatan walk): folder bisa berubah
         setelah walk dalam run yang sama, dan gerbang harus memuat keadaan akhir. */
      int64_t cur = -1;
      long long cs = 0;
      if (fsStampNsSize(abs, &cur, &cs)) wsGateAdd(out, 'D', abs, (long long)cur, 0);
      else wsGateAdd(out, 'X', abs, 0, 0);
      (void)mt;
    } else if (line[0] == 'S' && sscanf(line, "S %" WS_SCAN "s", rel) == 1) {
      if (wsGateIsState(rel) || wsGateExcluded(excl, rel)) continue;
      wsPathJoin(abs, sizeof(abs), projDir, rel);
      wsGateAdd(out, 'F', abs, 0, 0); /* mtime file sumber: diambil saat save (lihat wsStateSave) */
    }
  }
  fclose(fp);
}

/* Tambah teks format ke buffer dinamis (tumbuh sesuai kebutuhan, tanpa batas tetap). */
static bool wsAppendf(char **buf, size_t *used, size_t *cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return false;
  size_t n = (size_t)need;
  if (*used + n + 1 > *cap) {
    size_t ncap = (*used + n + 1) * 2;
    if (ncap < 256) ncap = 256;
    char *nb = realloc(*buf, ncap);
    if (!nb) return false;
    *buf = nb;
    *cap = ncap;
  }
  va_start(ap, fmt);
  vsnprintf(*buf + *used, *cap - *used, fmt, ap);
  va_end(ap);
  *used += n;
  return true;
}

static void wsStateSave(const WsModel *m, const char *wsDir) {
  (void)wsDir; /* gerbang memakai cwd (root workspace) */
  if (wsGateStartValid) return; /* no-op: gerbang sudah cocok, tidak perlu ditulis ulang */
  const char *wsName = workspaceFileName();
  if (!wsName) return;
  char wsAbs[WS_GATE_PATH];
  if (!fsGetCwd(wsAbs, sizeof(wsAbs))) return;
  if (strpbrk(wsAbs, " \t\r\n")) return; /* whitespace: format tak aman */

  List items = {0};
  char bf[WS_GATE_PATH];
  wsPathJoin(bf, sizeof(bf), wsAbs, wsName);
  int64_t bm = fsMTimeNs(bf);
  long long bs = fsFileSize(bf);
  if (bm < 0 || bs < 0) { listFree(&items); return; }
  wsGateAdd(&items, 'F', bf, bm, bs);

  for (int i = 0; i < m->projectCount; i++) {
    const char *root = m->projects[i].root[0] ? m->projects[i].root : ".";
    char projDir[WS_GATE_PATH];
    if (strcmp(root, ".") == 0) snprintf(projDir, sizeof(projDir), "%s", wsAbs);
    else wsPathJoin(projDir, sizeof(projDir), wsAbs, root);
    char f1[WS_GATE_PATH], f2[WS_GATE_PATH];
    wsPathJoin(f1, sizeof(f1), projDir, ".rbot/ws.state");
    wsPathJoin(f2, sizeof(f2), projDir, ".rbot/ws.lib.state");
    wsGateReadState(&items, projDir, f1);
    wsGateReadState(&items, projDir, f2);
    List excl = {0};
    wsGateCollectExcludes(&m->projects[i], &excl);
    wsGateReadSrc(&items, projDir, &excl);
    listFree(&excl);
  }

  if (items.count > 1) qsort(items.items, (size_t)items.count, sizeof(char *), wsGateCmp);

  /* Isi dibangun di memori dulu (kapasitas tumbuh sesuai isi, tanpa buffer tetap);
     file ditulis hanya bila berbeda dari yang ada. */
  size_t cap = 0, used = 0;
  char *buf = NULL;
  if (!wsAppendf(&buf, &used, &cap, "%s %s\n", WS_GATE_MAGIC, wsAbs)) {
    listFree(&items);
    return;
  }
  char prev[WS_GATE_PATH * 2] = "";
  for (int i = 0; i < items.count; i++) {
    if (strcmp(items.items[i], prev) == 0) continue;
    snprintf(prev, sizeof(prev), "%s", items.items[i]);
    bool ok = true;
    if (prev[0] == 'F' && strncmp(prev, "F 0 0 ", 6) == 0) {
      /* entri sumber: mtime dari memo (sudah dicek validasi), tanpa stat baru */
      const char *abs = prev + 6;
      int64_t mt = -1;
      long long sz = -1;
      if (cmdsStatCached(abs, &mt, &sz))
        ok = wsAppendf(&buf, &used, &cap, "F %lld %lld %s\n", (long long)mt, sz, abs);
      else
        ok = wsAppendf(&buf, &used, &cap, "X 0 0 %s\n", abs);
    } else {
      ok = wsAppendf(&buf, &used, &cap, "%s\n", prev);
    }
    if (!ok) {
      free(buf);
      listFree(&items);
      return;
    }
  }
  listFree(&items);

  char path[WS_GATE_PATH];
  wsPathJoin(path, sizeof(path), wsAbs, WS_STATE_FILE);
  /* Tulis hanya bila berubah: no-op biasa tidak menambah write. */
  FILE *old = fopen(path, "rb");
  bool same = false;
  if (old) {
    fseek(old, 0, SEEK_END);
    long on = ftell(old);
    fseek(old, 0, SEEK_SET);
    if (on == (long)used) {
      char *ob = malloc((size_t)on + 1);
      if (ob && fread(ob, 1, (size_t)on, old) == (size_t)on) same = memcmp(ob, buf, used) == 0;
      free(ob);
    }
    fclose(old);
  }
  if (!same) {
    mkdirs(".rbot");
    char tmp[WS_GATE_PATH];
    size_t pl = strlen(path);
    if (pl + 5 > sizeof(tmp)) {
      free(buf);
      return;
    }
    memcpy(tmp, path, pl);
    memcpy(tmp + pl, ".tmp", 5); /* path + ".tmp" (termasuk NUL) */
    FILE *f = fopen(tmp, "wb");
    if (f) {
      bool ok = fwrite(buf, 1, used, f) == used;
      if (fclose(f) == 0 && ok) rename(tmp, path);
      else fsRemoveFile(tmp);
    }
  }
  free(buf);
}

/* true bila file workspace.state ada (tanpa validasi stamp). */
bool wsStateFileExists(void) {
  const char *wsName = workspaceFileName();
  if (!wsName) return false;
  char wsAbs[WS_GATE_PATH];
  if (!fsGetCwd(wsAbs, sizeof(wsAbs))) return false;
  char path[WS_GATE_PATH];
  wsPathJoin(path, sizeof(path), wsAbs, WS_STATE_FILE);
  return fsFileExists(path);
}

/* true bila workspace.state ada dan stamp input MASIH COCOK — divalidasi
   HANYA untuk entri milik proyek `needed` (gate per-closure): baris
   Buildfile.ws + path di bawah root proyek needed. Dengan needed = semua
   proyek, filter ≡ validasi penuh (semua entri gate memang berada di root
   workspace atau root proyek). Dipanggil workspaceRun sebelum pass build —
   sekaligus menyetel wsGateStartValid untuk wsStateSave. */
static bool wsStateValidFiltered(const WsModel *m, const unsigned char *needed) {
  const char *wsName = workspaceFileName();
  if (!wsName) return false;
  char wsAbs[WS_GATE_PATH];
  if (!fsGetCwd(wsAbs, sizeof(wsAbs))) return false;
  char bfPath[WS_GATE_PATH]; /* Buildfile.ws absolut — selalu divalidasi */
  wsPathJoin(bfPath, sizeof(bfPath), wsAbs, wsName);
  /* Prefix root proyek needed: "<ws>/<root>/"; root "." -> "<ws>/". */
  char (*pref)[WS_GATE_PATH] = malloc(sizeof(char[WS_GATE_PATH]) * (size_t)m->projectCount);
  int pn = 0;
  bool allocOk = pref != NULL;
  if (allocOk) {
    for (int i = 0; i < m->projectCount && pn < m->projectCount; i++) {
      if (!needed[i]) continue;
      const char *root = m->projects[i].root[0] ? m->projects[i].root : ".";
      char dir[WS_GATE_PATH];
      int w;
      if (strcmp(root, ".") == 0)
        w = snprintf(dir, sizeof(dir), "%s/", wsAbs);
      else
        w = snprintf(dir, sizeof(dir), "%s/%s/", wsAbs, root);
      if (w <= 0 || (size_t)w >= sizeof(dir)) continue;
      snprintf(pref[pn], sizeof(pref[pn]), "%s", dir);
      pn++;
    }
  }
  char path[WS_GATE_PATH];
  wsPathJoin(path, sizeof(path), wsAbs, WS_STATE_FILE);
  FILE *f = fopen(path, "r");
  bool ok = allocOk && f != NULL;
  char line[WS_GATE_PATH * 2];
  if (ok) {
    ok = fgets(line, sizeof(line), f) != NULL;
    if (ok) {
      char magic[32], scwd[WS_GATE_PATH];
      ok = sscanf(line, "%31s %" WS_SCAN "s", magic, scwd) == 2 && strcmp(magic, WS_GATE_MAGIC) == 0 &&
           strcmp(scwd, wsAbs) == 0;
    }
  }
  while (ok && fgets(line, sizeof(line), f)) {
    /* F/D/X: parse + cocokkan stamp lewat helper bersama (cmds_state.c). */
    char kind = line[0];
    if (kind != 'F' && kind != 'D' && kind != 'X') continue;
    char p[WS_GATE_PATH];
    long long a = 0, b = 0;
    if (!cmdsParseStateEntry(line, &kind, &a, &b, p)) {
      ok = false;
      break;
    }
    /* Gate per-closure: entri di luar proyek needed tidak divalidasi. */
    bool mine = strcmp(p, bfPath) == 0;
    for (int i = 0; !mine && i < pn; i++)
      mine = strncmp(p, pref[i], strlen(pref[i])) == 0;
    if (!mine) continue;
    ok = cmdsStateEntryFresh(line);
  }
  if (f) fclose(f);
  free(pref);
  wsGateStartValid = ok;
  return ok;
}

/*
 * workspaceEnumerate — parse & validasi model workspace untuk enumerasi
 * (dipakai `rbot profile workspace=`): semua validasi model yang sama
 * dengan workspaceRun, TANPA sintesis/build apa pun. nama proyek hasil
 * topo sort diisi ke projectNames[]. Buffer internal model statis —
 * pointer hasil valid sampai panggilan berikutnya.
 */
int workspaceEnumerate(const char *wsDir, int *count, const char **projectNames, int maxProjects) {
  static WsModel mM;
  WsModel *m = &mM;
  *count = 0;

  if (!wsModelLoad(wsDir, m, NULL)) return 1;
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
