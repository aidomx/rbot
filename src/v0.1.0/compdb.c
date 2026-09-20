#include "compdb.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "compile.h"

bool compdbEnabled(const Config *c) {
  /* Nonaktif hanya kalau eksplisit "false"; "true" maupun "auto" (default)
     sama-sama berarti compile_commands.json dibuat/diperbarui otomatis. */
  return strcmp(c->outCompileCommands, "false") != 0;
}

static void jsonQuote(FILE *fp, const char *s) {
  fputc('"', fp);
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') fputc('\\', fp);
    fputc(*p, fp);
  }
  fputc('"', fp);
}

void writeCompdb(const Config *c, List *srcs) {
  char cwd[MAX_PATH];
  if (!getcwd(cwd, sizeof(cwd))) return;

  FILE *fp = fopen("compile_commands.json", "w");
  if (!fp) return;
  fprintf(fp, "[\n");

  for (int i = 0; i < srcs->count; i++) {
    const char *src = srcs->items[i];
    char obj[MAX_PATH];
    if (!objectPathFor(c, src, obj, sizeof(obj))) continue;

    fprintf(fp, "  {\n    \"arguments\": [\n      ");
    jsonQuote(fp, c->cc);
    fprintf(fp, ",\n      ");
    for (int h = 0; h < c->headerPublic.count; h++) {
      const char *entry = c->headerPublic.items[h];
      char flag[MAX_PATH + 8];
      if (entry[0] == '-')
        snprintf(flag, sizeof(flag), "%s", entry);
      else
        snprintf(flag, sizeof(flag), "-I%s", entry[0] == 'I' && entry[1] == '.' ? entry + 1 : entry);
      jsonQuote(fp, flag);
      fprintf(fp, ",\n      ");
    }
    for (int f = 0; f < c->flags.count; f++) {
      const char *fl = c->flags.items[f];
      if (fl[0] == '-')
        jsonQuote(fp, fl);
      else {
        char withDash[128];
        snprintf(withDash, sizeof(withDash), "-%s", fl);
        jsonQuote(fp, withDash);
      }
      fprintf(fp, ",\n      ");
    }
    fprintf(fp, "\"-std=");
    fputs(c->std, fp);
    fprintf(fp, "\",\n      \"-c\",\n      \"-o\",\n      ");
    jsonQuote(fp, obj);
    fprintf(fp, ",\n      ");
    jsonQuote(fp, src);
    fprintf(fp, "\n    ],\n    \"directory\": ");
    jsonQuote(fp, cwd);
    fprintf(fp, ",\n    \"file\": ");
    char abs[MAX_PATH * 2];
    snprintf(abs, sizeof(abs), "%s/%s", cwd, src);
    jsonQuote(fp, abs);
    fprintf(fp, ",\n    \"output\": ");
    snprintf(abs, sizeof(abs), "%s/%s", cwd, obj);
    jsonQuote(fp, abs);
    fprintf(fp, "\n  }%s\n", i + 1 < srcs->count ? "," : "");
  }
  fprintf(fp, "]\n");
  fclose(fp);
  printf("> CompDB    : compile_commands.json refreshed\n");
}
