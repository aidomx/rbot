#include "rbot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap.h"
#include "cmdhelp.h"
#include "commands.h"
#include "portability.h"
#include "profile.h"
#include "package_cli.h"
#include "prof/prof.h"
#include "uses/workspace.h"

#if __has_include("version.h")
#include "version.h" /* build/version.h — dihasilkan dari .rbot-version */
#endif

/*
 * Versi rbot. Sumber kebenaran: .rbot-version di root — saat build rbot
 * menuliskannya ke build/version.h (RBOT_VERSION_EMBEDDED) sehingga versi
 * ter-embed ke binary dan `rbot version` tetap benar di mana pun dijalankan.
 * Fallback literal hanya untuk build tanpa .rbot-version (mis. CMake murni).
 */
const char *rbotVersion(void) {
#ifdef RBOT_VERSION_EMBEDDED
  return RBOT_VERSION_EMBEDDED;
#else
  return "v0.2.0";
#endif
}

/*
 * design/bootstrap.md fase 3 — rbotRun adalah CONSUMER MURNI: tidak ada
 * parsing argv, strcmp bercabang atas argv, atau mutasi filesystem di sini.
 * Ia membaca Bootstrap hasil parse+apply lalu dispatch lewat switch enum.
 * Semua logika parsing ada di bootstrap.c; logika per-command ada di
 * commands.c / workspace.c / profile.c (sama seperti sebelum refactor).
 */
