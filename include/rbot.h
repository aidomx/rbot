#pragma once
#define RBOT_H

/*
 * Kontrak antara src/main.c (dispatcher/loader) dan implementasi versi aktif
 * (mis. src/v0.1.0/). Versi aktif ditentukan oleh file .version di root
 * project; src/main.c membaca file itu lalu mendelegasikan seluruh kerja ke
 * rbotRun(), yang diimplementasikan oleh folder src/<versi>/.
 *
 * Menambah versi baru cukup: buat src/v0.2.0/ dengan rbotRun() sendiri,
 * lalu ganti isi .version menjadi "v0.2.0" dan arahkan Buildfile: sources
 * ke folder tersebut.
 */
int rbotRun(int argc, const char *argv[]);

/* Versi implementasi aktif, dikompilasi tetap ke dalam binary (bukan
 * dibaca ulang dari .version saat runtime) — supaya `rbot version` selalu
 * benar di mana pun dijalankan, tidak tergantung direktori kerja.
 */
const char *rbotVersion(void);
