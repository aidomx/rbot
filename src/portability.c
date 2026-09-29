#include "portability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#endif

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

/* ---------- Status Ctrl+C & registry child ---------- */

static volatile bool g_wantInterrupt = false;
static bool g_foregroundMode = true;
static bool g_handlerAttached = false;

/* Registry pid child yang masih berjalan — dibaca console handler (dari
   thread lain) untuk meneruskan CTRL_BREAK ke process group child. */
#define PROC_MAX_TRACKED 256
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static DWORD g_pids[PROC_MAX_TRACKED];
static int g_pidCount = 0;

static void procEnsureCS(void) {
  if (!g_csInit) {
    InitializeCriticalSection(&g_cs);
    g_csInit = true;
  }
}

static void procRegisterPid(DWORD pid) {
  if (!pid) return;
  procEnsureCS();
  EnterCriticalSection(&g_cs);
  if (g_pidCount < PROC_MAX_TRACKED) g_pids[g_pidCount++] = pid;
  LeaveCriticalSection(&g_cs);
}

static void procUnregisterPid(DWORD pid) {
  procEnsureCS();
  EnterCriticalSection(&g_cs);
  for (int i = 0; i < g_pidCount; i++) {
    if (g_pids[i] == pid) {
      g_pids[i] = g_pids[--g_pidCount];
      break;
    }
  }
  LeaveCriticalSection(&g_cs);
}

static BOOL WINAPI procConsoleHandler(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
  g_wantInterrupt = true;
  /* CREATE_NEW_PROCESS_GROUP membuat compiler terpisah dari grup rbot,
     jadi terminal tidak menghentikannya langsung; teruskan CTRL_BREAK
     eksplisit ke setiap process group child yang masih berjalan. */
  procEnsureCS();
  EnterCriticalSection(&g_cs);
  DWORD pids[PROC_MAX_TRACKED];
  int n = g_pidCount;
  for (int i = 0; i < n; i++) pids[i] = g_pids[i];
  LeaveCriticalSection(&g_cs);
  for (int i = 0; i < n; i++)
    if (pids[i]) GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pids[i]);
  return TRUE;
}

static void procEnsureHandler(void) {
  if (g_handlerAttached) return;
  procEnsureCS();
  SetConsoleCtrlHandler(procConsoleHandler, TRUE);
  g_handlerAttached = true;
}

bool procSetForeground(bool foreground) {
  g_foregroundMode = foreground;
  g_wantInterrupt = false;
  procEnsureHandler();
  return true;
}

bool procInterrupted(void) { return g_wantInterrupt; }

/* Susun command line "cmd.exe /d /s /c <cmd>" — dipakai procRun & procStart. */
static bool procBuildShellCommand(const char *cmd, char *command, size_t n) {
  char shell[MAX_PATH];
  DWORD len = GetEnvironmentVariableA("COMSPEC", shell, sizeof(shell));
  if (len == 0 || len >= sizeof(shell)) strcpy(shell, "cmd.exe");
  int written = snprintf(command, n, "\"%s\" /d /s /c \"%s\"", shell, cmd);
  return written >= 0 && (size_t)written < n;
}

/* Buat proses lewat cmd.exe di process group terpisah (dilacak registry). */
static bool procCreate(const char *cmd, PROCESS_INFORMATION *pi) {
  STARTUPINFOA si;
  char command[MAX_PATH * 4];
  if (!procBuildShellCommand(cmd, command, sizeof(command))) return false;

  memset(&si, 0, sizeof(si));
  memset(pi, 0, sizeof(*pi));
  si.cb = sizeof(si);

  procEnsureHandler();
  if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                      &si, pi))
    return false;
  procRegisterPid(pi->dwProcessId);
  return true;
}

bool procRun(const char *cmd) {
  if (!cmd || !*cmd) return false;

  PROCESS_INFORMATION pi;
  if (!procCreate(cmd, &pi)) return false;

  DWORD wait = WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exitCode);

  procUnregisterPid(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return wait == WAIT_OBJECT_0 && exitCode == 0;
}

bool procStart(const char *cmd, ProcHandle *out) {
  if (!cmd || !*cmd || !out) return false;

  PROCESS_INFORMATION pi;
  if (!procCreate(cmd, &pi)) return false;
  CloseHandle(pi.hThread);

  out->pid = pi.dwProcessId;
  out->hProcess = (void *)pi.hProcess;
  out->finished = false;
  out->ok = false;
  out->interrupted = false;
  return true;
}

