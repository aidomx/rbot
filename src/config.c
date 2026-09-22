#include "config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"

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

/* ==================== Persistent Buildfile cache ==================== */

#define CONFIG_CACHE_MAGIC "RBOTCFG1"
#define CONFIG_CACHE_VERSION 1u
#define CONFIG_CACHE_DIR ".rbot"
#define CONFIG_CACHE_FILE "buildfile.cache"

static bool cacheWriteBytes(FILE *fp, const void *p, size_t n) {
  return fwrite(p, 1, n, fp) == n;
}

static bool cacheReadBytes(FILE *fp, void *p, size_t n) {
  return fread(p, 1, n, fp) == n;
}

static bool cacheWriteU32(FILE *fp, uint32_t v) { return cacheWriteBytes(fp, &v, sizeof(v)); }
static bool cacheReadU32(FILE *fp, uint32_t *v) { return cacheReadBytes(fp, v, sizeof(*v)); }
static bool cacheWriteU8(FILE *fp, uint8_t v) { return cacheWriteBytes(fp, &v, sizeof(v)); }
static bool cacheReadU8(FILE *fp, uint8_t *v) { return cacheReadBytes(fp, v, sizeof(*v)); }

static bool cacheWriteString(FILE *fp, const char *s) {
  uint32_t n = (uint32_t)(s ? strlen(s) : 0);
  return cacheWriteU32(fp, n) && (!n || cacheWriteBytes(fp, s, n));
}

static bool cacheReadString(FILE *fp, char *dst, size_t cap) {
  uint32_t n = 0;
  if (!cacheReadU32(fp, &n) || n >= cap) return false;
  if (n && !cacheReadBytes(fp, dst, n)) return false;
  dst[n] = '\0';
  return true;
}

static bool cacheWriteList(FILE *fp, const List *l) {
  if (!cacheWriteU32(fp, (uint32_t)l->count)) return false;
  for (int i = 0; i < l->count; i++)
    if (!cacheWriteString(fp, l->items[i])) return false;
  return true;
}

static bool cacheReadList(FILE *fp, List *l) {
  uint32_t count = 0;
  if (!cacheReadU32(fp, &count) || count > MAX_LIST) return false;
  for (uint32_t i = 0; i < count; i++) {
    char item[MAX_PATH * 2];
    if (!cacheReadString(fp, item, sizeof(item))) return false;
    listAdd(l, item);
  }
  return true;
}

static bool cacheWriteEmbedded(FILE *fp, const EmbeddedEntry *e) {
  uint8_t b = e->enable ? 1 : 0;
  uint8_t tar = e->tar ? 1 : 0;
  return cacheWriteString(fp, e->name) &&
         cacheWriteU8(fp, b) &&
         cacheWriteString(fp, e->src) &&
         cacheWriteString(fp, e->extract) &&
         cacheWriteString(fp, e->pattern) &&
         cacheWriteString(fp, e->archiveDir) &&
         cacheWriteString(fp, e->archiveName) &&
         cacheWriteU8(fp, tar) &&
         cacheWriteString(fp, e->ext) &&
         cacheWriteString(fp, e->archivePath) &&
         cacheWriteString(fp, e->objectPath);
}

static bool cacheReadEmbedded(FILE *fp, EmbeddedEntry *e) {
  uint8_t b = 0, tar = 0;
  memset(e, 0, sizeof(*e));
  if (!cacheReadString(fp, e->name, sizeof(e->name)) ||
      !cacheReadU8(fp, &b) ||
      !cacheReadString(fp, e->src, sizeof(e->src)) ||
      !cacheReadString(fp, e->extract, sizeof(e->extract)) ||
      !cacheReadString(fp, e->pattern, sizeof(e->pattern)) ||
      !cacheReadString(fp, e->archiveDir, sizeof(e->archiveDir)) ||
      !cacheReadString(fp, e->archiveName, sizeof(e->archiveName)) ||
      !cacheReadU8(fp, &tar) ||
      !cacheReadString(fp, e->ext, sizeof(e->ext)) ||
      !cacheReadString(fp, e->archivePath, sizeof(e->archivePath)) ||
      !cacheReadString(fp, e->objectPath, sizeof(e->objectPath)))
    return false;
  e->enable = b != 0;
  e->tar = tar != 0;
  return true;
}

