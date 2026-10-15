#ifndef SATRIA_V0_1_0_NINJACONV_H
#define SATRIA_V0_1_0_NINJACONV_H

#include <stddef.h>

#include "util.h"

/*
 * ninjaconv — konverter build.ninja -> Buildfile (format "use alias").
 * Dipakai opsi CLI `satria -xf build.ninja` / `satria -xcf build.ninja`.
 * Lihat ninjaconv.c untuk detail translasi.
 */

bool ninjaToBuildfile(const char *ninjaPath, const char *outPath, char *err, size_t errCap);

#endif /* SATRIA_V0_1_0_NINJACONV_H */
