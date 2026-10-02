/*
 * port_fs — lapisan FILESYSTEM dari portability (kedua OS).
 *
 * Dipindah apa adanya dari portability.c: stat/exists/mtime/size,
 * mkdir/remove (termasuk rm -rf), listdir, dan cwd. Satu-satunya helper
 * internal adalah toBackslash() untuk API Win32 yang menolak '/'.
 * Deklarasi publik tetap di portability.h.
 */
#include "../portability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32

/* ========== Windows: Filesystem ========== */

/* Ganti '/' ke '\\' — hanya untuk API Win32 yang menolak '/'. Jangan dipakai
   untuk command line: cmd.exe menerima '/' di path, sedangkan switch-nya
   (mis. /I, /Fo, /link) justru rusak bila diubah jadi '\\'. */
static void toBackslash(char *dst, size_t n, const char *src) {
  size_t j = 0;
  for (const char *p = src; *p && j + 1 < n; p++, j++) dst[j] = (*p == '/') ? '\\' : *p;
  dst[j] = '\0';
}

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

int64_t fsMTimeNs(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return -1;
  ULARGE_INTEGER u;
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  if (u.QuadPart == 0) return -1;
  /* FILETIME is already expressed in 100ns units since 1601. */
  return (int64_t)u.QuadPart * 100LL;
}

long long fsFileSize(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return -1;
  ULARGE_INTEGER u;
  u.LowPart = fad.nFileSizeLow;
  u.HighPart = fad.nFileSizeHigh;
  return (long long)u.QuadPart;
}

bool fsStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!statInfo(path, &fad)) return false;
  ULARGE_INTEGER u;
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  if (u.QuadPart == 0) return false;
  *mtimeNs = (int64_t)u.QuadPart * 100LL;
  u.LowPart = fad.nFileSizeLow;
  u.HighPart = fad.nFileSizeHigh;
  *size = (long long)u.QuadPart;
  return true;
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

bool fsSetMode(const char *path, unsigned mode) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  return _chmod(tmp, (mode_t)mode) == 0;
}

unsigned fsGetMode(const char *path) {
  char tmp[MAX_PATH * 2];
  toBackslash(tmp, sizeof(tmp), path);
  struct _stat st;
  if (_stat(tmp, &st) != 0) return 0;
  return (unsigned)st.st_mode & (unsigned)(_S_IREAD | _S_IWRITE | _S_IEXEC);
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

/* ========== POSIX: Filesystem ========== */

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

int64_t fsMTimeNs(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
  return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

long long fsFileSize(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long long)st.st_size;
}

bool fsStampNsSize(const char *path, int64_t *mtimeNs, long long *size) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
#if defined(__APPLE__)
  *mtimeNs = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  *mtimeNs = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  *size = (long long)st.st_size;
  return true;
}

void fsMakeDir(const char *path) { mkdir(path, 0755); }

bool fsRemoveFile(const char *path) { return unlink(path) == 0; }

bool fsSetMode(const char *path, unsigned mode) { return chmod(path, (mode_t)mode) == 0; }

unsigned fsGetMode(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return (unsigned)st.st_mode & 0777u;
}

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
  listFree(&dirs);
  listFree(&files);
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
    bool isDir;
#ifdef DT_DIR
    /* d_type sudah diberikan readdir: tanpa stat per entri (satu syscall per
       file/direktori yang dipindai). DT_UNKNOWN/DT_LNK/lainnya jatuh ke stat
       agar symlink tetap diikuti seperti sebelumnya. */
    if (ent->d_type == DT_DIR) {
      isDir = true;
    } else if (ent->d_type == DT_REG) {
      isDir = false;
    } else
#endif
    {
      struct stat st;
      if (stat(path, &st) != 0) continue;
      isDir = S_ISDIR(st.st_mode);
    }
    if (isDir) {
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
  int64_t ta = fsMTimeNs(a);
  if (ta < 0) return false;
  int64_t tb = fsMTimeNs(b);
  if (tb < 0) return true;
  return ta > tb;
}
