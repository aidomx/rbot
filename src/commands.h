#ifndef RBOT_V0_1_0_COMMANDS_H
#define RBOT_V0_1_0_COMMANDS_H

#include <stdbool.h>

void showHelp(void);

/*
 * buildfilePath: file konfigurasi yang dipakai (default "Buildfile";
 * dirujuk `rbot -f <file>`; NULL diterima dan diperlakukan sebagai
 * "Buildfile"). cmdInit menolak menulis jika file itu sudah ada.
 */
int cmdInit(const char *buildfilePath);

/* jobs <= 0 berarti serial (1); >= 2 memicu kompilasi paralel -jN. */
int cmdBuild(int jobs, const char *buildfilePath);
int cmdClean(const char *buildfilePath);

#endif /* RBOT_V0_1_0_COMMANDS_H */
