#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmdhelp.h"
#include "cmds/cmds_internal.h"
#include "compdb.h"
#include "compile.h"
#include "config.h"
#include "deps.h"
#include "embed.h"
#include "pack/pack.h"
#include "portability.h"
#include "prof/prof.h"
#include "uses/workspace.h"
#include "util.h"

/*
 * commands — command publik rbot (build/init/help; clean ada di
 * cmds/cmds_state.c). Fase build yang berdiri sendiri dipisah modular:
 *   - cmds/cmds_lib.c   : fase library, kompilasi paralel, header versi.
 *   - cmds/cmds_state.c : fingerprint build, fast no-op snapshot, clean.
 */

/* ==================== help (dari src/cmd.txt — sumber tunggal) ==================== */

void showHelp(void) { cmdHelpMain(); }

/* ==================== init ==================== */

int cmdInit(const char *buildfilePath) {
  const char *path = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  if (fsFileExists(path)) {
    printf("> %s already exists, nothing to do\n", path);
    return 0;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "rbot: cannot create %s\n", path);
    return 1;
  }

  /* Template minimal: proyek standar (src/ + include/ opsional) cukup
     `use project` — sisanya konvensi rbot (sources=src, headers=include
     bila ada, binary = nama folder). Proyek non-standar bebas menambah
     field eksplisit; lihat docs/guide/buildfile.md. */
  fputs("use project\n", fp);
  fclose(fp);

  printf("> Created   : %s\n", path);
  return 0;
}

/* ==================== init -w (template workspace) ==================== */

int cmdInitWorkspace(const char *buildfilePath) {
  const char *path = buildfilePath && *buildfilePath ? buildfilePath : WORKSPACE_FILENAME;
  if (fsFileExists(path)) {
    printf("> %s already exists, nothing to do\n", path);
    return 0;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "rbot: cannot create %s\n", path);
    return 1;
  }

  /* Template minimal workspace: cukup daftar proyek. Konvensi per proyek
     sama dengan proyek standar (folder <nama>/ berisi src/). */
  fputs("use workspace\n"
        "\n"
        "# Proyek standar: folder <nama>/ berisi src/ (include/ opsional).\n"
        "projects = app\n",
        fp);
  fclose(fp);

  printf("> Created   : %s\n", path);
  return 0;
}

/* ==================== init -p (interaktif) ==================== */

/* Satu baris jawaban; false bila stdin berakhir (EOF -> pembatalan).
   Prompt lengkap (termasuk simbol `|`) disiapkan askPrompt. */
static bool askLine(const char *prompt, char *buf, size_t n) {
  printf("%s", prompt);
  fflush(stdout);
  if (!fgets(buf, (int)n, stdin)) return false;
  size_t l = strlen(buf);
  while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
    buf[--l] = '\0';
  return true;
}

/* Nama proyek/folder: huruf, angka, '_' '-' (nama binary juga aman). */
static bool projectNameValid(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > 63) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

/* Nama acak rbot-xxxxxx: /dev/urandom (POSIX) atau rand() (Windows/fallback). */
static void randomProjectName(char *out, size_t n) {
  unsigned char rnd[4] = {0};
  FILE *f = fopen("/dev/urandom", "rb");
  if (f) {
    if (fread(rnd, 1, sizeof(rnd), f) != sizeof(rnd)) rnd[0] = 0;
    fclose(f);
  } else {
    static bool seeded = false;
    if (!seeded) {
      srand((unsigned)time(NULL));
      seeded = true;
    }
    for (size_t i = 0; i < sizeof(rnd); i++)
      rnd[i] = (unsigned char)(rand() & 0xFF);
  }
  unsigned v = ((unsigned)rnd[0] << 24) | ((unsigned)rnd[1] << 16) | ((unsigned)rnd[2] << 8) |
               (unsigned)rnd[3];
  snprintf(out, n, "rbot-%06x", v & 0xFFFFFFu);
}

/* Warna prompt hanya saat stdout terminal; NO_COLOR (standar no-color.org),
   RBOT_NO_COLOR, dan TERM=dumb mematikan. Di pipe/CI otomatis polos. */
static bool termColorWanted(void) {
  if (!termIsTTY()) return false;
  const char *no = getenv("NO_COLOR");
  if (no && *no) return false;
  no = getenv("RBOT_NO_COLOR");
  if (no && *no) return false;
  const char *term = getenv("TERM");
  if (term && strcmp(term, "dumb") == 0) return false;
  return true;
}

/* Prompt dengan simbol `▌` berwarna cyan bila warna aktif; polos selainnya. */
static bool askPrompt(const char *text, char *buf, size_t n) {
  char prompt[768];
  if (termColorWanted())
    snprintf(prompt, sizeof(prompt), "\033[36m▌\033[0m %s", text);
  else
    snprintf(prompt, sizeof(prompt), "▌ %s", text);
  return askLine(prompt, buf, n);
}

/* Pisah list koma, trim tiap item, gabung ulang dengan ", "; false bila
   hasil melebihi kapasitas. Item kosong dilewati; kosong total => dst[0]. */
static bool normalizeList(char *dst, size_t n, const char *src) {
  size_t w = 0;
  bool any = false;
  dst[0] = '\0';
  char buf[512];
  copyStr(buf, sizeof(buf), src);
  for (char *save = buf;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *item = trim(save);
    if (*item) {
      size_t l = strlen(item);
      if (w + l + 3 >= n) return false;
      if (any) {
        dst[w++] = ',';
        dst[w++] = ' ';
      }
      memcpy(dst + w, item, l + 1);
      w += l;
      any = true;
    }
    if (!comma) break;
    save = comma + 1;
  }
  return true;
}

