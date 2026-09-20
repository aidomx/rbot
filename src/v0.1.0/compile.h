#ifndef RBOT_V0_1_0_COMPILE_H
#define RBOT_V0_1_0_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

bool resolveCompiler(Config *c);

/* true bila compiler aktif adalah MSVC (cl.exe) — flag & linker beda. */
bool compilerIsMSVC(const Config *c);

char *includeFlags(const Config *c);
char *warningFlags(const Config *c);

/* src/x/y.c -> build/y.o mapping per source root, meniru Makefile. */
bool objectPathFor(const Config *c, const char *src, char *out, size_t n);

bool compileOne(const Config *c, const char *inc, const char *wf, const char *src,
                const char *obj);

#endif /* RBOT_V0_1_0_COMPILE_H */
