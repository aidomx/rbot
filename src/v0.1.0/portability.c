#include "portability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ==================== Util path ==================== */

void pathNormalizeSlash(char *s) {
  for (; s && *s; s++)
    if (*s == '\\') *s = '/';
}

#ifdef _WIN32

/* Ganti '/' ke '\' — hanya untuk API Win32 yang menolak '/'. Jangan dipakai
   untuk command line: cmd.exe menerima '/' di path, sedangkan switch-nya
   (mis. /I, /Fo, /link) justru rusak bila diubah jadi '\'. */
static void toBackslash(char *dst, size_t n, const char *src) {
  size_t j = 0;
  for (const char *p = src; *p && j + 1 < n; p++, j++) dst[j] = (*p == '/') ? '\\' : *p;
  dst[j] = '\0';
}

/* ==================== Windows (MinGW & MSVC) ==================== */

/* ========== Proses ========== */

bool probeAvailable(const char *exe) {
  if (!exe || !*exe) return false;
  if (strchr(exe, '/') || strchr(exe, '\\')) return fsFileExists(exe);

  /* PATH lengkap di Windows; mencoba sufiks .exe/.bat/.cmd.
     Eksekusi tetap lewat cmd.exe (procRun), jadi "cl" dari Developer
     Command Prompt ikut terdeteksi lewat PATH lingkungan itu. */
  static const char *kSuffixes[] = {"", ".exe", ".bat", ".cmd", NULL};
  const char *pathvar = getenv("PATH");
  if (!pathvar) return false;
  const char *p = pathvar;
  while (*p) {
    const char *semi = strchr(p, ';');
    size_t len = semi ? (size_t)(semi - p) : strlen(p);
    if (len > 0 && len < MAX_PATH) {
      char dir[MAX_PATH];
      memcpy(dir, p, len);
      dir[len] = '\0';
      /* buang tanda kutip yang kadang menyelimuti entri PATH Windows */
      if (dir[0] == '"') {
        size_t dl = strlen(dir);
        if (dl && dir[dl - 1] == '"') {
          dir[dl - 1] = '\0';
          memmove(dir, dir + 1, dl - 1);
        }
      }
      for (int s = 0; kSuffixes[s]; s++) {
        char full[MAX_PATH + 128];
        snprintf(full, sizeof(full), "%s/%s%s", dir, exe, kSuffixes[s]);
        if (fsFileExists(full)) return true;
      }
    }
    if (!semi) break;
    p = semi + 1;
  }
  return false;
}

bool procRun(const char *cmd) {
  /* Jalankan apa adanya via cmd.exe: '/' di path diterima, dan switch
     compiler (/I, /Fo, /link) tetap utuh. system() sudah mengembalikan
     exit code penuh di MSVCRT. */
  int status = system(cmd);
  return status == 0;
}

double monotonicSeconds(void) {
  LARGE_INTEGER freq, counter;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter)) return 0.0;
  return (double)counter.QuadPart / (double)freq.QuadPart;
}

/* ========== Filesystem ========== */

static bool statInfo(const char *path, WIN32_FILE_ATTRIBUTE_DATA *fad) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return GetFileAttributesExA(tmp, GetFileExInfoStandard, fad) != 0;
}

