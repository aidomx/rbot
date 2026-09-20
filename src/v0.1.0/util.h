#ifndef RBOT_V0_1_0_UTIL_H
#define RBOT_V0_1_0_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* ==================== Batas ukuran umum ==================== */

#define MAX_LIST 512
#define MAX_PATH 1024

/* Daftar string dinamis sederhana (dipakai untuk sources, flags, dst). */
typedef struct {
  char *items[MAX_LIST];
  int count;
} List;

void listAdd(List *l, const char *s);
void copyStr(char *dst, size_t n, const char *src);

char *trim(char *s);
bool parseBool(const char *s, bool *out);

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
