#include "rbot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
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
  int jobs = 1;
  const char *cmd = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *err = NULL;

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

  if (!cmd || !*cmd) return cmdBuild(jobs);

  if (strcmp(cmd, "init") == 0) return cmdInit();
  if (strcmp(cmd, "clean") == 0) return cmdClean();
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
    showHelp();
    return 0;
  }

  fprintf(stderr, "rbot: unknown command %s\n\n", cmd);
  showHelp();
  return 1;
}
