#include "compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

bool resolveCompiler(Config *c) {
  if (c->compilers.count == 0) listAdd(&c->compilers, "gcc");
  for (int i = 0; i < c->compilers.count; i++) {
    char probe[512];
    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", c->compilers.items[i]);
    if (system(probe) == 0) {
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
    else if (strncmp(src, root, rl) == 0 && src[rl] == '/')
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

bool compileOne(const Config *c, const char *inc, const char *wf, const char *src,
                const char *obj) {
  mkparent(obj);
  char *cmd = malloc(strlen(c->cc) + strlen(inc) + strlen(wf) + strlen(c->std) + 2 * strlen(src) +
                     strlen(obj) + 64);
  sprintf(cmd, "%s %s %s -std=%s -c %s -o %s", c->cc, inc, wf, c->std, src, obj);
  bool ok = runCmd(cmd);
  free(cmd);
  return ok;
}
