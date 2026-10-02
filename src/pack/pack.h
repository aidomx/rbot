#ifndef RBOT_V0_1_0_PACK_H
#define RBOT_V0_1_0_PACK_H

#include "../config.h"

/*
 * pack — pengemasan artefak build (Buildfile: pack.*).
 *
 * packRun menjalankan langkah pengemasan bila c->pack.requested:
 *   - tarball di pack.output (kompresi dari pack.compress atau ekstensi);
 *   - .deb tambahan bila pack.format = deb (metadata pack.deb.*);
 *   - <artefak>.sha256 untuk tiap artefak bila pack.checksum = sha256.
 *
 * Kemasan dibangun ulang hanya bila salah satu entri pack.files (folder
 * di-walk rekursif) lebih baru daripada artefaknya. Progress dicetak ke
 * stdout ("< Package  : ..."); true bila semua artefak siap.
 *
 * Proyek pack-only (tanpa sources) dipanggil dari jalur !binary di
 * cmdBuild; proyek dengan sources memanggil packRun setelah link sukses.
 */
bool packRun(const Config *c);

#endif /* RBOT_V0_1_0_PACK_H */
