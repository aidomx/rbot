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

/*
 * packRunAt — fase release workspace: packRun dengan CWD PEMANGGIL
 * (root workspace), bukan root proyek. Semua path dihitung relatif root
 * workspace: pack.output (dist/release/...) dan entri pack.files
 * berpath ../<root>/... menunjuk artefak proyek. Freshness tetap dari
 * mtime input (mekanisme pack standar); tidak ada fast state terpisah.
 */
bool packRunAt(const Config *c);

#endif /* RBOT_V0_1_0_PACK_H */
