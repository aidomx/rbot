#ifndef RBOT_V0_1_0_CONFIG_H
#define RBOT_V0_1_0_CONFIG_H

#include <stdbool.h>

#include "util.h"

/*
 * rbot — build tool didorong oleh Buildfile deklaratif.
 *
 * Model config sengaja meniru loader format .rupa: subset YAML-like kecil
 * dengan section, satu level nesting, properti `- key: value`, dan entri
 * list `- item`. Key yang tidak dikenal diabaikan demi kompatibilitas maju.
 */

#define MAX_EMBEDDED 8
#define EMBED_NAME_LEN 64
#define EMBED_PATH_LEN 256

/*
 * pack: proyek pengemasan artefak build (Buildfile: pack.*).
 *
 * pack.files/pack.output menandai proyek pengemasan aktif. Dua artefak:
 *   - tarball selalu dibuat di pack.output; kompresi dari pack.compress
 *     bila diset, kalau tidak dari ekstensi (.tar.gz/.tgz/.tar.xz/
 *     .tar.bz2/.tar).
 *   - pack.format = deb menambah .deb di sampingnya (nama: ekstensi tar
 *     pada pack.output diganti .deb) dengan metadata pack.deb.*; setiap
 *     entri pack.files dipasang di <install_prefix>/<entri>.
 * pack.checksum = sha256 menulis <artefak>.sha256 (format `sha256sum -c`).
 *
 * Di Buildfile.ws, setting per proyek disintesis TANPA prefix
 * projects.<nama>. — sehingga key bare (name, version, files, output,
 * compress, checksum, format, deb.*) juga dirutekan ke pack di sini.
 * Proyek pack tanpa sources otomatis output.binary = false (tidak ada
 * yang dikompilasi); proyek dengan sources membangun binary dulu, baru
 * mengemas.
 */
typedef struct {
  bool requested; /* diturunkan: pack.files/pack.output disebut */
  char name[128];            /* default: output.binaryName */
  char version[64];          /* default "0.0.0" */
  List files;                /* file/folder yang masuk paket */
  char output[MAX_PATH * 2]; /* template: {name} {version} {os} {arch} */
  char compress[16];         /* "" = dari ekstensi; gzip|none|xz|bz2 */
  char checksum[16];         /* "" = tanpa; sha256 */
  char format[16];           /* "" = tar; deb menambah artefak .deb */
  char debMaintainer[192];
  char debDescription[512];
  char debInstallPrefix[MAX_PATH];
  char debArchitecture[32];  /* kosong = otomatis dari arsitektur host */
} PackConfig;

typedef struct {
  char name[EMBED_NAME_LEN]; /* entry key; uppercase jadi prefix macro */
  bool enable;
  char src[MAX_PATH];     /* direktori yang diarsipkan */
  char extract[MAX_PATH]; /* direktori ekstraksi saat runtime (boleh kosong) */
  char pattern[128];      /* pola scan freshness, default ".rp" tidak generik */
  List excludes;          /* pola/path yang dikeluarkan dari archive */
  char archiveDir[EMBED_PATH_LEN];
  char archiveName[EMBED_NAME_LEN];
  bool tar;
  char ext[16];

  /*
   * Arsip jadi (embedded.<n>.file): bila diset, rbot TIDAK mengarsipkan
   * <src> sendiri — <file> dipakai sebagai arsip input (mis. hasil proyek
   * kemasan lain di workspace). Freshness tetap dicek terhadap mtime
   * <file>. <src> boleh kosong dalam mode ini.
   */
  bool usePrebuilt;
  char prebuiltPath[MAX_PATH];

  /* Override prefix macro EMBED_<N>_ (embedded.<n>.variable). */
  char variable[EMBED_NAME_LEN];

  /* diturunkan di configFinalizeEntry */
  char archivePath[MAX_PATH + 192]; /* <archiveDir>/<n>[.tar.<ext>] */
  char objectPath[MAX_PATH + 64];   /* <buildDir>/<n>.o */
} EmbeddedEntry;

