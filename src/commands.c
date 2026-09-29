#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compdb.h"
#include "compile.h"
#include "config.h"
#include "embed.h"
#include "portability.h"
#include "util.h"

/* ==================== help ==================== */

void showHelp(void) {
  printf("Rbot - A simple builder for you\n\n");
  printf("> rbot [-jN] <command>\n\n");
  printf("%-8s%s\n", "", "- build project from Buildfile (default, no command needed)");
  printf("%-8s%s\n", "-j[N]", "- build paralel, N job (default: jumlah core CPU)");
  printf("%-8s%s\n", "init", "- create a default Buildfile if none exists yet");
  printf("%-8s%s\n", "clean", "- clean build artifacts (Buildfile: clean)");
  printf("%-8s%s\n", "", "- output.libraryName/libraryShared membangun lib<name>.a + .so");
  printf("%-8s%s\n", "help", "- show this help");
  printf("%-8s%s\n", "version", "- show version of rbot");
}

/* ==================== init ==================== */

int cmdInit(void) {
  if (fsFileExists("Buildfile")) {
    printf("> Buildfile already exists, nothing to do\n");
    return 0;
  }

  FILE *fp = fopen("Buildfile", "w");
  if (!fp) {
    fprintf(stderr, "rbot: cannot create Buildfile\n");
    return 1;
  }

  fputs("root: .\n"
        "\n"
        "clean:\n"
        "  - build: false\n"
        "  - compdb: false # compile_commands.json (dulu: compileCommands)\n"
        "\n"
        "version: \"0.1.0\"\n"
        "\n"
        "sources:\n"
        "  - src\n"
        "\n"
        "flags:\n"
        "  - Wall\n"
        "  - Wextra\n"
        "\n"
        "std: gnu11\n"
        "\n"
        "headers:\n"
        "  - include\n"
        "  - I.\n"
        "\n"
        "compiler:\n"
        "  - gcc\n"
        "  - clang\n"
        "\n"
        "progress:\n"
        "  bar: true\n"
        "  error: always\n"
        "\n"
        "output:\n"
        "  - binaryName: rbot\n"
        "  - binaryDir: bin\n"
        "  - buildDir: build\n"
        "  - compileCommands: auto # compile_commands.json\n",
        fp);
  fclose(fp);

  printf("> Created   : Buildfile\n");
  return 0;
}

/* ==================== build ==================== */

/* Nama file library sesuai toolchain: lib<name>.a / <name>.lib (statis). */
static void libStaticPath(const Config *c, char *out, size_t n) {
#ifdef _WIN32
  if (compilerIsMSVC(c))
    snprintf(out, n, "%s/%s.lib", c->outLibDir, c->outLibName);
  else
    snprintf(out, n, "%s/lib%s.a", c->outLibDir, c->outLibName);
#else
  (void)c;
  snprintf(out, n, "%s/lib%s.a", c->outLibDir, c->outLibName);
#endif
}

/* Nama file library dinamis: lib<name>.so / .dylib / <name>.dll. */
static void libSharedPath(const Config *c, char *out, size_t n) {
#if defined(_WIN32)
  (void)c;
  snprintf(out, n, "%s/%s.dll", c->outLibDir, c->outLibName);
#elif defined(__APPLE__)
  (void)c;
  snprintf(out, n, "%s/lib%s.dylib", c->outLibDir, c->outLibName);
#else
  (void)c;
  snprintf(out, n, "%s/lib%s.so", c->outLibDir, c->outLibName);
#endif
}

static void printLibSummary(const Config *c) {
  if (!c->libRequested) return;
  char libp[MAX_PATH * 2];
  if (c->libStatic) {
    libStaticPath(c, libp, sizeof(libp));
    printf("Library  : %s\n", libp);
  }
  if (c->libShared) {
    libSharedPath(c, libp, sizeof(libp));
    printf("Library  : %s\n", libp);
  }
}

/*
 * Kompilasi paralel (rbot -jN, mirip make). Baris status tiap job dicetak
 * oleh parent tepat setelah job selesai — satu printf utuh per baris, jadi
 * tidak ada interleaving antar job. Return false bila ada yang gagal atau
 * build di-interupsi Ctrl+C.
 */