bool fsFileExists(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return false;
  return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool fsDirExists(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return false;
  return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static time_t filetimeToTimeT(const FILETIME *ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  if (u.QuadPart == 0) return (time_t)-1;
  /* 100ns sejak 1601 -> detik sejak epoch UNIX */
  return (time_t)((u.QuadPart / 10000000ULL) - 11644473600ULL);
}

time_t fsMTime(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return (time_t)-1;
  return filetimeToTimeT(&fad.ftLastWriteTime);
}

long long fsFileSize(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return -1;
  ULARGE_INTEGER u;
  u.LowPart = fad.nFileSizeLow;
  u.HighPart = fad.nFileSizeHigh;
  return (long long)u.QuadPart;
}

void fsMakeDir(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  CreateDirectoryA(tmp, NULL); /* diam bila sudah ada */
}

bool fsRemoveFile(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return DeleteFileA(tmp) != 0;
}

bool fsRemoveTree(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  /* SHFileOperation butuh path dobel-NUL-terminated */
  char doubled[MAX_PATH * 2 + 2];
  size_t len = strlen(tmp);
  if (len + 2 > sizeof(doubled)) return false;
  memcpy(doubled, tmp, len + 1);
  doubled[len + 1] = '\0';

  SHFILEOPSTRUCTA op;
  memset(&op, 0, sizeof(op));
  op.hwnd = NULL;
  op.wFunc = FO_DELETE;
  op.pFrom = doubled;
  op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  return SHFileOperationA(&op) == 0;
}

void fsListDir(const char *dir, List *dirs, List *files) {
  char pattern[MAX_PATH * 2];
  toBackslash(pattern, sizeof(pattern), dir);
  size_t plen = strlen(pattern);
  if (plen + 3 > sizeof(pattern)) return;
  pattern[plen] = '\\';
  pattern[plen + 1] = '*';
  pattern[plen + 2] = '\0';

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const char *name = fd.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (dirs) listAdd(dirs, path);
    } else {
      if (files) listAdd(files, path);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

bool fsGetCwd(char *out, size_t n) {
  char tmp[MAX_PATH * 2];
  DWORD len = GetCurrentDirectoryA((DWORD)sizeof(tmp), tmp);
  if (len == 0 || len > sizeof(tmp)) return false;
  pathNormalizeSlash(tmp);
  if ((size_t)len >= n) return false;
  memcpy(out, tmp, len + 1);
  return true;
}

bool fsSetCwd(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return SetCurrentDirectoryA(tmp) != 0;
}

#else /* !_WIN32 */

/* ==================== POSIX ==================== */

/* ========== Proses ========== */

bool probeAvailable(const char *exe) {
  if (!exe || !*exe) return false;
  if (exe[0] == '/') return fsFileExists(exe);

  /* Eksplisit menghindari system("command -v ...") — system() menelan
     exit code pada beberapa libc, dan "command" bukan binary biasa. */
  const char *pathvar = getenv("PATH");
  if (!pathvar) pathvar = "/usr/bin:/bin";
  const char *p = pathvar;
  while (*p) {
    const char *colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len > 0 && len < MAX_PATH) {
      char dir[MAX_PATH];
      memcpy(dir, p, len);
      dir[len] = '\0';
      char full[MAX_PATH + 128];
      snprintf(full, sizeof(full), "%s/%s", dir, exe);
      if (fsFileExists(full)) return true;
    }
    if (!colon) break;
    p = colon + 1;
  }
  return false;
}

bool procRun(const char *cmd) {
  int status = system(cmd);
  if (status == -1) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

double monotonicSeconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ========== Filesystem ========== */

static bool statMode(const char *path, mode_t *mode) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
  *mode = st.st_mode;
  return true;
}

bool fsFileExists(const char *path) {
  mode_t m;
  return statMode(path, &m) && S_ISREG(m);
}

bool fsDirExists(const char *path) {
  mode_t m;
  return statMode(path, &m) && S_ISDIR(m);
}

time_t fsMTime(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return (time_t)-1;
  return st.st_mtime;
}

long long fsFileSize(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long long)st.st_size;
}

void fsMakeDir(const char *path) { mkdir(path, 0755); }

bool fsRemoveFile(const char *path) { return unlink(path) == 0; }

bool fsRemoveTree(const char *path) {
  if (!fsDirExists(path)) {
    /* bukan direktori: hapus sebagai file biasa */
    return fsFileExists(path) ? fsRemoveFile(path) : true;
  }
  List dirs = {0}, files = {0};
  fsListDir(path, &dirs, &files);
  bool ok = true;
  for (int i = 0; i < files.count; i++) {
    if (!fsRemoveFile(files.items[i])) ok = false;
  }
  /* hapus subdirektori paling dalam dulu (list dari fsListDir tidak
     terjamin urut, jadi recurse; urutan aman karena anak dihapus sebelum
     induknya lewat rekursi fsRemoveTree sendiri) */
  for (int i = 0; i < dirs.count; i++) {
    if (!fsRemoveTree(dirs.items[i])) ok = false;
  }
  return ok && rmdir(path) == 0;
}

void fsListDir(const char *dir, List *dirs, List *files) {
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
      if (dirs) listAdd(dirs, path);
    } else {
      if (files) listAdd(files, path);
    }
  }
  closedir(d);
}

bool fsGetCwd(char *out, size_t n) { return getcwd(out, n) != NULL; }

bool fsSetCwd(const char *path) { return chdir(path) == 0; }

#endif /* !_WIN32 */

/* ==================== Portabel (kedua OS) ==================== */

bool fsNewerThan(const char *a, const char *b) {
  time_t ta = fsMTime(a);
  if (ta == (time_t)-1) return false;
  time_t tb = fsMTime(b);
  if (tb == (time_t)-1) return true;
  return ta > tb;
}
