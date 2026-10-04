#ifndef RBOT_V0_1_0_USES_H
#define RBOT_V0_1_0_USES_H

#include <stdbool.h>

/*
 * use — pembungkus format Buildfile.
 *
 * Buildfile boleh diawali SATU baris `use` di level-0:
 *
 *   use project    — satu project; sinonim persis `use alias` (jalur parse
 *                    sama, tidak ada jalur konfigurasi kedua).
 *   use workspace  — pembungkus banyak project (file: Buildfile.ws;
 *                    lihat uses/workspace.h).
 *   use alias      — nama lama untuk `use project`; tetap diterima demi
 *                    kompatibilitas (Buildfile rbot & template `rbot init`).
 *
 * Baris `use` adalah pembungkus namespace itu sendiri: key ditulis LANGSUNG
 * tanpa prefix (`projects.rupa as rupa`, bukan `w.project.rupa`), sama
 * seperti key ditulis langsung di `use project` tanpa `project.`.
 *
 * Tanpa baris `use` apa pun = jalur parse lama (perilaku sebelum ada use).
 * Singkatan nama tetap lewat baris alias biasa `X as Y` — baris `use` tidak
 * berargumen.
 */

typedef enum {
  USE_NONE = 0, /* tanpa `use` — parse lama */
  USE_PROJECT,  /* use project / use alias */
  USE_WORKSPACE /* use workspace */
} UseMode;

/* Klasifikasi baris `use ...` di level-0. Baris yang bukan `use` → USE_NONE
   (pemanggil tetap memproses baris itu seperti biasa). `use` dengan argumen
   tak dikenal juga USE_NONE — jatuh ke parse lama, tidak error. */
UseMode useLineMode(const char *text);

/* true bila `text` adalah baris `use` yang dikenal (use project/workspace/
   alias). Dipakai parser untuk membuang barisnya dari aliran konfigurasi. */
bool useLineKnown(const char *text);

#endif /* RBOT_V0_1_0_USES_H */
