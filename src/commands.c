#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmds/cmds_internal.h"
#include "compdb.h"
#include "compile.h"
#include "config.h"
#include "deps.h"
#include "embed.h"
#include "pack/pack.h"
#include "portability.h"
#include "prof/prof.h"
#include "uses/workspace.h"
#include "util.h"

/*
 * commands — command publik rbot (build/init/help; clean ada di
 * cmds/cmds_state.c). Fase build yang berdiri sendiri dipisah modular:
 *   - cmds/cmds_lib.c   : fase library, kompilasi paralel, header versi.
 *   - cmds/cmds_state.c : fingerprint build, fast no-op snapshot, clean.
 */

/* ==================== help ==================== */

void showHelp(void) {
  printf("Rbot - A simple builder for you\n\n");
  printf("> rbot [-f <file>] [-jN] <command>\n\n");
  printf("%-8s%s\n", "", "- build project from Buildfile (default, no command needed)");
  printf("%-8s%s\n", "-f <f>", "- pakai <f> sebagai Buildfile (default: Buildfile)");
  printf("%-8s%s\n", "-xf <n>",
         "- konversi build.ninja <n> -> Buildfile.xf.tmp (sementara; Buildfile tak disentuh)");
  printf("%-8s%s\n", "-xcf <n>",
         "- sama seperti -xf, tapi hasil konversi ditulis ke Buildfile (konfirmasi bila ada)");
  printf("%-8s%s\n", "-j[N]", "- build paralel, N job (tanpa -j: jumlah core CPU; -j1 = serial)");
  printf("%-8s%s\n", "-w", "- mode workspace: build semua proyek (Buildfile.ws)");
  printf("%-8s%s\n", "", "- rbot -w <nama>: hanya proyek itu (+ dependency-nya)");
  printf("%-8s%s\n", "", "- rbot -w release -- name=rupa: kemas release selektif (dist/release/<nama>)");
  printf("%-8s%s\n", "init", "- create a default Buildfile if none exists yet");
  printf("%-8s%s\n", "", "- rbot init -w: buat Buildfile.ws (mode workspace) bila belum ada");
  printf("%-8s%s\n", "clean", "- clean build artifacts (Buildfile: clean)");
  printf("%-8s%s\n", "", "- output.libraryName/libraryShared membangun lib<name>.a + .so");
  printf("%-8s%s\n", "", "- pack.* mengemas artefak: tar.gz + .deb + checksum sha256");
  printf("%-8s%s\n", "help", "- show this help");
  printf("%-8s%s\n", "version", "- show version of rbot");
}

/* ==================== init ==================== */

int cmdInit(const char *buildfilePath) {
  const char *path = buildfilePath && *buildfilePath ? buildfilePath : "Buildfile";
  if (fsFileExists(path)) {
    printf("> %s already exists, nothing to do\n", path);
    return 0;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "rbot: cannot create %s\n", path);
    return 1;
  }

  /* Template minimal: proyek standar (src/ + include/ opsional) cukup
     `use project` — sisanya konvensi rbot (sources=src, headers=include
     bila ada, binary = nama folder). Proyek non-standar bebas menambah
     field eksplisit; lihat docs/guide/buildfile.md. */
  fputs("use project\n", fp);
  fclose(fp);

  printf("> Created   : %s\n", path);
  return 0;
}

/* ==================== init -w (template workspace) ==================== */

int cmdInitWorkspace(const char *buildfilePath) {
  const char *path = buildfilePath && *buildfilePath ? buildfilePath : WORKSPACE_FILENAME;
  if (fsFileExists(path)) {
    printf("> %s already exists, nothing to do\n", path);
    return 0;
  }

  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "rbot: cannot create %s\n", path);
    return 1;
  }

  /* Template minimal workspace: cukup daftar proyek. Konvensi per proyek
     sama dengan proyek standar (folder <nama>/ berisi src/). */
  fputs("use workspace\n"
        "\n"
        "# Proyek standar: folder <nama>/ berisi src/ (include/ opsional).\n"
        "projects = app\n",
        fp);
  fclose(fp);

  printf("> Created   : %s\n", path);
  return 0;
}

/* ==================== build ==================== */

/* Build penuh satu proyek — jalur rbot satu-proyek & fase binary workspace. */
int cmdBuild(int jobs, const char *buildfilePath) { return cmdBuildEx(jobs, buildfilePath, false); }

