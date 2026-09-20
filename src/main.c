#include <stdio.h>
#include <string.h>

#include "rbot.h"

/*
 * src/main.c — dispatcher & loader.
 *
 * File ini TIDAK berisi operasi rbot itu sendiri. Tugasnya hanya:
 * mendelegasikan seluruh argumen ke rbotRun(), yang diimplementasikan oleh
 * folder versi aktif (lihat include/rbot.h). Nama folder itu sendiri
 * (mis. src/v0.1.0/) adalah versinya, dan versi itu dikompilasi tetap ke
 * binary lewat rbotVersion() — bukan dibaca ulang dari file .version saat
 * runtime, supaya `rbot version` selalu benar di mana pun rbot dijalankan
 * (tidak tergantung direktori kerja).
 *
 * Menaikkan versi berarti: tambah folder src/vX.Y.Z/ baru dengan rbotRun()
 * dan rbotVersion() sendiri, lalu arahkan Buildfile: sources ke folder itu.
 * .version di root tetap dipakai sebagai penanda dokumentasi folder mana
 * yang sedang aktif dibangun.
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
