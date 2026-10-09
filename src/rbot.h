#pragma once
#define RBOT_H

/*
 * Kontrak antara src/main.c (dispatcher/loader 
 */
int rbotRun(int argc, const char *argv[]);

/* Versi implementasi aktif, dikompilasi tetap ke dalam binary (bukan
 * dibaca ulang dari .version saat runtime) — supaya `rbot version` selalu
 * benar di mana pun dijalankan, tidak tergantung direktori kerja.
 */
const char *rbotVersion(void);

/* uji .d */

/* t */

/* u */
