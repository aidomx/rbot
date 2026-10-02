/*
 * nc_util — util dasar & model file ninja untuk konverter build.ninja.
 *
 * Dipindah apa adanya dari ninjaconv.c: string/token helper, kumpulan
 * string unik (NcList), model NcRule/NcEdge/NcFile beserta lifecycle-nya,
 * dan variabel top-level. Tipe & deklarasi bersama ada di nc_internal.h.
 */
#include "nc_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *ncStrndup(const char *a, const char *b) {
  size_t n = (size_t)(b - a);
  char *out = malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, a, n);
  out[n] = '\0';
  return out;
}

char *ncNextTok(char **cursor) {
  char *p = *cursor;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!*p) {
    *cursor = p;
    return NULL;
  }
  char *start = p;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  char *tok = ncStrndup(start, p);
  *cursor = p;
  return tok;
}

char *ncTok(char *str, const char *delim, char **save) {
  if (!str) str = *save;
  if (!str) return NULL;
  str += strspn(str, delim);
  if (!*str) {
    *save = str;
    return NULL;
  }
  char *end = str + strcspn(str, delim);
  if (*end) {
    *end = '\0';
    *save = end + 1;
  } else {
    *save = end;
  }
  return str;
}

bool ncAppendText(char **buf, size_t *len, size_t *cap, const char *text) {
  size_t add = strlen(text);
  if (add > SIZE_MAX - *len - 1) return false;
  size_t need = *len + add + 1;
  if (need > *cap) {
    size_t next = *cap ? *cap : 128;
    while (next < need) {
      if (next > SIZE_MAX / 2) { next = need; break; }
      next *= 2;
    }
    char *grown = realloc(*buf, next);
    if (!grown) return false;
    *buf = grown;
    *cap = next;
  }
  memcpy(*buf + *len, text, add + 1);
  *len += add;
  return true;
}

bool ncAdd(NcList *l, const char *s) {
  if (!l || !s || !*s) return false;
  for (int i = 0; i < l->count; i++)
    if (strcmp(l->items[i], s) == 0) return true;
  if (l->count == l->capacity) {
    if (l->capacity > 0x3fffffff) return false;
    int next = l->capacity ? l->capacity * 2 : 8;
    char **items = realloc(l->items, (size_t)next * sizeof(*items));
    if (!items) return false;
    l->items = items;
    l->capacity = next;
  }
  char *copy = ncStrndup(s, s + strlen(s));
  if (!copy) return false;
  l->items[l->count++] = copy;
  return true;
}

void ncFree(NcList *l) {
  for (int i = 0; i < l->count; i++)
    free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->count = 0;
  l->capacity = 0;
}

const NcRule *ncRuleFind(const NcFile *f, const char *name) {
  for (int i = 0; i < f->nrules; i++)
    if (strcmp(f->rules[i].name, name) == 0) return &f->rules[i];
  return NULL;
}

void ncFileFree(NcFile *f) {
  for (int i = 0; i < f->nrules; i++) {
    free(f->rules[i].name);
    free(f->rules[i].command);
  }
  for (int i = 0; i < f->nedges; i++) {
    free(f->edges[i].out);
    free(f->edges[i].rule);
    free(f->edges[i].in);
    free(f->edges[i].ins);
    free(f->edges[i].flags);
    free(f->edges[i].incs);
    free(f->edges[i].defs);
    free(f->edges[i].depfile);
  }
  free(f->edges);
  ncFree(&f->varNames);
  ncFree(&f->varVals);
}

void ncVarSet(NcFile *f, const char *key, const char *val) {
  for (int i = 0; i < f->varNames.count; i++)
    if (strcmp(f->varNames.items[i], key) == 0) {
      free(f->varVals.items[i]);
      f->varVals.items[i] = ncStrndup(val, val + strlen(val));
      return;
    }
  ncAdd(&f->varNames, key);
  ncAdd(&f->varVals, val);
}

const char *ncVarGet(const NcFile *f, const char *name) {
  for (int i = 0; i < f->varNames.count; i++)
    if (strcmp(f->varNames.items[i], name) == 0) return f->varVals.items[i];
  return NULL;
}
