#include "rbot.h"

#include <stdio.h>
#include <string.h>

#include "commands.h"

/*
 * Nama folder ini (src/v0.1.0) ADALAH versinya — konstanta di bawah cukup
 * mengikutinya. Bukan dibaca dari .version saat runtime; dikompilasi tetap
 * ke binary supaya `rbot version` selalu benar di mana pun dijalankan.
 */
const char *rbotVersion(void) {
  return "v0.1.1";
}

/*
 * Titik masuk implementasi versi v0.1.0. Dipanggil oleh src/main.c
 * (dispatcher/loader) setelah versi aktif ditentukan lewat .version.
 * Isi operasi rbot yang sesungguhnya (build/clean/help) ada di commands.c
 * dan modul-modul lain di folder ini.
 */
int rbotRun(int argc, const char *argv[]) {
  if (argc <= 1 || !argv[1]) {
    return cmdBuild();
  }

  const char *cmd = argv[1];
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
