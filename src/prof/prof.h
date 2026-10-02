#ifndef RBOT_PROF_H
#define RBOT_PROF_H

#include <stdbool.h>

/*
 * prof — profil fase ringan (RBOT_PROFILE=1).
 *
 * Tujuan: mendiagnosis biaya fase build (config/fingerprint/decide/compile/
 * link/pack) TANPA mengubah perilaku build dan nyaris tanpa syscall tambahan
 * (timespec_get C11, dibaca via vDSO — aman di proot/strace-less Termux).
 *
 * Pemakaian:
 *   RBOT_PROFILE=1 rbot -w        -> tiap proyek mencetak blok profil ke
 *                                    stderr (delta ms antar penanda fase).
 *
 * Penanda memakai label string statis; pencatatan di luar profOn() adalah
 * no-op (biaya satu branch). profReport() mencetak lalu me-reset buffer.
 */
void profInit(void);              /* baca env RBOT_PROFILE sekali + reset */
bool profOn(void);
void profMark(const char *label); /* catat waktu penanda fase */
void profReport(void);            /* cetak total + delta ke stderr, reset */

#endif /* RBOT_PROF_H */
