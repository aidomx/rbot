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

/*
 * `rbot init -w` — tulis template workspace (default "Buildfile.ws";
 * bila -f diberikan, path itulah yang dipakai). Menolak menulis jika
 * file sudah ada, sama seperti cmdInit.
 */
int cmdInitWorkspace(const char *buildfilePath);

/* jobs <= 0 berarti serial (1); >= 2 memicu kompilasi paralel -jN. */
int cmdBuild(int jobs, const char *buildfilePath);

/*
 * cmdBuildEx — build dengan mode fase:
 *   libOnly = false : build penuh (compile + link binary + library).
 *   libOnly = true  : fase library workspace — compile + buildEmbedded +
 *                     kemas lib<name>.a/.so TANPA link binary. Binary proyek
 *                     yang saling memakai library lintas-proyek (mis. rupa
 *                     <-> ruka di workspace) baru bisa link setelah SEMUA
 *                     proyek menyelesaikan fase library, jadi fase ini
 *                     juga tidak menyertakan library ber-path proyek lain
 *                     pada link .so dan tidak merekam fast state (state
 *                     menyangkut target binary — milik fase normal).
 */
int cmdBuildEx(int jobs, const char *buildfilePath, bool libOnly);
int cmdClean(const char *buildfilePath);

#endif /* RBOT_V0_1_0_COMMANDS_H */
