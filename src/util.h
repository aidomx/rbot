#ifndef RBOT_V0_1_0_UTIL_H
#define RBOT_V0_1_0_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ==================== Batas ukuran umum ==================== */

/* MAX_PATH Windows = 260 (windows.h). Definisi kita hanya bila belum ada
   agar tidak bentrok (C4005) — POSIX memakai 1024 untuk path panjang. */
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif

/* Daftar string dinamis sederhana (dipakai untuk sources, flags, dst). */
typedef struct {
  char **items;
  int count;
  int capacity;
} List;

bool listAdd(List *l, const char *s);
void listFree(List *l);
void copyStr(char *dst, size_t n, const char *src);

char *trim(char *s);
bool parseBool(const char *s, bool *out);

/* popen(cmd, "r")/pclose portabel — MSVC tidak punya keduanya (POSIX);
   di Windows diimplementasikan via _popen/_pclose (cmd.exe). */
FILE *popenRB(const char *cmd);
void pcloseRB(FILE *fp);

/* ==================== Filesystem ==================== */

void mkdirs(const char *path);
void mkparent(const char *path);
void walkDir(const char *dir, const char *ext, List *out);
bool newerThan(const char *a, const char *b);
bool safeRelative(const char *path);

/* ==================== Proses & waktu ==================== */

bool runCmd(const char *cmd);
double nowSeconds(void);

#endif /* RBOT_V0_1_0_UTIL_H */
