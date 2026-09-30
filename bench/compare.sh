#!/usr/bin/env bash
# ============================================================================
# compare.sh — benchmark build tool: ninja vs make vs bear+make vs rbot
#
# Membuat project sintetis (N source berantai header H, gaya /tmp/bench),
# menuliskan build.ninja + Makefile + Buildfile untuk project yang SAMA,
# lalu mengukur tiap tool pada skenario yang setara:
#
#   cold   : build pertama (semua object dihapus dulu)
#   noop   : build tanpa perubahan apa pun (metrik utama overhead)
#   touch  : touch 1 header (konten sama) — rbot: 0 recompile (content-based)
#   edit   : edit isi 1 source — tepat 1 recompile + relink
#   compdb : generate compile_commands.json (ninja -t compdb / bear make / rbot)
#   strace : hitung syscall no-op (jika strace tersedia)
#
# Pemakaian:
#   ./bench/compare.sh                # default: 150 source, 30 header, 2 run
#   ./bench/compare.sh -n 40 -H 10    # project kecil, cepat
#   ./bench/compare.sh -k edit,touch  # subset skenario
#
# Semua artefak di sys.tmp/ (dibuat & dibuang sendiri). Keluar: tabel ke
# stdout + sys.tmp/results.csv.
#
# Catatan kejujuran: keempat tool menghasilkan artefak yang setara, tapi
# fiturnya beda (rbot: fingerprint/fast-state/content-based header;
# ninja/make: mtime). Angka ini untuk melacak REGRESI rbot — bukan klaim
# bahwa satu tool "lebih baik" secara umum.
# ============================================================================

set -u

# ---------------------------- konfigurasi ----------------------------------
NSRC=150      # -n : jumlah source t1..tN.c
NHEAD=30      # -H : jumlah header berantai h1->hH
NRUN=2        # -r : pengulangan per skenario (ambil median)
RBOT=bin/rbot # -b : binary rbot yang dipakai
KEEP=0        # -k : 1 = jangan hapus sys.tmp di akhir
ONLY=""       # -s : hanya skenario ini (comma-separated)
OUTDIR=""     # -o : direktori kerja benchmark (default sys.tmp di cwd)

TIMEFORMAT='%R'

usage() { sed -n '2,30p' "$0"; exit 0; }

while getopts "n:H:r:b:ko:s:h" opt; do
  case $opt in
    n) NSRC=$OPTARG ;;
    H) NHEAD=$OPTARG ;;
    r) NRUN=$OPTARG ;;
    b) RBOT=$OPTARG ;;
    k) KEEP=1 ;;
    o) OUTDIR=$OPTARG ;;
    s) ONLY=$OPTARG ;;
    h) usage ;;
    *) usage ;;
  esac
done

[ -x "$RBOT" ] || { echo "rbot tidak ditemukan/dieksekusi: $RBOT" >&2; exit 1; }
RBOT=$(cd "$(dirname "$RBOT")" && pwd)/$(basename "$RBOT")

for t in ninja make gcc; do
  command -v "$t" >/dev/null 2>&1 || { echo "tool wajib tidak ada: $t" >&2; exit 1; }
done
HAVE_BEAR=0; command -v bear  >/dev/null 2>&1 && HAVE_BEAR=1
HAVE_STRACE=0; command -v strace >/dev/null 2>&1 && HAVE_STRACE=1

# ---------------------------- direktori kerja ------------------------------
if [ -z "$OUTDIR" ]; then OUTDIR="$PWD/sys.tmp"; fi
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
PROJ="$OUTDIR/proj"
CSV="$OUTDIR/results.csv"

