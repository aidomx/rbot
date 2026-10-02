#include <stdio.h>
#include "build/embedded.h"
#include <string.h>
int main(void) {
  printf("ruka ok, modules=%zu bytes\n", (size_t)EMBED_MODULES_SYMBOL_LEN);
  return 0;
}
