#ifndef RBOT_V0_1_0_PACK_STAGE_H
#define RBOT_V0_1_0_PACK_STAGE_H

#include <stdbool.h>
#include "pack_internal.h"

/* Split pack.files entry into source and archive destination.
   Without ':' destination equals source. */
bool packEntryParts(const char *entry, char *src, size_t srcN,
                    char *dst, size_t dstN);

/* Return newest mtime of the source side of a pack.files entry. */
int64_t packEntryMTimeNs(const char *entry);

#endif

/* Materialize mapped files below root. Returns false on copy failure. */
bool packStageEntries(const Config *c, const char *root);