static void upperName(char *dst, size_t n, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c == '-') c = '_'; /* guard macro tidak boleh berisi '-' */
    dst[i] = c;
  }
  dst[i] = '\0';
}

static bool writeNew(const char *path, const char *content) {
  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "rbot: cannot create %s\n", path);
    return false;
  }
  fputs(content, fp);
  fclose(fp);
  printf("> Created   : %s\n", path);
  return true;
}

int cmdInteractiveInit(void) {
  char line[512];
  char name[64];
  char stdv[64] = {0}, compv[192] = {0}, flagsv[512] = {0};
  bool wantInclude = false, wantWs = false, langCpp = false;

  printf("> rbot init interaktif - enter memakai default\n\n");

  if (!askPrompt("Project name? [enter for random name] ", line, sizeof(line))) goto eof;
  copyStr(name, sizeof(name), trim(line));
  if (!name[0]) randomProjectName(name, sizeof(name));
  if (!projectNameValid(name)) {
    fprintf(stderr,
            "rbot: nama proyek '%s' tidak valid - pakai huruf, angka, '_' atau '-' (maks 63)\n",
            name);
    return 2;
  }

  /* Bahasa: default c; cpp men-scaffold src/main.cpp. Konvensi default
     cpp (std c++17, linker C++) ada di configFinalize — proyek standar
     tak perlu menulis std/compiler eksplisit. */
  if (!askPrompt("language? [c, cpp] [enter = c] ", line, sizeof(line))) goto eof;
  {
    char *v = trim(line);
    langCpp = (v[0] | 32) == 'c' && (v[1] | 32) == 'p' && (v[2] | 32) == 'p' && v[3] == '\0';
  }

  /* Pertanyaan lanjutan: jawaban selain default ditulis eksplisit ke
     Buildfile; sisanya tetap mengandalkan konvensi (lihat configFinalize). */
  if (!askPrompt(langCpp ? "std? [enter = default c++17] " : "std? [enter = default gnu11] ", line,
                 sizeof(line)))
    goto eof;
  if (!normalizeList(stdv, sizeof(stdv), line)) {
    fprintf(stderr, "rbot: nilai std terlalu panjang\n");
    return 2;
  }
  if (!askPrompt("compiler? [enter = auto-detect] ", line, sizeof(line))) goto eof;
  if (!normalizeList(compv, sizeof(compv), line)) {
    fprintf(stderr, "rbot: nilai compiler terlalu panjang\n");
    return 2;
  }
  if (!askPrompt("flags? [enter = default] ", line, sizeof(line))) goto eof;
  if (!normalizeList(flagsv, sizeof(flagsv), line)) {
    fprintf(stderr, "rbot: nilai flags terlalu panjang\n");
    return 2;
  }
  if (!askPrompt("Create include/? [y/N] ", line, sizeof(line))) goto eof;
  {
    char *v = trim(line);
    wantInclude = (v[0] == 'y' || v[0] == 'Y');
  }
  if (!askPrompt("Uses workspace? [y/N] ", line, sizeof(line))) goto eof;
  {
    char *v = trim(line);
    wantWs = (v[0] == 'y' || v[0] == 'Y');
  }

  /* Daftar proyek workspace: proyek utama + anggota tambahan. */
  char names[16][64];
  int nn = 0;
  char depA[64] = {0}, depB[64] = {0};
  copyStr(names[nn++], sizeof(names[0]), name);
  if (wantWs) {
    if (!askPrompt("Workspace projects? [enter = app,tool] ", line, sizeof(line))) goto eof;
    char memlist[512];
    if (!normalizeList(memlist, sizeof(memlist), line)) {
      fprintf(stderr, "rbot: daftar proyek terlalu panjang\n");
      return 2;
    }
    if (!memlist[0]) copyStr(memlist, sizeof(memlist), "app,tool");
    char buf[512];
    copyStr(buf, sizeof(buf), memlist);
    for (char *save = buf;;) {
      char *comma = strchr(save, ',');
      if (comma) *comma = '\0';
      char *item = trim(save);
      if (*item) {
        if (!projectNameValid(item)) {
          fprintf(stderr, "rbot: nama proyek '%s' tidak valid - pakai huruf, angka, '_' atau '-'\n",
                  item);
          return 2;
        }
        if (nn >= 16) {
          fprintf(stderr, "rbot: init -p mendukung maksimal 16 proyek workspace\n");
          return 2;
        }
        if (strcmp(item, name) != 0) /* proyek utama sudah di daftar */
          copyStr(names[nn++], sizeof(names[0]), item);
      }
      if (!comma) break;
      save = comma + 1;
    }

    if (!askPrompt("depends_on? (format: <proyek> depends <proyek>) [enter = none] ", line,
                   sizeof(line)))
      goto eof;
    {
      char *v = trim(line);
      if (v[0]) {
        if (sscanf(v, "%63s depends %63s", depA, depB) != 2) {
          fprintf(stderr,
                  "rbot: format depends_on '%s' tidak dipahami - contoh: tool depends app\n", v);
          return 2;
        }
        bool aOk = false, bOk = false;
        for (int i = 0; i < nn; i++) {
          if (strcmp(names[i], depA) == 0) aOk = true;
          if (strcmp(names[i], depB) == 0) bOk = true;
        }
        if (!aOk || !bOk) {
          fprintf(stderr,
                  "rbot: depends_on '%s depends %s': proyek tidak ada di daftar - "
                  "tambahkan dulu di Workspace projects\n",
                  depA, depB);
          return 2;
        }
      }
    }
  }

  /* Pra-cek SEMUA target file sebelum menulis apa pun: satu konflik pun
     membatalkan scaffold - tidak ada yang ditimpa, tidak ada parsial. */
  {
    const char *srcName = langCpp ? "main.cpp" : "main.c";
    char p[MAX_PATH * 2];
    char conflictPath[MAX_PATH * 2] = "";
    snprintf(p, sizeof(p), "%s/src/%s", name, srcName);
    if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
    if (!conflictPath[0] && wantInclude) {
      snprintf(p, sizeof(p), "%s/include/%s.h", name, name);
      if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
    }
    for (int i = 1; i < nn && !conflictPath[0]; i++) {
      snprintf(p, sizeof(p), "%s/src/%s", names[i], srcName);
      if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
    }
    if (!conflictPath[0]) {
      if (wantWs) {
        if (fsFileExists(WORKSPACE_FILENAME))
          copyStr(conflictPath, sizeof(conflictPath), WORKSPACE_FILENAME);
      } else {
        snprintf(p, sizeof(p), "%s/Buildfile", name);
        if (fsFileExists(p)) copyStr(conflictPath, sizeof(conflictPath), p);
      }
    }
    if (conflictPath[0]) {
      fprintf(stderr, "rbot: %s sudah ada - init dihentikan (tidak ada yang ditimpa)\n",
              conflictPath);
      return 1;
    }
  }

  /* Bahasa cpp: std eksplisit ke Buildfile hasil scaffold — konvensi
     deteksi .cpp di configFinalize tetap melayani proyek manual. */
  if (langCpp && !stdv[0]) copyStr(stdv, sizeof(stdv), "c++17");

  /* Isi file: Buildfile hanya berisi tambahan non-default di atas
     `use project` (sisanya konvensi); main.c siap build & jalan. */
  char body[2048], tmp[640];
  body[0] = '\0';
  strcat(body, "use project\n");
  if (stdv[0]) {
    snprintf(tmp, sizeof(tmp), "\nstd = %s\n", stdv);
    strcat(body, tmp);
  }
  if (compv[0]) {
    snprintf(tmp, sizeof(tmp), "\ncompiler = %s\n", compv);
    strcat(body, tmp);
  }
  if (flagsv[0]) {
    snprintf(tmp, sizeof(tmp), "\nflags = %s\n", flagsv);
    strcat(body, tmp);
  }

  char mainc[1024];
  if (langCpp) {
    if (wantInclude)
      snprintf(
          mainc, sizeof(mainc),
          "#include <iostream>\n\n#include \"%s.h\"\n\nvoid hello() {\n"
          "  std::cout << \"hello from %s\\n\";\n}\n\nint main() {\n  hello();\n  return 0;\n}\n",
          name, name);
    else
      snprintf(mainc, sizeof(mainc),
               "#include <iostream>\n\nint main() {\n  std::cout << \"hello from %s\\n\";\n"
               "  return 0;\n}\n",
               name);
  } else if (wantInclude)
    snprintf(mainc, sizeof(mainc),
             "#include <stdio.h>\n\n#include \"%s.h\"\n\nvoid hello(void) {\n"
             "  printf(\"hello from %s\\n\");\n}\n\nint main(void) {\n  hello();\n  return 0;\n}\n",
             name, name);
  else
    snprintf(mainc, sizeof(mainc),
             "#include <stdio.h>\n\nint main(void) {\n  printf(\"hello from %s\\n\");\n"
             "  return 0;\n}\n",
             name);

  char hdr[512];
  if (wantInclude) {
    char guard[80];
    upperName(guard, sizeof(guard), name);
    snprintf(hdr, sizeof(hdr), "#ifndef %s_H\n#define %s_H\n\n%s\n\n#endif\n", guard, guard,
             langCpp ? "void hello();" : "void hello(void);");
  }

  char ws[4096];
  if (wantWs) {
    ws[0] = '\0';
    strcat(ws, "use workspace\n\nprojects = ");
    for (int i = 0; i < nn; i++) {
      strcat(ws, names[i]);
      if (i + 1 < nn) strcat(ws, ", ");
    }
    strcat(ws, "\n");
    if (depA[0]) {
      snprintf(tmp, sizeof(tmp), "%s.depends_on = %s\n", depA, depB);
      strcat(ws, tmp);
    }
  }

  /* Scaffold. */
  printf("\n");
  {
    char psrc[MAX_PATH * 2], pb[MAX_PATH * 2], phdr[MAX_PATH * 2];
    mkdirs(name);
    snprintf(psrc, sizeof(psrc), "%s/src", name);
    mkdirs(psrc);
    if (wantInclude) {
      snprintf(phdr, sizeof(phdr), "%s/include", name);
      mkdirs(phdr);
    }

    if (!wantWs) {
      snprintf(pb, sizeof(pb), "%s/Buildfile", name);
      if (!writeNew(pb, body)) return 1;
    }
    snprintf(psrc, sizeof(psrc), "%s/src/%s", name, langCpp ? "main.cpp" : "main.c");
    if (!writeNew(psrc, mainc)) return 1;
    if (wantInclude) {
      snprintf(phdr, sizeof(phdr), "%s/include/%s.h", name, name);
      if (!writeNew(phdr, hdr)) return 1;
    }

    /* Anggota workspace: folder <nama>/ + src/main.c (tanpa Buildfile —
       proyek workspace memakai Buildfile hasil sintesis rbot). */
    for (int i = 1; i < nn; i++) {
      char msrc[MAX_PATH * 2], mdir[MAX_PATH * 2], mbody[512];
      mkdirs(names[i]);
      snprintf(mdir, sizeof(mdir), "%s/src", names[i]);
      mkdirs(mdir);
      snprintf(msrc, sizeof(msrc), "%s/src/%s", names[i], langCpp ? "main.cpp" : "main.c");
      snprintf(mbody, sizeof(mbody),
               langCpp
                   ? "#include <iostream>\n\nint main() {\n  std::cout << \"hello from %s\\n\";\n"
                     "  return 0;\n}\n"
                   : "#include <stdio.h>\n\nint main(void) {\n  printf(\"hello from %s\\n\");\n"
                     "  return 0;\n}\n",
               names[i]);
      if (!writeNew(msrc, mbody)) return 1;
    }

    if (wantWs) {
      if (!writeNew(WORKSPACE_FILENAME, ws)) return 1;
      printf("\n> Next      : rbot -w\n");
    } else {
      printf("\n> Next      : cd %s && rbot\n", name);
    }
  }
  return 0;

eof:
  fprintf(stderr, "\nrbot: init dibatalkan (EOF)\n");
  return 1;
}

