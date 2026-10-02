#ifndef RBOT_V0_1_0_USES_WORKSPACE_H
#define RBOT_V0_1_0_USES_WORKSPACE_H

#include <stdbool.h>

/*
 * workspace — mode multi-proyek (use workspace / Buildfile.workspace).
 *
 * Implementasi dipisah dari use.c supaya use.c tetap satu titik dispatcher
 * kecil; semua logika daftar proyek, topo sort depends_on, dan sintesis
 * Buildfile ada di workspace.c.
 */

/* Nama file workspace di root workspace. */
#define WORKSPACE_FILENAME "Buildfile.workspace"

/* Folder tempat Buildfile hasil sintesis disimpan (stabil, di root
   workspace) agar cache config & fast path tidak miss tiap run. */
#define WORKSPACE_SYNTH_DIR ".rbot/workspace"

/*
 * Jalankan mode workspace untuk command `cmd` ("build"/"clean").
 * only: NULL/"" = semua proyek sesuai urutan depends_on; selain itu hanya
 * proyek itu (+ dependency-nya). Return code gaya main(): 0 sukses, 1
 * gagal, 130 interupsi.
 */
int workspaceRun(const char *cmd, int jobs, const char *only);

/* true bila Buildfile.workspace ada di cwd. */
bool workspaceFileExists(void);

#endif /* RBOT_V0_1_0_USES_WORKSPACE_H */
