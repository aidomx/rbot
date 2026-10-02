#ifndef RBOT_V0_1_0_USES_ALIAS_H
#define RBOT_V0_1_0_USES_ALIAS_H

#include <stdbool.h>
#include <stddef.h>

#include "../util.h"

/*
 * alias — mesin alias `X as Y` (diekstrak dari config.c; semantik tak
 * berubah). Dipakai parser `use project` maupun `use workspace` agar
 * penulisan singkatan identik di dua mode.
 *
 * Baris `X as Y` berarti alias Y untuk X — sama persis semantik
 * `make`/`git config`: X tetap valid, Y hanya nama kedua. Alias bisa
 * berantai: `projects.modules as mod` lalu `mod.archive as archive`.
 * Resolver iteratif: setiap segmen diganti selama masih ada pasangan alias
 * yang cocok (dengan batas iterasi anti-siklus; batas tercapai berarti
 * siklus — dibiarkan apa adanya, key tak dikenal memang diabaikan).
 *
 * Representasi: "canonical\talias" di sebuah List.
 */

#define ALIAS_MAX 32
#define ALIAS_KEY_MAX 256               /* key terqualifikasi yang diterima */
#define ALIAS_BUF_MAX (4 * 64)          /* "canonical\talias" & hasil resolve */

/* Tambah pasangan canonical->alias. Alias yang sudah ada tidak ditimpa
   (alias pertama menang); canonical == alias diabaikan. */
void aliasListAdd(List *l, const char *canonical, const char *alias);

/* Resolve key in-place (rewrite prefix, dengan batas titik agar alias
   `rupa` tidak menelan `rupamod.*`). Iterasi maksimum anti-siklus. */
void aliasResolve(const List *l, char *key, size_t cap);

/* Catat pasangan alias dari baris `X as Y` (memodifikasi text). true bila
   baris memang berisi " as ". */
bool aliasLineAdd(List *l, char *text);

#endif /* RBOT_V0_1_0_USES_ALIAS_H */