/* ==================== compdb -g (query-only) ==================== */

int cmdCompdbGenerate(const char *buildfilePath) {
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  if (!resolveCompiler(&c)) return 1;

  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "rbot: cannot enter root '%s'\n", c.root);
    return 1;
  }

  /* Kemasan murni (output.binary = false) tidak punya compile step —
     tidak ada yang bisa direkam ke compile_commands.json. */
  if (!c.binary) {
    printf("> CompDB    : skipped (output.binary = false)\n");
    return 0;
  }
  if (!compdbEnabled(&c)) {
    printf("> CompDB    : disabled (output.compileCommands = false)\n");
    return 0;
  }

  List srcs = {0};
  listReserve(&srcs, 1024);
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    walkDir(entry, ".c", &srcs);
    walkDir(entry, ".cpp", &srcs);
  }
  listSort(&srcs); /* urutan entri json = urutan link (reprodusible) */

  if (srcs.count == 0) {
    fprintf(stderr, "rbot: tidak ada source .c/.cpp di sources — tidak ada compdb untuk dibuat\n");
    return 1;
  }

  /* Cache hit / restore blob / render — mesin yang sama dengan jalur
     build (writeCompdb), tanpa decide/compile/link/fast-state. */
  writeCompdb(&c, &srcs);
  listFree(&srcs);
  return 0;
}

