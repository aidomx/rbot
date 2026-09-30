/*
 * ninjaconv — konverter build.ninja -> Buildfile (format baru "use alias").
 *
 * Dua gaya build.ninja ditangani:
 *   1. command literal per edge (build.ninja tulisan tangan sederhana):
 *        rule cc
 *          command = gcc -Iinclude -O2 -c $in -o $out
 *        build build/main.o: cc src/main.c
 *   2. rule template + variabel per-edge (gaya CMake):
 *        rule C_COMPILER
 *          command = $CC $DEFINES $INCLUDES $FLAGS -MD -MT $out -MF $DEP_FILE -o $out -c $in
 *        build CMakeFiles/app.dir/src/main.c.o: C_COMPILER src/main.c
 *          FLAGS = -O2
 *          INCLUDES = -I/tmp/proj/include
 *
 * Tiap edge compile (rule yang command-nya memuat "-c") diekspansi lalu
 * ditranslasi ke konfigurasi rbot: sources (file .c), flags (-D/-O/-W/-f),
 * std (-std=...), headers (dari -I). Edge link (tanpa "-c") dipakai untuk
 * menebak nama binary dari output-nya. Path dibuat relatif terhadap cwd;
 * path ninja relatif terhadap direktori build.ninja-nya.
 */
#include "ninjaconv.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "portability.h"
#include "util.h"

/* ==================== util dasar ==================== */

static char *ncStrndup(const char *a, const char *b) {
  size_t n = (size_t)(b - a);
  char *out = malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, a, n);
  out[n] = '\0';
  return out;
}

