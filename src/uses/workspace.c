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
#include "../compdb.h"
#include "../config.h"
#include "../compile.h"
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
 * di .satria/workspace/<nama>.Buildfile agar cache config & fast path tidak
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
 * Path relatif terhadap lokasi file workspace (aturan satria yang sudah
 * ada) — berarti relatif root workspace. Urutan build dari depends_on
 * (topological sort), bukan urutan `projects =`.
 */

#define WS_NAME_LEN 64
#define WS_LINE_LEN 1024

/* File state workspace di root — padanan build.state per-proyek untuk mode
   workspace, sehingga bootstrap bisa evaluasi no-op tanpa parse topologi
   (design/workspace.md). Definisi di bawah (setelah workspaceFileExists). */
#define WS_STATE_FILE ".satria/workspace.state"

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

/* Guard direktori anggota (wsMemberCwdDetect): selama keanggotaan divalidasi
   lewat model induk, pesan parser workspace dibungkam — induk yang rusak
   berarti "bukan anggota", bukan kesalahan run ini. Definisi & pemakaian di
   bagian guard (dekat akhir file). */
static bool wsGuardQuiet = false;

/* Fast-hit per proyek pada run berjalan (wsRunProject): proyek yang
   lolos fast-state source-nya dijamin tak berubah oleh stamp folder di
   ws.state — memungkinkan workspace_log memuat ulang daftar source dari
   snapshot <n>.src tanpa re-scan (keputusan (a) workspace_log). */