typedef struct {
  char src[MAX_PATH]; /* source yang dikompilasi slot ini */
  double start;       /* untuk durasi per job */
} JobSlot;

static bool runParallelJobs(const Config *c, const char *inc, const char *wf, const List *srcs,
                            char *objPath, size_t objCap, int jobs, int *outCompiled,
                            int *outFailed, int *outInterrupted) {
  JobSlot *slots = calloc((size_t)jobs, sizeof(JobSlot));
  ProcHandle *handles = calloc((size_t)jobs, sizeof(ProcHandle));
  if (!slots || !handles) {
    free(slots);
    free(handles);
    return false;
  }

  /* -fPIC untuk library shared (GNU/Clang) — setara compileLibraryOne. */
#ifndef _WIN32
  char *picwf = (c->libShared && !compilerIsMSVC(c)) ? picWarningFlags(c) : NULL;
  const char *wfUse = picwf ? picwf : wf;
#else
  const char *wfUse = wf;
#endif

  int total = srcs->count;
  int compiled = 0, failed = 0, interrupted = 0;
  int nextSrc = 0, active = 0;

  while (nextSrc < total || active > 0) {
    /* isi slot kosong dengan job baru (berhenti juga saat Ctrl+C) */
    while (active < jobs && nextSrc < total && !procInterrupted()) {
      const char *src = srcs->items[nextSrc];
      if (!objectPathFor(c, src, objPath, objCap)) {
        nextSrc++;
        continue;
      }
      mkparent(objPath);
      char *cmd = compileCmd(c, inc, wfUse, src, objPath);
      bool started = procStart(cmd, &handles[active]);
      free(cmd);
      if (!started) {
        fprintf(stderr, "rbot: cannot start compiler for %s\n", src);
        failed++;
        nextSrc++;
        continue;
      }
      snprintf(slots[active].src, sizeof(slots[active].src), "%s", src);
      slots[active].start = nowSeconds();
      nextSrc++;
      active++;
    }
    if (active == 0) break;

    ProcHandle *fin = NULL;
    int w = procWaitAny(handles, active, &fin);
    if (w < 0) break; /* tidak ada job tersisa (atau wait gagal) */

    double dt = nowSeconds() - slots[w].start;
    compiled++;
    const char *tag = fin->interrupted ? "INT" : (fin->ok ? "OK" : "FAIL");
    printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, tag, dt, slots[w].src);
    fflush(stdout);
    if (!fin->ok) {
      if (fin->interrupted)
        interrupted++;
      else
        failed++;
    }

    /* kompakkan: pindahkan slot terakhir ke slot yang baru kosong */
    handles[w] = handles[active - 1];
    slots[w] = slots[active - 1];
    active--;
  }

  free(slots);
  free(handles);
#ifndef _WIN32
  free(picwf);
#endif
  *outCompiled = compiled;
  *outFailed = failed;
  *outInterrupted = interrupted;
  return failed == 0 && interrupted == 0;
}

/*
 * Fase library: kumpulkan object hasil kompilasi (source + embedded),
 * kemas statis (ar / lib) dan/atau link shared (-shared / /LD).
 * Object perantara TIDAK dihapus — tetap dipakai link binary dan
 * agar build incremental berikutnya tidak kompilasi ulang total.
 */
