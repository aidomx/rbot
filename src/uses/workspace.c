#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../commands.h"
#include "../config.h"
#include "../embed.h"
#include "../portability.h"
#include "../prof/prof.h"
#include "alias.h"

/*
 * workspace — implementasi mode `use workspace` (Buildfile.workspace).
 *
 * Workspace adalah LAYER TIPIS di atas jalur build satu project yang sudah
 * ada: file workspace di-parse untuk daftar proyek + setting per proyek +
 * depends_on; tiap proyek disintesis jadi satu Buildfile (disimpan STABIL
 * di .rbot/workspace/<nama>.Buildfile agar cache config & fast path tidak
 * miss tiap run), lalu dieksekusi lewat cmdBuild/cmdClean dengan cwd = root
 * proyeknya. FastState, fingerprint, library, compdb — semuanya ikut
 * otomatis tanpa diubah.
 *
 * Rute key di Buildfile.workspace:
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
 * Path relatif terhadap lokasi Buildfile.workspace (aturan rbot yang sudah
 * ada) — berarti relatif root workspace. Urutan build dari depends_on
 * (topological sort), bukan urutan `projects =`.
 */

#define WS_NAME_LEN 64
#define WS_MAX_PROJECTS 16
#define WS_LINE_LEN 1024

typedef struct {
  char name[WS_NAME_LEN];
  char root[MAX_PATH];
  List lines; /* baris setting per proyek TANPA prefix projects.<nama>. */
} WsProject;

typedef struct {
  WsProject projects[WS_MAX_PROJECTS];
  int projectCount;
  List commonLines; /* key selain projects/projects.<nama> — default bersama */
  List order;       /* urutan build hasil topo sort (nama) */
  List depends[WS_MAX_PROJECTS];
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
                                NULL};
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

/* ==================== parse Buildfile.workspace ==================== */

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

/* Parse Buildfile.workspace di cwd. Alias di-resolve dengan mesin alias
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

static bool wsSynthesize(const WsModel *m, const char *outDir) {
  mkdirs(outDir);
  for (int i = 0; i < m->projectCount; i++) {
    const WsProject *p = &m->projects[i];

    /* Bandingkan isi lama vs baru: hanya tulis ulang bila berubah agar
       mtime stabil (cache config & fast path tidak miss sia-sia). */
    size_t n = 256;
    for (int j = 0; j < m->commonLines.count; j++) n += strlen(m->commonLines.items[j]) + 1;
    for (int j = 0; j < p->lines.count; j++) n += strlen(p->lines.items[j]) + 1;
    char *body = malloc(n);
    if (!body) return false;
    body[0] = '\0';

    char hdr[192];
    snprintf(hdr, sizeof(hdr),
             "/* Generated by rbot from Buildfile.workspace — project '%s'. Do not edit. */\n",
             p->name);
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
    for (int j = 0; j < p->lines.count; j++) {
      strcat(body, p->lines.items[j]);
      strcat(body, "\n");
    }

    /* outDir (2*MAX_PATH) + WS_NAME_LEN + "/"+".Buildfile" — tak pernah
       terpotong (nama proyek maks. WS_NAME_LEN-1). */
    char path[MAX_PATH * 2 + WS_NAME_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s.Buildfile", outDir, p->name);
    bool same = false;
    FILE *rf = fopen(path, "rb");
    if (rf) {
      char old[16384];
      size_t got = fread(old, 1, sizeof(old), rf);
      fclose(rf);
      same = (got == strlen(body)) && memcmp(old, body, got) == 0;
    }
    if (!same) {
      FILE *wf = fopen(path, "w");
      if (!wf) {
        fprintf(stderr, "rbot: cannot write %s\n", path);
        free(body);
        return false;
      }
      fputs(body, wf);
      fclose(wf);
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
                     const char *wsDir) {
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
  printf("\n> Project   : %s\n", name);
  int rc = cmdBuild(jobs, bf);
  fsSetCwd(wsDir);
  return rc;
}

int workspaceRun(const char *cmd, int jobs, const char *only) {
  char wsDir[MAX_PATH];
  if (!fsGetCwd(wsDir, sizeof(wsDir))) {
    fprintf(stderr, "rbot: cannot determine current directory\n");
    return 1;
  }
  char wsFile[MAX_PATH * 2];
  snprintf(wsFile, sizeof(wsFile), "%s/%s", wsDir, WORKSPACE_FILENAME);
  if (!fsFileExists(wsFile)) {
    fprintf(stderr, "rbot: %s not found\n", WORKSPACE_FILENAME);
    return 1;
  }

  WsModel m;
  if (!wsParse(&m, wsFile)) return 1;
  if (m.projectCount == 0) {
    fprintf(stderr, "rbot: workspace: no projects declared\n");
    return 1;
  }
  if (!wsTopoSort(&m)) {
    for (int i = 0; i < m.projectCount; i++) listFree(&m.depends[i]);
    return 1;
  }

  char synthDir[MAX_PATH * 2];
  snprintf(synthDir, sizeof(synthDir), "%s/%s", wsDir, WORKSPACE_SYNTH_DIR);
  if (!wsSynthesize(&m, synthDir)) return 1;
  profMark("workspace-parse+synth");

  unsigned char needed[WS_MAX_PROJECTS] = {0};
  if (only && *only) {
    int target = wsFindProject(&m, only);
    if (target < 0) {
      fprintf(stderr, "rbot: workspace: unknown project '%s'\n", only);
      for (int i = 0; i < m.projectCount; i++) listFree(&m.depends[i]);
      listFree(&m.order);
      for (int i = 0; i < m.projectCount; i++) listFree(&m.projects[i].lines);
      listFree(&m.commonLines);
      return 1;
    }
    unsigned char visiting[WS_MAX_PROJECTS] = {0};
    if (!wsMarkNeeded(&m, target, needed, visiting)) {
      for (int i = 0; i < m.projectCount; i++) listFree(&m.depends[i]);
      listFree(&m.order);
      for (int i = 0; i < m.projectCount; i++) listFree(&m.projects[i].lines);
      listFree(&m.commonLines);
      return 1;
    }
  } else {
    for (int i = 0; i < m.projectCount; i++) needed[i] = 1;
  }

  int rc = 0;
  for (int i = 0; i < m.order.count; i++) {
    int idx = wsFindProject(&m, m.order.items[i]);
    if (idx < 0 || !needed[idx]) continue;
    int one = wsExecOne(&m, m.order.items[i], cmd, jobs, wsDir);
    if (one != 0) {
      rc = one;
      if (strcmp(cmd, "build") == 0) break; /* gagal: hentikan rantai */
    }
  }

  for (int i = 0; i < m.projectCount; i++) listFree(&m.depends[i]);
  listFree(&m.order);
  for (int i = 0; i < m.projectCount; i++) listFree(&m.projects[i].lines);
  listFree(&m.commonLines);
  return rc;
}

bool workspaceFileExists(void) { return fsFileExists(WORKSPACE_FILENAME); }
