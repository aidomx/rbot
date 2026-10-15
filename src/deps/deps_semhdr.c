/*
 * deps_semhdr — pemeriksaan semantik perubahan header (opt-in: RBOT_HDR_SEM=1).
 *
 * Konservatif: header yang berubah hanya dimaafkan bila SEMUA perbedaan barisnya
 * adalah prototype fungsi (tanpa struct/enum/typedef/macro/static/inline), dan
 * nama fungsi-nya tidak muncul di TU mana pun dalam subtree dependensi (selain
 * baris prototype itu sendiri). Selain itu, TU dianggap berubah seperti biasa.
 *
 * Salinan header disimpan di .rbot/hdr/<hash-path>.txt saat snapshot direkam,
 * sehingga perbedaan dapat dihitung terhadap isi yang terakhir terkompilasi.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deps_internal.h"
#include "../cmds/cmds_internal.h"

#define SEM_MAX_NAMES 64

bool depsSemEnabled(void) {
  const char *e = getenv("RBOT_HDR_SEM");
  return e && strcmp(e, "1") == 0;
}

static bool isHeaderPath(const char *p) {
  size_t n = strlen(p);
  return n >= 2 && strcmp(p + n - 2, ".h") == 0;
}

static void copyPathFor(const char *path, char *out, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (const char *q = path; *q; q++) {
    h ^= (unsigned char)*q;
    h *= 1099511628211ULL;
  }
  char rel[64];
  snprintf(rel, sizeof(rel), "hdr/%016llx.txt", (unsigned long long)h);
  cmdsStatePath(out, n, rel);
}

static char *readWhole(const char *path, size_t *outLen) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  rewind(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  if (outLen) *outLen = got;
  return buf;
}

/* Simpan isi header saat snapshot direkam (dipanggil dari subtreeRecord). */
void depsSemRecordNode(DepCache *dc, int idx) {
  if (!depsSemEnabled() || !isHeaderPath(dc->nodes[idx].path)) return;
  char cp[MAX_PATH + 64];
  copyPathFor(dc->nodes[idx].path, cp, sizeof(cp));
  cmdsStateMkdirFor(cp);
  char *buf = readWhole(dc->nodes[idx].path, NULL);
  if (!buf) return;
  FILE *f = fopen(cp, "wb");
  if (f) {
    fputs(buf, f);
    fclose(f);
  }
  free(buf);
}

