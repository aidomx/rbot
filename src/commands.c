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
  printf("> rbot <command>\n\n");
  printf("%-8s%s\n", "", "- build project from Buildfile (default, no command needed)");
  printf("%-8s%s\n", "init", "- create a default Buildfile if none exists yet");
  printf("%-8s%s\n", "clean", "- clean build artifacts (Buildfile: clean)");
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
        "  - buildDir: false\n"
        "  - compileCommands: false\n"
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

int cmdBuild(void) {
  Config c = configDefaults();
  if (!loadConfig(&c, "Buildfile")) return 1;
  if (!resolveCompiler(&c)) return 1;

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

  /* Terbitkan build/embedded.h sebelum kompilasi agar konsumen melihat
     simbol yang benar; jika isinya berubah, paksa kompilasi ulang total. */
  char obj[MAX_PATH];
  bool embHeaderChanged = false;
  if (c.embCount > 0 && !emitEmbeddedHeader(&c, &embHeaderChanged)) return 1;
  if (embHeaderChanged) {
    printf("> Embedded  : config changed, recompiling all sources\n");
    for (int i = 0; i < srcs.count; i++) {
      if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
      fsRemoveFile(obj);
    }
  }

  printf("> Build with %s %s-std=%s\n", c.cc, wf, c.std);

  int total = 0, compiled = 0, skipped = 0, failed = 0;

  /* Fase 1: klasifikasi up-to-date vs perlu-kompilasi */
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    if (fsFileExists(obj) && !newerThan(src, obj))
      skipped++;
    else
      total++;
  }

  /* Fase 2: kompilasi hanya yang berubah */
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    if (fsFileExists(obj) && !newerThan(src, obj)) continue;

    double start = nowSeconds();
    bool ok = compileOne(&c, inc, wf, src, obj);
    double dt = nowSeconds() - start;

    compiled++;
    if (!ok) failed++;
    if (ok || c.progressErrorAlways)
      printf("[%3d/%3d] %-4s %5.2fs  %s\n", compiled, total, ok ? "OK" : "FAIL", dt, src);
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
        if (objectMtime < 0 || objectMtime > targetMtime)
          linkNeeded = true;
      }

      for (int i = 0; i < c.embCount && !linkNeeded; i++) {
        if (!c.emb[i].enable) continue;
        if (!fsFileExists(c.emb[i].objectPath)) {
          linkNeeded = true;
          break;
        }
        int64_t objectMtime = fsMTimeNs(c.emb[i].objectPath);
        if (objectMtime < 0 || objectMtime > targetMtime)
          linkNeeded = true;
      }
    }
  }

  if (!linkNeeded) {
    printf("> Linking   : %s (up-to-date)\n", target);
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
  if (c.cleanCompileCommands && fsFileExists("compile_commands.json")) {
    fsRemoveFile("compile_commands.json");
    printf("> Removed   : compile_commands.json\n");
    any = true;
  }
  if (!any) printf("> Nothing to clean (see Buildfile: clean)\n");
  return 0;
}
