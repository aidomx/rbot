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

typedef struct {
  char name[EMBED_NAME_LEN];       /* entry key; uppercase jadi prefix macro */
  bool enable;
  char src[MAX_PATH];              /* direktori yang diarsipkan */
  char extract[MAX_PATH];          /* direktori ekstraksi saat runtime (boleh kosong) */
  char pattern[128];               /* pola scan freshness, default ".rp" tidak generik */
  char archiveDir[EMBED_PATH_LEN];
  char archiveName[EMBED_NAME_LEN];
  bool tar;
  char ext[16];

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
  List libraries;      /* tiap entri jadi -l<entry> */
  List compilers;      /* yang pertama tersedia menang */

  char std[32];
  char cc[128];

  bool cleanBuildDir;
  bool cleanCompileCommands;

  bool progressBar;
  bool progressErrorAlways;

  char outBinaryName[128];
  char outBinaryDir[MAX_PATH];
  char outBuildDir[MAX_PATH];
  char outCompileCommands[16]; /* "true" | "false" | "auto" */

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

#endif /* RBOT_V0_1_0_CONFIG_H */