static bool wsLogFastOk[WS_MAX_PROJECTS];
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
    if (!wsGuardQuiet)
      fprintf(stderr, "satria: workspace: sintaks 'releases' tidak lagi didukung\n"
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
        if (!wsGuardQuiet)
          fprintf(stderr, "satria: workspace: 'root' diabaikan (root berlaku per proyek — "
                          "deklarasikan per proyek: <nama>.root = ...; output tetap "
                          "root-centric otomatis: bin/, build/, lib/ di root workspace)\n");
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
    if (!wsGuardQuiet) fprintf(stderr, "satria: cannot open %s\n", path);
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
          if (!wsGuardQuiet)
            fprintf(stderr, "satria: workspace: invalid project name '%s'\n", item);
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
            fprintf(stderr, "satria: workspace: '%s' pack.merge unknown project '%s'\n", owner->name,
                    item);
            return false;
          }
          List files = {0};
          bool ok = wsProjectPackFiles(&m->projects[dep], &files);
          listFree(&files);
          if (!ok) {
            fprintf(stderr, "satria: workspace: '%s' pack.merge project '%s' has no pack.files\n",
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
      fprintf(stderr, "satria: workspace: release.format '%s' tidak dikenal (tar, deb)\n", fmt);
      return false;
    }
    if (strcmp(fmt, "none") == 0) {
      fprintf(stderr, "satria: workspace: release.format 'none' tidak bermakna di workspace\n");
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
    fprintf(stderr, "satria: workspace: dependency cycle involving '%s'\n", m->projects[idx].name);
    return false;
  }
  color[idx] = 1;
  for (int i = 0; i < m->depends[idx].count; i++) {
    int dep = wsFindProject(m, m->depends[idx].items[i]);
    if (dep < 0) {
      fprintf(stderr, "satria: workspace: '%s' depends on unknown project '%s'\n",
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
      /* Path artifact lintas-proyek relatif ROOT WORKSPACE: library
         ditulis ke <libDir>/ di root workspace (design/workspace.md,
         "lib/ di root"), jadi path-nya <libDir>/lib<name>.a — tanpa nama
         folder proyek di depannya. Size dicek dulu agar path panjang
         tidak terpotong snprintf. */
      {
        const char *libPrefix = "/lib";
        const char *suffix = ".a";
        size_t need = strlen(libDir) + strlen(libPrefix) + strlen(libName) + strlen(suffix) + 1;
        if (need > sizeof(mapped)) return false;
        char *dst = mapped;
        const char *parts[] = {libDir, libPrefix, libName, suffix};
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
 *     <n>.headers = <dep>/include      (folder header publik proyek)
 *     <n>.library.<os> = <dep>          (artifact library per-OS)
 * Dengan cdeps, WsConfig menerbitkan `library = <dep>/<libDir>/lib<name>.a`
 * dan `headers = <dep>/<dir>` (keduanya relatif root workspace) secara
 * internal. Siklus antar proyek sah — sama seperti library.<os>: dua fase
 * build workspace yang menyelesaikannya (fase library melewatkan artifact
 * path proyek lain).
 */

/* Folder header publik proyek `dep`: semua nilai `headers = ...` proyek
   itu (diprefix <root>/ — relatif root workspace), default <root>/include
   bila proyek tidak menulis headers sama sekali. */
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
        /* <root>/<item> — relatif root workspace; dibangun manual
           (memcpy) agar gcc -Wformat-truncation tidak menolak path. */
        char joined[MAX_PATH];
        size_t need = strlen(dep->root) + 1 + strlen(item) + 1;
        if (need > sizeof(joined)) continue;
        char *dst = joined;
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
    size_t need = strlen(dep->root) + strlen("/include") + 1;
    if (need <= sizeof(joined)) {
      char *dst = joined;
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

/* ==================== Susun Config di memori (WsConfig) ====================
 *
 * Menggantikan sintesis file + loadConfig (design/workspace_v1.md §Pemisahan
 * Jalur Build): tiap proyek memperoleh Config hasil resolusi Buildfile.ws
 * di memori — tanpa file .Buildfile sintesis, tanpa chdir. Skoping path
 * per-kunci:
 *   - konten-proyek (sources, headers, archive.src, embedded.*.src) ->
 *     relatif root proyek (di-prefix <root>/);
 *   - lintas-proyek/workspace (headers.internal, archive.dir,
 *     embedded.*.file, library.<os> = <proyek>) -> relatif root workspace;
 *   - output: build/<proyek>, bin, lib di root workspace.
 */

/* true bila item adalah path relatif yang harus ditempel ke root proyek
   (bukan flag -I/-D, bukan path absolut POSIX maupun Windows). */
static bool wsIsRelPath(const char *item) {
  if (!item || !*item) return false;
  if (item[0] == '-' || item[0] == '/') return false;
  if (item[1] == ':' && item[2]) return false; /* C:\... / C:/... */
  return true;
}

/* Peta daftar "a, b, c" per item -> out (dipisah ", ").
   headersForm: sintaks headers — "I." = root proyek, "I.x" = <root>/.x
   (semantik headerEntryDir lama terhadap root proyek). */
static bool wsMapList(const char *root, const char *value, bool headersForm, char *out,
                      size_t n) {
  char buf[WS_LINE_LEN];
  if (!value || strlen(value) >= sizeof(buf)) return false;
  copyStr(buf, sizeof(buf), value);
  out[0] = '\0';
  bool first = true;
  for (char *save = buf;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *item = trim(save);
    char mapped[WS_LINE_LEN];
    mapped[0] = '\0';
    if (*item) {
      if (headersForm && item[0] == 'I' && item[1] == '.') {
        if (item[2] == '\0')
          snprintf(mapped, sizeof(mapped), "%s", root); /* "I." */
        else
          snprintf(mapped, sizeof(mapped), "%s/%s", root, item + 1); /* "<root>/.x" */
      } else if (strcmp(item, ".") == 0) {
        snprintf(mapped, sizeof(mapped), "%s", root);
      } else if (wsIsRelPath(item)) {
        if ((size_t)snprintf(mapped, sizeof(mapped), "%s/%s", root, item) >= sizeof(mapped))
          return false;
      } else {
        snprintf(mapped, sizeof(mapped), "%s", item); /* flag/abs */
      }
      if (!first && strlen(out) + 2 >= n) return false;
      if (!first) strcat(out, ", ");
      if (strlen(out) + strlen(mapped) + 1 >= n) return false;
      strcat(out, mapped);
      first = false;
    }
    if (!comma) break;
    save = comma + 1;
  }
  return true;
}

/* Normalize workspace header groups into a deterministic, duplicate-free
   include list. Parentheses group tracked roots; they do not change -I
   precedence based on their position in the declaration. */
static int wsHeaderCmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}



/* Grup `(owner, item, ...)` hanya berlaku untuk proyek `owner`. Di dalamnya,
   `I.` = <owner>/include dan item lain = path relatif root workspace. Entri di
   luar kurung berlaku untuk semua proyek (aturan bare di atas). */
/* `I.` di level workspace = include root; di level proyek = <proyek>/include.
   Hanya `I.` yang menjadi -I (headers). Item lain hanya dilacak (headers.internal):
   konsumen harus memanggilnya lewat path relatif, mis. "../../api/api.h".
   Grup `(owner, ...)` hanya berlaku untuk proyek `owner`; di dalamnya `I.` =
   <owner>/include dan item lain = path relatif root workspace. */
static bool wsMapHeaders(const char *root, const char *value, bool global, const char *proj,
                         bool split, char *outPub, char *outInt, size_t n) {
  char buf[WS_LINE_LEN];
  if (!value || strlen(value) >= sizeof(buf)) return false;
  copyStr(buf, sizeof(buf), value);
  char *pub[256], *intl[256];
  int npub = 0, nint = 0;
  char *p = buf;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (!*p) break;
    if (*p == '(') {
      char *close = strchr(p, ')');
      if (!close) goto fail;
      *close = '\0';
      char *owner = NULL;
      for (char *q = p + 1;;) {
        char *c = strchr(q, ',');
        if (c) *c = '\0';
        char *t = trim(q);
        if (*t) {
          if (!owner) {
            owner = t;
          } else if (strcmp(owner, proj) == 0) {
            char mapped[WS_LINE_LEN];
            bool isPub = strcmp(t, "I.") == 0;
            if (isPub) {
              if ((size_t)snprintf(mapped, sizeof(mapped), "%s/include", root) >= sizeof(mapped))
                goto fail;
            } else if (wsIsRelPath(t)) {
              if ((size_t)snprintf(mapped, sizeof(mapped), "./%s", t) >= sizeof(mapped)) goto fail;
            } else {
              if (!wsMapList(root, t, true, mapped, sizeof(mapped))) goto fail;
            }
            if (isPub ? npub >= 256 : nint >= 256) goto fail;
            char *dup = malloc(strlen(mapped) + 1);
            if (!dup) goto fail;
            strcpy(dup, mapped);
            if (isPub) pub[npub++] = dup; else intl[nint++] = dup;
          }
        }
        if (!c) break;
        q = c + 1;
      }
      p = close + 1;
    } else {
      char *end = p;
      while (*end && *end != ',' && *end != '(') end++;
      char save = *end;
      *end = '\0';
      char *t = trim(p);
      if (*t) {
        char mapped[WS_LINE_LEN];
        bool isPub = strcmp(t, "I.") == 0 || !split;
        if (strcmp(t, "I.") == 0) {
          /* global: include root workspace. proyek: include milik proyek itu. */
          if (global) snprintf(mapped, sizeof(mapped), "include");
          else if ((size_t)snprintf(mapped, sizeof(mapped), "%s/include", root) >= sizeof(mapped))
            goto fail;
        } else if (global && wsIsRelPath(t)) {
          char candidate[WS_LINE_LEN];
          if (snprintf(candidate, sizeof(candidate), "%s/include", t) >= (int)sizeof(candidate))
            goto fail;
          if (fsDirExists(candidate)) copyStr(mapped, sizeof(mapped), candidate);
          else if (!wsMapList(".", t, true, mapped, sizeof(mapped))) goto fail;
        } else if (!wsMapList(root, t, true, mapped, sizeof(mapped))) {
          goto fail;
        }
        if (isPub ? npub >= 256 : nint >= 256) goto fail;
        char *dup = malloc(strlen(mapped) + 1);
        if (!dup) goto fail;
        strcpy(dup, mapped);
        if (isPub) pub[npub++] = dup; else intl[nint++] = dup;
      }
      *end = save;
      p = end;
      if (save == '\0') break;
    }
  }
  /* gabungkan ke string, tanpa duplikat, dalam urutan deterministik */
  char **lists[2] = {pub, intl};
  int counts[2] = {npub, nint};
  char *outs[2] = {outPub, outInt};
  for (int li = 0; li < 2; li++) {
    qsort(lists[li], (size_t)counts[li], sizeof(char *), wsHeaderCmp);
    outs[li][0] = '\0';
    for (int i = 0; i < counts[li]; i++) {
      if (i && strcmp(lists[li][i], lists[li][i - 1]) == 0) continue;
      size_t have = strlen(outs[li]), add = strlen(lists[li][i]);
      if (have + add + (have ? 2 : 0) + 1 > n) goto fail;
      if (have) strcat(outs[li], ", ");
      strcat(outs[li], lists[li][i]);
    }
  }
  for (int i = 0; i < npub; i++) free(pub[i]);
  for (int i = 0; i < nint; i++) free(intl[i]);
  return true;
fail:
  for (int i = 0; i < npub; i++) free(pub[i]);
  for (int i = 0; i < nint; i++) free(intl[i]);
  return false;
}

/* "key = value" -> key kanonik (ter-trim) + pointer value di buffer
   mutable milik pemanggil. */
static bool wsSplitKV(char *line, char *key, size_t kn, char **val) {
  char *eq = strchr(line, '=');
  if (!eq) return false;
  *eq = '\0';
  char *k = trim(line);
  char *v = trim(eq + 1);
  if (!*k || strlen(k) >= kn) return false;
  memcpy(key, k, strlen(k) + 1);
  *val = v;
  return true;
}

/* pack.* per proyek di luar scope v1 — peringatan sekali per run. */
static bool wsPackWarned = false;

/* Terapkan SATU baris model (key sudah kanonik) ke Config dengan skoping
   per-kunci (lihat komentar seksi di atas). */
static void wsApplyLine(const WsModel *m, const WsProject *p, Config *c, const char *key,
                        const char *value, bool global) {
  char mapped[WS_LINE_LEN * 4];

  if (strncmp(key, "pack.", 5) == 0) {
    if (!wsPackWarned) {
      wsPackWarned = true;
      fprintf(stderr, "satria: workspace: pack.* per proyek diabaikan di mode workspace "
                      "(lihat design/workspace_v1.md)\n");
    }
    return;
  }
  if (strcmp(key, "cdeps") == 0) return; /* dikonsumsi wsApplyCdeps */
  /* headers.each = X: X diterapkan ke SETIAP proyek, diresolve relatif root
     tiap proyek (I. = <proyek>/include). Setara dengan menulis <proyek>.headers = X
     untuk setiap proyek, tanpa mengulang penulisan. */
  if (strcmp(key, "headers.each") == 0) {
    /* headers.each = [nama-proyek, ...] [I. | auto | path, ...]
       - Token yang cocok dengan nama proyek = PENYARING: hanya proyek itu yang
         menerima sisanya. Tanpa nama = semua proyek.
       - I. = <proyek>/include (-I), auto = I. bila include/ ada, path lain =
         dilacak relatif root proyek.
       - Token yang bukan nama proyek dan tidak ada sebagai folder di proyek mana
         pun diberi peringatan (salah tulis tidak lolos diam-diam). */
    char list[WS_LINE_LEN];
    if (strlen(value) >= sizeof(list)) return;
    snprintf(list, sizeof(list), "%s", value);
    bool hasNames = false, applies = false;
    char others[WS_LINE_LEN] = "";
    char *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
      while (*t == ' ' || *t == '\t') t++;
      size_t tl = strlen(t);
      while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) t[--tl] = '\0';
      if (tl == 0) continue;
      bool isName = false;
      for (int k = 0; k < m->projectCount; k++)
        if (strcmp(m->projects[k].name, t) == 0) { isName = true; break; }
      if (isName) {
        hasNames = true;
        if (strcmp(p->name, t) == 0) applies = true;
        continue;
      }
      /* bukan nama proyek: harus berupa flag (I./auto) atau folder yang ada */
      if (strcmp(t, "I.") != 0 && strcmp(t, "auto") != 0) {
        bool found = false;
        for (int k = 0; k < m->projectCount && !found; k++) {
          const char *r = m->projects[k].root[0] ? m->projects[k].root : ".";
          char probe[WS_PATH_LEN];
          if ((size_t)snprintf(probe, sizeof(probe), "%s/%s", r, t) < sizeof(probe) &&
              fsDirExists(probe))
            found = true;
        }
        /* peringatan dicetak sekali per nama (baris ini dipanggil per proyek/pass) */
        static char warnedTok[32][WS_LINE_LEN];
        static int nwarned = 0;
        bool already = false;
        for (int w = 0; w < nwarned; w++)
          if (strcmp(warnedTok[w], t) == 0) already = true;
        if (!found && !already && nwarned < 32) {
          snprintf(warnedTok[nwarned++], WS_LINE_LEN, "%s", t);
          fprintf(stderr, "satria: workspace: headers.each: '%s' bukan nama proyek dan tidak ada "
                          "sebagai folder di proyek mana pun, dilewati\n", t);
        }
      }
      if (strlen(others) + strlen(t) + 4 < sizeof(others)) {
        if (others[0]) strcat(others, ", ");
        strcat(others, t);
      }
    }
    if (hasNames && !applies) return;
    if (others[0] == '\0') return;
    /* auto: I. hanya bila include/ proyek ada */
    char finalv[WS_LINE_LEN] = "";
    char tmp[WS_LINE_LEN];
    snprintf(tmp, sizeof(tmp), "%s", others);
    char *s2 = NULL;
    for (char *t = strtok_r(tmp, ",", &s2); t; t = strtok_r(NULL, ",", &s2)) {
      while (*t == ' ') t++;
      if (strcmp(t, "auto") == 0) {
        char inc[WS_PATH_LEN];
        const char *r = p->root[0] ? p->root : ".";
        if ((size_t)snprintf(inc, sizeof(inc), "%s/include", r) >= sizeof(inc)) continue;
        if (!fsDirExists(inc)) continue;
        t = (char *)"I.";
      }
      if (strlen(finalv) + strlen(t) + 4 < sizeof(finalv)) {
        if (finalv[0]) strcat(finalv, ", ");
        strcat(finalv, t);
      }
    }
    if (finalv[0]) wsApplyLine(m, p, c, "headers", finalv, false);
    return;
  }
  /* headers.internal = folder shared di root workspace -> publik lewat
     `headers` supaya ikut -I (kriteria: shared headers di-root ditambahkan
     ke -I). Value relatif root workspace — tanpa prefix. */
  if (strcmp(key, "headers.internal") == 0) {
    configApplyLine(c, "headers.internal", value); /* dilacak, bukan -I */
    return;
  }

  const char *root = p->root[0] ? p->root : ".";

  if (strncmp(key, "sources", 7) == 0 && (key[7] == '\0' || key[7] == '.')) {
    if (wsMapList(root, value, false, mapped, sizeof(mapped))) configApplyLine(c, key, mapped);
    return;
  }
  if (strncmp(key, "headers", 7) == 0 && (key[7] == '\0' || key[7] == '.')) {
    char pub[WS_LINE_LEN], intl[WS_LINE_LEN];
    bool split = key[7] == '\0';
    if (wsMapHeaders(root, value, global, p->name, split, pub, intl, sizeof(pub))) {
      if (pub[0]) configApplyLine(c, key, pub);
      if (intl[0]) configApplyLine(c, "headers.internal", intl);
    }
    return;
  }
  if (strcmp(key, "archive.src") == 0) {
    if (wsIsRelPath(value)) {
      if (strcmp(value, ".") == 0)
        snprintf(mapped, sizeof(mapped), "%s", root);
      else if ((size_t)snprintf(mapped, sizeof(mapped), "%s/%s", root, value) >= sizeof(mapped))
        return;
      configApplyLine(c, key, mapped);
      return;
    }
    configApplyLine(c, key, value);
    return;
  }
  if (strncmp(key, "embedded.", 9) == 0) {
    /* embedded.<n>.src = konten proyek (prefix root proyek); .file/.dir/
       .extract dst lintas-proyek (apa adanya, relatif root workspace). */
    size_t kl = strlen(key);
    if (kl >= 4 && strcmp(key + kl - 4, ".src") == 0 && wsIsRelPath(value)) {
      if (strcmp(value, ".") == 0)
        snprintf(mapped, sizeof(mapped), "%s", root);
      else if ((size_t)snprintf(mapped, sizeof(mapped), "%s/%s", root, value) >= sizeof(mapped))
        return;
      configApplyLine(c, key, mapped);
      return;
    }
    configApplyLine(c, key, value);
    return;
  }
  if (strncmp(key, "library", 7) == 0 && (key[7] == '\0' || key[7] == '.')) {
    /* library.<os> = <nama proyek> -> path artifact; item non-proyek
       (ssl, m, pthread, ...) lewat apa adanya. */
    if (wsLibraryValue(m, p, value, mapped, sizeof(mapped))) configApplyLine(c, key, mapped);
    return;
  }
  /* Lainnya (compiler/flags/defines/std/exclude/output./clean./progress/
     foreground/target/depends_on): diterapkan apa adanya. */
  configApplyLine(c, key, value);
}

/* cdeps -> `library = <dep>/<libDir>/lib<name>.a` + `headers = <dep>/<dir>`
   (keduanya relatif root workspace; tanpa ../ — tidak ada chdir). */
static void wsApplyCdeps(const WsModel *m, const WsProject *p, Config *c) {
  List deps = {0};
  bool found = false;
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
          fprintf(stderr, "satria: workspace: '%s' cdeps berisi dirinya sendiri\n", p->name);
        } else if (wsFindProject(m, item) < 0) {
          fprintf(stderr, "satria: workspace: '%s' cdeps unknown project '%s'\n", p->name, item);
        } else if (!wsListHas(&deps, item)) {
          listAdd(&deps, item);
        }
      }
      if (!comma) break;
      save = comma + 1;
    }
  }
  if (!found) {
    listFree(&deps);
    return;
  }
  for (int j = 0; j < deps.count; j++) {
    int di = wsFindProject(m, deps.items[j]);
    const WsProject *dep = &m->projects[di];
    char mapped[WS_LINE_LEN];
    if (wsLibraryValue(m, p, dep->name, mapped, sizeof(mapped)))
      configApplyLine(c, "library", mapped);
    List depHeaders = {0};
    wsProjectHeaderDirs(dep, &depHeaders);
    if (depHeaders.count > 0) {
      char joined[WS_LINE_LEN * 4];
      size_t off = 0;
      off += (size_t)snprintf(joined + off, sizeof(joined) - off, "%s", depHeaders.items[0]);
      for (int k = 1; k < depHeaders.count && off < sizeof(joined) - 2; k++)
        off += (size_t)snprintf(joined + off, sizeof(joined) - off, ", %s", depHeaders.items[k]);
      configApplyLine(c, "headers", joined);
    }
    listFree(&depHeaders);
  }
  listFree(&deps);
}

