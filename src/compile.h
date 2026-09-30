#ifndef RBOT_V0_1_0_COMPILE_H
#define RBOT_V0_1_0_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

bool resolveCompiler(Config *c);

/* true bila compiler aktif adalah MSVC (cl.exe) — flag & linker beda. */
bool compilerIsMSVC(const Config *c);

char *includeFlags(const Config *c);

/* Direktori -I dari headers (tanpa flag non-I) — dipakai pelacak header. */
void includeDirs(const Config *c, List *out);
char *warningFlags(const Config *c);

/* warningFlags + "-fPIC " — untuk library shared & jalur paralel. */
char *picWarningFlags(const Config *c);

/*
 * Susun command kompilasi satu source (malloc; pemanggil yang membebaskan)
 * — dipakai compileOne dan jalur kompilasi paralel -jN.
 */
char *compileCmd(const Config *c, const char *inc, const char *wf, const char *src,
                 const char *obj);

/* src/x/y.c -> build/y.o mapping per source root, meniru Makefile. */
bool objectPathFor(const Config *c, const char *src, char *out, size_t n);

/* src/x/y.c -> build/y.d — file dependensi (-MMD) milik object-nya. */
bool dotDPathFor(const Config *c, const char *src, char *out, size_t n);

bool compileOne(const Config *c, const char *inc, const char *wf, const char *src,
                const char *obj);

/* "src/main.c" tercantum di Buildfile: exclude -> dilepas dari build. */
bool excludedSource(const Config *c, const char *src);

/*
 * Flag arsitektur dari Buildfile: target (malloc; pemanggil yang
 * membebaskan). NULL bila target kosong/tak dikenal — kompilasi host.
 */
char *targetFlags(const Config *c);

/* Kompilasi satu source untuk library: identik compileOne, plus -fPIC
   bila library shared diminta (GNU/Clang; MSVC tidak butuh flag). */
bool compileLibraryOne(const Config *c, const char *inc, const char *wf, const char *src,
                       const char *obj);

#endif /* RBOT_V0_1_0_COMPILE_H */