/* Token berikut (pemisah spasi/tab). NULL di akhir. Pemanggil yang free. */
static char *ncNextTok(char **cursor) {
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

/* Append tanpa fixed buffer: mencegah truncation dan underflow ukuran snprintf. */
static bool ncAppendText(char **buf, size_t *len, size_t *cap, const char *text) {
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

/* ==================== kumpulan string unik ==================== */

typedef struct {
  char **items;
  int count;
  int capacity;
} NcList;

static bool ncAdd(NcList *l, const char *s) {
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

static void ncFree(NcList *l) {
  for (int i = 0; i < l->count; i++)
    free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->count = 0;
  l->capacity = 0;
}

/* ==================== model file ninja ==================== */

#define NC_MAX_RULES 128

typedef struct {
  char *name;
  char *command; /* template; NULL bila rule tanpa command */
} NcRule;

typedef struct {
  char *out;     /* output pertama edge */
  char *rule;    /* nama rule */
  char *in;      /* input eksplisit pertama (sebelum '|') */
  char *flags;   /* nilai FLAGS edge (malloc) atau NULL */
  char *incs;    /* nilai INCLUDES edge */
  char *defs;    /* nilai DEFINES edge */
  char *depfile; /* nilai DEP_FILE edge — argumen -MF dalam template */
} NcEdge;

typedef struct {
  NcRule rules[NC_MAX_RULES];
  int nrules;
  NcEdge *edges;
  int nedges, edgeCap;
  NcList varNames; /* variabel top-level (build.ninja & file include) */
  NcList varVals;
} NcFile;

static const NcRule *ncRuleFind(const NcFile *f, const char *name) {
  for (int i = 0; i < f->nrules; i++)
    if (strcmp(f->rules[i].name, name) == 0) return &f->rules[i];
  return NULL;
}

static void ncFileFree(NcFile *f) {
  for (int i = 0; i < f->nrules; i++) {
    free(f->rules[i].name);
    free(f->rules[i].command);
  }
  for (int i = 0; i < f->nedges; i++) {
    free(f->edges[i].out);
    free(f->edges[i].rule);
    free(f->edges[i].in);
    free(f->edges[i].flags);
    free(f->edges[i].incs);
    free(f->edges[i].defs);
    free(f->edges[i].depfile);
  }
  free(f->edges);
  ncFree(&f->varNames);
  ncFree(&f->varVals);
}

/* Nilai variabel top-level, atau NULL. */
static const char *ncVarGet(const NcFile *f, const char *name) {
  for (int i = 0; i < f->varNames.count; i++)
    if (strcmp(f->varNames.items[i], name) == 0) return f->varVals.items[i];
  return NULL;
}

/* ==================== path helper ==================== */

/* Rapikan "a/./b" -> "a/b" (versi ringan; deps.c punya versi lengkapnya). */
static void ncNormalize(char *p) {
  char *w = p, *r = p;
  while (*r) {
    if (r[0] == '/' && r[1] == '/') {
      r++;
      continue;
    }
    if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || r[2] == '\0')) {
      r += r[2] ? 2 : 1;
      continue;
    }
    *w++ = *r++;
  }
  *w = '\0';
}

/* Bila path absolut di bawah cwd, buang prefix cwd (jadikan relatif).
   Path absolut di luar cwd dibiarkan apa adanya. */
static void ncRelativize(char *path, size_t cap) {
  char cwd[MAX_PATH * 2];
  if (!fsGetCwd(cwd, sizeof(cwd))) return;
  size_t cl = strlen(cwd);
  if (strncmp(path, cwd, cl) != 0) return;
  if (path[cl] != '/') return;
  memmove(path, path + cl + 1, strlen(path + cl + 1) + 1);
  (void)cap;
}

/* Raw path (relatif thd dir ninja, atau absolut) -> path relatif cwd. */
static void ncFixPath(const char *raw, const char *ninjaDir, char *out, size_t cap) {
  if (!raw || !*raw) {
    if (cap) out[0] = '\0';
    return;
  }
  char joined[MAX_PATH * 2];
  if (raw[0] == '/') {
    /* absolut: relatif-kan terhadap cwd bila berada di bawahnya */
    snprintf(joined, sizeof(joined), "%s", raw);
    ncNormalize(joined);
    ncRelativize(joined, sizeof(joined));
  } else if (raw[0] == '\'') {
    snprintf(out, cap, "%s", raw); /* bukan path — biarkan */
    return;
  } else {
    /* relatif terhadap dir build.ninja (cmake_ninja_workdir dsb.);
       jalur cross-dir (nbuild/...) dibuat absolut-itu-relevan dulu */
    if (strcmp(ninjaDir, ".") == 0) {
      snprintf(joined, sizeof(joined), "%s", raw);
    } else {
      snprintf(joined, sizeof(joined), "%s/%s", ninjaDir, raw);
      ncNormalize(joined);
      ncRelativize(joined, sizeof(joined));
    }
  }
  snprintf(out, cap, "%s", joined);
}

/* Sama, untuk token berprefix -I. *fixedIn: token sudah pernah difix
   (berasal dari INCLUDES yang di-pre-fix) — jangan join ninjaDir lagi. */
static void ncFixIncludeTok(const char *tok, const char *ninjaDir, bool fixedIn, char *out,
                            size_t cap) {
  if (strncmp(tok, "-I", 2) == 0 && tok[2]) {
    if (fixedIn) {
      snprintf(out, cap, "%s", tok);
      return;
    }
    char dir[MAX_PATH * 2];
    ncFixPath(tok + 2, ninjaDir, dir, sizeof(dir));
    size_t len = strlen(dir);
    if (len > cap || cap - len < 3) {
      if (cap) out[0] = '\0';
      return;
    }
    out[0] = '-';
    out[1] = 'I';
    memcpy(out + 2, dir, len + 1);
    return;
  }
  snprintf(out, cap, "%s", tok);
}

/* ==================== ekspansi template ==================== */

/* Ganti semua $var / ${var} pada template dengan nilai: parameter edge
   dulu, lalu variabel top-level, lalu nama khusus ninja (in/out/...).
   Var tak dikenal -> "". Hasil malloc. */
static char *ncExpand(const NcFile *f, const char *tpl, const char *in, const char *out,
                      const char *flags, const char *incs, const char *defs,
                      const char *depfile) {
  size_t cap = strlen(tpl) * 2 + 1024;
  char *res = malloc(cap);
  if (!res) return NULL;
  size_t w = 0;
  res[0] = '\0';

  for (const char *p = tpl; *p;) {
    if (*p == '$' && p[1] == '$') { /* $$ -> literal $ */
      if (w + 1 < cap) res[w++] = '$';
      p += 2;
      continue;
    }
    if (*p == '$') {
      char name[64];
      size_t nlen = 0;
      size_t skip; /* berapa karakter dari p yang dikonsumsi */
      if (p[1] == '{') { /* ${NAME} */
        const char *e = strchr(p + 2, '}');
        if (!e) break;
        nlen = (size_t)(e - (p + 2));
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, p + 2, nlen);
        name[nlen] = '\0';
        skip = 2 + nlen + 1; /* $ { NAME } */
      } else { /* $NAME */
        while (p[1 + nlen] && ((p[1 + nlen] >= 'A' && p[1 + nlen] <= 'Z') ||
                               (p[1 + nlen] >= 'a' && p[1 + nlen] <= 'z') ||
                               (p[1 + nlen] >= '0' && p[1 + nlen] <= '9') ||
                               p[1 + nlen] == '_')) {
          if (nlen < sizeof(name) - 1) name[nlen] = p[1 + nlen];
          nlen++;
        }
        name[nlen < sizeof(name) ? nlen : sizeof(name) - 1] = '\0';
        skip = 1 + nlen;
      }

      static const char *sNames[] = {"in_newline", "DEP_FILE", "INCLUDES",
                                     "DEFINES",     "FLAGS",    "out",
                                     "in",          NULL};
      const char *sVals[7];
      sVals[0] = in;
      sVals[1] = depfile ? depfile : "";
      sVals[2] = incs ? incs : "";
      sVals[3] = defs ? defs : "";
      sVals[4] = flags ? flags : "";
      sVals[5] = out;
      sVals[6] = in;

      const char *v = NULL;
      for (int k = 0; sNames[k]; k++)
        if (strcmp(name, sNames[k]) == 0) {
          v = sVals[k];
          break;
        }
      if (!v) v = ncVarGet(f, name);
      if (!v) v = "";

      size_t vl = strlen(v);
      if (vl >= cap - w) {
        free(res); /* jangan hasilkan command yang diam-diam terpotong */
        return NULL;
      }
      memcpy(res + w, v, vl);
      w += vl;
      p += skip;
      continue;
    }
    if (w + 1 >= cap) {
      free(res);
      return NULL;
    }
    res[w++] = *p++;
  }
  res[w] = '\0';
  return res;
}