/* Susun Config proyek `idx` dari model Buildfile.ws — tanpa file sintesis,
   tanpa loadConfig. Urutan: defaults -> baris bersama -> baris proyek ->
   cdeps -> fixup output/konvensi -> configFinalize -> resolveCompiler
   (dipanggil pemanggil, wsRunProject). */
static bool wsBuildConfig(const WsModel *m, int idx, Config *c) {
  const WsProject *p = &m->projects[idx];
  if (!p->root[0]) return false;
  *c = configDefaults();

  for (int pass = 0; pass < 2; pass++) {
    const List *lines = pass == 0 ? &m->commonLines : &p->lines;
    for (int i = 0; i < lines->count; i++) {
      char buf[WS_LINE_LEN * 2];
      char key[WS_LINE_LEN];
      if (strlen(lines->items[i]) >= sizeof(buf)) continue;
      copyStr(buf, sizeof(buf), lines->items[i]);
      char *val = NULL;
      if (!wsSplitKV(buf, key, sizeof(key), &val)) continue;
      wsApplyLine(m, p, c, key, val, pass == 0);
    }
  }
  wsApplyCdeps(m, p, c);

  const char *root = p->root[0] ? p->root : ".";
  /* Output root-centric: object di build/<proyek> (agar nama source antar-
     proyek tidak bertabrakan); bin/ & lib/ sudah root lewat default. */
  if (strcmp(c->outBuildDir, "build") == 0)
    snprintf(c->outBuildDir, sizeof(c->outBuildDir), "build/%s", p->name);
  /* Nama binary default = nama proyek (bukan basename cwd = root ws). */
  if (!c->outBinaryName[0]) copyStr(c->outBinaryName, sizeof(c->outBinaryName), p->name);
  /* Konvensi sources/headers berbasis root PROYEK — configFinalize berjalan
     dengan cwd root workspace, jadi defaults disediakan dahulu. */
  if (c->sources.count == 0) {
    char defSrc[MAX_PATH * 2];
    if (strcmp(root, ".") == 0)
      snprintf(defSrc, sizeof(defSrc), "src");
    else
      snprintf(defSrc, sizeof(defSrc), "%s/src", root);
    listAdd(&c->sources, defSrc);
  }
  /* Workspace-root include/ is conventional, even with explicit headers. */
  if (fsDirExists("include")) {
    bool present = false;
    for (int i = 0; i < c->headerPublic.count; i++)
      if (strcmp(c->headerPublic.items[i], "include") == 0) present = true;
    if (!present) listAdd(&c->headerPublic, "include");
  }
  bool hdrDeclared = c->headerPublic.count > 0;
  if (!hdrDeclared) {
    char incDir[MAX_PATH * 2];
    if (strcmp(root, ".") == 0)
      snprintf(incDir, sizeof(incDir), "include");
    else
      snprintf(incDir, sizeof(incDir), "%s/include", root);
    if (fsDirExists(incDir)) listAdd(&c->headerPublic, incDir);
  }
  int hpBefore = c->headerPublic.count;

  configFinalize(c);

  /* configFinalize bisa menyuntik default "include" telanjang (berbasis
     cwd root workspace) bila proyek tak mendeklarasikan headers — buang;
     default sah hanya <root>/include di atas. */
  if (!hdrDeclared && c->headerPublic.count > hpBefore && c->headerPublic.count > 0 &&
      strcmp(c->headerPublic.items[0], "include") == 0) {
    memmove(c->headerPublic.items, c->headerPublic.items + 1,
            sizeof(char *) * (size_t)(c->headerPublic.count - 1));
    c->headerPublic.count--;
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
    fprintf(stderr, "satria: workspace: dependency cycle involving '%s'\n", m->projects[idx].name);
    return false;
  }
  visiting[idx] = 1;
  for (int i = 0; i < m->depends[idx].count; i++) {
    int dep = wsFindProject(m, m->depends[idx].items[i]);
    if (dep < 0) {
      fprintf(stderr, "satria: workspace: '%s' depends on unknown project '%s'\n",
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

/*
 * Jalankan SATU proyek dari root workspace — pengganti wsExecOne (yang
 * dulu chdir ke root proyek lalu memanggil cmdBuildEx; keduanya kini
 * dilarang design/workspace_v1.md):
 *   cmd "clean" -> Config in-memory (wsBuildConfig) + cmdsCleanRun;
 *   cmd "build" -> label pass -> fast-state -> wsBuildConfig ->
 *                  resolveCompiler -> cmdsBuildPassRun (library/binary).
 * State per proyek ditandai cmdsStateSetWsProject (.satria/workspace/<n>.*).
 */
static int wsRunProject(const WsModel *m, const char *name, const char *cmd, int jobs,
                        bool libOnly) {
  int idx = wsFindProject(m, name);
  if (idx < 0) return 1;
  const WsProject *p = &m->projects[idx];
  if (!p->root[0]) return 1;

  cmdsStateSetWsProject(name);

  if (strcmp(cmd, "clean") == 0) {
    Config c;
    int rc = wsBuildConfig(m, idx, &c) ? cmdsCleanRun(&c) : 1;
    cmdsStateSetWsProject("");
    return rc;
  }

  /* Label fase: proyek kemasan (output.binary = false) fase 1-nya ARSIP. */
  const char *passLabel = "binary";
  if (libOnly) passLabel = wsProjectArchiveOnly(p) ? "archive" : "library";
  printf("\n%s Project : %s\n", repPrefix(), name);
  printf("%s Fase    : %s\n", repPrefix(), passLabel);

  /* Fast no-op per fase — padanan bagian atas cmdBuildEx. Label META
     stabil per proyek (file sintesis sudah tidak ada; label lama berupa
     path .Buildfile gagal validasi -> rebuild penuh sekali saat migrasi). */
  char bfLabel[WS_NAME_LEN + 32];
  snprintf(bfLabel, sizeof(bfLabel), "Buildfile.ws:%s", name);
  if (wsFastStateValid(bfLabel, libOnly)) {
    wsLogFastOk[idx] = true;
    cmdsStateSetWsProject("");
    return 0;
  }

  Config c;
  if (!wsBuildConfig(m, idx, &c) || !resolveCompiler(&c)) {
    cmdsStateSetWsProject("");
    return 1;
  }
  procSetForeground(jobs > 1 ? false : c.foreground);

  /* pack.* diabaikan (v1): proyek kemasan tanpa arsip & tanpa binary =
     tidak ada target build — lewati, jangan gagalkan seluruh run. */
  if (!c.binary && c.embCount == 0) {
    printf("%s Skipped : tanpa target build (pack.* diabaikan)\n", repPrefix());
    cmdsStateSetWsProject("");
    return 0;
  }

  int rc = cmdsBuildPassRun(jobs, c, bfLabel, libOnly);
  if (rc != 0) cmdsStatMemoReset(); /* build gagal: output mungkin berubah sebagian */
  cmdsStateSetWsProject("");
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
    fprintf(stderr, "satria: %s not found\n", WORKSPACE_FILENAME);
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
    fprintf(stderr, "satria: workspace: no projects declared\n");
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
        fprintf(stderr, "satria: release: key '%s' tidak dikenal (padanan pack.*)\n", k);
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
          /* Library di root workspace: lib/lib<name>.a/.so (design v1). */
          char libEntry[WS_LINE_LEN * 2];
          const char *libName = ln + 21;
          int w1 = snprintf(libEntry, sizeof(libEntry), "lib/lib%s.a", libName);
          if (w1 > 0 && (size_t)w1 < sizeof(libEntry) && !listAdd(&c.pack.files, libEntry))
            goto oom;
          char shEntry[WS_LINE_LEN * 4];
          int w2 = snprintf(shEntry, sizeof(shEntry), "lib/lib%s.so", libName);
          if (w2 > 0 && (size_t)w2 < sizeof(shEntry) && !listAdd(&c.pack.files, shEntry)) goto oom;
        }
      }
      if (!isArchive) {
        /* Binary di root workspace: bin/<binaryName>; default = nama proyek
           (sama dengan wsBuildConfig). */
        const char *binName = NULL;
        for (int l = 0; l < pp->lines.count; l++) {
          const char *ln = pp->lines.items[l];
          if (strncmp(ln, "output.binaryName = ", 20) == 0) binName = ln + 20;
        }
        if (!binName) binName = pp->name;
        char binEntry[WS_LINE_LEN * 3];
        int w = snprintf(binEntry, sizeof(binEntry), "bin/%s", binName);
        if (w > 0 && (size_t)w < sizeof(binEntry) && !listAdd(&c.pack.files, binEntry)) goto oom;
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
    fprintf(stderr, "satria: cannot enter workspace root\n");
    packFreeConfig(&c.pack);
    return 1;
  }
  bool ok = packRunAt(&c);
  packFreeConfig(&c.pack);
  if (!ok) fprintf(stderr, "satria: workspace: release gagal\n");
  return ok ? 0 : 1;

oom:
  fprintf(stderr, "satria: workspace: kehabisan memori (release)\n");
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
  (void)wsDir; /* proyek dieksekusi in-process dari root workspace */
  const char *pre = libOnly ? "ws-libpass-pre" : "ws-binpass-pre";
  const char *post = libOnly ? "ws-libpass-post" : "ws-binpass-post";
  for (int i = 0; i < n; i++) {
    profMark(pre);
    int rc = wsRunProject(m, batch[i].name, cmd, jobs, libOnly);
    profMark(post);
    if (rc != 0) return rc;
  }
  return 0;
}

#ifndef _WIN32
/* Paralel (POSIX): fork satu child per proyek — jobs dibagi rata (min 1).
   Output anak diarahkan ke file sementara .satria/ws-par-* agar laporan
   per-proyek tidak tercampur; parent mencetak per slot (urutan worklist)
   setelah semua anak batch selesai. Anak gagal -> rc non-nol -> batch
   berikutnya berhenti (seperti serial). Untuk batch paralel penanda fase
   dicetak sekali per batch (bukan per anak). */
static int wsRunBatchFork(const WsModel *m, const WsWork *batch, int n, int jobs,
                          const char *cmd, const char *wsDir, bool libOnly) {
  (void)wsDir; /* proyek dieksekusi in-process dari root workspace */
  const char *pre = libOnly ? "ws-libpass-pre" : "ws-binpass-pre";
  const char *post = libOnly ? "ws-libpass-post" : "ws-binpass-post";
  mkdirs(".satria");
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
    snprintf(tmpl[i], 32, ".satria/ws-par-XXXXXX");
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
      int crc = wsRunProject(m, batch[i].name, cmd, pj, libOnly);
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
      int crc = wsRunProject(m, batch[i].name, cmd, jobs, libOnly);
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
   hanya opt-in via SATRIA_WS_PAR=1: pengukuran di rupaws (proot, -j4) menunjukkan
   paralel anak ~netral pada inkremental kecil dan ~48% LEBIH LAMBAT pada warm
   build besar (213 compile: 310.8s paralel vs 208.8s serial) — pembagian jobs
   antar anak menurunkan paralelisme dalam-proyek dan proot/contended gcc tidak
   diuntungkan. Keuntungan nyata butuh scheduler global lintas proyek (satu pool
   jobs untuk semua compile), bukan fork per anak. Jalur paralel dipertahankan
   untuk lingkungan native yang bisa diuntungkan — ukur dulu sebelum memakai. */
static int wsRunBatch(const WsModel *m, const WsWork *batch, int n, int jobs, const char *cmd,
                      const char *wsDir, bool libOnly) {
  (void)wsDir; /* proyek dieksekusi in-process dari root workspace */
  if (n <= 0) return 0;
#ifndef _WIN32
  if (jobs > 1 && n > 1 && getenv("SATRIA_WS_PAR"))
    return wsRunBatchFork(m, batch, n, jobs, cmd, wsDir, libOnly);
#endif
  return wsRunBatchSerial(m, batch, n, jobs, cmd, wsDir, libOnly);
}

/* ==================== workspace_log + CompDB merged ==================== */

/* Indeks no-op & CompDB (design/workspace_v1.md §No-op dan CompDB): satu
   blok per proyek — (2) hash konfigurasi Buildfile.ws, (3) fingerprint
   daftar source dari snapshot <n>.src, (4) hash isi compile_commands.json
   terakhir (sama untuk seluruh workspace). Hanya dua keputusan: (a) proyek
   fast-hit + (2)+(3) identik -> daftar source dimuat dari snapshot tanpa
   re-scan; (b) (4) identik + file ada -> CompDB tidak ditulis ulang. */
#define WS_LOG_FILE ".satria/workspace_log"

static uint64_t wsFnv1a(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t wsFnv1aStr(uint64_t h, const char *s) {
  h = wsFnv1a(h, s, strlen(s));
  const unsigned char sep = 0xff;
  return wsFnv1a(h, &sep, 1);
}

/* Hash konfigurasi proyek (field 2): baris bersama + root override + baris
   proyek — semua baris Buildfile.ws yang memengaruhinya. */
static uint64_t wsLogCfgHash(const WsModel *m, int idx) {
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < m->commonLines.count; i++)
    h = wsFnv1aStr(h, m->commonLines.items[i]);
  h = wsFnv1aStr(h, m->projects[idx].root);
  for (int i = 0; i < m->projects[idx].lines.count; i++)
    h = wsFnv1aStr(h, m->projects[idx].lines.items[i]);
  return h;
}

/* Hash isi file — 0 bila tak bisa dibaca/kosong. */
static uint64_t wsLogFileHash(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  uint64_t h = 1469598103934665603ULL;
  char buf[4096];
  size_t n;
  bool any = false;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    h = wsFnv1a(h, buf, n);
    any = true;
  }
  fclose(f);
  return any ? h : 0;
}

static void wsLogSnapshotPath(char *out, size_t n, const char *name) {
  snprintf(out, n, ".satria/workspace/%s.src", name);
}

/* Baca entry log utk `name` (field 2/3/4). false bila log tak ada/tak cocok. */
static bool wsLogReadEntry(const char *name, uint64_t *cfg, uint64_t *src, uint64_t *cdb) {
  FILE *f = fopen(WS_LOG_FILE, "r");
  if (!f) return false;
  char line[512];
  bool found = false;
  if (fgets(line, sizeof(line), f) && strncmp(line, "SATRIAWLOG1 ", 10) == 0) {
    while (fgets(line, sizeof(line), f)) {
      char pname[WS_NAME_LEN];
      unsigned long long a = 0, b = 0, c = 0;
      if (sscanf(line, "P %63s %llx %llx %llx", pname, &a, &b, &c) == 4 &&
          strcmp(pname, name) == 0) {
        *cfg = (uint64_t)a;
        *src = (uint64_t)b;
        *cdb = (uint64_t)c;
        found = true;
        break;
      }
    }
  }
  fclose(f);
  return found;
}

/* Fingerprint compdb tercatat (field 4 entri mana pun — sama antarproyek);
   0 bila log tidak ada / tak berisi entri. */
static uint64_t wsLogCompdbFp(void) {
  FILE *f = fopen(WS_LOG_FILE, "r");
  if (!f) return 0;
  char line[512];
  uint64_t fp = 0;
  while (fgets(line, sizeof(line), f)) {
    char pname[WS_NAME_LEN];
    unsigned long long a = 0, b = 0, c = 0;
    if (sscanf(line, "P %63s %llx %llx %llx", pname, &a, &b, &c) == 4) {
      fp = (uint64_t)c;
      break;
    }
  }
  fclose(f);
  return fp;
}

/* Muat daftar source dari snapshot <n>.src TANPA re-scan (baris "S <path>").
   Pemanggil hanya memanggil bila fast-state proyek valid DAN hash snapshot
   cocok dengan log — source set dijamin tak berubah. */
static bool wsLogLoadSrcs(const char *name, List *out) {
  char path[WS_PATH_LEN];
  wsLogSnapshotPath(path, sizeof(path), name);
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[WS_PATH_LEN + 32];
  bool ok = fgets(line, sizeof(line), f) != NULL && strncmp(line, "SATRIAWSLIST1 ", 12) == 0;
  while (ok && fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = '\0';
    if (strncmp(line, "S ", 2) == 0)
      ok = listAdd(out, line + 2);
    else if (line[0])
      ok = false;
  }
  fclose(f);
  if (!ok || out->count == 0) {
    listFree(out);
    return false;
  }
  return true;
}

/* Tulis ulang workspace_log penuh (tmp + rename). */
static void wsLogWrite(const WsModel *m, const uint64_t *cfgH, const uint64_t *srcH,
                       uint64_t cdb) {
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  mkdirs(".satria");
  FILE *f = fopen(WS_LOG_FILE ".tmp", "w");
  if (!f) return;
  fprintf(f, "SATRIAWLOG1 %s\n", cwd);
  for (int i = 0; i < m->projectCount; i++)
    fprintf(f, "P %s %016llx %016llx %016llx\n", m->projects[i].name,
            (unsigned long long)cfgH[i], (unsigned long long)srcH[i], (unsigned long long)cdb);
  if (fclose(f) != 0) {
    fsRemoveFile(WS_LOG_FILE ".tmp");
    return;
  }
#ifdef _WIN32
  fsRemoveFile(WS_LOG_FILE);
#endif
  if (rename(WS_LOG_FILE ".tmp", WS_LOG_FILE) != 0) fsRemoveFile(WS_LOG_FILE ".tmp");
}

/*
 * wsCompdbRefresh — CompDB root merged SELURUH workspace + workspace_log:
 *   1. Susun Config + daftar source tiap proyek (urutan topo). Proyek yang
 *      fast-hit kali ini DAN cocok log (field 2+3) memuat source dari
 *      snapshot <n>.src tanpa re-scan — keputusan (a).
 *   2. Render/gabung ke SATU compile_commands.json di root; skip-write bila
 *      fingerprint gabungan == field 4 log — keputusan (b); backstop cache/
 *      blob di writeCompdbMulti.
 *   3. Tulis ulang workspace_log.
 * Tidak menyentuh build state apa pun — aman untuk `-g compdb` query-only.
 * Return false bila gagal (pesan sudah dicetak).
 */
static bool wsCompdbRefresh(const WsModel *m) {
  int n = m->projectCount;
  Config *cfgs = (Config *)calloc((size_t)n, sizeof(Config));
  List *srcs = (List *)calloc((size_t)n, sizeof(List));
  uint64_t *cfgH = (uint64_t *)calloc((size_t)n, sizeof(uint64_t));
  uint64_t *srcH = (uint64_t *)calloc((size_t)n, sizeof(uint64_t));
  const Config **cps = (const Config **)calloc((size_t)n, sizeof(Config *));
  List **sps = (List **)calloc((size_t)n, sizeof(List *));
  if (!cfgs || !srcs || !cfgH || !srcH || !cps || !sps) {
    free(cfgs);
    free(srcs);
    free(cfgH);
    free(srcH);
    free(cps);
    free(sps);
    return false;
  }

  bool ok = true;
  int cnt = 0;
  for (int i = 0; i < n && ok; i++) {
    cfgH[i] = wsLogCfgHash(m, i);
    if (!wsBuildConfig(m, i, &cfgs[i]) || !resolveCompiler(&cfgs[i])) {
      fprintf(stderr, "satria: workspace: gagal menyusun config '%s' (compdb)\n",
              m->projects[i].name);
      ok = false;
      break;
    }
    /* CompDB hanya mewakili proyek yang mengompilasi source; proyek
       kemasan (output.binary = false) & compdb=false tidak berkontribusi. */
    if (!cfgs[i].binary || !compdbEnabled(&cfgs[i]) || cfgs[i].sources.count == 0) continue;

    /* Keputusan (a): fast-hit + config cocok + snapshot tak berubah sejak
       log ditulis -> muat daftar dari snapshot, tanpa walk. */
    uint64_t logCfg = 0, logSrc = 0, logCdb = 0;
    char snap[WS_PATH_LEN];
    wsLogSnapshotPath(snap, sizeof(snap), m->projects[i].name);
    bool reused = false;
    if (wsLogFastOk[i] && wsLogReadEntry(m->projects[i].name, &logCfg, &logSrc, &logCdb) &&
        logCfg == cfgH[i] && logSrc != 0 && logSrc == wsLogFileHash(snap))
      reused = wsLogLoadSrcs(m->projects[i].name, &srcs[i]);
    if (!reused) cmdsCollectSources(&cfgs[i], &srcs[i]);
    srcH[i] = wsLogFileHash(snap); /* field 3: fingerprint snapshot (0 bila belum ada) */

    if (srcs[i].count > 0) {
      cps[cnt] = &cfgs[i];
      sps[cnt] = &srcs[i];
      cnt++;
    }
  }

  uint64_t fp = 0;
  if (ok && cnt > 0) {
    fp = compdbMultiFingerprint(cps, sps, cnt);
    /* Keputusan (b): hash CompDB tercatat identik + file ada -> tidak
       ditulis ulang (tanpa render, tanpa sentuh disk). */
    if (fp && fsFileExists("compile_commands.json") && wsLogCompdbFp() == fp)
      printf("%s CompDB  : compile_commands.json unchanged (workspace)\n", repPrefix());
    else
      writeCompdbMulti(cps, sps, cnt);
  }

  if (ok) wsLogWrite(m, cfgH, srcH, fp);

  free(cfgs);
  free(srcs);
  free(cfgH);
  free(srcH);
  free(cps);
  free(sps);
  return ok;
}

int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel) {
  char wsDir[MAX_PATH];
  if (!fsGetCwd(wsDir, sizeof(wsDir))) {
    fprintf(stderr, "satria: cannot determine current directory\n");
    return 1;
  }
  WsModel m;
  const char *wsName = NULL; /* diisi wsModelLoad: nama file ws (pesan) */
  if (!wsModelLoad(wsDir, &m, &wsName)) return 1;
  if (!wsTopoSort(&m)) {
    wsModelFree(&m);
    return 1;
  }
  memset(wsLogFastOk, 0, sizeof(wsLogFastOk));

  /* `-g compdb` query-only: hasilkan CompDB merged SELURUH workspace +
     perbarui workspace_log, TANPA build dan TANPA mengubah build state
     (design/workspace_v1.md §No-op dan CompDB). `-w <nama>` tidak
     membatasi: compdb selalu mewakili seluruh workspace. */
  if (strcmp(cmd, "compdb") == 0) {
    wsModeSet(true);
    cmdsStatMemoSetCwd();
    bool cok = wsCompdbRefresh(&m);
    wsModeSet(false);
    wsModelFree(&m);
    return cok ? 0 : 1;
  }

  /* Dua mode (design/release.md revisi 2):
     - releaseSel : `-- key=value[,...]` — OVERRIDE setting release untuk
                    run ini (name, version, format, target->format; dsb).
                    Konfigurasi dasar tetap dari section release.*.
     - only       : `satria -w <proyek>` — build proyek + closure-nya;
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
        fprintf(stderr, "satria: workspace: override release tidak valid\n");
        wsModelFree(&m);
        return 1;
      }
      memcpy(pair, p, len);
      pair[len] = '\0';
      char *eq = strchr(pair, '=');
      if (!eq) {
        fprintf(stderr,
                "satria: workspace: override '%s' tidak valid (pakai key=value, mis. name=rupa)\n",
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
        fprintf(stderr, "satria: workspace: format '%s' tidak dikenal (tar, deb)\n", v);
        wsModelFree(&m);
        return 1;
      }
      if (strcmp(k, "name") == 0 && !wsValidName(v)) {
        fprintf(stderr, "satria: workspace: nama release '%s' tidak valid\n", v);
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

  /* Config per proyek disusun di memori tiap pass (wsBuildConfig) —
     tidak ada file sintesis (design/workspace_v1.md §Pemisahan Jalur). */
  profMark("workspace-parse");

  unsigned char needed[WS_MAX_PROJECTS] = {0};
  if (releaseOnly || (only && *only)) {
    if (only && *only) {
      int target = wsFindProject(&m, only);
      if (target < 0) {
        fprintf(stderr, "satria: workspace: unknown project '%s'\n", only);
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
    /* .satria di direktori anggota = state lama: diabaikan + peringatan
       (design/workspace_v1.md: state hanya di root workspace). */
    for (int i = 0; i < m.projectCount; i++) {
      char mp[WS_PATH_LEN];
      snprintf(mp, sizeof(mp), "%s/%s/.satria", wsDir, m.projects[i].root);
      if (fsDirExists(mp))
        fprintf(stderr, "satria: workspace: %s/.satria diabaikan — state workspace di root (%s)\n",
                m.projects[i].root, WS_STATE_FILE);
    }
    wsStateValidFiltered(&m, needed);
    /* Status gerbang akurat (per closure yang dibangun), dihitung di sini setelah
       parse agar tidak mengubah urutan validasi build. */
    const char *rs = getenv("SATRIA_STATUS");
    if (rs && *rs && strcmp(rs, "0") != 0)
      fprintf(stderr, "satria status: mode=workspace gate=%s\n", wsGateStartValid ? "valid" : "cold");
  }

  /* Worklist sekali: proyek needed urut topo + level dependensi. */
  WsWork work[WS_MAX_PROJECTS];
  int wcount = wsBuildWorklist(&m, needed, work);

  if (strcmp(cmd, "build") == 0) {
    /* Mode ws aktif sekali per run: cache sumber (.satria/ws.src) dipakai
       lintas proyek — satu traversal per direktori (design §Pipeline). */
    wsModeSet(true);
    cmdsStatMemoSetCwd(); /* memo stat ber-key cwd root workspace */
    /* CompDB harus tersedia meskipun kompilasi/link gagal. */
    if (!wsCompdbRefresh(&m)) {
      wsModeSet(false);
      wsModelFree(&m);
      return 1;
    }
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
      rc = wsRunProject(&m, work[i].name, cmd, jobs, false);
  }
  if (strcmp(cmd, "build") == 0) wsModeSet(false);

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
     tulis .satria/workspace.state; clean -> buang agar build berikutnya cold. */
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
 * workspace.state (design/workspace_v1.md): stamp SEMUA input seluruh
 * workspace, dalam path absolut, diambil dari state yang sudah ditulis
 * build proyek:
 *   .satria/workspace/<n>.ws.state / <n>.ws.lib.state  (F/D/X per proyek)
 *   .satria/ws.src                                     (cache sumber di root)
 * Baris pertama: SATRIAWSG1 <wsDir absolut>. Lalu baris F/D/X seperti ws.state.
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

#define WS_GATE_MAGIC "SATRIAWSG1"

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

/* Folder .satria adalah state, bukan input: tidak pernah masuk stamp workspace. */
static bool wsGateIsState(const char *p) {
  return strstr(p, "/.satria") != NULL || strncmp(p, ".satria", 5) == 0;
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

/* Folder sumber & file yang ditelusuri (.satria/ws.src — SATU cache di root
   workspace untuk seluruh proyek): D dan S. Mtime D dari walk. Path di
   dalam file relatif root workspace -> baseDir. */
static void wsGateReadSrc(List *out, const char *baseDir, const char *file, const List *excl) {
  FILE *fp = fopen(file, "r");
  if (!fp) return;
  char line[WS_GATE_PATH * 2];
  if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
  while (fgets(line, sizeof(line), fp)) {
    char abs[WS_GATE_PATH], rel[WS_GATE_PATH];
    long long mt = 0;
    if (line[0] == 'D' && sscanf(line, "D %lld %" WS_SCAN "s", &mt, rel) == 2) {
      if (wsGateIsState(rel) || wsGateExcluded(excl, rel)) continue;
      wsPathJoin(abs, sizeof(abs), baseDir, rel);
      /* Mtime folder diambil SEKARANG (bukan dari catatan walk): folder bisa berubah
         setelah walk dalam run yang sama, dan gerbang harus memuat keadaan akhir. */
      int64_t cur = -1;
      long long cs = 0;
      if (fsStampNsSize(abs, &cur, &cs)) wsGateAdd(out, 'D', abs, (long long)cur, 0);
      else wsGateAdd(out, 'X', abs, 0, 0);
      (void)mt;
    } else if (line[0] == 'S' && sscanf(line, "S %" WS_SCAN "s", rel) == 1) {
      if (wsGateIsState(rel) || wsGateExcluded(excl, rel)) continue;
      wsPathJoin(abs, sizeof(abs), baseDir, rel);
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
    /* State proyek kini di root: .satria/workspace/<n>.ws[.lib].state — path
       DI DALAM file relatif root workspace (satu cwd), jadi base join =
       wsAbs, bukan root proyek. */
    char f1[WS_GATE_PATH * 2], f2[WS_GATE_PATH * 2];
    snprintf(f1, sizeof(f1), "%s/.satria/workspace/%s.ws.state", wsAbs, m->projects[i].name);
    snprintf(f2, sizeof(f2), "%s/.satria/workspace/%s.ws.lib.state", wsAbs, m->projects[i].name);
    wsGateReadState(&items, wsAbs, f1);
    wsGateReadState(&items, wsAbs, f2);
  }
  /* .satria/ws.src: satu cache sumber di root untuk seluruh proyek. */
  {
    List excl = {0};
    for (int i = 0; i < m->projectCount; i++) wsGateCollectExcludes(&m->projects[i], &excl);
    char srcFile[WS_GATE_PATH * 2];
    wsPathJoin(srcFile, sizeof(srcFile), wsAbs, ".satria/ws.src");
    wsGateReadSrc(&items, wsAbs, srcFile, &excl);
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
    mkdirs(".satria");
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

/* ==================== guard direktori anggota ======================== */

/* Batas naik ke parent saat mencari Buildfile.ws induk — jauh melebihi
   kedalaman workspace wajar, tetapi tetap terbatas supaya tidak pernah
   menembus ke root filesystem. */
#define WS_MEMBER_MAX_UP 16

/* Normalisasi path ("<ws>/a/../b/./c" -> "<ws>/b/c") secara leksikal atas
   string — cukup karena semua kandidat path dibangun dari fsGetCwd (sudah
   dinormalkan OS) + penyusunan string, tanpa resolusi symlink. */
static void wsPathCanon(char *s) {
  bool abs = s[0] == '/';
  const char *seg[128];
  int top = 0;
  bool overflow = false;
  char *p = s + (abs ? 1 : 0);
  while (*p && !overflow) {
    char *start = p;
    while (*p && *p != '/') p++;
    if (*p) *p++ = '\0';
    if (strcmp(start, ".") == 0) continue;
    if (strcmp(start, "..") == 0) {
      if (top > 0 && strcmp(seg[top - 1], "..") != 0)
        top--; /* pop segmen biasa */
      else if (abs)
        continue; /* ".." di atas root filesystem: buang */
      else if (top < 128)
        seg[top++] = start; /* path relatif: ".." dipertahankan */
      else
        overflow = true;
      continue;
    }
    if (!*start) continue;
    if (top < 128)
      seg[top++] = start;
    else
      overflow = true;
  }
  if (overflow) return; /* path absurd — biarkan apa adanya */

  char *w = s;
  if (abs) *w++ = '/';
  for (int i = 0; i < top; i++) {
    size_t l = strlen(seg[i]);
    if (i) *w++ = '/';
    memmove(w, seg[i], l); /* selalu mundur: aman in-place */
    w += l;
  }
  if (w == s) *w++ = '.';
  *w = '\0';
}

/*
 * wsMemberCwdDetect — true bila cwd berada DI DALAM direktori proyek anggota
 * sebuah workspace INDUK (Buildfile.ws di salah satu parent). Keanggotaan
 * divalidasi lewat model Buildfile.ws induk (parse daftar projects + root) —
 * keberadaan file workspace di parent saja TIDAK cukup (design/
 * workspace_v1.md aturan 1 & 5); proyek standalone di luar workspace tidak
 * terpengaruh (jalur single-project tidak berubah).
 *
 * Bila true: wsRootOut = root workspace induk (absolut), memberOut = nama
 * proyek anggota sesuai deklarasi. Cwd pada root workspace sendiri selalu
 * false — jalur mode workspace (workspaceRun) yang menanganinya.
 */
bool wsMemberCwdDetect(char *wsRootOut, size_t wsRootN, char *memberOut, size_t memberN) {
  if (workspaceFileName()) return false; /* cwd = root workspace sendiri */
  char cwd[WS_PATH_LEN];
  if (!fsGetCwd(cwd, sizeof(cwd))) return false;
  wsPathCanon(cwd);

  char probe[WS_PATH_LEN];
  copyStr(probe, sizeof(probe), cwd);
  for (int up = 0; up < WS_MEMBER_MAX_UP; up++) {
    char *slash = strrchr(probe, '/');
    if (!slash || slash == probe) break; /* mencapai root filesystem */
    *slash = '\0';
    if (!probe[0]) break;

    /* Kandidat induk: Buildfile.ws (atau nama lama) di parent. */
    char wsf[WS_PATH_LEN + 40];
    const char *wsName = NULL;
    snprintf(wsf, sizeof(wsf), "%s/%s", probe, WORKSPACE_FILENAME);
    if (fsFileExists(wsf))
      wsName = WORKSPACE_FILENAME;
    else {
      snprintf(wsf, sizeof(wsf), "%s/%s", probe, WORKSPACE_FILENAME_LEGACY);
      if (fsFileExists(wsf)) wsName = WORKSPACE_FILENAME_LEGACY;
    }
    if (!wsName) continue;

    /* Validasi keanggotaan lewat model — parse TENANG; file rusak/gagal
       parse = bukan workspace valid, lanjut naik ke parent berikutnya. */
    WsModel m;
    wsGuardQuiet = true;
    bool parsed = wsParse(&m, wsf);
    wsGuardQuiet = false;
    if (!parsed) {
      wsModelFree(&m);
      continue;
    }

    for (int i = 0; i < m.projectCount; i++) {
      const char *root = m.projects[i].root[0] ? m.projects[i].root : m.projects[i].name;
      /* Buffer muat worst case "probe/root" tanpa potong (bukti statis
         untuk snprintf — tanpa warning format-truncation). */
      char mem[WS_PATH_LEN + MAX_PATH + 4];
#ifdef _WIN32
      bool absRoot = root[0] == '/' || (root[0] && root[1] == ':');
#else
      bool absRoot = root[0] == '/';
#endif
      if (absRoot)
        snprintf(mem, sizeof(mem), "%s", root);
      else
        snprintf(mem, sizeof(mem), "%s/%s", probe, root);
      wsPathCanon(mem);
      size_t ml = strlen(mem);
      /* cwd == root anggota, atau di bawahnya (boundary '/'). */
      bool hit = strcmp(cwd, mem) == 0 || (strncmp(cwd, mem, ml) == 0 && cwd[ml] == '/');
      if (hit) {
        copyStr(wsRootOut, wsRootN, probe);
        copyStr(memberOut, memberN, m.projects[i].name);
        wsModelFree(&m);
        return true;
      }
    }
    wsModelFree(&m);
  }
  return false;
}

/*
 * workspaceEnumerate — parse & validasi model workspace untuk enumerasi
 * (dipakai `satria profile workspace=`): semua validasi model yang sama
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
