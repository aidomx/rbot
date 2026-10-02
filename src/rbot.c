#include "rbot.h"

#include "uses/use.h"
#include "uses/workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "ninjaconv.h"
#include "portability.h"
#include "prof/prof.h"

#if __has_include("version.h")
#include "version.h" /* build/version.h — dihasilkan dari .rbot-version */
#endif

/*
 * Versi rbot. Sumber kebenaran: .rbot-version di root — saat build rbot
 * menuliskannya ke build/version.h (RBOT_VERSION_EMBEDDED) sehingga versi
 * ter-embed ke binary dan `rbot version` tetap benar di mana pun dijalankan.
 * Fallback literal hanya untuk build tanpa .rbot-version (mis. CMake murni).
 */
const char *rbotVersion(void) {
#ifdef RBOT_VERSION_EMBEDDED
  return RBOT_VERSION_EMBEDDED;
#else
  return "v0.1.8";
#endif
}

/* -xf TIDAK pernah menulis Buildfile: konversi masuk file sementara ini
   (dihapus setelah command selesai), sehingga Buildfile project tak mungkin
   tertimpa oleh hasil konversi. */
#define XF_TMP "Buildfile.xf.tmp"

/* ---------- Opsi CLI: -jN / -j / --jobs=N ---------- */

static bool parseJobsArg(const char *inlineVal, bool hasInline, int *outJobs, const char **err) {
  const char *v = NULL;

  if (hasInline) { /* -j4 / --jobs=4 */
    v = inlineVal;
  } else { /* -j / --jobs (tanpa nilai) -> default jumlah core CPU */
    *outJobs = cpuCount();
    return true;
  }

  if (!v || !*v) { /* -j'' : nilai kosong -> default core */
    *outJobs = cpuCount();
    return true;
  }

  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end == v || *end != '\0' || n < 1) {
    *err = v;
    return false;
  }
  *outJobs = (int)n;
  return true;
}

/*
 * Titik masuk implementasi versi v0.1.0. Dipanggil oleh src/main.c
 * (dispatcher/loader) setelah versi aktif ditentukan lewat .version.
 * Isi operasi rbot yang sesungguhnya (build/clean/help) ada di commands.c
 * dan modul-modul lain di folder ini.
 */
