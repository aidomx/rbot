#include "deps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"

#define DEPS_MAX_NODES 8192

typedef struct {
  char *path;      /* path ternormalisasi relatif root */
  int64_t mtime;   /* mtime ns; -1 bila tidak terbaca */
  int *kids;       /* index node hasil resolusi #include */
  int nkids;
  unsigned mark;   /* generasi kunjungan terakhir (deteksi siklus) */
  bool parsed;
} DepNode;

struct DepCache {
  DepNode *nodes;
  int count, cap;
  unsigned gen;
  List incDirs;
};

/* ==================== path ==================== */

/* Rapikan "a/./b/../c" -> "a/c" (in-place) agar satu file = satu node. */
static void normalizePath(char *p) {
  char *seg[128];
  int ns = 0;
  char *save = p;
  size_t len = strlen(p);
  char *tmp = malloc(len + 1);
  memcpy(tmp, p, len + 1);
  bool abs = tmp[0] == '/';

  for (char *tok = tmp; tok && *tok;) {
    char *slash = strchr(tok, '/');
    if (slash) *slash = '\0';
    if (strcmp(tok, ".") == 0 || *tok == '\0') {
      /* lewati */
    } else if (strcmp(tok, "..") == 0 && ns > 0 && strcmp(seg[ns - 1], "..") != 0) {
      ns--;
    } else if (ns < 128) {
      seg[ns++] = tok;
    }
    tok = slash ? slash + 1 : NULL;
  }

  char *w = save;
  if (abs) *w++ = '/';
  for (int i = 0; i < ns; i++) {
    size_t sl = strlen(seg[i]);
    memcpy(w, seg[i], sl);
    w += sl;
    if (i + 1 < ns) *w++ = '/';
  }
  *w = '\0';
  if (w == save) strcpy(save, ".");
  free(tmp);
}

static void joinPath(char *out, size_t n, const char *dir, const char *name) {
  if (!dir || !*dir || strcmp(dir, ".") == 0)
    snprintf(out, n, "%s", name);
  else
    snprintf(out, n, "%s/%s", dir, name);
  normalizePath(out);
}

/* ==================== node cache ==================== */

static int nodeFind(const DepCache *dc, const char *path) {
  for (int i = 0; i < dc->count; i++)
    if (strcmp(dc->nodes[i].path, path) == 0) return i;
  return -1;
}

static int nodeAdd(DepCache *dc, const char *path) {
  if (dc->count >= DEPS_MAX_NODES) return -1;
  if (dc->count == dc->cap) {
    int ncap = dc->cap ? dc->cap * 2 : 64;
    DepNode *nn = realloc(dc->nodes, (size_t)ncap * sizeof(DepNode));
    if (!nn) return -1;
    dc->nodes = nn;
    dc->cap = ncap;
  }
  DepNode *n = &dc->nodes[dc->count];
  memset(n, 0, sizeof(*n));
  n->path = strdup(path);
  n->mtime = fsMTimeNs(path);
  return dc->count++;
}

static int nodeGet(DepCache *dc, const char *path) {
  int i = nodeFind(dc, path);
  return i >= 0 ? i : nodeAdd(dc, path);
}

/* ==================== pemindai #include ==================== */

static char *readWhole(const char *path, size_t *outLen) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  long long sz = fsFileSize(path);
  if (sz < 0) sz = 0;
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[got] = '\0';
  *outLen = got;
  return buf;
}

/* Cari file yang di-include: "..." -> dir includer dulu, lalu -I; <...> -> -I
   saja. Return false bila bukan file project (header sistem). */
static bool resolveInclude(const DepCache *dc, const char *fromPath, const char *name,
                           bool quoted, char *out, size_t n) {
  if (quoted) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", fromPath);
    char *slash = strrchr(dir, '/');
    if (slash)
      *slash = '\0';
    else
      strcpy(dir, ".");
    joinPath(out, n, dir, name);
    if (fsFileExists(out)) return true;
  }
  for (int i = 0; i < dc->incDirs.count; i++) {
    joinPath(out, n, dc->incDirs.items[i], name);
    if (fsFileExists(out)) return true;
  }
  return false;
}

static void kidAdd(DepNode *n, int kid) {
  for (int i = 0; i < n->nkids; i++)
    if (n->kids[i] == kid) return;
  int *nk = realloc(n->kids, (size_t)(n->nkids + 1) * sizeof(int));
  if (!nk) return;
  n->kids = nk;
  n->kids[n->nkids++] = kid;
}

/* Baca satu file, catat semua #include yang teresolusi sebagai anak node.
   Komentar blok, komentar baris, dan isi string literal tidak dianggap
   direktif. */