/* ==================== build ==================== */

/* Build penuh satu proyek — jalur rbot satu-proyek & fase binary workspace. */
int cmdBuild(int jobs, const char *buildfilePath) {
  return cmdBuildEx(jobs, buildfilePath, false);
}

/* ==================== [OPT] NaiveStatPlan (satu stat / file) ====================
 *
 * Audit bench compdb menunjukkan biaya dominan fase decide adalah stat
 * per file (mtime-ns) di lingkungan proot — bukan render compdb. Plan ini
 * mengumpulkan hasil stat SEKALI di awal (satu fsStampNsSize per file)
 * supaya fase lain tinggal memakai hasilnya tanpa men-stat ulang.
 *
 * Kontrol via env (tanpa env = perilaku lama 100%, plan hanya diisi):
 *   RBOT_STATPLAN_OBS=1      tanggal-observasi: tambahkan stat objek saat
 *                            gather (mengukur porsi stat object yang bisa
 *                            dihemat konsumen berikutnya).
 *   RBOT_DECIDE_FROM_PLAN=1  decide membaca plan.srcM (tanpa stat src ulang).
 *
 *Konsumen aktif saat ini: decide (opsional). Konsumen masa depan:
 * keputusan link, PSA, restore compdb.
 */
typedef struct {
  int count;                 /* == srcs.count */
  const char **path;         /* path source (alias ke list srcs) */
  int64_t *srcM;             /* mtime ns source (<0 = hilang) */
  int *objIdx;               /* -1 = path object gagal; else slot objPath */
  int objCount;              /* jumlah object unik yang distamp */
  char (*objPath)[MAX_PATH]; /* path object unik */
  int64_t *objM;             /* mtime ns object (<0 = hilang) */
  bool obsObj;               /* D1: objek ikut distamp saat gather */
} StatPlan;

static void statPlanInit(StatPlan *p) {
  memset(p, 0, sizeof(*p));
}

static void statPlanFree(StatPlan *p) {
  free(p->path);
  free(p->srcM);
  free(p->objIdx);
  free(p->objPath);
  free(p->objM);
  statPlanInit(p);
}

/* Stamp semua source sekali jalan; opsional (obsObj) stamp object unik.
   Dedup object pakai linear search: objCount kecil dan objectPathFor
   deterministik, jadi O(n^2) kecil ini masih jauh lebih murah daripada
   stat ganda pada FS lambat. */
static void statPlanBuild(StatPlan *p, const Config *c, const List *srcs) {
  int n = srcs->count;
  p->count = n;
  p->obsObj = getenv("RBOT_STATPLAN_OBS") != NULL;
  /* +1 menghindari malloc(0) saat n==0 */
  p->path = malloc(sizeof(char *) * (size_t)(n + 1));
  p->srcM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  p->objIdx = malloc(sizeof(int) * (size_t)(n + 1));
  p->objPath = malloc(sizeof(char[MAX_PATH]) * (size_t)(n + 1));
  p->objM = malloc(sizeof(int64_t) * (size_t)(n + 1));
  if (!p->path || !p->srcM || !p->objIdx || !p->objPath || !p->objM) {
    statPlanFree(p);
    return;
  }
  p->objCount = 0;
  for (int i = 0; i < n; i++) {
    const char *src = srcs->items[i];
    p->path[i] = src;
    long long sz = 0;
    int64_t m = -1;
    fsStampNsSize(src, &m, &sz); /* gagal -> -1, semantik fsMTimeNs */
    p->srcM[i] = m;

    char objPath[MAX_PATH];
    if (!objectPathFor(c, src, objPath, sizeof(objPath))) {
      p->objIdx[i] = -1;
      continue;
    }
    int slot = -1;
    for (int j = 0; j < p->objCount; j++) {
      if (strcmp(p->objPath[j], objPath) == 0) {
        slot = j;
        break;
      }
    }
    if (slot < 0) {
      slot = p->objCount;
      snprintf(p->objPath[slot], MAX_PATH, "%s", objPath);
      if (p->obsObj) {
        int64_t om = -1;
        fsStampNsSize(objPath, &om, &sz);
        p->objM[slot] = om;
      } else {
        p->objM[slot] = -99; /* belum distat — konsumen WAJIB stat sendiri */
      }
      p->objCount++;
    }
    p->objIdx[i] = slot;
  }
}

