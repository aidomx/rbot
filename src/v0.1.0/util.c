#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

void mkparent(const char *path) {
  char tmp[MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
  if (!slash) return;
  *slash = '\0';
  mkdirs(tmp);
}

void walkDir(const char *dir, const char *ext, List *out) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      walkDir(path, ext, out);
      continue;
    }
    size_t len = strlen(path), elen = strlen(ext);
    if (len > elen && strcmp(path + len - elen, ext) == 0) listAdd(out, path);
  }
  closedir(d);
}

bool newerThan(const char *a, const char *b) {
  struct stat sa, sb;
  if (stat(a, &sa) != 0) return false;
  if (stat(b, &sb) != 0) return true;
  return sa.st_mtime > sb.st_mtime;
}

bool safeRelative(const char *path) {
  if (!path || !*path) return false;
  if (path[0] == '/') return false;
  if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0) return false;
  if (strstr(path, "..")) return false;
  return true;
}

/* ==================== Proses & waktu ==================== */

bool runCmd(const char *cmd) {
  int status = system(cmd);
  if (status == -1) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

double nowSeconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
