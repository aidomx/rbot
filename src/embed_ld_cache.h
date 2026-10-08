#ifndef RBOT_V0_1_0_EMBED_LD_CACHE_H
#define RBOT_V0_1_0_EMBED_LD_CACHE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * embed_ld_cache — cache persisten hasil deteksi GNU ld (.rbot/ld.cache).
 *
 * ldAvailable() (embed.c) menjalankan probe `ld --version` paling sering
 * sekali per proses — hasilnya (g_ldAvailable) mati begitu rbot keluar,
 * jadi setiap run yang membangun proyek ber-`embedded` men-spawn sh + ld
 * lagi (3–5 ms per run; lihat problem_note.txt P1). Modul ini menyimpan
 * verdict probe lintas-run: run berikutnya cukup SATU stat ke binary ld
 * hasil resolusi PATH (path + mtime ns + size asli) untuk memvalidasi —
 * tanpa spawn.
 */

/* Resolusi binary `exe` (mis. "ld") di PATH, semantik setara probeAvailable.
   Path tersimpan di `out` (kapasitas n) sebagai key cache; false bila
   tidak ditemukan. Hanya ada di POSIX — cache tidak dipakai di Windows.
*/
bool ldCacheResolve(const char *exe, char *out, size_t n);

/* Baca verdict tersimpan; hanya return true bila binary di `ldPath` masih
   persis yang diperiksa (path + mtime ns + size cocok — satu stat).
   false = cache tidak ada/rusak/key beda → pemanggil menjalankan probe
   fisik sendiri lalu menyimpannya via cacheWriteLD. */
bool cacheReadLD(const char *ldPath, bool *out);

/* Fast cache check: validate .rbot/ld.cache for an already-resolved ld; never probes. */
bool cacheCheckLD(const char *ldPath, bool *out);

/* Update cache for an already-resolved ld using an already-probed verdict. */
void cacheUpdateLD(const char *ldPath, bool gnu);

/* Simpan verdict probe (gnu=true berarti GNU ld) terkait binary ldPath.
   Diam bila stat path gagal; tulis atomik (file .tmp lalu rename). */
void cacheWriteLD(const char *ldPath, bool gnu);

/* Nama file cache (".rbot/ld.cache") — dipakai cmdClean saat menghapus
   state build. */
const char *ldCacheFile(void);

#endif /* RBOT_V0_1_0_EMBED_LD_CACHE_H */
