#ifndef RBOT_PROF_H
#define RBOT_PROF_H

#include <stdbool.h>
#include <stdint.h>

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

/* ---------- sesi profil: `rbot profile <context>` (design/profile.md) ---------- */

/*
 * Sesi fase bernama untuk command profile. Berbeda dari profMark (aktif
 * lewat env RBOT_PROFILE), sesi ini SELALU aktif dan hanya dipakai oleh
 * `rbot profile` — command yang tugasnya memang mengukur. Mesin timing
 * tetap prof (profNow) — bukan subsistem timing kedua.
 *
 * Label = string literal pemanggil (tidak disalin, pola profMark).
 * Byte in/out opsional; 0 berarti tidak dicetak di laporan rinci.
 */
typedef struct {
  const char *label;
  double ms;
  uint64_t inBytes;
  uint64_t outBytes;
} ProfPhase;

void profSessionReset(void);            /* kosongkan fase sesi */
void profPhaseBegin(const char *label); /* mulai fase (tandai waktu) */
void profPhaseEnd(uint64_t inBytes, uint64_t outBytes); /* akhiri + rekam */
/* Fase sesi terakhir (buffer statis; count boleh NULL). */
const ProfPhase *profPhases(int *count);

#endif /* RBOT_PROF_H */