int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly) {
  /* Fast no-op hanya untuk fase normal (binary): snapshot state menyangkut
     target binary. Fase library (workspace pass 1) selalu jalur penuh —
     murah bila semua object & library up-to-date. */
  if (!libOnly && cmdsFastStateValid(buildfilePath)) return 0;
  Config c = configDefaults();
  if (!loadConfig(&c, buildfilePath)) return 1;
  if (!resolveCompiler(&c)) return 1;
  profMark("startup+config");

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

  /* Proyek kemasan (output.binary = false): SEBELUM koleksi source —
     proyek kemasan sah tidak punya satu .c pun. Dua varian:
       pack.*    -> pengemasan artefak (tar.gz/deb/checksum);
       embedded  -> arsip aset untuk di-embed binary lain. */
  if (!c.binary) {
    if (c.pack.requested) return cmdsPackOnlyProject(&c);
    if (c.embCount == 0) {
      fprintf(stderr, "rbot: nothing to build (output.binary = false)\n");
      return 1;
    }
    mkdirs(c.outBuildDir);
    if (!buildEmbeddedArchives(&c)) return 1;
    printf("\n> Summary\n");
    for (int i = 0; i < c.embCount; i++) {
      if (!c.emb[i].enable) continue;
      printf("Archive  : %s%s\n", c.emb[i].archivePath,
             fsFileExists(c.emb[i].archivePath) ? "" : " (missing)");
    }
    printf("Status   : Success\n");
    return 0;
  }

  List srcs = {0};
  for (int i = 0; i < c.sources.count; i++) {
    const char *entry = c.sources.items[i];
    /* Entri FILE .c (mis. hasil `rbot -xf` untuk build.ninja flat) masuk
       langsung; selain itu dianggap folder dan discan rekursif. */
    size_t elen = strlen(entry);
    if (elen >= 2 && strcmp(entry + elen - 2, ".c") == 0 && fsFileExists(entry)) {
      listAdd(&srcs, entry);
      continue;
    }
    walkDir(entry, ".c", &srcs);
  }
  if (srcs.count == 0) {
    fprintf(stderr,
            "rbot: tidak ada source .c di sources — proyek standar memakai folder src/ "
            "(atau set sources = ... untuk tata letak lain)\n");
    return 1;
  }

  CmdsBuildFingerprint currentFp;
  cmdsFingerprintMake(&c, &srcs, &currentFp);
  CmdsBuildFingerprint previousFp = {0};
  bool haveBuildFp = cmdsFingerprintLoad(&previousFp);
  bool compileConfigChanged = !haveBuildFp || previousFp.compile != currentFp.compile;
  bool sourceSetChanged = !haveBuildFp || previousFp.sources != currentFp.sources;
  bool linkConfigChanged = !haveBuildFp || previousFp.link != currentFp.link;

  profMark("fingerprint");

  if (haveBuildFp && compileConfigChanged)
    printf("> Fingerprint: compile configuration changed; rebuilding objects\n");
  if (haveBuildFp && sourceSetChanged)
    printf("> Fingerprint: source set changed; refreshing link inputs\n");
  if (haveBuildFp && linkConfigChanged)
    printf("> Fingerprint: link configuration changed; relinking\n");

  mkdirs(c.outBuildDir);
  mkdirs(c.outBinaryDir);

  char *inc = includeFlags(&c);
  char *wf = warningFlags(&c);

  /* compile_commands.json tidak perlu dihitung ulang pada setiap build.
     Daftar command hanya berubah jika file belum ada, konfigurasi compile
     berubah, atau set source berubah. Perubahan isi source/header tidak
     mengubah command compile. */
  if (compdbEnabled(&c)) {
    bool compdbMissing = !fsFileExists("compile_commands.json");
    if (compdbMissing || !haveBuildFp || compileConfigChanged || sourceSetChanged)
      writeCompdb(&c, &srcs);
  }

  /* Terbitkan build/embedded.h & build/version.h sebelum kompilasi agar
     konsumen melihat simbol/versi yang benar; jika salah satunya berubah,
     paksa kompilasi ulang total. */
  char obj[MAX_PATH];
  bool embHeaderChanged = false, verHeaderChanged = false;
  if (c.embCount > 0 && !emitEmbeddedHeader(&c, &embHeaderChanged)) return 1;
  if (!cmdsEmitVersionHeader(&c, &verHeaderChanged)) return 1;
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
  /*
   * Header tracking dua lapis:
   *   1. mtime — object usang bila header transitive lebih baru. Murah.
   *   2. konten — bila mtime mengatakan stale (mis. setelah `touch`), hash
   *      isi dibandingkan dengan snapshot build sukses terakhir
   *      (.rbot/deps.cache). Konten sama => tidak perlu kompilasi ulang.
   *
   * Edges header diambil dari file .d compiler (flag -MMD, GCC/Clang) —
   * akurat & lengkap; pemindai #include hanya fallback (build pertama,
   * MSVC, .d belum ada). Lihat deps/.
   */
  List pending = {0};
  DepCache *deps = depsNew(&c);
  int headerStale = 0, headerTouched = 0;
  /* mtime object terbesar & apakah ada object yang hilang — dikumpulkan di
     sini agar keputusan link (no-op) tidak men-stat ulang semua object. */
  int64_t maxObjM = -1;
  bool anyObjMissing = false;
  for (int i = 0; i < srcs.count; i++) {
    const char *src = srcs.items[i];
    if (!objectPathFor(&c, src, obj, sizeof(obj))) continue;
    int64_t srcM = fsMTimeNs(src);
    int64_t objM = fsMTimeNs(obj); /* -1 = object belum ada (sekali stat) */
    if (objM < 0)
      anyObjMissing = true;
    else if (objM > maxObjM)
      maxObjM = objM;
    if (!compileConfigChanged && objM >= 0 && srcM >= 0 && objM >= srcM) {
      if (depsNewestHeaderMTimeAt(deps, src, srcM) > objM) {
        /* header lebih baru — tapi mungkin hanya `touch`: cek konten */
        if (depsContentUpToDate(deps, src)) {
          headerTouched++;
          skipped++;
          continue;
        }
        headerStale++;
      } else {
        skipped++;
        continue;
      }
    }
    listAdd(&pending, src);
  }
  profMark("decide");
  if (headerTouched > 0)
    printf("> Headers   : %d source(s) skipped (touched, content unchanged)\n", headerTouched);
  if (headerStale > 0)
    printf("> Headers   : %d source(s) stale due to header change\n", headerStale);
  total = pending.count;

  /* Fase 2: kompilasi hanya yang berubah — paralel (-jN) atau serial. */
  if (jobs != 1 && total > 0) {
    cmdsRunParallelJobs(&c, inc, wf, &pending, obj, sizeof(obj), jobs, &compiled, &failed,
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
  profMark("compile");

  if (interrupted > 0) {
    /* Jangan percaya fingerprint lama setelah sebagian object mungkin telah
       ditulis dengan konfigurasi baru sebelum interupsi. */
    if (compileConfigChanged) cmdsFingerprintInvalidate();
    fprintf(stderr, "\nrbot: build interrupted; stopping\n");
    free(inc);
    free(wf);
    return 130;
  }

  if (failed > 0) {
    /* Sebagian object bisa berhasil dibuat sebelum object lain gagal. Hapus
       state agar build berikutnya tidak melewatkan rebuild konfigurasi penuh. */
    if (compileConfigChanged) cmdsFingerprintInvalidate();
    fprintf(stderr, "\nrbot: build failed with %d error(s); linking skipped\n", failed);
    free(inc);
    free(wf);
    return 1;
  }

  if (!buildEmbedded(&c)) {
    free(inc);
    free(wf);
    profReport();
    return 1;
  }

  /* Fase library workspace (pass 1 dari cmdBuildEx, lihat commands.h):
     kemas lib<name>.a/.so dari object milik proyek ini lalu berhenti —
     link binary ditunda ke fase normal (pass 2) agar proyek yang saling
     memakai library lintas-proyek bisa link setelah SEMUA library ada.
     Library ber-path proyek lain (mis. ../ruka/lib/libruka.a) tidak
     disertakan pada link .so: .a proyek lain belum tentu ada di titik ini
     dan shared library memang tidak perlu inline-kan kode proyek lain. */
  if (libOnly) {
    if (!cmdsBuildLibraryEx(&c, &srcs, false)) {
      free(inc);
      free(wf);
      depsFree(deps);
      return 1;
    }
    if (compiled > 0 || depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    }
    depsSave(deps);
    depsFree(deps);
    cmdsFingerprintSave(&currentFp);
    printf("\n> Summary\n");
    if (c.libRequested) {
      char libp[MAX_PATH * 2];
      if (c.libStatic) {
        cmdsLibStaticPath(&c, libp, sizeof(libp));
        printf("Library  : %s\n", libp);
      }
      if (c.libShared) {
        cmdsLibSharedPath(&c, libp, sizeof(libp));
        printf("Library  : %s\n", libp);
      }
    }
    printf("Compiled : %d\n", compiled);
    printf("Skipped  : %d\n", skipped);
    printf("Status   : Success\n");
    free(inc);
    free(wf);
    profReport();
    return 0;
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

  bool linkNeeded = !fsFileExists(target) || linkConfigChanged || sourceSetChanged || compiled > 0;
  if (!linkNeeded) {
    int64_t targetMtime = fsMTimeNs(target);
    if (targetMtime < 0) {
      linkNeeded = true;
    } else {
      if (compiled == 0 && !anyObjMissing) {
        /* Tidak ada object yang ditulis ulang sejak fase 1: mtime yang sudah
           dikumpulkan masih berlaku — nol stat tambahan pada build no-op. */
        if (maxObjM > targetMtime) linkNeeded = true;
      } else {
        for (int i = 0; i < srcs.count && !linkNeeded; i++) {
          if (!objectPathFor(&c, srcs.items[i], obj, sizeof(obj))) continue;
          int64_t objectMtime = fsMTimeNs(obj);
          if (objectMtime < 0 || objectMtime > targetMtime) linkNeeded = true;
        }
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
    if (c.libRequested && !cmdsBuildLibraryEx(&c, &srcs, true)) {
      free(inc);
      free(wf);
      return 1;
    }
    free(inc);
    free(wf);
    profReport();
    /* Build sukses: rekam snapshot hash source + dependensi agar build
       berikutnya bisa membedakan `touch` (isi sama) dari perubahan konten.
       No-op murni (tidak ada kompilasi, tidak ada yang berubah sejak rekam
       terakhir) melewatinya — DFS + ribuan snapshotSet murni CPU sia-sia
       pada project besar. depsSave tetap dipanggil: verifikasi konten
       (kasus touch) memperbarui mtime snapshot, dan depsSave sendiri murah
       bila tidak ada yang berubah. */
    if (depsSnapshotIncomplete(deps)) {
      for (int i = 0; i < srcs.count; i++)
        depsRecordUpdate(deps, srcs.items[i]);
    } else if (compiled > 0) {
      for (int i = 0; i < pending.count; i++)
        depsRecordUpdate(deps, pending.items[i]);
    }
    depsSave(deps);
    depsFree(deps);
    cmdsFingerprintSave(&currentFp);
    /* Packaging (pack.*) sebelum summary; fast state disimpan di dalamnya
       bila pack sukses — gagal kemasan tidak merekam state. */
    bool fastSaved = false;
    if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) return 1;
    printf("\n> Summary\n");
    long long sizeBytes = fsFileSize(target);
    double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;
    printf("Target   : %s\n", target);
    printf("Size     : %.1fKB\n", sizeKb);
    printf("Compiled : %d\n", compiled);
    printf("Skipped  : %d\n", skipped);
    printf("Status   : Success\n");
    if (!fastSaved) cmdsFastStateSave(&c, &srcs, target, buildfilePath);
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
       .lib/.exp sampingan, jadi tidak perlu /IMPLIB. (Nama variabel beda
       dari `target` luar — MSVC /W4 memperingatkan shadowing, C4456.) */
    char linkTarget[MAX_PATH * 2];
    snprintf(linkTarget, sizeof(linkTarget), "%s/%s", c.outBinaryDir, c.outBinaryName);
    strcat(cmd, " /link /nologo /INCREMENTAL:NO /OUT:");
    strcat(cmd, linkTarget);
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
      if (strchr(lib, '/') || strchr(lib, '\\')) {
        /* Workspace project-library resolution may provide an explicit
           artifact path (../project/lib/libname.a). Do not turn it into
           -l../...; pass the path directly to the linker. */
        strcat(cmd, lib);
      } else {
        if (lib[0] == '-' || lib[0] == 'l')
          strcat(cmd, "-");
        else
          strcat(cmd, "-l");
        strcat(cmd, lib);
      }
    }
  }

  printf("> Linking   : %s/%s\n", c.outBinaryDir, c.outBinaryName);
  bool linked = runCmd(cmd);
  profMark("link");
  free(cmd);

  free(inc);
  free(wf);

  if (!linked) {
    fprintf(stderr, "rbot: link failed\n");
    return 1;
  }

  /* Library statis/shared dari object yang sama — object tidak dihapus. */
  if (!cmdsBuildLibraryEx(&c, &srcs, true)) {
    return 1;
  }

  /* Build sukses: rekam snapshot hash (lihat jalur up-to-date di atas). */
  if (compiled > 0 || depsSnapshotIncomplete(deps)) {
    for (int i = 0; i < srcs.count; i++)
      depsRecordUpdate(deps, srcs.items[i]);
  }
  depsSave(deps);
  depsFree(deps);
  cmdsFingerprintSave(&currentFp);

  /* Packaging (pack.*) setelah link & state — lihat jalur up-to-date. */
  bool fastSaved = false;
  if (!cmdsPackAfterBinary(&c, &fastSaved, &srcs, target, buildfilePath)) return 1;
  profReport();

  long long sizeBytes = fsFileSize(target);
  double sizeKb = sizeBytes > 0 ? (double)sizeBytes / 1024.0 : 0;

  printf("\n> Summary\n");
  printf("Target   : %s\n", target);
  printf("Size     : %.1fKB\n", sizeKb);
  printf("Compiled : %d\n", compiled);
  printf("Skipped  : %d\n", skipped);
  printf("Status   : Success\n");
  if (!fastSaved) cmdsFastStateSave(&c, &srcs, target, buildfilePath);
  return 0;
}