typedef struct {
  char root[MAX_PATH];

  List sources;
  List flags;
  List headerInternal; /* metadata saja, tidak dilewatkan ke compiler */
  List headerPublic;   /* tiap entri jadi -I<entry> */
  List libraries;      /* library aktif: flat + platform yang dipilih */
  List librariesLinux;
  List librariesMacOS;
  List librariesWindows;
  List compilers; /* yang pertama tersedia menang */

  char std[32];
  char cc[128];

  /*
   * target (Buildfile: target, opsional): arsitektur yang dituju compiler.
   * Kosong = host (perilaku lama). Nilai yang dikenal: x86_64, arm64,
   * riscv64 — dipetakan ke flag compiler/linker sesuai toolchain (GNU/Clang:
   * -march/-arch/-mabi; MSVC: /ARCH:AVX512 dsb.) dan disertakan di compdb.
   */
  char target[32];

  bool cleanBuildDir;
  bool cleanCompileCommands;

  /*
   * Bahasa proyek (diturunkan, bukan setting Buildfile): true bila salah
   * satu sumber berakhiran .cpp (folder/entri sources discan saat
   * loadConfig — SEBELUM defaults & finalisasi). Mengubah default std
   * (c++17 vs gnu11), urutan kandidat compiler (g++/clang++ vs gcc/clang),
   * dan pemilihan toolchain link. Proyek campuran C/C++ ikut jalur C++
   * (linker C++ menyelesaikan runtime keduanya).
   */
  bool langCpp;

  bool progressBar;
  bool progressErrorAlways;

  /*
   * foreground (default true): bila true, child build berjalan di process
   * group yang sama dengan rbot (perilaku klasik — Ctrl+C dibawa terminal
   * ke semua proses). Bila false, child berjalan di process group terpisah
   * sehingga SIGINT dari terminal hanya sampai ke rbot; rbot meneruskannya
   * ke child secara eksplisit lalu membatalkan build dengan rapi.
   */
  bool foreground;

  List excludes; /* nama/path file yang dilepas dari build (mis. main.c) */

  /*
   * Alias "canonical\talias" (format "use alias", lihat config.c). Hanya
   * terisi saat parse Buildfile — tidak di-cache (cache menyimpan config
   * final yang key-nya sudah kanonik).
   */
  List aliases;

  /* false akan mengabaikan menghasilkan binary */
  bool binary;

  char outBinaryName[128];
  char outBinaryDir[MAX_PATH];
  char outBuildDir[MAX_PATH];
  char outCompileCommands[16]; /* "true" | "false" | "auto" */

  /*
   * library (opsional): selain binary, build memproduksi library dari
   * object yang sama. Hasil:
   *   statis  -> <libDir>/lib<libName>.a     (ar; Windows: <libName>.lib)
   *   shared  -> <libDir>/lib<libName>.so    (Linux/macOS: .so/.dylib;
   *                                            Windows: <libName>.dll)
   * <libDir> default "lib", <libName> default = outBinaryName.
   * Library hanya dibangun bila libraryName/libraryStatic/libraryShared
   * disebut di Buildfile (libRequested).
   */
  bool libRequested;
  bool libStatic;
  bool libShared;
  char outLibName[128];
  char outLibDir[MAX_PATH];

  /*
   * pack: pengemasan artefak (Buildfile: pack; lihat PackConfig di atas).
   */
  PackConfig pack;

  /*
   * embedded: entri bernama yang asetnya diarsipkan dan di-embed ke binary
   * (Buildfile: embedded). Generik — project yang menentukan apa yang
   * di-embed; rbot hanya mengarsipkan, mengonversi via ld -r -b binary,
   * dan menerbitkan header.
   */
  EmbeddedEntry emb[MAX_EMBEDDED];
  int embCount;
} Config;

Config configDefaults(void);
bool loadConfig(Config *c, const char *path);

/*
 * loadConfigEx — varian loadConfig dengan kontrol penulisan cache.
 * cacheWrite=false: parse Buildfile TANPA menulis .rbot (cache config) —
 * untuk jalur non-mutating (`rbot profile`) yang tidak boleh memperbarui
 * cache build normal. Membaca cache yang valid tetap diizinkan (read-only).
 */
bool loadConfigEx(Config *c, const char *path, bool cacheWrite);

/*
 * configFinalize — terapkan konvensi proyek standar pada Config yang sudah
 * di-parse (sources->src, headers->include bila folder ada, nama binary
 * dari cwd, deteksi C++, resolusi path embedded). Dipanggil loadConfig
 * setelah parse; juga publik untuk jalur tanpa Buildfile (rbot profile).
 * MURNI derivasi in-memory — tidak menulis file apa pun.
 */
void configFinalize(Config *c);

#endif /* RBOT_V0_1_0_CONFIG_H */
