#ifndef SATRIA_V0_1_0_MAKECONV_H
#define SATRIA_V0_1_0_MAKECONV_H

#include <stddef.h>

#include "util.h"

/*
 * makeconv — konverter make -> Buildfile (plain key=value, lihat
 * design/convert.md Bagian 3 untuk format output).
 * Dipakai opsi CLI `satria -xf make` / `satria -xcf make`.
 * Lihat src/make/mk_convert.c untuk detail translasi.
 */

bool makeToBuildfile(const char *makePath, const char *outPath, char *err, size_t errCap);

#endif /* SATRIA_V0_1_0_MAKECONV_H */