/* ==================== klasifikasi argumen compile ==================== */

typedef struct {
  NcList flags;
  NcList includes;
  char std[MAX_PATH * 4];
  char src[MAX_PATH * 2];
  char obj[MAX_PATH * 2];
} NcCompile;

static void ncConsumeArg(NcCompile *cc, const char *a) {
  if (strncmp(a, "-I", 2) == 0 && a[2]) {
    /* tandai bahwa dir ini sudah relatif cwd — ncParseCompile tidak
       menjalankan ncFixPath kedua kali untuk include */
    char marked[MAX_PATH * 2];
    snprintf(marked, sizeof(marked), "\x01%s", a + 2);
    ncAdd(&cc->includes, marked);
    return;
  }
  if (strncmp(a, "-std=", 5) == 0) {
    if (!cc->std[0]) snprintf(cc->std, sizeof(cc->std), "%s", a + 5);
    return;
  }
  if (strncmp(a, "-l", 2) == 0 && a[2] && a[2] != '-') return; /* di link, bukan compile */
  if (strncmp(a, "-D", 2) == 0 || strncmp(a, "-O", 2) == 0 || strncmp(a, "-W", 2) == 0 ||
      strncmp(a, "-f", 2) == 0 || strcmp(a, "-g") == 0 || strcmp(a, "-pipe") == 0) {
    if (strncmp(a, "-Wno-", 5) != 0) ncAdd(&cc->flags, a);
    return;
  }
}

