#ifndef RBOT_V0_1_0_USES_WORKSPACE_H
#define RBOT_V0_1_0_USES_WORKSPACE_H

#include <stdbool.h>

/*
 * workspace — mode multi-proyek (use workspace / Buildfile.ws).
 *
 * Implementasi dipisah dari use.c supaya use.c tetap satu titik dispatcher
 * kecil; semua logika daftar proyek, topo sort depends_on, dan sintesis
 * Buildfile ada di workspace.c.
 */

/* Nama file workspace di root workspace. */
#define WORKSPACE_FILENAME "Buildfile.ws"

/* Batas model workspace (dipakai workspace.c & profile workspace=). */
#define WS_MAX_PROJECTS 16
#define WS_MAX_RELEASES 8

/* Nama lama (sebelum 3 Okt 2026) — masih diterima bila Buildfile.ws
   tidak ada, supaya workspace lama tetap jalan tanpa rename. */
#define WORKSPACE_FILENAME_LEGACY "Buildfile.workspace"

/* Folder tempat Buildfile hasil sintesis disimpan (stabil, di root
   workspace) agar cache config & fast path tidak miss tiap run. */
#define WORKSPACE_SYNTH_DIR ".rbot/workspace"

/*
 * Jalankan mode workspace untuk command `cmd` ("build"/"clean").
 * only: NULL/"" = semua proyek sesuai urutan depends_on; selain itu hanya
 * proyek itu (+ dependency-nya).
 * releaseSel: `-- key=value[,key=value...]` (mis. "name=rupa") — release
 * via CLI: konfigurasi boleh penuh dari CLI tanpa deklarasi `releases`
 * di Buildfile.ws. `name=<proyek>` wajib (error bila proyek tidak ada;
 * release implisit dibuat bila proyek ada tapi belum dideklarasikan);
 * `target=<tar|deb>` override format kemasan. NULL/"" = build + release
 * seluruh workspace (dist/release).
 * Return code gaya main(): 0 sukses, 1 gagal, 130 interupsi.
 */
int workspaceRun(const char *cmd, int jobs, const char *only, const char *releaseSel);

/*
 * workspaceEnumerate — baca+validasi Buildfile.ws di `wsDir` (parse, topo
 * sort, validasi release) lalu ENUMErasikan saja: cekBuildfile menunjukkan
 * apakah Buildfile project `<wsDir>/<name>/Buildfile` atau file `.Buildfile`
 * hasil sintesis ada (dipakai `rbot profile workspace=` untuk menghitung
 * proyek tanpa menjalankan build apa pun).
 * projectNames[count] diisi pointer ke buffer internal model (valid selama
 * panggilan; TIDAK perlu dibebaskan oleh pemanggil).
 * Return 0 sukses; 1 bila file tidak ada / parse / validasi gagal.
 */
int workspaceEnumerate(const char *wsDir, int *count, const char **projectNames,
                       int maxProjects);

/* true bila Buildfile.ws (atau nama lama Buildfile.workspace) ada di cwd. */
bool workspaceFileExists(void);

/* Nama file workspace yang terdeteksi di cwd — WORKSPACE_FILENAME dulu,
   lalu WORKSPACE_FILENAME_LEGACY; NULL bila tak ada keduanya. Dipakai
   supaya pesan error & sintesis menyebut nama file yang benar-benar
   dipakai. */
const char *workspaceFileName(void);

#endif /* RBOT_V0_1_0_USES_WORKSPACE_H */
