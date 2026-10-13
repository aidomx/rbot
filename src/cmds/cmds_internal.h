#ifndef RBOT_V0_1_0_CMDS_INTERNAL_H
#define RBOT_V0_1_0_CMDS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../config.h" /* Config, List */

/* Ukuran buffer path untuk state workspace. WS_SCAN = WS_PATH_LEN - 1 sebagai teks
   lebar sscanf: lebar baca selalu lebih kecil dari buffer tujuan (-Wfortify-source). */
#define WS_PATH_LEN 3072
#define WS_SCAN "3071"

/*
 * cmds_internal — helper bersama antar-file modul cmds/.
 *
 * commands.c menyimpan command publik (cmdBuild/cmdInit/showHelp; cmdClean
 * ada di cmds_state.c). File di sini menyimpan fase-fase build yang
 * dipisah dari commands.c: fingerprint + fast no-op snapshot (cmds_state.c)
 * dan fase library/kompilasi paralel/emit header versi (cmds_lib.c).
 * Semua dulunya static di commands.c — di-prefix cmds* agar unik.
 */

/* ==================== cmds_lib.c ==================== */

/* Nama file library sesuai toolchain: lib<name>.a / <name>.lib (statis),
   lib<name>.so / .dylib / <name>.dll (dinamis). */
void cmdsLibStaticPath(const Config *c, char *out, size_t n);
void cmdsLibSharedPath(const Config *c, char *out, size_t n);

/* True bila SATU varian library (statis/shared) sudah ada dan lebih baru
   daripada seluruh object inputnya — varian itu boleh dilewati. */
bool cmdsLibVariantUpToDate(const Config *c, bool isStatic, const List *srcs);

/* Fase library: kemas statis (ar / lib) dan/atau link shared dari object
   source + embedded. true bila semua varian yang diminta sukses.
   linkPathLibs=false (fase library workspace) melewatkan entri library
   ber-path proyek lain pada link shared (lihat cmds_lib.c). */
bool cmdsBuildLibraryEx(const Config *c, const List *srcs, bool linkPathLibs);

/* Build penuh (fase normal binary): library lintas-proyek disertakan. */
bool cmdsBuildLibrary(const Config *c, const List *srcs);

/* Kompilasi paralel (-jN). Return false bila ada yang gagal/interrupt.
   quiet=true menekan baris status per job (dipakai `rbot profile`, yang
   outputnya harus bersih agar laporan tidak tercampur log kompilasi). */
bool cmdsRunParallelJobs(const Config *c, const char *inc, const char *wf, const List *srcs,
                         char *objPath, size_t objCap, int jobs, int *outCompiled,
                         int *outFailed, int *outInterrupted, bool quiet);

/* Terbitkan build/version.h dari .rbot-version (root project). */
bool cmdsEmitVersionHeader(const Config *c, bool *changed);

/* ============ fase pack (proyek binary + pack.*) ============ */

/*
 * Jalankan packRun untuk proyek binary dengan pack.*: dipanggil di KEDUA
 * jalur sukses cmdBuild, setelah fingerprint disimpan. Return false bila
 * kemasan gagal (state tidak direkam — build berikutnya mengulang penuh).
 * *fastStateSaved diset true bila state sudah disimpan di sini; proyek
 * tanpa pack membiarkannya false dan pemanggil menyimpan seperti biasa.
 */
bool cmdsPackAfterBinary(const Config *c, bool *fastStateSaved, const List *srcs,
                         const char *target, const char *buildfilePath);

/* Proyek kemasan murni (output.binary = false + pack.*): embedded dulu
   (arsip untuk proyek lain), lalu packaging, lalu summary ringkas. */
int cmdsPackOnlyProject(const Config *c);

/* ==================== cmds_state.c ==================== */

typedef struct {
  uint64_t compile;
  uint64_t link;
  uint64_t sources;
} CmdsBuildFingerprint;

/* Hash konfigurasi compile/link + set source (FNV-1a). */
void cmdsFingerprintMake(const Config *c, const List *srcs, CmdsBuildFingerprint *fp);
bool cmdsFingerprintLoad(CmdsBuildFingerprint *fp);
/* Simpan ke .rbot/build.fingerprint. libOnly = fase library workspace:
   hanya hash compile yang dikommit; hash sources/link dipertahankan dari
   catatan sebelumnya agar pass binary run yang sama masih mendeteksi
   source set / link config berubah (source dihapus -> link diulang). */
void cmdsFingerprintSave(const CmdsBuildFingerprint *fp, bool libOnly);

/* Buang fingerprint tersimpan (dipanggil saat build gagal/terinterupsi
   setelah konfigurasi berubah). */
void cmdsFingerprintInvalidate(void);

/* Fast no-op: true bila state build terakhir masih persis sama — summary
   diulang dari cache dan build dilewati sepenuhnya. libOnly memilih file
   state fase library (.rbot/build.lib.state); fase binary memakai
   .rbot/build.state. */
bool cmdsFastStateValid(const char *buildfilePath, bool libOnly);

/* Jalur fast-state workspace (cmds_wsstate.c). Aktif hanya saat workspace
   memanggil cmdBuildEx; single project tidak pernah mengaktifkannya. */
struct DepCache; /* forward: didefinisikan di deps.h */
void wsModeSet(bool on);
bool wsFastStateValid(const char *buildfilePath, bool libOnly);
bool cmdsStatCached(const char *path, int64_t *mtimeNs, long long *size);
void cmdsStatMemoSetCwd(void);
void cmdsStatMemoForget(const char *path);
int64_t cmdsMTimeDecide(const char *path);
/* Parser & validasi satu baris format state F/D/X (format bersama ws.state,
   ws.lib.state dan workspace.state — lihat header cmds_wsstate.c):
     F <mtime_ns> <size> <path> | D <mtime_ns> 0 <path> | X 0 0 <path>
   path harus menampung WS_PATH_LEN karakter (lebar WS_SCAN). */
bool cmdsParseStateEntry(const char *line, char *kind, long long *mt, long long *sz, char *path);
/* true bila baris F/D/X masih cocok stamp disk saat ini (F: mtime+size,
   D: mtime, X: path tidak ada); false bila parse gagal atau stamp beda. */
bool cmdsStateEntryFresh(const char *line);
void cmdsStatMemoForgetTree(const char *dir);
void wsWalkDirCached(const char *dir, const char *ext, List *out);
void wsWalkDirStamps(const char *dir, const char *ext, List *out);
bool wsModeActive(void);
void wsFastStateSave(struct DepCache *dc, const Config *c, const List *srcs, const char *target,
                     const char *buildfilePath, bool libOnly);
/* Kosongkan memo stat validasi fast-state; dipanggil sebelum build mengubah file. */
void cmdsStatMemoReset(void);

/* Rekam state build sukses (mtime+size seluruh input) untuk fast no-op.
   target = "" (fase library tanpa library) direkam sebagai "-" — validasi
   murni dari stamp input + object. */
void cmdsFastStateSave(const Config *c, const List *srcs, const char *target,
                       const char *buildfilePath, bool libOnly);

#endif /* RBOT_V0_1_0_CMDS_INTERNAL_H */
