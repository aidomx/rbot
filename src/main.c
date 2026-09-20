#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "rbot.h"

/*
 * src/main.c — dispatcher & loader.
 *
 * File ini TIDAK berisi operasi rbot itu sendiri. Tugasnya hanya dua:
 *   1. Membaca .version di root project untuk tahu versi implementasi mana
 *      yang sedang aktif (mis. "v0.1.0" -> folder src/v0.1.0/).
 *   2. Mendelegasikan seluruh argumen ke rbotRun(), yang diimplementasikan
 *      oleh folder versi tersebut (lihat include/rbot.h).
 *
 * Menaikkan versi berarti: tambah folder src/vX.Y.Z/ baru dengan rbotRun()
 * sendiri, arahkan Buildfile: sources ke folder itu, lalu update isi
 * .version (dan Buildfile: version) menjadi "vX.Y.Z".
 */

#define VERSION_FILE ".version"

static void readVersion(char *out, size_t n) {
  snprintf(out, n, "unknown");

  FILE *fp = fopen(VERSION_FILE, "rb");
  if (!fp) return;
  if (fgets(out, (int)n, fp)) {
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
      out[--len] = '\0';
  }
  fclose(fp);
}

int main(int argc, const char *argv[]) {
  if (argc > 1 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)) {
    char version[64];
    readVersion(version, sizeof(version));
    printf("rbot %s\n", version);
    return 0;
  }

  /* Semua command lain (build, clean, help, ...) didelegasikan ke
     implementasi versi aktif. */
  return rbotRun(argc, argv);
}
