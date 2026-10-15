/*
 * mk_convert — inti konversi make -> Buildfile (design/convert.md).
 *
 * Filosofi "Declarative Extraction": hanya konfigurasi statis deklaratif yang
 * diekstrak (compiler, std, flags, headers, library, sources, target). Logika
 * prosedural (ifeq, $(MAKE), $(shell), nilai ber-$(VAR)) diabaikan; bila
 * terdeteksi, file hasil diberi blok WARNING di bagian bawah.
 *
 * Strategi parsing: fgets baris demi baris, abaikan resep (baris Tab),
 * komentar (#) dan direktif (include/ifeq/...). Deklarasi variabel
 * `NAME = / := / += / ?=` dipetakan lewat Mapping Rules design Bagian 2;
 * token -D dari flags/link flags menjadi properti `defines` tanpa prefix
 * (design/flags.md §2.1 — translasi -D//D urusan satria saat build).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../makeconv.h"
#include "../portability.h"
#include "../util.h"

#define MK_LINE 2048
#define MK_COMPLEX_LINES 500

/* ==================== helper ==================== */

/* Token kecil (flag, header, lib) — dedup linear, urutan sisipan dipertahankan
   (sesuai contoh output design: Wall, Wextra, O2). */
static void addUnique(List *l, const char *s) {
  if (!s || !*s) return;
  for (int i = 0; i < l->count; i++)
    if (strcmp(l->items[i], s) == 0) return;
  listAdd(l, s);
}

static void joinList(char *out, size_t n, const List *l) {
  out[0] = '\0';
  size_t off = 0;
  for (int i = 0; i < l->count; i++) {
    size_t tl = strlen(l->items[i]);
    if (off + tl + (i ? 2 : 1) >= n) break;
    if (i) {
      out[off++] = ',';
      out[off++] = ' ';
    }
    memcpy(out + off, l->items[i], tl + 1);
    off += tl;
  }
}

/* Tokenisasi nilai variabel per spasi, terapkan aturan pemetaan.
   kind: 'f' = flags (CFLAGS/CXXFLAGS/CPPFLAGS), 'l' = library (LDLIBS/LDFLAGS). */
static void consumeTokens(const char *val, char kind, List *flags, List *stds, List *headers,
                          List *libs, List *defines) {
  char buf[MK_LINE];
  copyStr(buf, sizeof(buf), val);
  char *save = NULL;
  for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
    if (tok[0] == '-' && tok[1] == 's' && strncmp(tok, "-std=", 5) == 0) {
      addUnique(stds, tok + 5);
    } else if (tok[0] == '-' && tok[1] == 'D') {
      /* -DFOO=1 -> `defines = FOO=1` (tanpa prefix; design/flags.md §2.1). */
      addUnique(defines, tok + 2);
    } else if (tok[0] == '-' && tok[1] == 'I') {
      const char *p = tok + 2;
      if (p[0] == '.' && p[1] == '/') p += 2; /* -I./include -> include */
      addUnique(headers, p);
    } else if (kind == 'l' && tok[0] == '-' && tok[1] == 'l' && tok[2]) {
      addUnique(libs, tok + 2);
    } else if (kind == 'f' && tok[0] == '-' && tok[1] && tok[1] != 'l' && tok[1] != 'L' &&
               tok[1] != 'I') {
      /* -Wall -> Wall, -O2 -> O2; -l/-L/-I sudah/bukan tugas flags. */
      addUnique(flags, tok + 1);
    }
  }
}

/* Directive keyword sebagai kata utuh: `kw` diikuti akhir-string, spasi,
   atau '(' (gaya ifeq(...)). Mencegah `include_dirs = x` terkira direktif. */
static bool dirIs(const char *t, const char *kw) {
  size_t n = strlen(kw);
  if (strncmp(t, kw, n) != 0) return false;
  char c = t[n];
  return c == '\0' || c == ' ' || c == '\t' || c == '(';
}

/* Target rule yang bukan phony/bawaan — kandidat fallback output.binaryName. */
static bool isOrdinaryTarget(const char *t) {
  static const char *phony[] = {"all",     "clean",     "install", "uninstall", "test",
                                "check",   "dist",      "help",    "format",    "lint",
                                "rebuild", "distclean", NULL};
  if (!t || !*t || t[0] == '.' || t[0] == '$' || strchr(t, '/')) return false;
  for (int i = 0; phony[i]; i++)
    if (strcmp(t, phony[i]) == 0) return false;
  return true;
}

/* ==================== konverter utama ==================== */