/* Parse command hasil ekspansi; true bila edge compile valid (-c src, -o obj). */
static bool ncParseCompile(char *cmd, NcCompile *cc, const char *ninjaDir) {
  memset(cc, 0, sizeof(*cc));
  char *cur = cmd;
  char *tok;
  bool haveSrc = false, haveOut = false, isCompile = false;
  while ((tok = ncNextTok(&cur)) != NULL) {
    if (strcmp(tok, "-c") == 0) {
      isCompile = true;
    } else if (strcmp(tok, "-o") == 0) {
      char *next = ncNextTok(&cur);
      if (next) {
        ncFixPath(next, ninjaDir, cc->obj, sizeof(cc->obj));
        haveOut = true;
      }
      free(next);
    } else if (strncmp(tok, "-MF", 3) == 0 || strncmp(tok, "-MT", 3) == 0 ||
               strncmp(tok, "-MQ", 3) == 0) {
      /* dep-file flag yang membawa argumen (terpisah atau digabung); argumen
         hanya bila token berikutnya ada & bukan flag (DEP_FILE bisa kosong) */
      if (tok[3] == '\0' && *cur && *cur != '-') {
        char *next = ncNextTok(&cur);
        free(next);
      }
    } else if (tok[0] == '-' && tok[1] == 'M') {
      /* -MD/-MMD/-MP: dep-file flag tanpa argumen — abaikan */
    } else if (tok[0] == '-' && tok[1] != ' ') {
      char fixed[MAX_PATH * 2];
      ncFixIncludeTok(tok, ninjaDir, false, fixed, sizeof(fixed));
      ncConsumeArg(cc, fixed);
    } else if (tok[0] != '$') {
      /* path polos: kandidat source (.c) — posisi apapun, -c cukup ada di
         suatu tempat pada command */
      size_t tl = strlen(tok);
      if (tl > 2 && strcmp(tok + tl - 2, ".c") == 0) {
        ncFixPath(tok, ninjaDir, cc->src, sizeof(cc->src));
        haveSrc = true;
      }
    }
    free(tok);
  }

  /* -c biasanya muncul sebelum -o di template cmake; urutan bebas: terima
     bila keduanya ada dan obj berakhiran .o */
  if (!isCompile || !haveSrc || !haveOut) return false;
  size_t ol = strlen(cc->obj);
  if (ol < 2 || strcmp(cc->obj + ol - 2, ".o") != 0) return false;
  return true;
}

/* ==================== parsing build.ninja ==================== */

/* Baca seluruh file, gabungkan continuation ninja ('$', EOL) -> baris logis
   dipisah '\n'. Return malloc; pemanggil yang free. */
static char *ncReadLogical(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 0 || sz > 32 * 1024 * 1024) {
    fclose(fp);
    return NULL;
  }
  char *raw = malloc((size_t)sz + 1);
  if (!raw) {
    fclose(fp);
    return NULL;
  }
  size_t got = fread(raw, 1, (size_t)sz, fp);
  fclose(fp);
  raw[got] = '\0';

  char *out = malloc(got + 2);
  if (!out) {
    free(raw);
    return NULL;
  }
  size_t w = 0;
  for (size_t i = 0; i < got; i++) {
    /* buang '#'-comment sampai EOL (ninja: # di awal token) */
    if (raw[i] == '#') {
      while (i < got && raw[i] != '\n')
        i++;
      if (i >= got) break;
    }
    if (raw[i] == '$' && i + 1 < got && raw[i + 1] == '\n') {
      i++; /* continuation: buang '$' + newline */
      continue;
    }
    if (raw[i] == '\r') continue;
    out[w++] = raw[i];
  }
  out[w] = '\0';
  free(raw);
  return out;
}

/* Parse file ninja (dan `include`-nya, satu tingkat) ke dalam f. */
static bool ncParseInto(NcFile *f, const char *path, const char *dirHint, int depth);

