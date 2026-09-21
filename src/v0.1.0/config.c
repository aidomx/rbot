#include "config.h"

#include <stdio.h>
#include <string.h>

/* ==================== Default & finalisasi ==================== */

Config configDefaults(void) {
  Config c = {0};
  copyStr(c.root, sizeof(c.root), ".");
  copyStr(c.std, sizeof(c.std), "gnu11");
  c.cleanBuildDir = true;
  c.cleanCompileCommands = false;
  c.progressBar = true;
  c.progressErrorAlways = true;
  copyStr(c.outBinaryName, sizeof(c.outBinaryName), "rbot");
  copyStr(c.outBinaryDir, sizeof(c.outBinaryDir), "bin");
  copyStr(c.outBuildDir, sizeof(c.outBuildDir), "build");
  copyStr(c.outCompileCommands, sizeof(c.outCompileCommands), "auto");
  /* default embedded per-entri; tidak ada yang di-preset di sini */
  return c;
}

static void configFinalizeEntry(EmbeddedEntry *e, const char *buildDir) {
  /* archive path: <archiveDir>/<n>[.tar.<ext>] tergantung with.tar/with.ext */
  if (e->tar) {
    if (e->ext[0])
      snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.tar.%s", e->archiveDir,
               e->archiveName, e->ext);
    else
      snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.tar", e->archiveDir,
               e->archiveName);
  } else if (e->ext[0]) {
    snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s.%s", e->archiveDir, e->archiveName,
             e->ext);
  } else {
    snprintf(e->archivePath, sizeof(e->archivePath), "%s/%s", e->archiveDir, e->archiveName);
  }

  /* object ada di bawah build dir, mis. build/rupa_modules.o */
  snprintf(e->objectPath, sizeof(e->objectPath), "%s/%s.o", buildDir, e->archiveName);
}

static void appendLibraries(List *dst, const List *src) {
  for (int i = 0; i < src->count; i++) listAdd(dst, src->items[i]);
}

/* Pilih library nested berdasarkan target host. Library flat tetap selalu dipakai. */
static void configFinalizeLibraries(Config *c) {
#if defined(__linux__)
  appendLibraries(&c->libraries, &c->librariesLinux);
#elif defined(__APPLE__)
  appendLibraries(&c->libraries, &c->librariesMacOS);
#elif defined(_WIN32)
  appendLibraries(&c->libraries, &c->librariesWindows);
#endif
}

static void configFinalize(Config *c) {
  configFinalizeLibraries(c);
  for (int i = 0; i < c->embCount; i++)
    configFinalizeEntry(&c->emb[i], c->outBuildDir);
}

/* ==================== Penerapan konfigurasi ==================== */

/*
 * Buildfile sengaja berupa file konfigurasi YAML-like kecil. Kita hanya
 * mem-parse subset yang dimiliki rbot: section, dua level nesting, dan
 * pasangan key/value skalar (mis. embedded -> archive -> with). Opsi yang
 * tidak dikenal tetap aman diabaikan.
 */
static void configApply(Config *c, const char *section, const char *sub, const char *subsub,
                        const char *key, const char *value) {
  if (!c || !key || !value) return;
  bool b;

  if (!section) { /* skalar top-level */
    if (strcmp(key, "root") == 0)
      copyStr(c->root, sizeof(c->root), value);
    else if (strcmp(key, "std") == 0)
      copyStr(c->std, sizeof(c->std), value);
    return;
  }

  if (strcmp(section, "clean") == 0) {
    if (strcmp(key, "buildDir") == 0 && parseBool(value, &b))
      c->cleanBuildDir = b;
    else if (strcmp(key, "compileCommands") == 0 && parseBool(value, &b))
      c->cleanCompileCommands = b;
    return;
  }

  if (strcmp(section, "progress") == 0) {
    if (strcmp(key, "bar") == 0 && parseBool(value, &b))
      c->progressBar = b;
    else if (strcmp(key, "error") == 0)
      c->progressErrorAlways = (strcmp(value, "always") == 0);
    return;
  }

  if (strcmp(section, "output") == 0) {
    if (strcmp(key, "binaryName") == 0)
      copyStr(c->outBinaryName, sizeof(c->outBinaryName), value);
    else if (strcmp(key, "binaryDir") == 0)
      copyStr(c->outBinaryDir, sizeof(c->outBinaryDir), value);
    else if (strcmp(key, "buildDir") == 0)
      copyStr(c->outBuildDir, sizeof(c->outBuildDir), value);
    else if (strcmp(key, "compileCommands") == 0)
      copyStr(c->outCompileCommands, sizeof(c->outCompileCommands), value);
    return;
  }

  if (strcmp(section, "embedded") == 0) {
    /* sub adalah nama entri (key apa pun pilihan project); lazily ditambahkan */
    if (!sub) return;

    EmbeddedEntry *e = NULL;
    for (int i = 0; i < c->embCount; i++) {
      if (strcmp(c->emb[i].name, sub) == 0) {
        e = &c->emb[i];
        break;
      }
    }
    if (!e) {
      if (c->embCount >= MAX_EMBEDDED) return;
      e = &c->emb[c->embCount++];
      memset(e, 0, sizeof(*e));
      copyStr(e->name, sizeof(e->name), sub);
      e->enable = true;
      e->tar = true;
      copyStr(e->archiveDir, sizeof(e->archiveDir), "modules");
      copyStr(e->archiveName, sizeof(e->archiveName), sub);
      copyStr(e->ext, sizeof(e->ext), "gz");
    }

    if (subsub && strcmp(subsub, "with") == 0) {
      if (strcmp(key, "tar") == 0 && parseBool(value, &b))
        e->tar = b;
      else if (strcmp(key, "ext") == 0)
        copyStr(e->ext, sizeof(e->ext), value);
    } else if (strcmp(key, "src") == 0) {
      copyStr(e->src, sizeof(e->src), value);
    } else if (strcmp(key, "extract") == 0) {
      copyStr(e->extract, sizeof(e->extract), value);
    } else if (strcmp(key, "pattern") == 0) {
      copyStr(e->pattern, sizeof(e->pattern), value);
    } else if (strcmp(key, "enable") == 0 && parseBool(value, &b)) {
      e->enable = b;
    } else if (strcmp(key, "dir") == 0) {
      copyStr(e->archiveDir, sizeof(e->archiveDir), value);
    } else if (strcmp(key, "name") == 0) {
      copyStr(e->archiveName, sizeof(e->archiveName), value);
    }
    return;
  }
}

