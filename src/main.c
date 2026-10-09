#include <stdio.h>
#include <string.h>

#include "rbot.h"

/*
 * src/main.c — dispatcher & loader.
 *
 * File ini TIDAK berisi operasi rbot itu sendiri. Tugasnya hanya:
 * mendelegasikan seluruh argumen ke rbotRun(), yang diimplementasikan oleh src/rbot.c
 */

int main(int argc, const char *argv[]) {
  if (argc > 1 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)) {
    printf("rbot %s\n", rbotVersion());
    return 0;
  }

  /* Semua command lain (default build, init, clean, help, ...)
     didelegasikan ke implementasi versi aktif. */
  return rbotRun(argc, argv);
}