# ---------------------------- generator project ----------------------------
gen_project() {
  rm -rf "$PROJ"
  mkdir -p "$PROJ/src" "$PROJ/include" "$PROJ/bin" "$PROJ/build"

  local h
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      [ "$h" -gt 1 ] && echo "#include \"h$((h - 1)).h\""
      echo "static const int v$h = $h;"
    } >"$PROJ/include/h$h.h"
  done

  local i
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int t$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } >"$PROJ/src/t$i.c"
  done
  {
    echo "#include \"h1.h\""
    echo "int t1(void);"
    echo "int main(void) { return t1() - 1; }"
  } >"$PROJ/src/main.c"

  # ---- build.ninja ----
  {
    echo "cflags = -O2 -Iinclude"
    echo
    echo "rule cc"
    echo "  command = gcc \$cflags -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "rule link"
    echo "  command = gcc \$in -o \$out"
    echo
    for ((i = 1; i <= NSRC; i++)); do
      echo "build build/t$i.o: cc src/t$i.c"
    done
    echo "build build/main.o: cc src/main.c"
    echo "build bin/app: link build/main.o $(for ((i = 1; i <= NSRC; i++)); do printf 'build/t%d.o ' "$i"; done)"
    echo
    echo "default bin/app"
  } >"$PROJ/build.ninja"

  # ---- Makefile ----
  {
    echo "CC      := gcc"
    echo "CFLAGS  := -O2 -Iinclude"
    echo "OBJS    := build/main.o $(for ((i = 1; i <= NSRC; i++)); do printf 'build/t%d.o ' "$i"; done)"
    echo
    echo "bin/app: \$(OBJS)"
    echo -e "\t\$(CC) \$^ -o \$@"
    echo
    echo "build/%.o: src/%.c"
    echo -e "\t@mkdir -p build"
    echo -e "\t\$(CC) \$(CFLAGS) -MMD -MP -c \$< -o \$@"
    echo
    echo "-include \$(OBJS:.o=.d)"
  } >"$PROJ/Makefile"

  # ---- Buildfile (rbot) ----
  {
    echo "use alias"
    echo
    echo "clean as c"
    echo "output as o"
    echo
    echo "root = ."
    echo
    echo "sources = src"
    echo "headers = include"
    echo "flags = O2, MMD, MP"
    echo "std = gnu11"
    echo "compiler = gcc"
    echo "c.compdb = false"
    echo "foreground = false"
    echo
    echo "o.binaryName = app"
    echo "o.binaryDir = bin"
    echo "o.buildDir = build"
    echo "o.compileCommands = false"
  } >"$PROJ/Buildfile"

  ( cd "$PROJ" && "$RBOT" init >/dev/null 2>&1 ) # pastikan format diterima
}

# ---------------------------- util pengukuran ------------------------------
# run_once <label> <skenario> <command...> — ukur wall time, catat ke CSV.
# Eksekusi di $PROJ; stdout/stderr dibuang kecuali gagal (exit != 0).
ROWNUM=0
run_once() {
  local label=$1 scen=$2; shift 2
  local t0 t1 rc us
  t0=${EPOCHREALTIME/./}                     # microseconds sejak epoch
  ( cd "$PROJ" && "$@" ) >/dev/null 2>&1
  rc=$?
  t1=${EPOCHREALTIME/./}
  us=$(( t1 - t0 ))
  local ms=$((( us + 500 ) / 1000))          # bulatkan ke ms
  if [ $rc -ne 0 ]; then
    echo "  !! GAGAL: $label [$scen] (exit $rc): $*" >&2
    ms=-1
  fi
  ROWNUM=$(( ROWNUM + 1 ))
  printf '%s,%s,%s,%s\n' "$label" "$scen" "$ms" "$rc" >>"$CSV"
  printf '  %-22s %-8s %8s ms\n' "$label" "$scen" "$ms"
}

# rata-rata per (label,skenario) dari CSV -> tabel akhir (awk polos,
# tanpa fitur GNU — jalan di busybox awk sekalipun).
report() {
  echo
  printf '%-12s %-22s %12s\n' SKENARIO TOOL 'RERATA(ms)'
  printf '%-12s %-22s %12s\n' '--------' '----------------------' '-----------'
  awk -F, '
    $4 == 0 { k = $2 "|" $1; sum[k] += $3; cnt[k]++ }
    END {
      for (k in sum) {
        split(k, p, "|")
        printf "%-12s %-22s %12d\n", p[1], p[2], sum[k] / cnt[k]
      }
    }' "$CSV" | sort
}

