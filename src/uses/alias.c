#include "alias.h"

#include <stdio.h>
#include <string.h>

/*
 * alias — implementasi mesin `X as Y`. Dipindah apa adanya dari config.c;
 * hanya nama struct List & trim() yang sudah di header util bersama.
 */

void aliasListAdd(List *l, const char *canonical, const char *alias) {
  if (!canonical || !*canonical || !alias || !*alias || !strcmp(canonical, alias)) return;
  if (l->count >= ALIAS_MAX) return;
  for (int i = 0; i < l->count; i++) {
    const char *tab = strchr(l->items[i], '\t');
    if (tab && strcmp(tab + 1, alias) == 0) return; /* alias pertama menang */
  }
  char buf[ALIAS_BUF_MAX];
  snprintf(buf, sizeof(buf), "%s\t%s", canonical, alias);
  listAdd(l, buf);
}

static bool aliasResolveOnce(const List *l, char *key, size_t cap) {
  for (int i = 0; i < l->count; i++) {
    const char *tab = strchr(l->items[i], '\t');
    if (!tab) continue;
    size_t cl = (size_t)(tab - l->items[i]);
    const char *al = tab + 1;
    size_t alLen = strlen(al);
    size_t kLen = strlen(key);
    if (kLen < alLen || strncmp(key, al, alLen) != 0) continue;
    if (alLen < kLen && key[alLen] != '.') continue;

    char out[ALIAS_BUF_MAX + ALIAS_KEY_MAX];
    snprintf(out, sizeof(out), "%.*s%s", (int)cl, l->items[i], key + alLen);
    if (strcmp(out, key) == 0) continue;
    copyStr(key, cap, out);
    return true;
  }
  return false;
}

void aliasResolve(const List *l, char *key, size_t cap) {
  for (int guard = 0; guard < 8 && aliasResolveOnce(l, key, cap); guard++) {
  }
}

bool aliasLineAdd(List *l, char *text) {
  char *as = strstr(text, " as ");
  if (!as) return false;
  *as = '\0';
  aliasListAdd(l, trim(text), trim(as + 4));
  return true;
}