int procWaitAny(ProcHandle *handles, int count, ProcHandle **finished) {
  if (finished) *finished = NULL;
  if (!handles || count <= 0) return -1;

  for (;;) {
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    int idx[MAXIMUM_WAIT_OBJECTS];
    int n = 0;
    for (int i = 0; i < count && n < MAXIMUM_WAIT_OBJECTS; i++) {
      if (handles[i].finished) continue;
      hs[n] = (HANDLE)handles[i].hProcess;
      idx[n] = i;
      n++;
    }
    if (n == 0) return -1;

    DWORD w = WaitForMultipleObjects((DWORD)n, hs, FALSE, INFINITE);
    if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + (DWORD)n) {
      if (w == WAIT_FAILED) return -1;
      continue; /* WAIT_ABANDONED_0 dsb: tunggu lagi */
    }

    int i = idx[w - WAIT_OBJECT_0];
    DWORD exitCode = 1;
    GetExitCodeProcess((HANDLE)handles[i].hProcess, &exitCode);
    procUnregisterPid(handles[i].pid);
    CloseHandle((HANDLE)handles[i].hProcess);
    handles[i].hProcess = NULL;
    handles[i].finished = true;
    handles[i].ok = exitCode == 0;
    handles[i].interrupted = g_wantInterrupt && exitCode != 0;
    if (finished) *finished = &handles[i];
    return i;
  }
}

void procStopAll(ProcHandle *handles, int count) {
  if (!handles || count <= 0) return;

  for (int i = 0; i < count; i++) {
    if (handles[i].finished || !handles[i].pid) continue;
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, handles[i].pid);
  }

  for (int i = 0; i < count; i++) {
    if (handles[i].finished) continue;
    WaitForSingleObject((HANDLE)handles[i].hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess((HANDLE)handles[i].hProcess, &exitCode);
    procUnregisterPid(handles[i].pid);
    CloseHandle((HANDLE)handles[i].hProcess);
    handles[i].hProcess = NULL;
    handles[i].finished = true;
    handles[i].ok = exitCode == 0;
    handles[i].interrupted = true;
  }
}

double monotonicSeconds(void) {
  LARGE_INTEGER freq, counter;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter)) return 0.0;
  return (double)counter.QuadPart / (double)freq.QuadPart;
}

int cpuCount(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
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

/*
 * Ctrl+C, mode foreground, dan job paralel.
 *
 * foreground=true (default): child (shell/compiler) dibalikkan ke SIG_DFL
 *   untuk SIGINT/SIGQUIT lalu dibiarkan berbagi process group dengan rbot,
 *   sehingga Ctrl+C dari terminal menghentikan semuanya sekaligus.
 *
 * foreground=false: rbot memasang handler SIGINT sendiri (child tidak
 *   ikut menerima Ctrl+C dari terminal). Ctrl+C ditandai, diteruskan ke
 *   process group child secara eksplisit, dan build dibatalkan dengan rapi
 *   (procRun mengembalikan PROC_RUN_INTERRUPTED).
 *
 * Job paralel (-jN): tiap child diletakkan di process group sendiri
 * (setpgid(0,0)), sehingga forward sinyal dari rbot terarah per proses.
 */

#define JOB_MAX 4096
static volatile sig_atomic_t g_wantInterrupt = 0;
static bool g_foregroundMode = true;
static volatile sig_atomic_t g_jobCount = 0;
static pid_t g_jobs[JOB_MAX];

static void procForwardToJobs(void) {
  for (int i = 0; i < g_jobCount; i++) {
    pid_t pid = g_jobs[i];
    if (pid > 1) kill(-pid, SIGINT); /* grup child: forward terarah */
  }
}

static void procSigintHandler(int sig) {
  (void)sig;
  g_wantInterrupt = 1;
  procForwardToJobs();
}

/* Handler kosong untuk mode klasik (foreground): hanya memutus waitpid()
   dengan EINTR; Ctrl+C ditangani terminal langsung ke process group. */
static void procSigintIgnore(int sig) {
  (void)sig;
}

bool procSetForeground(bool foreground) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);

  g_wantInterrupt = 0;
  g_foregroundMode = foreground;

  if (foreground) {
    /* Terminal yang mengelola Ctrl+C: kembali ke SIG_DFL, buang job lama. */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sa, NULL);
    g_jobCount = 0;
    return true;
  }

  sa.sa_handler = procSigintHandler;
  sigaction(SIGINT, &sa, NULL);
  return true;
}

bool procInterrupted(void) { return g_wantInterrupt != 0; }

bool procStart(const char *cmd, ProcHandle *out) {
  if (!cmd || !*cmd || !out) return false;

  pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    setpgid(0, 0); /* grup sendiri: forward sinyal dari rbot terarah */

    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  /* Race setpgid: anak bisa exec lebih dulu; lakukan juga dari parent
     (EACCES setelah exec diabaikan secara diam). */
  setpgid(pid, pid);

  out->pid = pid;
  out->finished = false;
  out->ok = false;
  out->interrupted = false;

  if (g_jobCount < JOB_MAX) g_jobs[g_jobCount++] = pid;
  return true;
}

