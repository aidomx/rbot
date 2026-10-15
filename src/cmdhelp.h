#ifndef SATRIA_CMDHELP_H
#define SATRIA_CMDHELP_H

#include <stdbool.h>

/*
 * cmdhelp — bantuan satria dari src/cmd.txt (sumber tunggal, pola
 * mupa/core prompt.c): section `---<name> ... ---end<name>`.
 *
 * Loader mencari file di:
 *   1. $SATRIA_CMD
 *   2. ./src/cmd.txt            (run dari repo)
 *   3. <exe>/../src/cmd.txt     (run dari build/bin)
 *   4. <exe>/cmd.txt            (terpasang di samping binary)
 *   5. <exe>/../share/satria/cmd.txt
 */

/* Cetak section `<name>` ke stdout. true bila ditemukan. */
bool cmdHelpSection(const char *name);

/* Bantuan utama (section "help"); fallback teks tanpa file. */
void cmdHelpMain(void);

/* true bila `<name>` adalah topic yang dikenal (section ada). */
bool cmdHelpKnown(const char *name);

#endif /* SATRIA_CMDHELP_H */