bool makeToBuildfile(const char *makePath, const char *outPath, char *err, size_t errCap) {
  FILE *in = fopen(makePath, "r");
  if (!in) {
    snprintf(err, errCap, "cannot read %s", makePath);
    return false;
  }

  List flags = {0}, stds = {0}, headers = {0}, libs = {0}, srcs = {0}, defines = {0};
  char compiler[256] = {0}; /* CC lebih diprioritaskan daripada CXX */
  char cxx[256] = {0};
  char target[256] = {0};     /* dari variabel TARGET */
  char ruleTarget[256] = {0}; /* fallback: target rule biasa pertama */
  bool complex = false;
  int nlines = 0;

  char line[MK_LINE], logical[MK_LINE * 2];

  while (fgets(line, sizeof(line), in)) {
    nlines++;
    if (nlines > MK_COMPLEX_LINES) complex = true;

    /* Sambung baris backslash-continuation. */
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    copyStr(logical, sizeof(logical), line);
    while (len && line[len - 1] == '\\') {
      line[--len] = '\0';
      if (!fgets(line, sizeof(line), in)) break;
      len = strlen(line);
      while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
      char *cont = trim(line);
      size_t cur = strlen(logical);
      while (cur && (logical[cur - 1] == ' ' || logical[cur - 1] == '\t'))
        logical[--cur] = '\0';
      if (cur + strlen(cont) + 2 < sizeof(logical)) {
        logical[cur] = ' ';
        strcpy(logical + cur + 1, cont);
      }
    }

    char *t = logical;
    if (*t == '\t') continue; /* resep (recipe) = kode shell, bukan config */
    while (*t == ' ' || *t == '\t')
      t++; /* spasi awal: bukan resep */
    if (!*t) continue;

    char *pound = strchr(t, '#');
    if (pound) *pound = '\0';
    t = trim(t);
    if (!*t) continue;

    /* `export VAR = ...`: deklaratif — strip prefix, ekstrak biasa.
       `export`/`unexport` polos (tanpa nilai): lewati. */
    if (strncmp(t, "export", 6) == 0 && (t[6] == '\0' || t[6] == ' ' || t[6] == '\t')) {
      t = trim(t + 6);
      if (!*t) continue;
    } else if (strncmp(t, "unexport", 8) == 0 && (t[8] == '\0' || t[8] == ' ' || t[8] == '\t')) {
      continue;
    }
    /* Direktif & kompleksitas prosedural (harus kata utuh — jangan
       tangkap variabel seperti `include_dirs`). */
    if (dirIs(t, "include") || dirIs(t, "ifeq") || dirIs(t, "ifneq") || dirIs(t, "ifdef") ||
        dirIs(t, "ifndef") || dirIs(t, "else") || dirIs(t, "endif") || dirIs(t, "define") ||
        dirIs(t, "endef") || dirIs(t, "vpath")) {
      if (!dirIs(t, "include")) complex = true;
      continue;
    }
    if (strstr(t, "$(MAKE)") || strstr(t, "${MAKE}")) {
      complex = true;
      continue;
    }

    /* Deklarasi variabel: NAME op= value (op: = := += ?=). Spasi diizinkan
       sebelum operator (gaya `CC = gcc`). `:` tanpa `=` = baris rule —
       BUKAN assignment (jangan sampai `all: x` masuk jalur variabel). */
    char *eq = NULL;
    for (char *p = t; *p; p++) {
      if (*p == '=') {
        eq = p;
        break;
      }
      if (*p == ':') {
        if (p[1] == '=') {
          eq = p + 1; /* := */
          break;
        }
        eq = NULL; /* rule line */
        break;
      }
    }
    if (eq) {
      /* Validasi nama variabel (bagian sebelum operator, trim spasi). */
      char name[256];
      char *nameEnd = eq;
      while (nameEnd > t && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t'))
        nameEnd--;
      if (nameEnd > t && (nameEnd[-1] == '?' || nameEnd[-1] == '+' || nameEnd[-1] == ':'))
        nameEnd--; /* ?= += := */
      while (nameEnd > t && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t'))
        nameEnd--;
      size_t nl = (size_t)(nameEnd - t);
      if (nl == 0 || nl >= sizeof(name)) continue;
      bool validName = true;
      for (size_t i = 0; i < nl; i++) {
        char c = t[i];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9' && i > 0))) {
          validName = false;
          break;
        }
      }
      if (!validName) continue;
      memcpy(name, t, nl);
      name[nl] = '\0';

      bool isAppend = false, isDefault = false;
      if (eq > t) { /* operator terlihat di karakter sebelum '=' */
        char prev = eq[-1];
        if (prev == '+')
          isAppend = true;
        else if (prev == '?')
          isDefault = true;
      }
      char *v = eq + 1; /* lewati '=' */
      v = trim(v);

      /* Referensi $(VAR)/${VAR} prosedural: tidak diekstrak, tandai kompleks. */
      if (strstr(v, "$(") || strstr(v, "${")) {
        complex = true;
        continue;
      }

      /* `?=` (isDefault): isi hanya bila tujuan masih kosong. */
      if (strcmp(name, "CC") == 0) {
        if (*v && (!isDefault || !compiler[0])) copyStr(compiler, sizeof(compiler), v);
      } else if (strcmp(name, "CXX") == 0) {
        if (*v && (!isDefault || !cxx[0])) copyStr(cxx, sizeof(cxx), v);
      } else if (strcmp(name, "CFLAGS") == 0 || strcmp(name, "CXXFLAGS") == 0 ||
                 strcmp(name, "CPPFLAGS") == 0) {
        if (*v) consumeTokens(v, 'f', &flags, &stds, &headers, &libs, &defines);
      } else if (strcmp(name, "LDLIBS") == 0 || strcmp(name, "LDFLAGS") == 0) {
        if (*v) consumeTokens(v, 'l', &flags, &stds, &headers, &libs, &defines);
      } else if (strcmp(name, "SRCS") == 0 || strcmp(name, "SOURCES") == 0) {
        if (*v && !isAppend) {
          listFree(&srcs);
          srcs.items = NULL;
          srcs.count = srcs.capacity = 0;
        }
        char buf[MK_LINE];
        copyStr(buf, sizeof(buf), v);
        char *save = NULL;
        for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
          addUnique(&srcs, tok);
      } else if (strcmp(name, "TARGET") == 0) {
        if (*v && (!isDefault || !target[0])) copyStr(target, sizeof(target), v);
      }
      continue;
    }

    /* Rule `target: deps` — fallback binaryName bila TARGET tidak ada. */
    char *colon = strchr(t, ':');
    if (colon && colon > t && colon[1] != ':' && !ruleTarget[0]) {
      char name[256];
      size_t nl = (size_t)(colon - t);
      if (nl < sizeof(name)) {
        bool hasSpace = false;
        for (size_t i = 0; i < nl; i++)
          if (t[i] == ' ' || t[i] == '\t') hasSpace = true;
        if (!hasSpace) {
          memcpy(name, t, nl);
          name[nl] = '\0';
          char *sp = strchr(name, ' '); /* hapus suffix rule (pattern) */
          if (sp) *sp = '\0';
          if (isOrdinaryTarget(name)) copyStr(ruleTarget, sizeof(ruleTarget), name);
        }
      }
    }
  }
  fclose(in);

  /* CC diprioritaskan; CXX hanya bila CC kosong (contoh design: CC=gcc). */
  if (!compiler[0] && cxx[0]) copyStr(compiler, sizeof(compiler), cxx);

  const char *binName = target[0] ? target : ruleTarget;

  FILE *out = fopen(outPath, "w");
  if (!out) {
    snprintf(err, errCap, "cannot write %s", outPath);
    listFree(&flags);
    listFree(&stds);
    listFree(&headers);
    listFree(&libs);
    listFree(&srcs);
    listFree(&defines);
    return false;
  }

  char buf[MK_LINE * 2];
  fprintf(out, "# Generated by satria -xf make\n");
  fprintf(out, "# Original file: %s\n\n", makePath);

  if (compiler[0]) fprintf(out, "compiler = %s\n", compiler);
  if (stds.count) {
    joinList(buf, sizeof(buf), &stds);
    fprintf(out, "std = %s\n", buf);
  }
  if (flags.count) {
    joinList(buf, sizeof(buf), &flags);
    fprintf(out, "flags = %s\n", buf);
  }
  if (defines.count) {
    joinList(buf, sizeof(buf), &defines);
    fprintf(out, "defines = %s\n", buf);
  }
  if (headers.count) {
    joinList(buf, sizeof(buf), &headers);
    fprintf(out, "headers = %s\n", buf);
  }
  if (libs.count) {
    joinList(buf, sizeof(buf), &libs);
    fprintf(out, "library = %s\n", buf);
  }
  if (srcs.count) {
    joinList(buf, sizeof(buf), &srcs);
    fprintf(out, "\nsources = %s\n", buf);
  }
  if (binName[0]) fprintf(out, "\noutput.binaryName = %s\n", binName);

  if (complex) {
    fprintf(out, "\n# ---------------------------------------------------------\n"
                 "# WARNING: Makefile asli memiliki target kompleks/prosedural\n"
                 "# (seperti recursive make, conditional ifeq, atau skrip shell)\n"
                 "# yang tidak dapat dikonversi secara otomatis ke Buildfile\n"
                 "# deklaratif. Silakan periksa logika build kustom Anda secara "
                 "manual.\n"
                 "# ---------------------------------------------------------\n");
  }
  fclose(out);

  listFree(&flags);
  listFree(&stds);
  listFree(&headers);
  listFree(&libs);
  listFree(&srcs);
  listFree(&defines);
  return true;
}