static void parseNode(DepCache *dc, int idx) {
  DepNode *node = &dc->nodes[idx];
  node->parsed = true;
  if (node->mtime < 0) return;

  size_t len = 0;
  char *buf = readWhole(node->path, &len);
  if (!buf) return;
  char *fromPath = strdup(node->path); /* nodes bisa realloc saat nodeGet */

  bool inBlock = false;
  char *p = buf;
  while (*p) {
    char *eol = strchr(p, '\n');
    size_t ll = eol ? (size_t)(eol - p) : strlen(p);

    /* salin baris dengan komentar diganti spasi */
    char line[1024];
    size_t o = 0;
    for (size_t i = 0; i < ll && o < sizeof(line) - 1;) {
      if (inBlock) {
        if (p[i] == '*' && i + 1 < ll && p[i + 1] == '/') {
          inBlock = false;
          i += 2;
        } else {
          i++;
        }
        line[o++] = ' ';
      } else if (p[i] == '/' && i + 1 < ll && p[i + 1] == '*') {
        inBlock = true;
        i += 2;
        line[o++] = ' ';
      } else if (p[i] == '/' && i + 1 < ll && p[i + 1] == '/') {
        break;
      } else if (p[i] == '"') {
        /* string literal: salin apa adanya (bisa memuat pola komentar) */
        line[o++] = p[i++];
        while (i < ll && p[i] != '"' && o < sizeof(line) - 1) {
          if (p[i] == '\\' && i + 1 < ll) line[o++] = p[i++];
          line[o++] = p[i++];
        }
        if (i < ll && o < sizeof(line) - 1) line[o++] = p[i++];
      } else {
        line[o++] = p[i++];
      }
    }
    line[o] = '\0';

    const char *q = line;
    while (*q == ' ' || *q == '\t' || *q == '\r') q++;
    if (*q == '#') {
      q++;
      while (*q == ' ' || *q == '\t') q++;
      if (strncmp(q, "include", 7) == 0) {
        q += 7;
        if (strncmp(q, "_next", 5) == 0) q += 5;
        /* "#include_foo" bukan direktif include */
        if (*q != ' ' && *q != '\t' && *q != '"' && *q != '<') q = "";
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '"' || *q == '<') {
          bool quoted = *q == '"';
          char close = quoted ? '"' : '>';
          const char *s = ++q;
          while (*q && *q != close) q++;
          if (*q == close && q > s) {
            char name[MAX_PATH];
            size_t nl = (size_t)(q - s);
            if (nl < sizeof(name)) {
              memcpy(name, s, nl);
              name[nl] = '\0';
              char resolved[MAX_PATH];
              if (resolveInclude(dc, fromPath, name, quoted, resolved, sizeof(resolved))) {
                int kid = nodeGet(dc, resolved);
                if (kid >= 0) kidAdd(&dc->nodes[idx], kid);
              }
            }
          }
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
  free(fromPath);
  free(buf);
}

/* DFS iteratif-rekursif: mtime terbaru di subtree, tiap node sekali per source. */
static int64_t newestUnder(DepCache *dc, int idx) {
  if (!dc->nodes[idx].parsed) parseNode(dc, idx);
  dc->nodes[idx].mark = dc->gen;
  int64_t best = dc->nodes[idx].mtime;
  for (int i = 0; i < dc->nodes[idx].nkids; i++) {
    int kid = dc->nodes[idx].kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = newestUnder(dc, kid);
    if (m > best) best = m;
  }
  return best;
}

/* ==================== API ==================== */

DepCache *depsNew(const List *incDirs) {
  DepCache *dc = calloc(1, sizeof(*dc));
  if (!dc) return NULL;
  for (int i = 0; incDirs && i < incDirs->count; i++)
    listAdd(&dc->incDirs, incDirs->items[i]);
  return dc;
}

void depsFree(DepCache *dc) {
  if (!dc) return;
  for (int i = 0; i < dc->count; i++) {
    free(dc->nodes[i].path);
    free(dc->nodes[i].kids);
  }
  free(dc->nodes);
  for (int i = 0; i < dc->incDirs.count; i++)
    free(dc->incDirs.items[i]);
  free(dc);
}

int64_t depsNewestHeaderMTime(DepCache *dc, const char *src) {
  if (!dc || !src) return 0;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s", src);
  normalizePath(path);
  int root = nodeGet(dc, path);
  if (root < 0) return 0;

  dc->gen++;
  dc->nodes[root].mark = dc->gen;
  if (!dc->nodes[root].parsed) parseNode(dc, root);

  int64_t best = 0;
  for (int i = 0; i < dc->nodes[root].nkids; i++) {
    int kid = dc->nodes[root].kids[i];
    if (dc->nodes[kid].mark == dc->gen) continue;
    int64_t m = newestUnder(dc, kid);
    if (m > best) best = m;
  }
  return best;
}
