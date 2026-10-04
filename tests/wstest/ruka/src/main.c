#include "build/embedded.h"
#include <rupa.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  rupa_hello();
  printf("ruka ok, modules=%zu bytes\n", (size_t)EMBED_MODULES_SYMBOL_LEN);
  return 0;
}
