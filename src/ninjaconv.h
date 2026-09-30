#ifndef RBOT_V0_1_0_NINJACONV_H
#define RBOT_V0_1_0_NINJACONV_H

#include <stddef.h>

#include "util.h"

/*
 * ninjaconv — konverter build.ninja -> Buildfile (format "use alias").
 * Dipakai opsi CLI `rbot -xf build.ninja` / `rbot -xcf build.ninja`.
 * Lihat ninjaconv.c untuk detail translasi.
 */

bool ninjaToBuildfile(const char *ninjaPath, const char *outPath, char *err, size_t errCap);

#endif /* RBOT_V0_1_0_NINJACONV_H */