static bool cacheWriteConfig(FILE *fp, const Config *c) {
  uint8_t cleanBuild = c->cleanBuildDir ? 1 : 0;
  uint8_t cleanCompdb = c->cleanCompileCommands ? 1 : 0;
  uint8_t progress = c->progressBar ? 1 : 0;
  uint8_t progressError = c->progressErrorAlways ? 1 : 0;

  if (!cacheWriteString(fp, c->root) || !cacheWriteList(fp, &c->sources) ||
      !cacheWriteList(fp, &c->flags) || !cacheWriteList(fp, &c->headerInternal) ||
      !cacheWriteList(fp, &c->headerPublic) || !cacheWriteList(fp, &c->libraries) ||
      !cacheWriteList(fp, &c->librariesLinux) || !cacheWriteList(fp, &c->librariesMacOS) ||
      !cacheWriteList(fp, &c->librariesWindows) || !cacheWriteList(fp, &c->compilers) ||
      !cacheWriteString(fp, c->std) || !cacheWriteString(fp, c->cc) ||
      !cacheWriteU8(fp, cleanBuild) || !cacheWriteU8(fp, cleanCompdb) ||
      !cacheWriteU8(fp, progress) || !cacheWriteU8(fp, progressError) ||
      !cacheWriteString(fp, c->outBinaryName) || !cacheWriteString(fp, c->outBinaryDir) ||
      !cacheWriteString(fp, c->outBuildDir) || !cacheWriteString(fp, c->outCompileCommands) ||
      !cacheWriteU32(fp, (uint32_t)c->embCount))
    return false;

  for (int i = 0; i < c->embCount; i++)
    if (!cacheWriteEmbedded(fp, &c->emb[i])) return false;
  return true;
}

static bool cacheReadConfig(FILE *fp, Config *c) {
  uint8_t cleanBuild = 0, cleanCompdb = 0, progress = 0, progressError = 0;
  uint32_t embCount = 0;

  if (!cacheReadString(fp, c->root, sizeof(c->root)) ||
      !cacheReadList(fp, &c->sources) || !cacheReadList(fp, &c->flags) ||
      !cacheReadList(fp, &c->headerInternal) || !cacheReadList(fp, &c->headerPublic) ||
      !cacheReadList(fp, &c->libraries) || !cacheReadList(fp, &c->librariesLinux) ||
      !cacheReadList(fp, &c->librariesMacOS) || !cacheReadList(fp, &c->librariesWindows) ||
      !cacheReadList(fp, &c->compilers) || !cacheReadString(fp, c->std, sizeof(c->std)) ||
      !cacheReadString(fp, c->cc, sizeof(c->cc)) || !cacheReadU8(fp, &cleanBuild) ||
      !cacheReadU8(fp, &cleanCompdb) || !cacheReadU8(fp, &progress) ||
      !cacheReadU8(fp, &progressError) ||
      !cacheReadString(fp, c->outBinaryName, sizeof(c->outBinaryName)) ||
      !cacheReadString(fp, c->outBinaryDir, sizeof(c->outBinaryDir)) ||
      !cacheReadString(fp, c->outBuildDir, sizeof(c->outBuildDir)) ||
      !cacheReadString(fp, c->outCompileCommands, sizeof(c->outCompileCommands)) ||
      !cacheReadU32(fp, &embCount) || embCount > MAX_EMBEDDED)
    return false;

  c->cleanBuildDir = cleanBuild != 0;
  c->cleanCompileCommands = cleanCompdb != 0;
  c->progressBar = progress != 0;
  c->progressErrorAlways = progressError != 0;
  c->embCount = (int)embCount;
  for (int i = 0; i < c->embCount; i++)
    if (!cacheReadEmbedded(fp, &c->emb[i])) return false;
  return true;
}