int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly) {
  /* Fast no-op per fase (miniws): fase binary memakai build.state, fase
     library workspace memakai build.lib.state — no-op `rbot -w` tidak
     membayar loadConfig + fingerprint + decide penuh per proyek. */
  if (cmdsFastStateValid(buildfilePath, libOnly)) return 0;
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  if (!resolveCompiler(&c)) return 1;
  profMark("startup+config");

  /* Mode sinyal: build paralel selalu butuh handler forward SIGINT karena
     tiap compiler ada di process group sendiri (Ctrl+C dari terminal hanya
     sampai ke rbot). Build serial mengikuti Buildfile: foreground. */
  procSetForeground(jobs > 1 ? false : c.foreground);

  if (!fsDirExists(c.root)) {
    fprintf(stderr, "rbot: root '%s' is not a directory\n", c.root);
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "rbot: cannot enter root '%s'\n", c.root);
    return 1;
  }

  /* Proyek kemasan (output.binary = false): SEBELUM koleksi source —
     proyek kemasan sah tidak punya satu .c pun. Dua varian:
       pack.*    -> pengemasan artefak (tar.gz/deb/checksum);
       embedded  -> arsip aset untuk di-embed binary lain. */
  if (!c.binary) {
    if (c.pack.requested) return cmdsPackOnlyProject(&c);
    if (c.embCount == 0) {
      fprintf(stderr, "rbot: nothing to build (output.binary = false)\n");
      return 1;
    }
    mkdirs(c.outBuildDir);
    if (!buildEmbeddedArchives(&c)) return 1;
    printf("\n> Summary\n");
    bool anyMissing = false;
    for (int i = 0; i < c.embCount; i++) {
      if (!c.emb[i].enable) continue;
      bool have = fsFileExists(c.emb[i].archivePath);
      printf("Archive  : %s%s\n", c.emb[i].archivePath, have ? "" : " (missing)");
      if (!have) anyMissing = true; /* entri hilang = build gagal, bukan sukses sunyi*/
    }
    if (anyMissing) {
      fprintf(stderr, "rbot: build failed: entri embedded hilang (lihat '(missing)' di atas)\n");
      return 1;
    }
    printf("Status   : Success\n");
    return 0;
  }

  List srcs = {0};
  listReserve(&srcs, 1024);
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    /* Entri FILE .c (mis. hasil `rbot -xf` untuk build.ninja flat) masuk
       langsung; selain itu dianggap folder dan discan rekursif. */
    size_t elen = strlen(entry);
    if (elen >= 2 &&
        (strcmp(entry + elen - 2, ".c") == 0 || strcmp(entry + elen - 2, ".cpp") == 0) &&
        fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    walkDir(entry, ".c", &srcs);
    walkDir(entry, ".cpp", &srcs);
  }
  /* Urutan final source = urutan link (driver C++ tetap mendukung urutan):
     .c dan .cpp dihasilkan dua walkDir terpisah per folder root — gabungan
     dua walk tidak selalu leksikografis ("a.cpp" dulu lalu "b.c" melanggar
     urutan a.c, a.cpp, b.c). Sort ulang jadi satu barisan stabil agar
     baris link reprodusible antar mesin dan urutan inisialisasi object
     statis C++ tidak lagi melekat pada readdir. */
  listSort(&srcs);

  /* One-pass stat plan: stamp source (selalu) + object (bila RBOT_STATPLAN_OBS)
   * SEKALI di sini. Konsumen berikutnya memakai hasilnya tanpa stat ulang. */
  StatPlan plan;
  statPlanInit(&plan);
  statPlanBuild(&plan, &c, &srcs);

  if (srcs.count == 0) {
    fprintf(stderr, "rbot: tidak ada source .c/.cpp di sources — proyek standar memakai folder "
                    "src/ (atau set sources = ... untuk tata letak lain)\n");
    statPlanFree(&plan);
    return 1;
  }

  CmdsBuildFingerprint currentFp;
  cmdsFingerprintMake(&c, &srcs, &currentFp);
  CmdsBuildFingerprint previousFp = {0};
  bool haveBuildFp = cmdsFingerprintLoad(&previousFp);
  bool compileConfigChanged = !haveBuildFp || previousFp.compile != currentFp.compile;
  bool sourceSetChanged = !haveBuildFp || previousFp.sources != currentFp.sources;
  bool linkConfigChanged = !haveBuildFp || previousFp.link != currentFp.link;

  profMark("fingerprint");

  if (haveBuildFp && compileConfigChanged)
    printf("> Fingerprint: compile configuration changed; rebuilding objects\n");
  if (haveBuildFp && sourceSetChanged)
    printf("> Fingerprint: source set changed; refreshing link inputs\n");
  if (haveBuildFp && linkConfigChanged)
    printf("> Fingerprint: link configuration changed; relinking\n");

  mkdirs(c.outBuildDir);
  mkdirs(c.outBinaryDir);

  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);

  /* compile_commands.json tidak perlu dihitung ulang pada setiap build.
     Daftar command hanya berubah jika file belum ada, konfigurasi compile
     berubah, atau set source berubah. Perubahan isi source/header tidak
     mengubah command compile. */
  if (compdbEnabled(&c)) {
    bool compdbMissing = !fsFileExists("compile_commands.json");
    if (compdbMissing || !haveBuildFp || compileConfigChanged || sourceSetChanged)
      writeCompdb(&c, &srcs);
  }

  /* Terbitkan build/embedded.h & build/version.h sebelum kompilasi agar
     konsumen melihat simbol/versi yang benar; jika salah satunya berubah,
     paksa kompilasi ulang total. */
  char obj[MAX_PATH];
  bool embHeaderChanged = false, verHeaderChanged = false;
  if (c.embCount > 0 && !emitEmbeddedHeader(&c, &embHeaderChanged)) {
    statPlanFree(&plan);
    return 1;
  }
  if (!cmdsEmitVersionHeader(&c, &verHeaderChanged)) {
    statPlanFree(&plan);
    return 1;
  }
  if (embHeaderChanged || verHeaderChanged) {
    if (verHeaderChanged)
      printf("> Version   : .rbot-version changed, recompiling all sources\n");
    else
      printf("> Embedded  : config changed, recompiling all sources\n");
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      fsRemoveFile(obj);
      /* Stamp plan objek yang baru saja dihapus: tanpa ini decide-from-plan
         akan melihat objek lama (masih ada & fresh) dan melewati kompilasi
         yang justru wajib diulang. */
      if (plan.objIdx && plan.objIdx[i] >= 0) plan.objM[plan.objIdx[i]] = -1;
    }
  }

  printf("> Build with %s %s-std=%s\n", c.cc, wf, c.std);

  int total = 0, compiled = 0, skipped = 0, failed = 0, interrupted = 0;

  /* Fase 1: klasifikasi up-to-date vs perlu-kompilasi. Pakai mtime
     nanodetik (bukan detik) agar perubahan dalam detik yang sama tetap
     terdeteksi — sama seperti keputusan link di bawah. */
  /*
   * Header tracking dua lapis:
   *   1. mtime — object usang bila header transitive lebih baru. Murah.
   *   2. konten — bila mtime mengatakan stale (mis. setelah `touch`), hash
   *      isi dibandingkan dengan snapshot build sukses terakhir
   *      (.rbot/deps.cache). Konten sama => tidak perlu kompilasi ulang.
   *
   * Edges header diambil dari file .d compiler (flag -MMD, GCC/Clang) —
   * akurat & lengkap; pemindai #include hanya fallback (build pertama,
   * MSVC, .d belum ada). Lihat deps/.
   */
  List pending = {0};
  DepCache *deps = depsNew(&c);
  int headerStale = 0, headerTouched = 0;
  /* mtime object terbesar & apakah ada object yang hilang — dikumpulkan di
     sini agar keputusan link (no-op) tidak men-stat ulang semua object. */
  int64_t maxObjM = -1;
  bool anyObjMissing = false;
  bool decideFromPlan = getenv("RBOT_DECIDE_FROM_PLAN") != NULL;
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    /* Sumber mtime dari plan one-pass (RBOT_DECIDE_FROM_PLAN=1) atau stat
       sendiri (perilaku lama). Objek: dari plan bila tersedia (observasi),
       selain itu stat sendiri. Semantik -1 (hilang) dipertahankan. */
    int64_t srcM = (decideFromPlan && plan.srcM) ? plan.srcM[i] : fsMTimeNs(src);
    int64_t objM;
    /* Object dari plan hanya saat decideFromPlan: stamp bisa basi bila
       objek dihapus jalur embedded/version header di atas (loop di bawah
       meng-invalidasi slot itu), dan tanpa env ini perilaku lama utuh. */
    if (decideFromPlan && plan.objIdx && plan.objIdx[i] >= 0 && plan.obsObj)
      objM = plan.objM[plan.objIdx[i]];
    else
      objM = fsMTimeNs(obj); /* -1 = object belum ada (sekali stat) */
    if (objM < 0)
      anyObjMissing = true;
    else if (objM > maxObjM)
      maxObjM = objM;
    if (!compileConfigChanged && objM >= 0 && srcM >= 0 && objM >= srcM) {
      if (depsNewestHeaderMTimeAt(deps, src, srcM) > objM) {
        /* header lebih baru — tapi mungkin hanya `touch`: cek konten */
        if (depsContentUpToDate(deps, src)) {
          headerTouched++;
          skipped++;
          continue;
        }
        headerStale++;
      } else {
        skipped++;
        continue;
      }
    }
    listAdd(&pending, src);
  }
  profMark("decide");
  if (headerTouched > 0)
    printf("> Headers   : %d source(s) skipped (touched, content unchanged)\n", headerTouched);
  if (headerStale > 0)
    printf("> Headers   : %d source(s) stale due to header change\n", headerStale);
  total = pending.count;

  /* Fase 2: kompilasi hanya yang berubah — paralel (-jN) atau serial. */
  if (jobs != 1 && total > 0) {
    cmdsRunParallelJobs(&c, inc, wf, &pending, obj, sizeof(obj), jobs, &compiled, &failed,
                        &interrupted, false);
  } else {
    for (int i = 0; i < pending.count; i++) {
      const char *src = pending.items[i];
      if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;

      double start = nowSeconds();
      bool ok = compileLibraryOne(&c, inc, wf, src, obj);
      double dt = nowSeconds() - start;

      compiled++;
      if (procInterrupted()) {
        /* foreground=false: child mati karena forward SIGINT dari rbot —
           hentikan build, jangan lanjut ke source berikutnya. */
        interrupted++;
        if (ok || c.progressErrorAlways)
          printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, "INT", dt, src);
        break;
      }
      if (!ok) failed++;
      if (ok || c.progressErrorAlways)
        printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, ok ? "OK" : "FAIL", dt, src);
    }
  }
  profMark("compile");

  if (interrupted > 0) {
    /* Jangan percaya fingerprint lama setelah sebagian object mungkin telah
       ditulis dengan konfigurasi baru sebelum interupsi. */
    if (compileConfigChanged) cmdsFingerprintInvalidate();
    fprintf(stderr, "\nrbot: build interrupted; stopping\n");
    free(inc);
    free(wf);
    statPlanFree(&plan);
    return 130;
  }

  if (failed > 0) {
    /* Sebagian object bisa berhasil dibuat sebelum object lain gagal. Hapus
       state agar build berikutnya tidak melewatkan rebuild konfigurasi penuh. */
    if (compileConfigChanged) cmdsFingerprintInvalidate();
    fprintf(stderr, "\nrbot: build failed with %d error(s); linking skipped\n", failed);
    free(inc);
    free(wf);
    statPlanFree(&plan);
    return 1;
  }

  if (!buildEmbedded(&c)) {
    free(inc);
    free(wf);
    statPlanFree(&plan);
    profReport();
    return 1;
  }

  /* Fase library workspace (pass 1 dari cmdBuildEx, lihat commands.h):
     kemas lib<name>.a/.so dari object milik proyek ini lalu berhenti —
     link binary ditunda ke fase normal (pass 2) agar proyek yang saling
     memakai library lintas-proyek bisa link setelah SEMUA library ada.
     Library ber-path proyek lain (mis. ../ruka/lib/libruka.a) tidak
     disertakan pada link .so: .a proyek lain belum tentu ada di titik ini
     dan shared library memang tidak perlu inline-kan kode proyek lain. */
  if (libOnly) {
    if (!cmdsBuildLibraryEx(&c, &srcs, false)) {
      free(inc);
      free(wf);
      depsFree(deps);
      return 1;
    }
    if (compiled > 0 || depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    }
    depsSave(deps);
    depsFree(deps);
    cmdsFingerprintSave(&currentFp);
    /* State fase library (miniws): no-op pass 1 berikutnya dilewati total.
       target "" = proyek tanpa library — direkam sebagai "-". */
    cmdsFastStateSave(&c, &srcs, "", buildfilePath, true);
    printf("\n> Summary\n");
    if (c.libRequested) {
      char libp[MAX_PATH * 2];
      if (c.libStatic) {
        cmdsLibStaticPath(&c, libp, sizeof(libp));
        printf("Library  : %s\n", libp);
      }
      if (c.libShared) {
        cmdsLibSharedPath(&c, libp, sizeof(libp));
        printf("Library  : %s\n", libp);
      }
    }
    printf("Compiled : %d\n", compiled);
    printf("Skipped  : %d\n", skipped);
    printf("Status   : Success\n");
    free(inc);
    free(wf);
    profReport();
    return 0;
  }

  /* link: hanya jalankan linker bila target belum ada atau salah satu
     input object lebih baru daripada target. Gunakan nanosecond mtime agar
     keputusan incremental tidak kehilangan perubahan yang terjadi dalam
     detik yang sama. */
  char target[MAX_PATH * 2];
  snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