int rbotRun(int argc, const char *argv[]) {
  int packageStatus = 0;
  if (packageCliRun(argc, argv, &packageStatus)) return packageStatus;
  profInit(); /* profil fase aktif bila RBOT_PROFILE=1 (stderr, tanpa syscall) */

  Bootstrap bs;
  int rc = bootstrapParse(argc, argv, &bs);
  if (rc != 0) return bootstrapCleanup(&bs), rc;

  /* help <topic>: topic diambil dari argv setelah command help
     (satu kata; kata berikutnya diabaikan seperti command lain). */
  if (bs.cmd == CMD_HELP) {
    for (int i = 0; i < argc; i++) {
      if (argv[i] && (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0 ||
                      strcmp(argv[i], "-h") == 0)) {
        if (i + 1 < argc && argv[i + 1][0] && argv[i + 1][0] != '-')
          bs.help_topic = argv[i + 1];
        break;
      }
    }
  }

  /* version hanya cetak string — tanpa chdir/apply agar aman dari -f yang
     salah (perilaku sama dengan main.c lama yang memotong lebih awal). */
  if (bs.cmd == CMD_VERSION) {
    printf("rbot %s\n", rbotVersion());
    bootstrapCleanup(&bs);
    return 0;
  }

  rc = bootstrapApply(&bs);
  if (rc != 0) {
    bootstrapCleanup(&bs);
    return rc;
  }
  if (bs.conv_canceled) { /* -xcf dibatalkan oleh pengguna: selesai, rc 0 */
    bootstrapCleanup(&bs);
    return 0;
  }

  /* rbot profile <context> [key=value ...] — non-mutating, standalone. */
  if (bs.cmd == CMD_PROFILE) {
    bootstrapCleanup(&bs);
    return cmdProfile(bs.profile_argv, bs.profile_argc);
  }

  /* Mode workspace: -w eksplisit, atau auto-detect Buildfile.ws saat rbot
     polos di root workspace. init tetap jalur satu-project kecuali `init -w`
     yang menulis template workspace (Buildfile.ws). */
  bs.ws_auto = !bs.want_workspace && (bs.cmd == CMD_BUILD) && workspaceFileExists();
  if (bs.want_workspace || bs.ws_auto) {
    /* init -p di mode workspace: scaffold interaktif bisa menghasilkan
       Buildfile.ws sendiri (pilihan "Uses workspace?"), jadi diajukan
       sebelum template cmdInitWorkspace. */
    if (bs.cmd == CMD_INIT) {
      if (bs.interactive) {
        bootstrapCleanup(&bs);
        return cmdInteractiveInit();
      }
      int irc = cmdInitWorkspace(bs.have_f ? bs.buildfile : WORKSPACE_FILENAME);
      bootstrapCleanup(&bs);
      return irc;
    }
    if (bs.cmd != CMD_BUILD && bs.cmd != CMD_CLEAN && bs.cmd != CMD_RELEASE) {
      fprintf(stderr,
              "rbot: command '%s' tidak berlaku di mode workspace (pakai build/clean/release)\n",
              bs.cmd_token ? bs.cmd_token : "");
      rc = 2;
      goto done;
    }
    if (bs.ws_sel_argv && bs.cmd != CMD_RELEASE) {
    /* `-- key=value` hanya berlaku untuk release; tapi perilaku lama juga
       menerimanya bersama build polos (default release selektif), jadi
       hanya ditolak bila command eksplisit bukan release/build. */
    if (bs.cmd != CMD_BUILD) {
      fprintf(stderr,
              "rbot: -- <key=value> hanya berlaku untuk release (pakai 'rbot -w release -- ...')\n");
      bootstrapCleanup(&bs);
      return 2;
    }
  }
    if (bs.cmd == CMD_RELEASE && !bs.ws_sel_argv) {
      fprintf(stderr, "rbot: release memerlukan selektor (pakai 'rbot -w release -- name=rupa')\n");
      rc = 2;
      goto done;
    }
    if (bs.generate_compdb) {
      fprintf(stderr,
              "rbot: -g compdb saat ini hanya di mode satu-proyek (jalankan di folder proyek, bukan root workspace)\n");
      rc = 2;
      goto done;
    }
    /* `release` = build selektif + kemas release yang cocok; di workspace
       command ini berjalan sebagai build dengan selektor aktif. */
    char *sel = bootstrapJoinSelector(&bs);
    if (!sel && bs.ws_sel_argv) {
      bootstrapCleanup(&bs);
      return 2;
    }
    rc = workspaceRun(bs.cmd == CMD_CLEAN ? "clean" : "build", bs.jobs, bs.ws_only, sel);
    if (sel) free(sel);
    bootstrapCleanup(&bs);
    return rc;
  }

  /* Dispatch perintah standar — switch enum, bukan strcmp. */
  switch (bs.cmd) {
    case CMD_BUILD:
      if (bs.generate_compdb)
        rc = cmdCompdbGenerate(bs.buildfile);
      else
        rc = cmdBuild(bs.jobs, bs.buildfile);
      break;

    case CMD_INIT:
      rc = bs.interactive ? cmdInteractiveInit() : cmdInit(bs.buildfile);
      break;

    case CMD_CLEAN:
      rc = cmdClean(bs.buildfile);
      break;

    case CMD_HELP:
      if (bs.help_topic) {
        /* help <topic>: topic = nama section di src/cmd.txt. Tidak
           dikenal -> pesan + help utama (pola rupa/loader.c). */
        if (cmdHelpSection(bs.help_topic)) {
          rc = 0;
        } else {
          fprintf(stderr, "rbot: unknown help topic '%s'\n", bs.help_topic);
          showHelp();
          rc = 1;
        }
      } else {
        showHelp();
        rc = 0;
      }
      break;

    case CMD_RELEASE: /* tanpa -w ditolak di bawah */
      fprintf(stderr, "rbot: release memerlukan mode workspace (pakai 'rbot -w release -- name=rupa')\n");
      rc = 2;
      break;

    default:
      rc = 1; /* tidak terjangkau: parser menolak command asing */
      break;
  }

  /* -xf: hapus file sementara setelah command selesai. Buildfile asli
     tidak pernah ditulis, jadi tidak ada yang berisiko hilang. */
  if (bs.conv_abs && !bs.conv_keep) {
    fsRemoveFile("Buildfile.xf.tmp");
    printf("> Removed    : %s (sementara, dari %s)\n", "Buildfile.xf.tmp", bs.conv_abs);
  }

done:
  bootstrapCleanup(&bs);
  return rc;
}