static bool ncParseInto(NcFile *f, const char *path, const char *dirHint, int depth) {
  char *text = ncReadLogical(path);
  if (!text) return false;

  /* direktori file ini untuk resolve include & path relatif */
  char myDir[MAX_PATH];
  snprintf(myDir, sizeof(myDir), "%s", path);
  char *sl = strrchr(myDir, '/');
  if (sl)
    *sl = '\0';
  else
    snprintf(myDir, sizeof(myDir), "%s", dirHint && *dirHint ? dirHint : ".");

  char *save = NULL;
  NcRule *curRule = NULL;
  NcEdge *curEdge = NULL;

  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (line[0] == ' ' || line[0] == '\t') {
      /* variabel milik rule/edge terakhir */
      char *q = line;
      while (*q == ' ' || *q == '\t')
        q++;
      char *eq = strchr(q, '=');
      if (!eq) continue;
      *eq = '\0';
      char *key = q;
      char *val = eq + 1;
      while (*key == ' ' || *key == '\t')
        key++;
      char *ke = key + strlen(key);
      while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
        ke--;
      *ke = '\0';
      while (*val == ' ' || *val == '\t')
        val++;

      if (curEdge) {
        char **dst = NULL;
        if (strcmp(key, "FLAGS") == 0)
          dst = &curEdge->flags;
        else if (strcmp(key, "INCLUDES") == 0)
          dst = &curEdge->incs;
        else if (strcmp(key, "DEFINES") == 0)
          dst = &curEdge->defs;
        else if (strcmp(key, "DEP_FILE") == 0)
          dst = &curEdge->depfile;
        if (dst) {
          free(*dst);
          *dst = ncStrndup(val, val + strlen(val));
        }
      } else if (curRule && strcmp(key, "command") == 0) {
        free(curRule->command);
        curRule->command = ncStrndup(val, val + strlen(val));
      }
      continue;
    }

    curEdge = NULL;
    curRule = NULL;

    if (strncmp(line, "include ", 8) == 0 && depth < 4) {
      char incPath[MAX_PATH * 2];
      ncFixPath(line + 8, myDir, incPath, sizeof(incPath));
      /* include dibaca relatif cwd hasil ncFixPath; bila file itu relatif
         dan tidak ada, coba lagi relatif direktori file induk */
      FILE *probe = fopen(incPath, "rb");
      if (!probe && line[8] != '/') {
        snprintf(incPath, sizeof(incPath), "%s/%s", myDir, line + 8);
        probe = fopen(incPath, "rb");
      }
      if (probe) fclose(probe);
      ncParseInto(f, incPath, myDir, depth + 1);
      continue;
    }
    if (strncmp(line, "rule ", 5) == 0 && f->nrules < NC_MAX_RULES) {
      NcRule *r = &f->rules[f->nrules++];
      r->name = ncStrndup(line + 5, line + strlen(line));
      r->command = NULL;
      curRule = r;
      continue;
    }
    if (strncmp(line, "build ", 6) == 0) {
      if (f->nedges == f->edgeCap) {
        int ncap = f->edgeCap ? f->edgeCap * 2 : 128;
        void *nn = realloc(f->edges, (size_t)ncap * sizeof(NcEdge));
        if (!nn) break;
        f->edges = nn;
        f->edgeCap = ncap;
      }
      /* "build OUT(|OUT2)*: RULE IN... || extra" */
      char *colon = strchr(line + 6, ':');
      if (!colon) continue;
      *colon = '\0';
      char *ruleAndIns = colon + 1;
      while (*ruleAndIns == ' ' || *ruleAndIns == '\t')
        ruleAndIns++; /* spasi setelah ':' — tanpa ini nama rule kosong */
      char *sp = strchr(ruleAndIns, ' ');
      char *ruleName = ruleAndIns;
      char *ins = NULL;
      if (sp) {
        *sp = '\0';
        ins = sp + 1;
      }
      /* buang pipe segmen dari input */
      if (ins) {
        char *pipe = strchr(ins, '|');
        if (pipe) *pipe = '\0';
      }
      char *outTok = line + 6;
      char *pipeOut = strchr(outTok, '|');
      if (pipeOut) *pipeOut = '\0';
      /* out bisa multi token: ambil token pertama */
      char *save2 = NULL;
      char *firstOut = strtok_r(outTok, " \t", &save2);

      NcEdge *e = &f->edges[f->nedges++];
      memset(e, 0, sizeof(*e));
      e->out = firstOut ? ncStrndup(firstOut, firstOut + strlen(firstOut)) : NULL;
      e->rule = ncStrndup(ruleName, ruleName + strlen(ruleName));
      while (ins && (*ins == ' ' || *ins == '\t'))
        ins++;
      if (ins && *ins) {
        char *save3 = NULL;
        char *firstIn = strtok_r(ins, " \t", &save3);
        e->in = firstIn ? ncStrndup(firstIn, firstIn + strlen(firstIn)) : NULL;
      }
      curEdge = e;
      continue;
    }

    /* variabel top-level: "name = value" (bukan rule/build/include) */
    if (!strncmp(line, "rule ", 5) || !strncmp(line, "build ", 6)) continue;
    char *eq = strchr(line, '=');
    if (eq && eq != line) {
      *eq = '\0';
      char *key = line;
      char *val = eq + 1;
      while (*key == ' ' || *key == '\t')
        key++;
      char *ke = key + strlen(key);
      while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
        ke--;
      *ke = '\0';
      while (*val == ' ' || *val == '\t')
        val++;
      if (*key && *val) {
        ncAdd(&f->varNames, key);
        ncAdd(&f->varVals, val);
        /* pasangan index sejajar: bila nama baru, nilai harus baru juga;
           ncAdd menolak duplikat, jadi pasangan bisa bergeser — aman karena
           kita hanya butuh lookup nama -> nilai terakhir */
      }
    }
    /* baris lain (default, pool, dsb.) diabaikan */
  }
  free(text);
  return true;
}