#ifdef _WIN32
  /* Windows dapat menambahkan .exe otomatis saat link. */
  if (!fsFileExists(target)) {
    char withExe[MAX_PATH * 2];
    snprintf(withExe, sizeof(withExe), "%s.exe", target);
    if (fsFileExists(withExe)) snprintf(target, sizeof(target), "%s", withExe);
  }
#endif

  bool linkNeeded = !fsFileExists(target) || linkConfigChanged || sourceSetChanged || compiled > 0;
  if (!linkNeeded) {
    int64_t targetMtime = fsMTimeNs(target);
    if (targetMtime < 0) {
      linkNeeded = true;
    } else {
      if (compiled == 0 && !anyObjMissing) {
        /* Tidak ada object yang ditulis ulang sejak fase 1: mtime yang sudah
           dikumpulkan masih berlaku — nol stat tambahan pada build no-op. */
        if (maxObjM > targetMtime) linkNeeded = true;
      } else {
        for (int i = 0; i < srcs.count && !linkNeeded; i++) {
          if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
          int64_t objectMtime = fsMTimeNs(obj);
          if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
        }
      }

      for (int i = 0; i < c.embCount && !linkNeeded; i++) {
        if (!c.emb[i].enable) continue;
        if (!fsFileExists(c.emb[i].objectPath)) {
          linkNeeded = true;
          break;
        }
        int64_t objectMtime = fsMTimeNs(c.emb[i].objectPath);
        if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
      }
    }
  }

  if (!linkNeeded) {
    printf("> Linking   : %s (up-to-date)\n", target);
    /* Library bisa jadi masih perlu dibangun (baru diaktifkan di
       Buildfile / terhapus manual) meski binary sudah up-to-date. */
    if (c.libRequested && !cmdsBuildLibraryEx(&c, &srcs, true)) {
      free(inc);
      free(wf);
      return 1;
    }
    free(inc);
    free(wf);
    profReport();
    /* Build sukses: rekam snapshot hash source + dependensi agar build
       berikutnya bisa membedakan `touch` (isi sama) dari perubahan konten.
       No-op murni (tidak ada kompilasi, tidak ada yang berubah sejak rekam
       terakhir) melewatinya — DFS + ribuan snapshotSet murni CPU sia-sia
       pada project besar. depsSave tetap dipanggil: verifikasi konten
       (kasus touch) memperbarui mtime snapshot, dan depsSave sendiri murah
       bila tidak ada yang berubah. */
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    } else if (compiled > 0) {
      for (int i = 0; i < pending.count; i++)
        depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    depsFree(deps);
    cmdsFingerprintSave(&currentFp);
    /* Packaging (pack.*) sebelum summary; fast state disimpan di dalamnya
       bila pack sukses — gagal kemasan tidak merekam state. */
    bool fastSaved = false;
    if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) {
      statPlanFree(&plan);
      return 1;
    }
    printf("\n> Summary\n");
    long long sizeBytes = fsFileSize(target);
    double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;
    printf("Target   : %s\n", target);
    printf("Size     : %.1fKB\n", sizeKb);
    printf("Compiled : %d\n", compiled);
    printf("Skipped  : %d\n", skipped);
    printf("Status   : Success\n");
    if (!fastSaved) cmdsFastStateSave(&c, &srcs, target, buildfilePath, false);
    return 0;
  }

  size_t n = strlen(c.cc) + strlen(c.outBinaryDir) + strlen(c.outBinaryName) + 256;
  for (int i = 0; i < srcs.count; i++)
    if (objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) n += strlen(obj) + 2;
  for (int i = 0; i < c.embCount; i++)
    if (c.emb[i].enable) n += strlen(c.emb[i].objectPath) + 2;
  for (int i = 0; i < c.libraries.count; i++)
    n += strlen(c.libraries.items[i]) + 8;

  char *cmd = malloc(n);
  strcpy(cmd, c.cc);
  for (int i = 0; i < srcs.count; i++) {
    if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
    strcat(cmd, " ");
    strcat(cmd, obj);
  }
  for (int i = 0; i < c.embCount; i++) {
    if (!c.emb[i].enable) continue;
    if (fsFileExists(c.emb[i].objectPath)) {
      strcat(cmd, " ");
      strcat(cmd, c.emb[i].objectPath);
    }
  }
  if (compilerIsMSVC(&c)) {
    /* MSVC: object dikumpulkan dulu, opsi linker setelah token /link.
       /OUT menentukan target; link.exe untuk EXE tanpa /LD tidak menulis
       .lib/.exp sampingan, jadi tidak perlu /IMPLIB. (Nama variabel beda
       dari `target` luar — MSVC /W4 memperingatkan shadowing, C4456.) */
    char linkTarget[MAX_PATH * 2];
    snprintf(linkTarget, sizeof(linkTarget), "%s/%s", c.outBinaryDir, c.outBinaryName);
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, linkTarget);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++; /* -lssl -> ssl */
      if (lib[0] == 'l') lib++; /* "lssl" -> "ssl", seperti konvensi headers "I." */
      strcat(cmd, " ");
      strcat(cmd, lib);
      strcat(cmd, ".lib");
    }
  } else {
    strcat(cmd, " -o ");
    strcat(cmd, c.outBinaryDir);
    strcat(cmd, "/");
    strcat(cmd, c.outBinaryName);
    /* Driver C (compiler = gcc eksplisit) + source C++: runtime libstdc++
       harus di-link manual; driver C++ (g++/cl) sudah membawanya sendiri. */
    if (c.langCpp && !compilerIsCppDriver(&c)) strcat(cmd, " -lstdc++");
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      /* "ssl" -> -lssl, "lm" -> -lm (leading 'l' sudah termasuk, seperti "I."
         di headers); entri yang sudah diawali '-' diteruskan apa adanya */
      strcat(cmd, " ");
      if (strchr(lib, '/') || strchr(lib, '\\')) {
        /* Workspace project-library resolution may provide an explicit
           artifact path (../project/lib/libname.a). Do not turn it into
           -l../...; pass the path directly to the linker. */
        strcat(cmd, lib);
      } else {
        if (lib[0] == '-' || lib[0] == 'l')
          strcat(cmd, "-");
        else
          strcat(cmd, "-l");
        strcat(cmd, lib);
      }
    }
  }

  printf("> Linking   : %s/%s\n", c.outBinaryDir, c.outBinaryName);
  bool linked = runCmd(cmd);
  profMark("link");
  free(cmd);

  free(inc);
  free(wf);

  if (!linked) {
    fprintf(stderr, "rbot: link failed\n");
    statPlanFree(&plan);
    return 1;
  }

  /* Library statis/shared dari object yang sama — object tidak dihapus. */
  if (!cmdsBuildLibraryEx(&c, &srcs, true)) {
    statPlanFree(&plan);
    return 1;
  }

  /* Build sukses: rekam snapshot hash (lihat jalur up-to-date di atas). */
  if (compiled > 0 || depsSnapshotIncomplete(deps)) {
    for (int i = 0; i < srcs.count; i++)
      depsRecordUpdate(deps, srcs.items[i]);
  }
  depsSave(deps);
  depsFree(deps);
  cmdsFingerprintSave(&currentFp);

  /* Packaging (pack.*) setelah link & state — lihat jalur up-to-date. */
  bool fastSaved = false;
  if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) {
    statPlanFree(&plan);
    return 1;
  }
  profReport();

  long long sizeBytes = fsFileSize(target);
  double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;

  printf("\n> Summary\n");
  printf("Target   : %s\n", target);
  printf("Size     : %.1fKB\n", sizeKb);
  printf("Compiled : %d\n", compiled);
  printf("Skipped  : %d\n", skipped);
  printf("Status   : Success\n");
  if (!fastSaved) cmdsFastStateSave(&c, &srcs, target, buildfilePath, false);
  statPlanFree(&plan);
  return 0;
}
