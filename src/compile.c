#include "compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"
#include "util.h"

/* Keluarga toolchain yang didukung */
typedef enum { CC_GCC, CC_CLANG, CC_MSVC, CC_UNKNOWN } CompilerKind;

static CompilerKind compilerKind(const char *cc) {
  const char *base = strrchr(cc, '/');
  base = base ? base + 1 : cc;
  const char *dot = strrchr(base, '.');
  char basebuf[128];
  size_t bl = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  if (bl >= sizeof(basebuf)) bl = sizeof(basebuf) - 1;
  memcpy(basebuf, base, bl);
  basebuf[bl] = '\0';

  if (strcmp(basebuf, "cl") == 0) return CC_MSVC;
  if (strncmp(basebuf, "clang", 5) == 0) return CC_CLANG;
  return CC_GCC; /* gcc, cc, mingw32-gcc, dst. */
}

bool compilerIsMSVC(const Config *c) { return compilerKind(c->cc) == CC_MSVC; }

bool resolveCompiler(Config *c) {
  if (c->compilers.count == 0) {
#ifdef _WIN32
    listAdd(&c->compilers, "cl");
    listAdd(&c->compilers, "gcc");
    listAdd(&c->compilers, "clang");
#else
    listAdd(&c->compilers, "gcc");
    listAdd(&c->compilers, "clang");
#endif
  }
  for (int i = 0; i < c->compilers.count; i++) {
    if (probeAvailable(c->compilers.items[i])) {
      copyStr(c->cc, sizeof(c->cc), c->compilers.items[i]);
      return true;
    }
  }
  fprintf(stderr, "rbot: no usable compiler found (tried compiler list)\n");
  return false;
}

/*
 * Resolusi entri headers menjadi direktori polos:
 *   "-Ifoo" diteruskan apa adanya sebagai flag lengkap, "I." adalah
 *   shorthand terdokumentasi untuk ".", (meniru library-nya "lm" = -lm),
 *   selain itu dianggap sebagai direktori itu sendiri.
 */
static const char *headerEntryDir(const char *entry) {
  if (entry[0] == '-') return entry; /* flag lengkap, teruskan */
  if (entry[0] == 'I' && entry[1] == '.') return entry + 1;
  return entry;
}

/*
 * includeFlags & warningFlags memakai sintaks GNU (-I/-W) sebagai bentuk
 * kanonik di seluruh program; translasi ke sintaks MSVC (/I, /W4, /W3)
 * dilakukan satu titik di compileOne() — termasuk kompilasi object embed.
 */

/* "-Iinclude -I." dari headers.public (menerima bentuk flat & nested) */
char *includeFlags(const Config *c) {
  size_t n = 1;
  for (int i = 0; i < c->headerPublic.count; i++)
    n += strlen(c->headerPublic.items[i]) + 8;
  char *s = malloc(n);
  s[0] = '\0';
  for (int i = 0; i < c->headerPublic.count; i++) {
    const char *entry = c->headerPublic.items[i];
    if (entry[0] == '-') {
      strcat(s, entry);
    } else {
      strcat(s, "-I");
      strcat(s, headerEntryDir(entry));
    }
    strcat(s, " ");
  }
  return s;
}

/* "-Wall -Wextra" dari flags; entri mempertahankan '-' di depan bila ada */
char *warningFlags(const Config *c) {
  size_t n = 1;
  for (int i = 0; i < c->flags.count; i++)
    n += strlen(c->flags.items[i]) + 8;
  char *s = malloc(n);
  s[0] = '\0';
  for (int i = 0; i < c->flags.count; i++) {
    const char *f = c->flags.items[i];
    if (f[0] != '-') strcat(s, "-");
    strcat(s, f);
    strcat(s, " ");
  }
  return s;
}

bool objectPathFor(const Config *c, const char *src, char *out, size_t n) {
  for (int i = 0; i < c->sources.count; i++) {
    const char *root = c->sources.items[i];
    size_t rl = strlen(root);
    const char *rest = NULL;
    if (strcmp(root, ".") == 0)
      rest = src;
    else if (strncmp(src, root, rl) == 0 && (src[rl] == '/' || src[rl] == '\\'))
      rest = src + rl + 1;
    if (!rest) continue;

    char rel[MAX_PATH];
    snprintf(rel, sizeof(rel), "%s", rest);
    char *dot = strrchr(rel, '.');
    if (dot) *dot = '\0';
    snprintf(out, n, "%s/%s.o", c->outBuildDir, rel);
    return true;
  }
  return false;
}

/* "token1 token2" -> "token1","token2" — pecah per spasi lalu terjemahkan:
   -I<dir> -> /I<dir>, -W* -> /W4 (Wall/Wextra) atau /W3 (lainnya),
   token lain diteruskan (mis. /std dari pemanggil). */
static char *translateFlagsToMsvc(const char *flags) {
  size_t n = strlen(flags) + 16;
  char *out = malloc(n);
  out[0] = '\0';
  const char *p = flags;
  while (*p) {
    while (*p == ' ') p++;
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t len = (size_t)(p - start);
    if (len == 0) continue;
    if (len >= 2 && start[0] == '-' && start[1] == 'I') {
      strncat(out, "/I", n - strlen(out) - 1);
      strncat(out, start + 2, n - strlen(out) - 1);
    } else if (len >= 2 && start[0] == '-' && start[1] == 'W') {
      /* -Wall/-Wextra -> /W4; warning lain dinormalisasi ke /W3 */
      strcat(out, (len == 5 && strncmp(start, "-Wall", 5) == 0) ||
                          (len == 7 && strncmp(start, "-Wextra", 7) == 0)
                      ? "/W4"
                      : "/W3");
    } else {
      strncat(out, start, len);
    }
    strcat(out, " ");
  }
  return out;
}

bool compileOne(const Config *c, const char *inc, const char *wf, const char *src,
                const char *obj) {
  mkparent(obj);

  if (compilerKind(c->cc) == CC_MSVC) {
    /* MSVC (cl.exe): flag GNU diterjemahkan ke /I, /W4|/W3; gnu11/c11 ->
       /std:c11, c17 -> /std:c17; object -> /Fo<path>. Flag -D diteruskan
       (MSVC menerima -DNAME juga). */
    char msvcstd[16] = "c11";
    if (strstr(c->std, "17"))
      strcpy(msvcstd, "c17");
    else if (strstr(c->std, "2"))
      strcpy(msvcstd, "clatest");

    char *minc = translateFlagsToMsvc(inc);
    char *mwf = translateFlagsToMsvc(wf);
    size_t n = strlen(minc) + strlen(mwf) + 2 * strlen(src) + strlen(obj) + 128;
    char *cmd = malloc(n);
    snprintf(cmd, n, "cl /nologo %s %s /std:%s /c %s /Fo%s", minc, mwf, msvcstd, src, obj);
    bool ok = runCmd(cmd);
    free(cmd);
    free(minc);
    free(mwf);
    return ok;
  }

  char *cmd = malloc(strlen(c->cc) + strlen(inc) + strlen(wf) + strlen(c->std) + 2 * strlen(src) +
                     strlen(obj) + 64);
  sprintf(cmd, "%s %s %s -std=%s -c %s -o %s", c->cc, inc, wf, c->std, src, obj);
  bool ok = runCmd(cmd);
  free(cmd);
  return ok;
}
