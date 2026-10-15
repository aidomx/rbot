#pragma once
#define SATRIA_H

/*
 * Kontrak antara src/main.c (dispatcher/loader 
 */
int satriaRun(int argc, const char *argv[]);

/* Versi implementasi aktif, dikompilasi tetap ke dalam binary (bukan
 * dibaca ulang dari .version saat runtime) — supaya `satria version` selalu
 * benar di mana pun dijalankan, tidak tergantung direktori kerja.
 */
const char *satriaVersion(void);

/* uji .d */

/* t */

/* u */