static List *libraryListForPlatform(Config *c, const char *platform) {
  if (!platform) return NULL;
  if (strcmp(platform, "linux") == 0) return &c->librariesLinux;
  if (strcmp(platform, "macos") == 0 || strcmp(platform, "macOS") == 0 ||
      strcmp(platform, "darwin") == 0) return &c->librariesMacOS;
  if (strcmp(platform, "windows") == 0 || strcmp(platform, "win32") == 0 ||
      strcmp(platform, "mingw") == 0) return &c->librariesWindows;
  return NULL;
}

static void listForSection(Config *c, const char *section, const char *sub, const char *item) {
  if (strcmp(section, "sources") == 0)
    listAdd(&c->sources, item);
  else if (strcmp(section, "flags") == 0)
    listAdd(&c->flags, item);
  else if (strcmp(section, "library") == 0) {
    List *platform = libraryListForPlatform(c, sub);
    listAdd(platform ? platform : &c->libraries, item);
  } else if (strcmp(section, "compiler") == 0)
    listAdd(&c->compilers, item);
  else if (strcmp(section, "headers") == 0) {
    if (sub && strcmp(sub, "internal") == 0)
      listAdd(&c->headerInternal, item);
    else
      listAdd(&c->headerPublic, item);
  }
}

#define SECTION_LEN 64

static void parseLine(Config *c, char *section, char *sub, char *subsub, int *subIndent,
                      int *subsubIndent, char *text, int indent) {
  bool isItem = text[0] == '-';
  if (isItem) text = trim(text + 1);

  char *colon = strchr(text, ':');
  if (!colon) {
    if (!isItem || !section[0]) return; /* teks nyasar, abaikan */
    listForSection(c, section, sub[0] ? sub : NULL, text);
    return;
  }

  *colon = '\0';
  char *key = trim(text);
  char *value = trim(colon + 1);

  if (!*value) {
    if (indent == 0) {
      snprintf(section, SECTION_LEN, "%s", key);
      sub[0] = '\0';
      subsub[0] = '\0';
    } else if (!sub[0] || indent <= *subIndent) {
      snprintf(sub, SECTION_LEN, "%s", key);
      *subIndent = indent;
      subsub[0] = '\0';
    } else {
      snprintf(subsub, SECTION_LEN, "%s", key);
      *subsubIndent = indent;
    }
    return;
  }

  if (indent == 0) {
    configApply(c, NULL, NULL, NULL, key, value);
    return;
  }
  if (!section[0]) return;
  if (subsub[0] && indent > *subsubIndent)
    configApply(c, section, sub, subsub, key, value);
  else
    configApply(c, section, sub, NULL, key, value);
}

bool loadConfig(Config *c, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "rbot: cannot open %s\n", path);
    return false;
  }

  char line[1024];
  char section[SECTION_LEN] = {0};
  char sub[SECTION_LEN] = {0};
  char subsub[SECTION_LEN] = {0};
  int subIndent = 0, subsubIndent = 0;
  while (fgets(line, sizeof(line), fp)) {
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';

    int indent = 0;
    for (char *q = line; *q && (*q == ' ' || *q == '\t'); q++)
      indent += (*q == '\t') ? 2 : 1;

    char *text = trim(line);
    if (!*text) continue;
    parseLine(c, section, sub, subsub, &subIndent, &subsubIndent, text, indent);
  }
  fclose(fp);
  configFinalize(c);
  return true;
}
