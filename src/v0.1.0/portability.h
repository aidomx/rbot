#ifndef RBOT_V0_1_0_PORTABILITY_H
#define RBOT_V0_1_0_PORTABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "util.h"

/*
 * portability — lapisan tipis di atas API OS supaya rbot bisa dibangun di
 * Linux/POSIX maupun Windows (MinGW & MSVC). Seluruh panggilan yang
 * POSIX-only (dirent.h, unistd.h, command -v, rm -rf, dst.) dipindahkan ke
 * sini; modul lain cukup memakai API netral di bawah.
 *
 * Konvensi path: rbot secara internal selalu memakai '/' (diterima GCC,
 * Clang, maupun MSVC di Windows), jadi lapisan ini menormalkan hasil OS
 * (mis. GetCurrentDirectory memberi '\\') lewat pathNormalizeSlash().
 */

void pathNormalizeSlash(char *s);

/* ==================== Proses & waktu ==================== */

/* true bila `exe` bisa dieksekusi: path eksplisit, atau ada di PATH
   (di Windows otomatis mencoba sufiks .exe/.bat/.cmd). */
bool probeAvailable(const char *exe);

/* Jalankan command lewat shell OS (sh / cmd.exe); true bila exit 0. */
bool procRun(const char *cmd);

/* Detik monotonic untuk pengukuran durasi build. */
double monotonicSeconds(void);

/* ==================== Filesystem ==================== */

void fsMakeDir(const char *path); /* mkdir satu level, diam bila sudah ada */
bool fsFileExists(const char *path);
bool fsDirExists(const char *path);
time_t fsMTime(const char *path); /* (time_t)-1 bila tidak ada */
long long fsFileSize(const char *path);
bool fsNewerThan(const char *a, const char *b);
bool fsRemoveFile(const char *path);
bool fsRemoveTree(const char *path); /* rm -rf portabel */

/* Satu level isi direktori; subdirektori ke `dirs`, file biasa ke `files`
   (boleh NULL). Path hasil berformat '/' konsisten. */
void fsListDir(const char *dir, List *dirs, List *files);

bool fsGetCwd(char *out, size_t n);
bool fsSetCwd(const char *path);

#endif /* RBOT_V0_1_0_PORTABILITY_H */