static bool buildLibrary(const Config *c, const List *srcs) {
  if (!c->libRequested) return true;

  List objs = {0};
  char obj[MAX_PATH];
  for (int i = 0; i < srcs->count; i++) {
    /* exclude: source tidak ikut DIKEMAS ke library (mis. main.c milik
       binary) — tetap dikompilasi untuk executable. */
    if (excludedSource(c, srcs->items[i])) continue;
    if (objectPathFor(c, srcs->items[i], obj, sizeof(obj))) listAdd(&objs, obj);
  }
  for (int i = 0; i < c->embCount; i++)
    if (c->emb[i].enable && fsFileExists(c->emb[i].objectPath))
      listAdd(&objs, c->emb[i].objectPath);

  if (objs.count == 0) {
    fprintf(stderr, "rbot: library requested but no object files found\n");
    return false;
  }

  mkdirs(c->outLibDir);

  bool staticDone = true, sharedDone = true;
  char target[MAX_PATH * 2];

  if (c->libStatic) {
    libStaticPath(c, target, sizeof(target));
    size_t n = 64;
    for (int i = 0; i < objs.count; i++)
      n += strlen(objs.items[i]) + 3;
    char *cmd = malloc(n);
    if (compilerIsMSVC(c))
      snprintf(cmd, n, "lib /nologo /OUT:%s", target);
    else
      snprintf(cmd, n, "ar rcs %s", target);
    for (int i = 0; i < objs.count; i++) {
      strcat(cmd, " ");
      strcat(cmd, objs.items[i]);
    }
    printf("> Library   : %s\n", target);
    staticDone = runCmd(cmd);
    free(cmd);
    if (!staticDone) fprintf(stderr, "rbot: static library build failed\n");
  }

  if (c->libShared) {
    libSharedPath(c, target, sizeof(target));
    size_t n = strlen(c->cc) + strlen(target) + 96;
    for (int i = 0; i < objs.count; i++)
      n += strlen(objs.items[i]) + 3;
    for (int i = 0; i < c->libraries.count; i++)
      n += strlen(c->libraries.items[i]) + 8;
    char *cmd = malloc(n);
    if (compilerIsMSVC(c)) {
      snprintf(cmd, n, "cl /nologo /LD");
      for (int i = 0; i < objs.count; i++) {
        strcat(cmd, " ");
        strcat(cmd, objs.items[i]);
      }
      strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
      strcat(cmd, target);
      for (int i = 0; i < c->libraries.count; i++) {
        const char *lib = c->libraries.items[i];
        if (lib[0] == '-') lib++;
        if (lib[0] == 'l') lib++;
        strcat(cmd, " ");
        strcat(cmd, lib);
        strcat(cmd, ".lib");
      }
    } else {
      snprintf(cmd, n, "%s -shared", c->cc);
      for (int i = 0; i < objs.count; i++) {
        strcat(cmd, " ");
        strcat(cmd, objs.items[i]);
      }
      strcat(cmd, " -o ");
      strcat(cmd, target);
      for (int i = 0; i < c->libraries.count; i++) {
        const char *lib = c->libraries.items[i];
        strcat(cmd, " ");
        if (lib[0] == '-' || lib[0] == 'l')
          strcat(cmd, "-");
        else
          strcat(cmd, "-l");
        strcat(cmd, lib);
      }
    }
    printf("> Library   : %s\n", target);
    sharedDone = runCmd(cmd);
    free(cmd);
    if (!sharedDone) fprintf(stderr, "rbot: shared library build failed\n");
  }

  return staticDone && sharedDone;
}

/* True bila semua varian library yang diminta sudah ada dan lebih baru
   daripada seluruh object inputnya — fase library boleh dilewati. */
static bool libTargetsUpToDate(const Config *c, const List *srcs) {
  char obj[MAX_PATH];
  char target[MAX_PATH * 2];

  for (int v = 0; v < 2; v++) {
    bool isStatic = v == 0;
    if (isStatic ? !c->libStatic : !c->libShared) continue;
    if (isStatic)
      libStaticPath(c, target, sizeof(target));
    else
      libSharedPath(c, target, sizeof(target));

    int64_t mtime = fsMTimeNs(target);
    if (mtime < 0) return false; /* target belum ada */

    for (int i = 0; i < srcs->count; i++) {
      if (excludedSource(c, srcs->items[i])) continue;
      if (!objectPathFor(c, srcs->items[i], obj, sizeof(obj))) continue;
      int64_t om = fsMTimeNs(obj);
      if (om < 0 || om > mtime) return false;
    }
    for (int i = 0; i < c->embCount; i++) {
      if (!c->emb[i].enable) continue;
      int64_t om = fsMTimeNs(c->emb[i].objectPath);
      if (om < 0 || om > mtime) return false;
    }
  }
  return true;
}

/*
 * Terbitkan build/version.h dari .rbot-version (root project). Bila file
 * itu ada, project yang menambahkan build ke headers bisa memakai
 * RBOT_VERSION_EMBEDDED — versi ikut ter-embed ke binary saat build, bukan
 * dibaca ulang saat runtime, sehingga `rbot version` tetap benar di mana
 * pun binary dijalankan (tidak tergantung path lokal/repo). Header hanya
 * ditulis ulang saat isinya berubah; perubahan memaksa kompilasi ulang.
 */