/* Identifier boundary: kata utuh (bukan bagian dari identifier yang lebih panjang). */
static bool isIdentCh(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool hasWord(const char *s, const char *w) {
  size_t wl = strlen(w);
  if (wl == 0) return false;
  for (const char *p = s; (p = strstr(p, w)) != NULL; p += wl) {
    bool lb = (p == s) || !isIdentCh(p[-1]);
    bool rb = !isIdentCh(p[wl]);
    if (lb && rb) return true;
  }
  return false;
}

static char *trimInPlace(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
  return s;
}

/* Pecah buffer per baris (in-place), trim, buang baris kosong. */
static int splitLines(char *buf, char ***out) {
  int cap = 64, n = 0;
  char **v = malloc((size_t)cap * sizeof(char *));
  if (!v) return -1;
  char *p = buf;
  while (p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    char *t = trimInPlace(p);
    if (*t) {
      if (n == cap) {
        cap *= 2;
        char **nv = realloc(v, (size_t)cap * sizeof(char *));
        if (!nv) {
          free(v);
          return -1;
        }
        v = nv;
      }
      v[n++] = t;
    }
    p = nl ? nl + 1 : NULL;
  }
  *out = v;
  return n;
}

static bool inSet(char **set, int n, const char *s) {
  for (int i = 0; i < n; i++)
    if (strcmp(set[i], s) == 0) return true;
  return false;
}

/* Prototype fungsi tunggal: `<tipe> <nama>(<args>);` tanpa badan/keyword berbahaya. */
static bool isProtoLine(const char *ln, char *name, size_t nsz) {
  if (!ln[0] || ln[0] == '#' || ln[0] == '/' || ln[0] == '*') return false;
  size_t n = strlen(ln);
  if (n < 4 || ln[n - 1] != ';') return false;
  if (strpbrk(ln, "{}=\"'")) return false;
  const char *bad[] = {"struct", "union", "enum", "typedef", "static", "inline", "define"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    if (hasWord(ln, bad[i])) return false;
  const char *lp = strchr(ln, '(');
  const char *rp = strrchr(ln, ')');
  if (!lp || !rp || rp < lp || strcmp(rp, ");") != 0) return false;
  /* nama = identifier tepat sebelum '(' (boleh spasi di antara) */
  const char *e = lp;
  while (e > ln && (e[-1] == ' ' || e[-1] == '\t')) e--;
  const char *b = e;
  while (b > ln && isIdentCh(b[-1])) b--;
  size_t nl = (size_t)(e - b);
  if (nl == 0 || nl >= nsz) return false;
  /* butuh tipe di depan (bukan panggilan makro seperti `FOO(x);`) */
  const char *pre = ln;
  while (pre < b && (*pre == ' ' || *pre == '\t')) pre++;
  if (pre >= b) return false;
  memcpy(name, b, nl);
  name[nl] = '\0';
  return true;
}

/* Hitung nama prototype dari baris yang berubah; false bila ada baris berubah
   yang bukan prototype murni. */
static bool collectNames(char **chg, int nchg, char names[][128], int *nnames) {
  *nnames = 0;
  for (int i = 0; i < nchg; i++) {
    char nm[128];
    if (!isProtoLine(chg[i], nm, sizeof(nm))) return false;
    bool dup = false;
    for (int k = 0; k < *nnames; k++)
      if (strcmp(names[k], nm) == 0) dup = true;
    if (!dup) {
      if (*nnames >= SEM_MAX_NAMES) return false;
      snprintf(names[(*nnames)++], 128, "%s", nm);
    }
  }
  return true;
}

/* true bila `name` dipakai (kata utuh) di node `idx` — baris perubahan header
   sendiri dilewati (itu deklarasi yang memang baru). */
static bool nodeUsesName(DepCache *dc, int idx, const char *name, char **chg, int nchg) {
  size_t n = 0;
  char *buf = readWhole(dc->nodes[idx].path, &n);
  if (!buf) return true; /* tak terbaca: konservatif */
  char **lines = NULL;
  int nl = splitLines(buf, &lines);
  bool used = nl < 0;
  for (int i = 0; i < nl && !used; i++) {
    if (inSet(chg, nchg, lines[i]) && isHeaderPath(dc->nodes[idx].path)) continue;
    if (hasWord(lines[i], name)) used = true;
  }
  free(lines);
  free(buf);
  return used;
}

/* Inti: apakah perubahan header `idx` (vs salinan terakhir terkompilasi) aman
   untuk TU `root`? */
bool depsSemAcceptNode(DepCache *dc, int root, int idx) {
  if (!depsSemEnabled() || root < 0 || idx < 0 || root == idx) return false;
  if (!isHeaderPath(dc->nodes[idx].path) || dc->nodes[idx].mtime < 0) return false;

  char cp[MAX_PATH + 64];
  copyPathFor(dc->nodes[idx].path, cp, sizeof(cp));
  char *oldb = readWhole(cp, NULL);
  if (!oldb) return false;
  char *newb = readWhole(dc->nodes[idx].path, NULL);
  if (!newb) {
    free(oldb);
    return false;
  }
  char **oldl = NULL, **newl = NULL;
  int on = splitLines(oldb, &oldl), nn = splitLines(newb, &newl);
  bool ok = false;
  char **chg = NULL;
  int nchg = 0;
  char (*names)[128] = NULL;
  int nnames = 0;
  if (on < 0 || nn < 0) goto done;

  /* baris berubah = yang ada di salah satu sisi tapi tidak di sisi lain */
  chg = malloc((size_t)(on + nn + 1) * sizeof(char *));
  if (!chg) goto done;
  for (int i = 0; i < nn; i++)
    if (!inSet(oldl, on, newl[i])) chg[nchg++] = newl[i];
  for (int i = 0; i < on; i++)
    if (!inSet(newl, nn, oldl[i])) chg[nchg++] = oldl[i];
  if (nchg == 0) goto done; /* hash beda tapi himpunan baris sama: konservatif */

  names = malloc((size_t)SEM_MAX_NAMES * 128);
  if (!names) goto done;
  if (!collectNames(chg, nchg, names, &nnames)) goto done;

  /* telusuri subtree TU: setiap node tidak boleh memakai nama prototype yang berubah */
  int cap = dc->count + 256;
  bool *vis = calloc((size_t)cap, sizeof(bool));
  int *st = malloc((size_t)cap * sizeof(int));
  if (!vis || !st) {
    free(vis);
    free(st);
    goto done;
  }
  int sp = 0;
  st[sp++] = root;
  vis[root] = true;
  bool walk = true;
  while (sp > 0 && walk) {
    int cur = st[--sp];
    if (cur >= cap) {
      walk = false;
      break;
    }
    depParseNodeIfNeeded(dc, cur);
    for (int k = 0; k < nnames && walk; k++)
      if (nodeUsesName(dc, cur, names[k], chg, nchg)) walk = false;
    if (!walk) break;
    int nk = dc->nodes[cur].nkids;
    for (int j = 0; j < nk; j++) {
      int kid = dc->nodes[cur].kids[j];
      if (kid >= cap) {
        walk = false;
        break;
      }
      if (!vis[kid]) {
        vis[kid] = true;
        st[sp++] = kid;
      }
    }
  }
  free(vis);
  free(st);
  ok = walk;

done:
  free(chg);
  free(names);
  free(oldl);
  free(newl);
  free(oldb);
  free(newb);
  return ok;
}
