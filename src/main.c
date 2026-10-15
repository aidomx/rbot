#include <stdio.h>
#include <string.h>

#include "satria.h"

/*
 * src/main.c — dispatcher & loader.
 *
 * File ini TIDAK berisi operasi satria itu sendiri. Tugasnya hanya:
 * mendelegasikan seluruh argumen ke satriaRun(), yang diimplementasikan oleh src/satria.c
 */

int main(int argc, const char *argv[]) {
  if (argc > 1 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)) {
    printf("satria %s\n", satriaVersion());
    return 0;
  }

  /* Semua command lain (default build, init, clean, help, ...)
     didelegasikan ke implementasi versi aktif. */
  return satriaRun(argc, argv);
}