static void buildfileCachePath(const char *path, char *out, size_t n) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
  if (slash) {
    size_t dirLen = (size_t)(slash - path);
    if (dirLen == 0)
      snprintf(out, n, "/%s/%s", CONFIG_CACHE_DIR, CONFIG_CACHE_FILE);
    else
      snprintf(out, n, "%.*s/%s/%s", (int)dirLen, path, CONFIG_CACHE_DIR, CONFIG_CACHE_FILE);
  } else {
    snprintf(out, n, "%s/%s", CONFIG_CACHE_DIR, CONFIG_CACHE_FILE);
  }
}

static bool loadConfigCache(Config *c, const char *path, const char *cachePath) {
  FILE *fp = fopen(cachePath, "rb");
  if (!fp) return false;

  char magic[sizeof(CONFIG_CACHE_MAGIC) - 1];
  uint32_t version = 0;
  int64_t cachedMTimeNs = 0;
  int64_t cachedSize = 0;
  int64_t currentMTimeNs = fsMTimeNs(path);
  long long currentSize = fsFileSize(path);

  bool ok = cacheReadBytes(fp, magic, sizeof(magic)) &&
            memcmp(magic, CONFIG_CACHE_MAGIC, sizeof(magic)) == 0 &&
            cacheReadU32(fp, &version) && version == CONFIG_CACHE_VERSION &&
            cacheReadBytes(fp, &cachedMTimeNs, sizeof(cachedMTimeNs)) &&
            cacheReadBytes(fp, &cachedSize, sizeof(cachedSize)) &&
            currentMTimeNs >= 0 && currentSize >= 0 &&
            cachedMTimeNs == currentMTimeNs && cachedSize == (int64_t)currentSize &&
            cacheReadConfig(fp, c);
  fclose(fp);
  return ok;
}

static void saveConfigCache(const Config *c, const char *path, const char *cachePath) {
  int64_t mtimeNs = fsMTimeNs(path);
  long long size = fsFileSize(path);
  if (mtimeNs < 0 || size < 0) return;

  char parent[MAX_PATH];
  snprintf(parent, sizeof(parent), "%s", cachePath);
  char *slash = strrchr(parent, '/');
#ifdef _WIN32
  char *backslash = strrchr(parent, '\\');
  if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
  if (slash) {
    *slash = '\0';
    mkdirs(parent);
  }

  char tmp[MAX_PATH + 32];
  snprintf(tmp, sizeof(tmp), "%s.tmp", cachePath);
  FILE *fp = fopen(tmp, "wb");
  if (!fp) return;

  const char magic[] = CONFIG_CACHE_MAGIC;
  bool ok = cacheWriteBytes(fp, magic, sizeof(magic) - 1) &&
            cacheWriteU32(fp, CONFIG_CACHE_VERSION) &&
            cacheWriteBytes(fp, &mtimeNs, sizeof(int64_t)) &&
            cacheWriteBytes(fp, &size, sizeof(int64_t)) &&
            cacheWriteConfig(fp, c);
  if (fclose(fp) != 0) ok = false;
  if (ok) {
    /* POSIX rename is atomic; on Windows remove the old cache first because
       the MSVCRT rename() refuses to replace an existing file. */
#ifdef _WIN32
    fsRemoveFile(cachePath);
#endif
    if (rename(tmp, cachePath) != 0) fsRemoveFile(tmp);
  } else {
    fsRemoveFile(tmp);
  }
}

bool loadConfig(Config *c, const char *path) {
  char cachePath[MAX_PATH];
  buildfileCachePath(path, cachePath, sizeof(cachePath));

  /* Cache validation only stats Buildfile; the Buildfile itself is not opened
     on a cache hit. */
  if (loadConfigCache(c, path, cachePath)) return true;

  /* A corrupt/stale cache may have partially populated c before failing. */
  *c = configDefaults();

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
  saveConfigCache(c, path, cachePath);
  return true;
}