int procWaitAny(ProcHandle *handles, int count, ProcHandle **finished) {
  if (finished) *finished = NULL;
  if (!handles || count <= 0) return -1;

  for (;;) {
    int alive = 0;
    for (int i = 0; i < count; i++)
      if (!handles[i].finished) alive++;
    if (alive == 0) return -1;

    int status;
    pid_t pid = wait(&status);
    if (pid < 0) {
      if (errno == EINTR) {
        /* Ctrl+C: forward eksplisit ke grup tiap child yang masih jalan —
           child di grup terpisah tidak menerima Ctrl+C dari terminal. */
        if (g_wantInterrupt)
          for (int i = 0; i < count; i++)
            if (!handles[i].finished && handles[i].pid > 1) kill(-handles[i].pid, SIGINT);
        continue;
      }
      return -1;
    }

    for (int i = 0; i < count; i++) {
      if (handles[i].finished || handles[i].pid != pid) continue;

      handles[i].finished = true;
      handles[i].ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      handles[i].interrupted = g_wantInterrupt && !handles[i].ok;
      if (finished) *finished = &handles[i];

      for (int j = 0; j < g_jobCount; j++) {
        if (g_jobs[j] == pid) {
          g_jobs[j] = g_jobs[--g_jobCount];
          break;
        }
      }
      return i;
    }
    /* pid bukan milik batch ini — abaikan dan lanjut menunggu */
  }
}

void procStopAll(ProcHandle *handles, int count) {
  if (!handles || count <= 0) return;

  for (int i = 0; i < count; i++) {
    if (handles[i].finished || handles[i].pid <= 1) continue;
    kill(-handles[i].pid, SIGINT);
  }

  for (int i = 0; i < count; i++) {
    if (handles[i].finished) continue;
    int status;
    while (waitpid(handles[i].pid, &status, 0) < 0 && errno == EINTR) {}
    handles[i].finished = true;
    handles[i].ok = false;
    handles[i].interrupted = true;
  }
  g_jobCount = 0;
}

int cpuCount(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}

bool procRun(const char *cmd) {
  if (!cmd || !*cmd) return false;

  bool separateGroup = !g_foregroundMode;
  pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0) {
    /* system() membuat signal SIGINT/SIGQUIT diabaikan saat shell berjalan.
       Child build harus kembali ke default agar Ctrl+C — langsung dari
       terminal saat foreground, atau forward dari rbot saat background —
       benar-benar menghentikan shell/compiler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    if (separateGroup) setpgid(0, 0);

    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  if (separateGroup) {
    /* Race setpgid: lakukan juga dari parent (EACCES setelah exec diam-
       diabaikan), lalu daftarkan agar Ctrl+C diteruskan seketika. */
    setpgid(pid, pid);
    if (g_jobCount < JOB_MAX) g_jobs[g_jobCount++] = pid;
  }

  struct sigaction ignoreInt, oldInt, oldQuit;
  memset(&ignoreInt, 0, sizeof(ignoreInt));
  sigemptyset(&ignoreInt.sa_mask);

  if (separateGroup) {
    /* foreground=false: child di grup terpisah, rbot tetap hidup menerima
       Ctrl+C lewat handler procSigintHandler. */
    ignoreInt.sa_handler = procSigintHandler;
    sigaction(SIGINT, &ignoreInt, &oldInt);
    sigaction(SIGQUIT, &ignoreInt, &oldQuit);
  } else {
    /* Klasik (foreground): child berbagi process group dan menerima Ctrl+C
       langsung dari terminal; parent cukup tetap hidup selama menunggu. */
    ignoreInt.sa_handler = procSigintIgnore;
    sigaction(SIGINT, &ignoreInt, &oldInt);
    sigaction(SIGQUIT, &ignoreInt, &oldQuit);
  }

  int status;
  for (;;) {
    if (waitpid(pid, &status, 0) >= 0) break;
    if (errno == EINTR) continue;
    sigaction(SIGINT, &oldInt, NULL);
    sigaction(SIGQUIT, &oldQuit, NULL);
    return false;
  }

  sigaction(SIGINT, &oldInt, NULL);
  sigaction(SIGQUIT, &oldQuit, NULL);

  if (separateGroup) {
    for (int j = 0; j < g_jobCount; j++) {
      if (g_jobs[j] == pid) {
        g_jobs[j] = g_jobs[--g_jobCount];
        break;
      }
    }
    kill(-pid, SIGINT); /* jaga-jaga bila forward dari handler terlewat */
    if (g_wantInterrupt) return PROC_RUN_INTERRUPTED;
  }

  if (WIFEXITED(status)) return WEXITSTATUS(status) == 0;
  if (WIFSIGNALED(status)) {
    /* Mode klasik: child mati oleh Ctrl+C grup terminal (SIGINT/SIGQUIT) —
       tandai interrupted agar pemanggil berhenti, bukan lanjut kompilasi. */
    int sig = WTERMSIG(status);
    if (sig == SIGINT || sig == SIGQUIT) {
      g_wantInterrupt = 1;
      return PROC_RUN_INTERRUPTED;
    }
    return false;
  }
  return false;
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
