#include "rbot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "ninjaconv.h"
#include "portability.h"

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
  return "v0.1.2";
#endif
}

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
  /* Opsi build paralel: -j[N] / --jobs[=N]. Opsi boleh berada sebelum atau
     sesudah command (mis. `rbot -j4`, `rbot clean -j2`); hanya build yang
     memakai jobs, command lain mengabaikannya. */
  /* Konversi lintas konfigurasi: -xf <build.ninja> menyalinnya menjadi
     Buildfile sementara (dihapus setelah command selesai); -xcf membuat
     Buildfile hasil konversi tetap ada. */
  enum { CONV_NONE, CONV_DELETE, CONV_KEEP } conv = CONV_NONE;
  const char *convFile = NULL;
  int jobs = 1;
  const char *cmd = NULL;
  const char *buildfile = "Buildfile"; /* -f <file> untuk memakai yang lain */

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *err = NULL;

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

  /* Konversi build.ninja -> Buildfile (di direktori file itu, setelah chdir). */
  if (conv != CONV_NONE) {
    char err[256];
    if (fsFileExists("Buildfile"))
      printf("> Overwrite  : Buildfile (hasil konversi %s)\n", convFile);
    if (!ninjaToBuildfile(convFile, "Buildfile", err, sizeof(err))) {
      fprintf(stderr, "rbot: %s\n", err);
      return 1;
    }
    buildfile = "Buildfile";
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

  /* -xf: Buildfile sementara dihapus setelah command selesai. */
  if (conv == CONV_DELETE && strcmp(buildfile, "Buildfile") == 0) {
    fsRemoveFile("Buildfile");
    printf("> Removed    : Buildfile (sementara, dari %s)\n", convFile);
  }
  return rc;
}