/* ==================== konversi ==================== */

/* Root source terkecil ("src" bila semua di src/, "." bila campur). */
static void ncCommonRoot(const NcList *srcs, char *out, size_t n) {
  if (srcs->count == 0) {
    snprintf(out, n, "src");
    return;
  }
  char root[MAX_PATH];
  snprintf(root, sizeof(root), "%s", srcs->items[0]);
  char *slash = strrchr(root, '/');
  if (!slash) {
    snprintf(out, n, ".");
    return;
  }
  *slash = '\0';
  for (int i = 1; i < srcs->count; i++) {
    if (strncmp(srcs->items[i], root, strlen(root)) != 0 || srcs->items[i][strlen(root)] != '/') {
      snprintf(out, n, ".");
      return;
    }
  }
  snprintf(out, n, "%s", root);
}

bool ninjaToBuildfile(const char *ninjaPath, const char *outPath, char *err, size_t errCap) {
  char ninjaDir[MAX_PATH];
  snprintf(ninjaDir, sizeof(ninjaDir), "%s", ninjaPath);
  char *slash = strrchr(ninjaDir, '/');
  if (slash)
    *slash = '\0';
  else
    snprintf(ninjaDir, sizeof(ninjaDir), ".");

  NcFile f;
  memset(&f, 0, sizeof(f));
  if (!ncParseInto(&f, ninjaPath, ".", 0)) {
    if (err) snprintf(err, errCap, "cannot read %s", ninjaPath);
    return false;
  }

  NcList srcs = {0};
  NcList allFlags = {0};
  NcList allIncs = {0};
  char std[MAX_PATH * 4] = {0};
  char binGuess[128] = {0};
  int converted = 0;

  for (int i = 0; i < f.nedges; i++) {
    NcEdge *e = &f.edges[i];
    const NcRule *r = ncRuleFind(&f, e->rule);
    if (!r || !r->command || !e->in) continue;

    /* edge link (rule tanpa "-c" pada template): tebak nama binary */
    if (!strstr(r->command, "-c") && !binGuess[0] && e->out) {
      const char *base = strrchr(e->out, '/');
      base = base ? base + 1 : e->out;
      snprintf(binGuess, sizeof(binGuess), "%s", base);
      continue;
    }

    /* include edge: path -I dibuat relatif sebelum substitusi */
    char *incsFixed = NULL;
    size_t incsLen = 0, incsCap = 0;
    bool incsOk = true;
    if (e->incs) {
      char *dup = ncStrndup(e->incs, e->incs + strlen(e->incs));
      if (!dup) incsOk = false;
      char *cur = dup, *tok;
      while (incsOk && (tok = ncNextTok(&cur)) != NULL) {
        char fixed[MAX_PATH * 2];
        ncFixIncludeTok(tok, ninjaDir, true, fixed, sizeof(fixed)); /* sudah pre-fix */
        if (incsLen && !ncAppendText(&incsFixed, &incsLen, &incsCap, " "))
          incsOk = false;
        if (incsOk && !ncAppendText(&incsFixed, &incsLen, &incsCap, fixed))
          incsOk = false;
        free(tok);
      }
      free(dup);
    }
    if (!incsOk) {
      free(incsFixed);
      continue;
    }
    if (!incsFixed) {
      incsFixed = calloc(1, 1);
      if (!incsFixed) continue;
    }

    char *cmd = ncExpand(&f, r->command, e->in, e->out, e->flags, incsFixed, e->defs,
                         e->depfile);
    free(incsFixed);
    if (!cmd) continue;

    NcCompile cc;
    if (ncParseCompile(cmd, &cc, ninjaDir)) {
      ncAdd(&srcs, cc.src);
      for (int k = 0; k < cc.flags.count; k++)
        ncAdd(&allFlags, cc.flags.items[k]);
      for (int k = 0; k < cc.includes.count; k++) {
        /* buang penanda \x01 dari include yang sudah di-pre-fix */
        const char *p = cc.includes.items[k];
        ncAdd(&allIncs, p[0] == '\x01' ? p + 1 : p);
      }
      if (!std[0] && cc.std[0]) snprintf(std, sizeof(std), "%s", cc.std);
      converted++;
      ncFree(&cc.flags);
      ncFree(&cc.includes);
    }
    free(cmd);
  }

  if (converted == 0) {
    if (err) snprintf(err, errCap, "no compilable edges found in %s", ninjaPath);
    ncFileFree(&f);
    ncFree(&srcs);
    ncFree(&allFlags);
    ncFree(&allIncs);
    return false;
  }

  FILE *out = fopen(outPath, "w");
  if (!out) {
    if (err) snprintf(err, errCap, "cannot create %s", outPath);
    ncFileFree(&f);
    ncFree(&srcs);
    ncFree(&allFlags);
    ncFree(&allIncs);
    return false;
  }

  char root[MAX_PATH];
  ncCommonRoot(&srcs, root, sizeof(root));

  fprintf(out, "use alias\n\n");
  fprintf(out, "clean as c\noutput as o\n\n");
  fprintf(out, "# Dikonversi otomatis dari %s oleh `rbot -xf`.\n\n", ninjaPath);
  fprintf(out, "root = .\n\n");
  if (strcmp(root, ".") != 0) {
    fprintf(out, "sources = %s\n\n", root);
  } else {
    fprintf(out, "sources =");
    for (int i = 0; i < srcs.count; i++)
      fprintf(out, "%s %s", i ? "," : "", srcs.items[i]);
    fprintf(out, "\n\n");
  }
  fprintf(out, "flags =");
  for (int i = 0; i < allFlags.count; i++)
    fprintf(out, "%s %s", i ? "," : "", allFlags.items[i]);
  fprintf(out, "%s MMD, MP # pelacakan header rbot\n\n", allFlags.count ? "," : "");
  fprintf(out, "std = %s\n\n", std[0] ? std : "gnu11");
  if (allIncs.count) {
    fprintf(out, "headers =");
    for (int i = 0; i < allIncs.count; i++)
      fprintf(out, "%s %s", i ? "," : "", allIncs.items[i]);
    fprintf(out, "\n\n");
  }
  fprintf(out, "progress.bar = true\nprogress.error = always\n\n");
  fprintf(out, "o.binaryName = %s\n", binGuess[0] ? binGuess : "app");
  fprintf(out, "o.binaryDir = bin\n");
  fprintf(out, "o.buildDir = build\n");
  fprintf(out, "o.compileCommands = auto\n");
  fclose(out);

  printf("> Converted  : %s -> %s (%d compile edge, %d source)\n", ninjaPath, outPath, converted,
         srcs.count);
  ncFileFree(&f);
  ncFree(&srcs);
  ncFree(&allFlags);
  ncFree(&allIncs);
  return true;
}
