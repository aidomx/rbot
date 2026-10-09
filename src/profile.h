#ifndef RBOT_PROFILE_H
#define RBOT_PROFILE_H

/*
 * profile — `rbot profile <context> [key=value ...] [--report <file>]`
 * (design/profile.md).
 *
 * Antarmuka diagnostik: mengukur biaya nyata operasi rbot pada direktori
 * yang sudah ada, TANPA Buildfile dan TANPA menyentuh state build normal
 * (non-mutating; keluaran operasi ditulis ke direktori sementara yang
 * terisolasi dan dihapus setelah pengukuran).
 *
 * Context (semua context design "Prioritas Awal" diimplementasikan):
 *   archive=<dir> [with=tar|tar,gz|tar,xz|tar,bz2] — discover/collect/tar/kompresi
 *   embed=<dir|file>                — discover/collect/(tar)/ld
 *   library=<dir> [with=static|shared] — discover/collect/ar(/shared)
 *   binary=<dir> [jobs=N]           — config/gather/compile/link (terisolasi)
 *   project=<dir> [jobs=N]          — config/fingerprint/decide/compile/link
 *                                     (fase incremental; sandbox deps)
 *   workspace=<dir>                 — parse/dependency/library (baca
 *                                     model Buildfile.ws; fase build
 *                                     tidak bermakna diisolasi -> tak
 *                                     dikarang)
 * Opsi global: --report <file>, --repeat N (1..100; tiap run sandbox
 * fresh, fase hasil bolak-balik diakumulasi).
 *
 * with= = pemilih backend/operasi yang bermakna per context (design: opsi
 * tidak harus valid global). Untuk archive ini alat pembanding kompresi
 * (gzip vs xz vs bz2) — biaya & ukuran hasil terukur per fase.
 *
 * Output: stdout TTY = ringkasan singkat ("apa yang mahal?"); non-TTY
 * (pipe/redirect, mis. `rbot profile archive=. > profile.md`) = laporan
 * rinci. `--report <file>` menulis laporan rinci ke file, stdout tetap
 * ringkas (pembuatan file tidak ikut terukur).
 */
int cmdProfile(const char *const *args, int nargs);

#endif /* RBOT_PROFILE_H */