# ---------------------------- skenario -------------------------------------
S_NINJA="ninja"; S_MAKE="make"; S_BEAR="bear-make"; S_RBOT="rbot"

have_scenario() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

# reset_build: buang semua artefak (build/ & bin/) tanpa menyentuh cache
# state tiap tool yang relevan untuk skenario "cold".
reset_build() { ( cd "$PROJ" && rm -rf build bin/app ); }

s_cold() {   # build pertama dari nol
  have_scenario cold || return 0
  for ((r = 1; r <= NRUN; r++)); do
    reset_build
    run_once "$S_NINJA" cold ninja
    reset_build
    run_once "$S_MAKE"  cold make -j"$(nproc)"
    reset_build
    [ "$HAVE_BEAR" = 1 ] && { reset_build; run_once "$S_BEAR" cold bear -- make -j"$(nproc)"; }
    reset_build
    run_once "$S_RBOT"  cold "$RBOT" -j"$(nproc)"
  done
}

s_noop() {   # build tanpa perubahan — panggil dua kali agar state tool
             # terbentuk dulu (bear run-1 merekam; rbot run-1 membuat
             # fingerprint/deps.cache), ukur mulai run-2
  have_scenario noop || return 0
  ( cd "$PROJ" && ninja >/dev/null 2>&1 )
  for ((r = 1; r <= NRUN + 1; r++)); do
    if [ $r -gt 1 ]; then
      run_once "$S_NINJA" noop ninja
      run_once "$S_MAKE"  noop make -j"$(nproc)"
      [ "$HAVE_BEAR" = 1 ] && run_once "$S_BEAR" noop bear -- make -j"$(nproc)"
      run_once "$S_RBOT"  noop "$RBOT"
    fi
  done
}

s_touch() {  # touch header (konten sama) — rbot harus 0 recompile
  have_scenario touch || return 0
  for ((r = 1; r <= NRUN; r++)); do
    ( cd "$PROJ" && touch "include/h$(( NHEAD / 2 + 1 )).h" )
    run_once "$S_NINJA" touch ninja
    ( cd "$PROJ" && touch "include/h$(( NHEAD / 2 + 1 )).h" )
    run_once "$S_MAKE"  touch make -j"$(nproc)"
    ( cd "$PROJ" && touch "include/h$(( NHEAD / 2 + 1 )).h" )
    [ "$HAVE_BEAR" = 1 ] && run_once "$S_BEAR" touch bear -- make -j"$(nproc)"
    ( cd "$PROJ" && touch "include/h$(( NHEAD / 2 + 1 )).h" )
    run_once "$S_RBOT"  touch "$RBOT"
  done
}

s_edit() {   # edit 1 source — semua tool harus tepat 1 recompile + relink
  have_scenario edit || return 0
  for ((r = 1; r <= NRUN; r++)); do
    ( cd "$PROJ" && printf '\n/* edit */\n' >>src/t1.c )
    run_once "$S_NINJA" edit ninja
    ( cd "$PROJ" && printf '\n/* edit */\n' >>src/t2.c )
    run_once "$S_MAKE"  edit make -j"$(nproc)"
    ( cd "$PROJ" && printf '\n/* edit */\n' >>src/t3.c )
    [ "$HAVE_BEAR" = 1 ] && run_once "$S_BEAR" edit bear -- make -j"$(nproc)"
    ( cd "$PROJ" && printf '\n/* edit */\n' >>src/t4.c )
    run_once "$S_RBOT"  edit "$RBOT"
  done
}

