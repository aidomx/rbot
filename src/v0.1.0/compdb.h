#ifndef RBOT_V0_1_0_COMPDB_H
#define RBOT_V0_1_0_COMPDB_H

#include <stdbool.h>

#include "config.h"
#include "util.h"

bool compdbEnabled(const Config *c);
void writeCompdb(const Config *c, List *srcs);

#endif /* RBOT_V0_1_0_COMPDB_H */