static bool emitVersionHeader(const Config *c, bool *changed) {
  if (!fsFileExists(".rbot-version")) return true;

  FILE *rf = fopen(".rbot-version", "rb");
  if (!rf) return true;
  char raw[64] = {0};
  size_t n = fread(raw, 1, sizeof(raw) - 1, rf);
  fclose(rf);
  while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r' || raw[n - 1] == ' ' || raw[n - 1] == '\t'))
    raw[--n] = '\0';
  char *s = raw;
  while (*s == ' ' || *s == '\t') s++;
  if (!*s) return true; /* kosong: jangan terbitkan apa pun */

  char body[192];
  snprintf(body, sizeof(body),
           "/* Auto-generated by rbot from .rbot-version. Do not edit. */\n"
           "#define RBOT_VERSION_EMBEDDED \"%s\"\n",
           s);

  char path[MAX_PATH + 16];
  snprintf(path, sizeof(path), "%s/version.h", c->outBuildDir);
  mkparent(path);

  /* lewati penulisan ulang bila tidak berubah agar mtime tetap stabil */
  char existing[192] = {0};
  FILE *ef = fopen(path, "rb");
  if (ef) {
    size_t en = fread(existing, 1, sizeof(existing) - 1, ef);
    fclose(ef);
    if (en == strlen(body) && memcmp(existing, body, en) == 0) return true;
  }

  FILE *wf = fopen(path, "w");
  if (!wf) {
    fprintf(stderr, "rbot: cannot write %s\n", path);
    return false;
  }
  fwrite(body, 1, strlen(body), wf);
  fclose(wf);
  if (changed) *changed = true;
  return true;
}

int cmdBuild(int jobs) {
  Config c = configDefaults();
  if (!loadConfig(&c, "Buildfile")) return 1;
  if (!resolveCompiler(&c)) return 1;

  /* Mode sinyal: build paralel selalu butuh handler forward SIGINT karena
     tiap compiler ada di process group sendiri (Ctrl+C dari terminal hanya
     sampai ke rbot). Build serial mengikuti Buildfile: foreground. */
  procSetForeground(jobs > 1 ? false : c.foreground);

  if (!fsDirExists(c.root)) {
    fprintf(stderr, "rbot: root '%s' is not a directory\n", c.root);
    return 1;
  }
  if (!fsSetCwd(c.root)) {
    fprintf(stderr, "rbot: cannot enter root '%s'\n", c.root);
    return 1;
  }

  List srcs = {0};
  for (int i = 0; i < c.sources.count; i++)
    walkDir(c.sources.items[i], ".c", &srcs);
  if (srcs.count == 0) {
    fprintf(stderr, "rbot: no source files found in sources\n");
    return 1;
  }

  mkdirs(c.outBuildDir);
  mkdirs(c.outBinaryDir);

  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);

  if (compdbEnabled(&c)) writeCompdb(&c, &srcs);

  /* Terbitkan build/embedded.h & build/version.h sebelum kompilasi agar
     konsumen melihat simbol/versi yang benar; jika salah satunya berubah,
     paksa kompilasi ulang total. */
  char obj[MAX_PATH];
  bool embHeaderChanged = false, verHeaderChanged = false;
  if (c.embCount > 0 && !emitEmbeddedHeader(&c, &embHeaderChanged)) return 1;
  if (!emitVersionHeader(&c, &verHeaderChanged)) return 1;
  if (embHeaderChanged || verHeaderChanged) {
    if (verHeaderChanged)
      printf("> Version   : .rbot-version changed, recompiling all sources\n");
    else
      printf("> Embedded  : config changed, recompiling all sources\n");
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      fsRemoveFile(obj);
    }
  }

  printf("> Build with %s %s-std=%s\n", c.cc, wf, c.std);

  int total = 0, compiled = 0, skipped = 0, failed = 0, interrupted = 0;

  /* Fase 1: klasifikasi up-to-date vs perlu-kompilasi. Pakai mtime
     nanodetik (bukan detik) agar perubahan dalam detik yang sama tetap
     terdeteksi — sama seperti keputusan link di bawah. */
  List pending = {0};
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    int64_t objM = fsFileExists(obj) ? fsMTimeNs(obj) : -1;
    int64_t srcM = fsMTimeNs(src);
    if (objM >= 0 && srcM >= 0 && objM >= srcM) {
      skipped++;
      continue;
    }
    listAdd(&pending, src);
  }
  total = pending.count;

  /* Fase 2: kompilasi hanya yang berubah — paralel (-jN) atau serial. */
  if (jobs != 1 && total > 0) {
    runParallelJobs(&c, inc, wf, &pending, obj, sizeof(obj), jobs, &compiled, &failed,
                    &interrupted);
  } else {
    for (int i = 0; i < pending.count; i++) {
      const char *src = pending.items[i];
      if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;

      double start = nowSeconds();
      bool ok = compileLibraryOne(&c, inc, wf, src, obj);
      double dt = nowSeconds() - start;

      compiled++;
      if (procInterrupted()) {
        /* foreground=false: child mati karena forward SIGINT dari rbot —
           hentikan build, jangan lanjut ke source berikutnya. */
        interrupted++;
        if (ok || c.progressErrorAlways)
          printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, "INT", dt, src);
        break;
      }
      if (!ok) failed++;
      if (ok || c.progressErrorAlways)
        printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, ok ? "OK" : "FAIL", dt, src);
    }
  }

  if (interrupted > 0) {
    fprintf(stderr, "\nrbot: build interrupted; stopping\n");
    free(inc);
    free(wf);
    return 130;
  }

  if (failed > 0) {
    fprintf(stderr, "\nrbot: build failed with %d error(s); linking skipped\n", failed);
    free(inc);
    free(wf);
    return 1;
  }

  if (!buildEmbedded(&c)) {
    free(inc);
    free(wf);
    return 1;
  }

  /* link: hanya jalankan linker bila target belum ada atau salah satu
     input object lebih baru daripada target. Gunakan nanosecond mtime agar
     keputusan incremental tidak kehilangan perubahan yang terjadi dalam
     detik yang sama. */
  char target[MAX_PATH * 2];
  snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
