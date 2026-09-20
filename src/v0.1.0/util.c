#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"

/* ==================== List & string helpers ==================== */

void listAdd(List *l, const char *s) {
  if (!l || !s || !*s || l->count >= MAX_LIST) return;
  l->items[l->count++] = strdup(s);
}

void copyStr(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%s", src ? src : "");
}

char *trim(char *s) {
  while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
    s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
    --end;
  *end = '\0';
  return s;
}

bool parseBool(const char *s, bool *out) {
  if (!s || !out) return false;
  if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "on") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 || strcmp(s, "off") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/* ==================== Filesystem helpers ==================== */

void mkdirs(const char *path) {
  char tmp[MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      fsMakeDir(tmp);
      *p = '/';
    }
  }
  fsMakeDir(tmp);
}

void mkparent(const char *path) {
  char tmp[MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
  if (!slash) return;
  *slash = '\0';
  mkdirs(tmp);
}

/* Recursively collect paths under `dir` whose name ends with `ext`.
   With ext == "" every regular file matches; with ext == "/" only
   directories are collected (used by fsRemoveTree). */
void walkDir(const char *dir, const char *ext, List *out) {
  List dirs = {0}, files = {0};
  fsListDir(dir, &dirs, &files);
  if (ext[0] == '/') {
    for (int i = 0; i < dirs.count; i++) listAdd(out, dirs.items[i]);
    for (int i = 0; i < dirs.count; i++) walkDir(dirs.items[i], ext, out);
    return;
  }
  for (int i = 0; i < files.count; i++) {
    const char *path = files.items[i];
    size_t len = strlen(path), elen = strlen(ext);
    if (len > elen && strcmp(path + len - elen, ext) == 0) listAdd(out, path);
  }
  for (int i = 0; i < dirs.count; i++) walkDir(dirs.items[i], ext, out);
}

bool newerThan(const char *a, const char *b) { return fsNewerThan(a, b); }

bool safeRelative(const char *path) {
  if (!path || !*path) return false;
  if (path[0] == '/' || path[0] == '\\') return false;
  if (path[0] && path[1] == ':') return false; /* drive Windows: C:\... */
  if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0) return false;
  if (strstr(path, "..")) return false;
  return true;
}

/* ==================== Proses & waktu ==================== */

bool runCmd(const char *cmd) { return procRun(cmd); }

double nowSeconds(void) { return monotonicSeconds(); }