int rbotRun(int argc, const char *argv[]) {
  profInit(); /* profil fase aktif bila RBOT_PROFILE=1 (stderr, tanpa syscall) */
  /* Opsi build paralel: -j[N] / --jobs[=N]. Opsi boleh berada sebelum atau
     sesudah command (mis. `rbot -j4`, `rbot clean -j2`); hanya build yang
     memakai jobs, command lain mengabaikannya. */
  /* Konversi lintas konfigurasi: -xf <build.ninja> menyalinnya menjadi
     Buildfile sementara (dihapus setelah command selesai); -xcf membuat
     Buildfile hasil konversi tetap ada. */
  enum { CONV_NONE, CONV_DELETE, CONV_KEEP } conv = CONV_NONE;
  const char *convFile = NULL;
  int jobs = 0; /* 0 = otomatis: jumlah core CPU (di bawah); -j1 = serial */
  const char *cmd = NULL;
  const char *buildfile = "Buildfile"; /* -f <file> untuk memakai yang lain */
  bool wantWorkspace = false; /* -w: paksa mode workspace */
  const char *wsOnly = NULL;  /* -w <nama>: hanya proyek itu */

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *err = NULL;

    /* -w: mode workspace (Buildfile.workspace). `rbot -w` = semua proyek;
       `rbot -w <nama>` = hanya proyek itu. OPSI, bukan command — argumen
       setelahnya TIDAK diperlakukan sebagai command (rupakan dengan
       dispatcher "command pertama non-opsi"). */
    if (strcmp(a, "-w") == 0) {
      wantWorkspace = true;
      /* Kata berikutnya = nama proyek KECUALI command yang dikenal
         (rbot -w clean harus tetap berarti clean, bukan proyek "clean"). */
      if (i + 1 < argc && argv[i + 1][0] && argv[i + 1][0] != '-' &&
          strcmp(argv[i + 1], "build") != 0 && strcmp(argv[i + 1], "clean") != 0 &&
          strcmp(argv[i + 1], "init") != 0 && strcmp(argv[i + 1], "help") != 0 &&
          strcmp(argv[i + 1], "version") != 0)
        wsOnly = argv[++i];
      continue;
    }

    if (strcmp(a, "-xf") == 0 || strcmp(a, "-xcf") == 0) {
      conv = (a[2] == 'c') ? CONV_KEEP : CONV_DELETE;
      if (i + 1 >= argc || !argv[i + 1][0]) {
        fprintf(stderr, "rbot: %s requires a build.ninja argument\n", a);
        return 2;
      }
      convFile = argv[++i];
      continue;
    }

    if (strcmp(a, "-f") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0]) {
        fprintf(stderr, "rbot: -f requires a file argument\n");
        return 2;
      }
      buildfile = argv[++i];
      continue;
    }
    if (strncmp(a, "-f", 2) == 0 && a[2]) { /* -fBuildfile.aliased */
      buildfile = a + 2;
      continue;
    }

    if (strcmp(a, "-j") == 0 || strcmp(a, "--jobs") == 0) {
      if (!parseJobsArg(NULL, false, &jobs, &err)) {
        fprintf(stderr, "rbot: invalid job count '%s'\n", err ? err : a);
        return 2;
      }
      continue;
    }
    if (strncmp(a, "-j", 2) == 0 && a[2] != '\0') { /* -j4 */
      if (!parseJobsArg(a + 2, true, &jobs, &err)) {
        fprintf(stderr, "rbot: invalid job count '%s'\n", err ? err : a + 2);
        return 2;
      }
      continue;
    }
    if (strncmp(a, "--jobs=", 7) == 0) { /* --jobs=4 */
      if (!parseJobsArg(a + 7, true, &jobs, &err)) {
        fprintf(stderr, "rbot: invalid job count '%s'\n", err ? err : a + 7);
        return 2;
      }
      continue;
    }

    if (!cmd) cmd = a; /* command pertama non-opsi */
  }
  if (jobs < 1) jobs = cpuCount(); /* tanpa -j: paralel sebanyak core, seperti ninja */

  /* build.ninja relatif dievaluasi terhadap cwd PEMANGGIL — absolut-kan
     sebelum chdir -f di bawah. */
  char convAbs[MAX_PATH * 2] = {0};
  if (conv != CONV_NONE) {
    if (convFile[0] == '/') {
      snprintf(convAbs, sizeof(convAbs), "%s", convFile);
    } else {
      char cwd0[MAX_PATH];
      if (fsGetCwd(cwd0, sizeof(cwd0)))
        snprintf(convAbs, sizeof(convAbs), "%s/%s", cwd0, convFile);
      else
        snprintf(convAbs, sizeof(convAbs), "%s", convFile);
    }
  }

  /* -f menunjuk file di luar cwd (mis. `rbot -f ../proj/Buildfile.example`):
     masuk ke direktori file itu dulu — sources/headers/output di Buildfile
     relatif terhadap lokasinya, bukan cwd pemanggil (gaya make -C /
     ninja -C). File polos tanpa direktori berarti cwd sudah benar. */
  {
    /* static: buildfile menunjuk ke dalam buffer ini setelah blok berakhir */
    static char dir[MAX_PATH];
    copyStr(dir, sizeof(dir), buildfile);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bs = strrchr(dir, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    if (slash) {
      *slash = '\0';
      if (!fsSetCwd(dir[0] ? dir : "/")) {
        fprintf(stderr, "rbot: cannot enter directory '%s'\n", dir[0] ? dir : "/");
        return 2;
      }
      buildfile = slash + 1; /* lanjut sebagai nama file polos */
    }
  }

  /* Konversi build.ninja -> Buildfile (di direktori kerja saat ini).
     -xf  : selalu ke file SEMENTARA — Buildfile eksisting tidak disentuh.
     -xcf : menulis Buildfile, tapi konfirmasi dulu bila sudah ada. */
  if (conv != CONV_NONE) {
    char err[256];
    const char *dst = (conv == CONV_DELETE) ? XF_TMP : "Buildfile";
    if (conv == CONV_KEEP && fsFileExists(dst)) {
      printf("> '%s' sudah ada. Timpa dengan hasil konversi %s? [y/N] ", dst, convAbs);
      fflush(stdout);
      char ans[16] = {0};
      if (!fgets(ans, sizeof(ans), stdin)) ans[0] = '\0';
      if (ans[0] != 'y' && ans[0] != 'Y') {
        printf("> Dibatalkan : %s tidak diubah\n", dst);
        return 0;
      }
    }
    if (!ninjaToBuildfile(convAbs, dst, err, sizeof(err))) {
      fprintf(stderr, "rbot: %s\n", err);
      return 1;
    }
    if (conv == CONV_DELETE)
      printf("> Temporary  : %s (dihapus setelah command; Buildfile tak disentuh)\n", dst);
    buildfile = dst;
  }

  /* Mode workspace: -w eksplisit, atau auto-detect Buildfile.workspace
     saat rbot polos di root workspace. init/help tetap jalur satu-project. */
  bool wsAuto = !wantWorkspace && (!cmd || !*cmd) && workspaceFileExists();
  if (wantWorkspace || (wsAuto)) {
    if (cmd && *cmd && strcmp(cmd, "clean") != 0) {
      fprintf(stderr, "rbot: command '%s' tidak berlaku di mode workspace (pakai build/clean)\n", cmd);
      return 2;
    }
    return workspaceRun(cmd && *cmd ? cmd : "build", jobs, wsOnly);
  }

  int rc;
  if (!cmd || !*cmd) {
    rc = cmdBuild(jobs, buildfile);
  } else if (strcmp(cmd, "init") == 0) {
    rc = cmdInit(buildfile);
  } else if (strcmp(cmd, "clean") == 0) {
    rc = cmdClean(buildfile);
  } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
             strcmp(cmd, "-h") == 0) {
    showHelp();
    rc = 0;
  } else {
    fprintf(stderr, "rbot: unknown command %s\n\n", cmd);
    showHelp();
    return 1;
  }

  /* -xf: hapus file sementara setelah command selesai. Buildfile asli
     tidak pernah ditulis, jadi tidak ada yang berisiko hilang. */
  if (conv == CONV_DELETE) {
    fsRemoveFile(XF_TMP);
    printf("> Removed    : %s (sementara, dari %s)\n", XF_TMP, convAbs);
  }
  return rc;
}