s_compdb() { # compile_commands.json
  have_scenario compdb || return 0
  for ((r = 1; r <= NRUN; r++)); do
    ( cd "$PROJ" && rm -f compile_commands.json )
    run_once "$S_NINJA" compdb ninja -t compdb
    ( cd "$PROJ" && rm -f compile_commands.json )
    [ "$HAVE_BEAR" = 1 ] && run_once "$S_BEAR" compdb bear -- make -j"$(nproc)"
    # rbot: aktifkan o.compileCommands dulu (Buildfile di-flip via sed),
    # ukur, lalu kembalikan.
    ( cd "$PROJ" && rm -f compile_commands.json \
      && sed -i 's/^o.compileCommands = false/o.compileCommands = auto/' Buildfile )
    run_once "$S_RBOT"  compdb "$RBOT"
    ( cd "$PROJ" && sed -i 's/^o.compileCommands = auto/o.compileCommands = false/' Buildfile )
  done
}

s_strace() { # hitung syscall no-op (sekali jalan per tool)
  have_scenario strace || return 0
  [ "$HAVE_STRACE" = 1 ] || { echo "  (strace tidak ada — lewati)"; return 0; }
  echo "  -- syscall no-op --"
  local n
  # baris total strace: "100.00 <secs> <usecs/call> <CALLS> [errors] total"
  # -> kolom ke-4 = jumlah calls.
  ( cd "$PROJ" && strace -c -f ninja >/dev/null 2>"$OUTDIR/strace.ninja.txt" )
  n=$(awk '$NF == "total" {print $4}' "$OUTDIR/strace.ninja.txt")
  printf '  %-22s %-8s %8s syscall\n' "$S_NINJA" noop "${n:-?}"

  ( cd "$PROJ" && strace -c -f make -j"$(nproc)" >/dev/null 2>"$OUTDIR/strace.make.txt" )
  n=$(awk '$NF == "total" {print $4}' "$OUTDIR/strace.make.txt")
  printf '  %-22s %-8s %8s syscall\n' "$S_MAKE" noop "${n:-?}"

  if [ "$HAVE_BEAR" = 1 ]; then
    ( cd "$PROJ" && strace -c -f bear -- make -j"$(nproc)" >/dev/null 2>"$OUTDIR/strace.bear-make.txt" )
    n=$(awk '$NF == "total" {print $4}' "$OUTDIR/strace.bear-make.txt")
    printf '  %-22s %-8s %8s syscall\n' "$S_BEAR" noop "${n:-?}"
  fi

  ( cd "$PROJ" && strace -c -f "$RBOT" >/dev/null 2>"$OUTDIR/strace.rbot.txt" )
  n=$(awk '$NF == "total" {print $4}' "$OUTDIR/strace.rbot.txt")
  printf '  %-22s %-8s %8s syscall\n' "$S_RBOT" noop "${n:-?}"
}

# ---------------------------- main -----------------------------------------
echo "== rbot benchmark: ninja vs make vs bear+make vs rbot"
echo "   project : $NSRC source, $NHEAD header @ $PROJ"
echo "   run     : $NRUN per skenario (tabel akhir = rerata)"
[ "$HAVE_BEAR" = 0 ] && echo "   (bear tidak ada — kolom bear-make dilewati)"
echo "gen   : membuat project sintetis di $PROJ ..."
gen_project

echo "warmup: mengisi cache state semua tool..."
( cd "$PROJ" && ninja >/dev/null 2>&1 && make -j"$(nproc)" >/dev/null 2>&1 \
  && "$RBOT" >/dev/null 2>&1 )
echo

echo "run   : skenario..."
s_cold
s_noop
s_touch
s_edit
s_compdb
s_strace

report
echo
echo "hasil lengkap: $CSV"
grep -q '^label' "$CSV" 2>/dev/null || sed -i '1i label,scenario,ms,rc' "$CSV"

[ "$KEEP" = 1 ] || { rm -rf "$PROJ" "$OUTDIR"/strace.*.txt; }
exit 0
