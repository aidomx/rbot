#include "use.h"

#include <string.h>

/*
 * use — deteksi baris `use` (implementasi). Sengaja satu titik kecil:
 * menambah varian `use` baru berarti menambah satu strcmp di sini dan satu
 * cabang di parser, tidak lebih.
 */

UseMode useLineMode(const char *text) {
  if (!text) return USE_NONE;
  if (strcmp(text, "use project") == 0 || strcmp(text, "use alias") == 0)
    return USE_PROJECT;
  if (strcmp(text, "use workspace") == 0) return USE_WORKSPACE;
  return USE_NONE;
}

bool useLineKnown(const char *text) { return useLineMode(text) != USE_NONE; }