#ifdef _WIN32
  /* Windows dapat menambahkan .exe otomatis saat link. */
  if (!fsFileExists(target)) {
    char withExe[MAX_PATH * 2];
    snprintf(withExe, sizeof(withExe), "%s.exe", target);
    if (fsFileExists(withExe)) snprintf(target, sizeof(target), "%s", withExe);
  }
#endif

  bool linkNeeded = !fsFileExists(target);
  if (!linkNeeded) {
    int64_t targetMtime = fsMTimeNs(target);
    if (targetMtime < 0) {
      linkNeeded = true;
    } else {
      for (int i = 0; i < srcs.count && !linkNeeded; i++) {
        if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
        int64_t objectMtime = fsMTimeNs(obj);
        if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
      }

      for (int i = 0; i < c.embCount && !linkNeeded; i++) {
        if (!c.emb[i].enable) continue;
        if (!fsFileExists(c.emb[i].objectPath)) {
          linkNeeded = true;
          break;
        }
        int64_t objectMtime = fsMTimeNs(c.emb[i].objectPath);
        if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
      }
    }
  }

  if (!linkNeeded) {
    printf("> Linking   : %s (up-to-date)\n", target);
    /* Library bisa jadi masih perlu dibangun (baru diaktifkan di
       Buildfile / terhapus manual) meski binary sudah up-to-date. */
    if (c.libRequested && !libTargetsUpToDate(&c, &srcs) && !buildLibrary(&c, &srcs)) {
      free(inc);
      free(wf);
      return 1;
    }
    printLibSummary(&c);
    free(inc);
    free(wf);
    printf("\n> Summary\n");
    long long sizeBytes = fsFileSize(target);
    double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;
    printf("Target   : %s\n", target);
    printf("Size     : %.1fKB\n", sizeKb);
    printf("Compiled : %d\n", compiled);
    printf("Skipped  : %d\n", skipped);
    printf("Status   : Success\n");
    return 0;
  }

  size_t n = strlen(c.cc) + strlen(c.outBinaryDir) + strlen(c.outBinaryName) + 256;
  for (int i = 0; i < srcs.count; i++)
    if (objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) n += strlen(obj) + 2;
  for (int i = 0; i < c.embCount; i++)
    if (c.emb[i].enable) n += strlen(c.emb[i].objectPath) + 2;
  for (int i = 0; i < c.libraries.count; i++)
    n += strlen(c.libraries.items[i]) + 8;

  char *cmd = malloc(n);
  strcpy(cmd, c.cc);
  for (int i = 0; i < srcs.count; i++) {
    if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
    strcat(cmd, " ");
    strcat(cmd, obj);
  }
  for (int i = 0; i < c.embCount; i++) {
    if (!c.emb[i].enable) continue;
    if (fsFileExists(c.emb[i].objectPath)) {
      strcat(cmd, " ");
      strcat(cmd, c.emb[i].objectPath);
    }
  }
  if (compilerIsMSVC(&c)) {
    /* MSVC: object dikumpulkan dulu, opsi linker setelah token /link.
       /OUT menentukan target; link.exe untuk EXE tanpa /LD tidak menulis
       .lib/.exp sampingan, jadi tidak perlu /IMPLIB. */
    char target[MAX_PATH * 2];
    snprintf(target, sizeof(target), "%s/%s", c.outBinaryDir, c.outBinaryName);
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, target);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      if (lib[0] == '-') lib++; /* -lssl -> ssl */
      if (lib[0] == 'l') lib++; /* "lssl" -> "ssl", seperti konvensi headers "I." */
      strcat(cmd, " ");
      strcat(cmd, lib);
      strcat(cmd, ".lib");
    }
  } else {
    strcat(cmd, " -o ");
    strcat(cmd, c.outBinaryDir);
    strcat(cmd, "/");
    strcat(cmd, c.outBinaryName);
    for (int i = 0; i < c.libraries.count; i++) {
      const char *lib = c.libraries.items[i];
      /* "ssl" -> -lssl, "lm" -> -lm (leading 'l' sudah termasuk, seperti "I."
         di headers); entri yang sudah diawali '-' diteruskan apa adanya */
      strcat(cmd, " ");
      if (lib[0] == '-' || lib[0] == 'l')
        strcat(cmd, "-");
      else
        strcat(cmd, "-l");
      strcat(cmd, lib);
    }
  }

  printf("> Linking   : %s/%s\n", c.outBinaryDir, c.outBinaryName);
  bool linked = runCmd(cmd);
  free(cmd);

  free(inc);
  free(wf);

  if (!linked) {
    fprintf(stderr, "rbot: link failed\n");
    return 1;
  }

  /* Library statis/shared dari object yang sama — object tidak dihapus. */
  if (!buildLibrary(&c, &srcs)) {
    return 1;
  }
  printLibSummary(&c);

  long long sizeBytes = fsFileSize(target);
  double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;

  printf("\n> Summary\n");
  printf("Target   : %s\n", target);
  printf("Size     : %.1fKB\n", sizeKb);
  printf("Compiled : %d\n", compiled);
  printf("Skipped  : %d\n", skipped);
  printf("Status   : Success\n");
  return 0;
}

/* ==================== clean ==================== */

int cmdClean(void) {
  Config c = configDefaults();
  if (!loadConfig(&c, "Buildfile")) return 1;

  bool any = false;
  if (c.cleanBuildDir && safeRelative(c.outBuildDir)) {
    if (fsRemoveTree(c.outBuildDir))
      printf("> Removed   : %s\n", c.outBuildDir);
    else
      printf("> Removed   : %s (sebagian gagal dihapus)\n", c.outBuildDir);
    any = true;
  }
  /*if (c.cleanBuildDir && c.libRequested && safeRelative(c.outLibDir)) {*/
  /*if (fsRemoveTree(c.outLibDir))*/
  /*printf("> Removed   : %s\n", c.outLibDir);*/
  /*else*/
  /*printf("> Removed   : %s (sebagian gagal dihapus)\n", c.outLibDir);*/
  /*any = true;*/
  /*}*/
  if (c.cleanCompileCommands && fsFileExists("compile_commands.json")) {
    fsRemoveFile("compile_commands.json");
    printf("> Removed   : compile_commands.json\n");
    any = true;
  }
  if (!any) printf("> Nothing to clean (see Buildfile: clean)\n");
  return 0;
}
