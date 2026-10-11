#!/usr/bin/env bash
# ============================================================================
# workspace.sh — benchmark mode workspace rbot (Buildfile.ws)
#
# Meniru tests/wstest: 3 proyek dalam satu workspace
#   rupamod : proyek kemasan (archive .rp -> modules/rupa_modules.tar)
#   core    : binary C + library (rupa)
#   bade    : binary C + library (bade), depends_on rupamod, embed arsip
#
# PERBEDAAN PENTING:
# Skrip ini memaksa ninja/make melakukan pekerjaan yang SAMA dengan rbot:
# 1. Membangun static library (libcore.a, libbade.a).
# 2. Menyelesaikan circular dependency (core <-> bade) menggunakan
#    linker group (-Wl,--start-group ... -Wl,--end-group).
# Tidak ada lagi "curang" dengan me-link .o secara langsung.
# ============================================================================
set -u
[ -n "${EPOCHREALTIME:-}" ] || {
  echo "butuh bash >= 5 (EPOCHREALTIME)" >&2
  exit 1
}

# ---------------------------- konfigurasi ----------------------------------
NSRC=80                     # -n : jumlah source per proyek
NHEAD=20                    # -H : jumlah header berantai per proyek
NMOD=40                     # -m : jumlah file .rp di rupamod
NRUN=5                      # -r : pengulangan per skenario (median)
RBOT=bin/rbot               # -b : binary rbot
RBOT_ARGS=""                # -a : argumen tambahan untuk rbot
COMPILERS="gcc"             # -c : nilai 'compiler =' di Buildfile.ws
KEEP=0                      # -k : jangan hapus workspace di akhir
ONLY=""                     # -s : subset skenario
TOOLS_OPT="rbot,ninja,make" # -t : subset tool pembanding
OUTDIR=""                   # -o : direktori kerja
NPROC=$(nproc 2> /dev/null || echo 4)
MUT_DELAY=0 # -d : jeda (detik) sebelum tiap edit

usage() {
  sed -n '2,/^# ====.*$/p' "$0"
  exit 0
}
while getopts "n:H:m:r:b:a:c:d:ko:s:t:h" opt; do
  case $opt in
    n) NSRC=$OPTARG ;; H) NHEAD=$OPTARG ;; m) NMOD=$OPTARG ;;
    r) NRUN=$OPTARG ;; b) RBOT=$OPTARG ;; a) RBOT_ARGS=$OPTARG ;;
    c) COMPILERS=$OPTARG ;; k) KEEP=1 ;; o) OUTDIR=$OPTARG ;;
    s) ONLY=$OPTARG ;; t) TOOLS_OPT=$OPTARG ;; d) MUT_DELAY=$OPTARG ;; *) usage ;;
  esac
done

[ -x "$RBOT" ] || {
  echo "rbot tidak ditemukan/dieksekusi: $RBOT" >&2
  exit 1
}
RBOT=$(cd "$(dirname "$RBOT")" && pwd)/$(basename "$RBOT")
IFS=, read -r -a TOOLS <<< "$TOOLS_OPT"
for t in "${TOOLS[@]}"; do
  case $t in rbot | ninja | make) ;; *)
    echo "tool tidak dikenal: $t" >&2
    exit 1
    ;;
  esac
done
for t in gcc tar ar $(printf '%s\n' "${TOOLS[@]}" | grep -vx rbot); do
  command -v "$t" > /dev/null 2>&1 || {
    echo "tool wajib tidak ada: $t" >&2
    exit 1
  }
done

HAVE_STRACE=0
command -v strace > /dev/null 2>&1 && HAVE_STRACE=1

# ---------------------------- versi ----------------------------------------
first_ver() { grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
rbot_id() {
  local d c h v_bin
  d=$(dirname "$RBOT")
  c=$(git -C "$d" describe --tags --exact-match 2> /dev/null)
  if [ -n "$c" ]; then
    c="release ${c}"
  else
    c=$(git -C "$d" describe --tags --always --dirty 2> /dev/null)
    if [ -n "$c" ]; then
      c="dev git:${c}"
    else
      v_bin=$($RBOT version 2> /dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
      [ -z "$v_bin" ] && v_bin=$(strings "$RBOT" 2> /dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
      c="release ${v_bin:-unknown}"
    fi
  fi
  h=$(sha1sum "$RBOT" 2> /dev/null | cut -c1-6)
  echo "${c} bin:${h:-n/a}"
}
declare -A VER
VER_GCC=$(gcc -dumpfullversion 2> /dev/null || gcc -dumpversion 2> /dev/null)
command -v ninja > /dev/null 2>&1 && VER[ninja]=$(ninja --version 2>&1 | first_ver)
command -v make > /dev/null 2>&1 && VER[make]=$(make --version 2>&1 | first_ver)
VER[rbot]=$(rbot_id)

# ---------------------------- direktori kerja ------------------------------
[ -n "$OUTDIR" ] || OUTDIR="$PWD/ws.tmp"
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
WS="$OUTDIR/wstest"
CSV="$OUTDIR/results.csv"
SYSCALL_CSV="$OUTDIR/workspace-syscalls.csv"
MARK="$OUTDIR/.mark"
ERRLOG="$OUTDIR/stderr.log"
rm -f "$CSV" "$ERRLOG"
echo 'label,scenario,ms,rc,obj,arc,bin,version' > "$CSV"
echo 'tool,scenario,run,rank,percent,seconds,usecs_per_call,calls,errors,syscall' > "$SYSCALL_CSV"

EXPECT_OBJ=$((2 * (NSRC + 1))) # core + bade objects

# ---------------------------- tampilan -------------------------------------
TTY=0
[ -t 1 ] && TTY=1
if [ "$TTY" = 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RST=$'\033[0m'
  C_BLD=$'\033[1m'
  C_DIM=$'\033[2m'
  C_GRN=$'\033[32m'
  C_RED=$'\033[31m'
  C_YEL=$'\033[33m'
  C_CYN=$'\033[36m'
else
  C_RST=""
  C_BLD=""
  C_DIM=""
  C_GRN=""
  C_RED=""
  C_YEL=""
  C_CYN=""
fi
COLS=$(tput cols 2> /dev/null || echo 80)
[ "$COLS" -gt 0 ] 2> /dev/null || COLS=80
if ((COLS >= 90)); then
  WIDE=1
  BW=$((COLS - 70))
else
  WIDE=0
  BW=$((COLS - 38))
fi
((BW > 24)) && BW=24
((BW < 6)) && BW=6
PBW=$((COLS - 30))
((PBW > 30)) && PBW=30
((PBW < 8)) && PBW=8
STEP=0
TOTAL=1
CUR=""
T_START=$SECONDS
fmt_t() { printf '%d:%02d' $(($1 / 60)) $(($1 % 60)); }
bar() {
  local d=$1 t=$2 w=$3 f i s=""
  ((t > 0)) || t=1
  f=$((d * w / t))
  for ((i = 0; i < w; i++)); do if ((i < f)); then s+="█"; else s+="░"; fi; done
  printf '%s' "$s"
}
clear_line() {
  [ "$TTY" = 1 ] && printf '\r\033[K'
  return 0
}
draw_progress() {
  [ "$TTY" = 1 ] || return 0
  local el=$((SECONDS - T_START)) eta=0 pct
  pct=$((STEP * 100 / TOTAL))
  ((STEP > 0)) && eta=$((el * (TOTAL - STEP) / STEP))
  printf '\r\033[K  %s%s%s %3d%% %s%d/%d ETA %s%s' \
    "$C_CYN" "$(bar "$STEP" "$TOTAL" "$PBW")" "$C_RST" "$pct" \
    "$C_DIM" "$STEP" "$TOTAL" "$(fmt_t "$eta")" "$C_RST"
  if ((COLS >= 90)); then printf '  %s▸ %s%s' "$C_YEL" "$CUR" "$C_RST"; fi
}

# ---------------------------- generator workspace --------------------------
gen_c() { # gen_c <proyek> <prefix>
  local proj=$1 pre=$2 h i
  mkdir -p "$WS/$proj/src"
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      [ "$h" -gt 1 ] && echo "#include \"h$((h - 1)).h\""
      echo "static const int v$h = $h;"
    } > "$WS/$proj/src/h$h.h"
  done
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int ${pre}$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } > "$WS/$proj/src/${pre}$i.c"
  done
  {
    echo "#include \"h1.h\""
    echo "int ${pre}1(void);"
    echo "int main(void) { return ${pre}1() - 1; }"
  } > "$WS/$proj/src/main.c"
}

gen_ws() {
  rm -rf "$WS"
  mkdir -p "$WS/rupamod"
  local i
  gen_c core r
  gen_c bade k
  for ((i = 1; i <= NMOD; i++)); do
    printf 'module m%s\nexport f%s\n' "$i" "$i" > "$WS/rupamod/m$i.rp"
  done

  # ---- Buildfile.ws (Sesuai desain terbaru) ----
  {
    echo "use workspace"
    echo "projects.rupamod as mod"
    echo "projects.core as core"
    echo "projects = bade, core, rupamod"
    echo "compiler = $COMPILERS"
    echo "flags = Wall, Wextra, MMD, MP"
    echo "std = gnu11"
    echo "mod.output.binary = false"
    echo "mod.archive.src = ."
    echo "mod.archive.name = rupa_modules"
    echo "mod.archive.with = tar"
    echo "mod.archive.dir = ../modules"
    echo "core.sources = src"
    echo "core.exclude = main.c"
    echo "core.output.binaryName = rupa"
    echo "core.output.libraryName = core"
    echo "core.output.libraryShared = false"
    echo "bade.sources = manager, src"
    echo "bade.exclude = manager/main.c"
    echo "bade.output.binaryName = bade"
    echo "bade.output.libraryName = bade"
    echo "bade.output.libraryShared = false"
    echo "bade.depends_on = rupamod"
    echo "bade.embedded.modules.file = ../modules/rupa_modules.tar"
    echo "bade.embedded.modules.variable = MODULES"
    echo "bade.library.cdeps = core"
    echo "core.library.cdeps = bade"
  } > "$WS/Buildfile.ws"

  # ---- pembanding: build.ninja (FAIR: Static Libs + Circular Deps) ----
  {
    echo "cflags = -Wall -Wextra -std=gnu11"
    echo "rule cc"
    echo "  command = gcc \$cflags -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "rule ar"
    echo "  command = ar rcs \$out \$in"
    echo "rule link_group"
    echo "  command = gcc -Wl,--start-group \$in -Wl,--end-group -o \$out"
    echo "rule pack"
    echo "  command = tar -cf \$out -C rupamod ."
    echo "rule embed"
    echo "  command = ld -r -b binary -o \$out \$in"

    local p pre
    for p in core:r bade:k; do
      pre=${p#*:}
      p=${p%%:*}
      echo "build $p/build/main.o: cc $p/src/main.c"
      for ((i = 1; i <= NSRC; i++)); do echo "build $p/build/$pre$i.o: cc $p/src/$pre$i.c"; done
    done

    echo -n "build rupamod/modules/rupa_modules.tar: pack"
    for ((i = 1; i <= NMOD; i++)); do printf ' rupamod/m%d.rp' "$i"; done
    echo

    echo "build bade/build/embed.o: embed rupamod/modules/rupa_modules.tar"

    echo -n "build core/build/libcore.a: ar core/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' core/build/r%d.o' "$i"; done
    echo

    echo -n "build bade/build/libbade.a: ar bade/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' bade/build/k%d.o' "$i"; done
    echo " bade/build/embed.o"

    # Circular dependency diselesaikan dengan linker group
    echo "build core/bin/rupa: link_group core/build/libcore.a bade/build/libbade.a"
    echo "build bade/bin/bade: link_group core/build/libcore.a bade/build/libbade.a"

    echo "default core/bin/rupa bade/bin/bade"
  } > "$WS/build.ninja"

  # ---- pembanding: Makefile (FAIR: Static Libs + Circular Deps) ----
  {
    echo "CFLAGS := -Wall -Wextra -std=gnu11"
    echo -n "CORE_OBJS := core/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' core/build/r%d.o' "$i"; done
    echo
    echo -n "BADE_OBJS := bade/build/main.o"
    for ((i = 1; i <= NSRC; i++)); do printf ' bade/build/k%d.o' "$i"; done
    echo " bade/build/embed.o"
    echo -n "MODS :="
    for ((i = 1; i <= NMOD; i++)); do printf ' rupamod/m%d.rp' "$i"; done
    echo

    echo "all: core/bin/rupa bade/bin/bade"

    # Linking dengan --start-group untuk circular dependency
    printf 'core/bin/rupa: core/build/libcore.a bade/build/libbade.a\n\t@mkdir -p $(@D)\n\tgcc -Wl,--start-group $^ -Wl,--end-group -o $@\n'
    printf 'bade/bin/bade: core/build/libcore.a bade/build/libbade.a\n\t@mkdir -p $(@D)\n\tgcc -Wl,--start-group $^ -Wl,--end-group -o $@\n'

    printf 'core/build/libcore.a: $(CORE_OBJS)\n\tar rcs $@ $^\n'
    printf 'bade/build/libbade.a: $(BADE_OBJS)\n\tar rcs $@ $^\n'

    printf 'rupamod/modules/rupa_modules.tar: $(MODS)\n\t@mkdir -p $(@D)\n\ttar -cf $@ -C rupamod .\n'
    printf 'bade/build/embed.o: rupamod/modules/rupa_modules.tar\n\t@mkdir -p $(@D)\n\tld -r -b binary -o $@ $<\n'

    printf 'core/build/%%.o: core/src/%%.c\n\t@mkdir -p $(@D)\n\tgcc $(CFLAGS) -MMD -MP -c $< -o $@\n'
    printf 'bade/build/%%.o: bade/src/%%.c\n\t@mkdir -p $(@D)\n\tgcc $(CFLAGS) -MMD -MP -c $< -o $@\n'

    echo '-include $(CORE_OBJS:.o=.d) $(BADE_OBJS:.o=.d)'
  } > "$WS/Makefile"
}

# ---------------------------- perintah per tool ----------------------------
tool_cmd() {
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -w ;;
    ninja) ninja ;;
    make) make -j"$NPROC" ;;
  esac
}
sel_cmd() { # build selektif: hanya proyek core
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -w core ;;
    ninja) ninja core/bin/rupa ;;
    make) make -j"$NPROC" core/bin/rupa ;;
  esac
}
dep_cmd() { # build selektif bade
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -w bade ;;
    ninja) ninja bade/bin/bade ;;
    make) make -j"$NPROC" bade/bin/bade ;;
  esac
}
build_full() {
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -j"$NPROC" -w ;;
    *) tool_cmd "$1" ;;
  esac
}

# ---------------------------- pengukuran -----------------------------------
count_new() { find "$WS" "$@" -newer "$MARK" 2> /dev/null | wc -l | tr -d '[:space:]'; }

RC_MISMATCH=97
MISMATCH_RUNS=0
EXPECT_MISS=""
exp_chk() { [ "$2" -eq "$3" ] || EXPECT_MISS+=" $1=$2(harap $3)"; }
expect_check() {
  local s=$2 o=$3 a=$4 b=$5
  EXPECT_MISS=""
  case $s in
    cold)
      exp_chk obj "$o" $((EXPECT_OBJ + 1))
      exp_chk arc "$a" 1
      exp_chk bin "$b" 2
      ;;
    noop)
      exp_chk obj "$o" 0
      exp_chk arc "$a" 0
      exp_chk bin "$b" 0
      ;;
    leaf | bade | sel)
      exp_chk obj "$o" 1
      exp_chk arc "$a" 0
      exp_chk bin "$b" 1
      ;;
    mod | dep)
      exp_chk obj "$o" 1
      exp_chk arc "$a" 1
      exp_chk bin "$b" 1
      ;;
    touch)
      exp_chk obj "$o" 0
      exp_chk arc "$a" 0
      exp_chk bin "$b" 0
      ;;
  esac
}

run_once() {
  local label=$1 scen=$2
  shift 2
  local t0 t1 rc ms nobj narc nbin
  CUR="$label/$scen"
  draw_progress
  touch "$MARK"
  t0=${EPOCHREALTIME/[.,]/}
  (cd "$WS" && "$@") > /dev/null 2>> "$ERRLOG"
  rc=$?
  t1=${EPOCHREALTIME/[.,]/}
  ms=$(((t1 - t0 + 500) / 1000))
  nobj=$(count_new -name '*.o')
  narc=$(count_new -type f -name 'rupa_modules.tar')
  nbin=$(count_new -type f \( -name rupa -o -name bade \) -perm -u+x)

  expect_check "$label" "$scen" "$nobj" "$narc" "$nbin"
  if [ "$rc" -eq 0 ] && [ -n "$EXPECT_MISS" ]; then
    rc=$RC_MISMATCH
    MISMATCH_RUNS=$((MISMATCH_RUNS + 1))
  fi
  clear_line
  if [ "$rc" -eq "$RC_MISMATCH" ]; then
    printf '  %s✗ %-6s %-6s TIDAK SESUAI HARAPAN:%s%s\n' "$C_RED" "$label" "$scen" "$EXPECT_MISS" "$C_RST"
    ms=-1
  elif [ "$rc" -ne 0 ]; then
    printf '  %s✗ %-6s %-6s GAGAL (exit %s)%s\n' "$C_RED" "$label" "$scen" "$rc" "$C_RST"
    MISMATCH_RUNS=$((MISMATCH_RUNS + 1))
    ms=-1
  else
    printf '  %s✓%s %-6s %-6s %6s ms  %sobj:%s arc:%s bin:%s%s\n' \
      "$C_GRN" "$C_RST" "$label" "$scen" "$ms" "$C_DIM" "$nobj" "$narc" "$nbin" "$C_RST"
  fi
  local ver=${VER[$label]:-}
  ver=${ver//,/ }
  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$label" "$scen" "$ms" "$rc" "$nobj" "$narc" "$nbin" "$ver" >> "$CSV"
  STEP=$((STEP + 1))
  draw_progress
}

have_scenario() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

reset_all() {
  (cd "$WS" && rm -rf core/build core/bin bade/build bade/bin \
    rupamod/build rupamod/modules .ninja_log .ninja_deps)
}

ARTIFACT_WARNED=0
check_artifacts() {
  [ "$ARTIFACT_WARNED" = 1 ] && return 0
  local miss=""
  [ -n "$(find "$WS" -type f -name rupa -perm -u+x 2> /dev/null | head -1)" ] || miss+=" rupa"
  [ -n "$(find "$WS" -type f -name bade -perm -u+x 2> /dev/null | head -1)" ] || miss+=" bade"
  [ -n "$(find "$WS" -type f -name rupa_modules.tar 2> /dev/null | head -1)" ] || miss+=" rupa_modules.tar"
  if [ -n "$miss" ]; then
    ARTIFACT_WARNED=1
    clear_line
    printf '  %s! %s tidak menghasilkan:%s (cek stderr.log / opsi -a)%s\n' "$C_YEL" "$1" "$miss" "$C_RST"
  fi
}

prime_tool() {
  CUR="prime $1"
  draw_progress
  reset_all
  if ! (cd "$WS" && build_full "$1" && tool_cmd "$1") > /dev/null 2>> "$ERRLOG"; then
    clear_line
    printf '  %s! prime gagal: %s (lihat %s)%s\n' "$C_YEL" "$1" "$ERRLOG" "$C_RST"
    tail -n 3 "$ERRLOG" 2> /dev/null | sed 's/^/      /'
  fi
  check_artifacts "$1"
}

# mutator: <run#> <tool>
mut_leaf() { (cd "$WS" && printf '\n/* leaf %s-%s */\n' "$2" "$1" >> core/src/r1.c); }
mut_bade() { (cd "$WS" && printf '\n/* bade %s-%s */\n' "$2" "$1" >> bade/src/k1.c); }
mut_mod() { (cd "$WS" && printf '\n# edit %s-%s\n' "$2" "$1" >> rupamod/m1.rp); }
mut_sel() {
  mut_leaf "$@"
  mut_bade "$@"
}
mut_dep() {
  mut_mod "$@"
  mut_leaf "$@"
}
mut_touch() { (cd "$WS" && touch "bade/src/h$((NHEAD / 2 + 1)).h"); }

s_cold() {
  have_scenario cold || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      reset_all
      run_once "$k" cold build_full "$k"
    done
  done
}

s_strace() {
  have_scenario strace || return 0
  [ "$HAVE_STRACE" != 1 ] && {
    clear_line
    printf '  %s! strace tidak tersedia%s\n' "$C_YEL" "$C_RST"
    return 0
  }
  local k r report total_sec total_calls total_err rank
  local pct sec usecs calls errs syscall
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      reset_all
      CUR="strace/$k#$r"
      draw_progress
      report="$OUTDIR/workspace.strace.${k}.${r}.txt"
      rm -f "$report"
      if [ "$k" = rbot ]; then
        tcmd=("$RBOT" $RBOT_ARGS -j"$NPROC" -w)
      elif [ "$k" = ninja ]; then
        tcmd=(ninja)
      else tcmd=(make -j"$NPROC"); fi
      (cd "$WS" && strace -c -f -o "$report" "${tcmd[@]}") > /dev/null 2>> "$ERRLOG"
      total_sec=$(awk '$NF == "total" { print $2; exit }' "$report")
      total_calls=$(awk '$NF == "total" { print $4; exit }' "$report")
      total_err=$(awk '$NF == "total" { if (NF >= 6) print $5; else print 0; exit }' "$report")
      [ -n "$total_sec" ] || total_sec=0
      [ -n "$total_calls" ] || total_calls=0
      [ -n "$total_err" ] || total_err=0
      rank=0
      while IFS=$'\t' read -r pct sec usecs calls errs syscall; do
        [ -n "$syscall" ] || continue
        rank=$((rank + 1))
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$k" strace "$r" "$rank" "$pct" "$sec" "$usecs" "$calls" "$errs" "$syscall" >> "$SYSCALL_CSV"
        [ "$rank" -ge 10 ] && break
      done < <(awk 'NR == 1 { next } /^[- ]*$/ { next } $NF == "total" { next } $NF ~ /^[[:alnum:]_?]+$/ { if (NF >= 6) printf "%s\t%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5, $6; else if (NF == 5) printf "%s\t%s\t%s\t%s\t0\t%s\n", $1, $2, $3, $4, $5 }' "$report" | sort -t$'\t' -k2,2nr)
      clear_line
      printf '  %s✓%s %-6s strace %-4s total=%ss calls=%s err=%s\n' "$C_GRN" "$C_RST" "$k" "$r" "$total_sec" "$total_calls" "$total_err"
      awk 'NR == 1 { next } /^[- ]*$/ { next } $NF == "total" { next } $NF ~ /^[[:alnum:]_?]+$/ { if (NF >= 6) printf "%.2f\t%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5, $6; else if (NF == 5) printf "%.2f\t%s\t%s\t%s\t0\t%s\n", $1, $2, $3, $4, $5 }' "$report" | sort -t$'\t' -k2,2nr | head -5 | awk -F '\t' '{ printf "      %5s%% %9ss %8s us/call %7s calls %6s err  %s\n", $1, $2, $3, $4, $5, $6, $7 }'
      STEP=$((STEP + 1))
      draw_progress
    done
  done
}

run_scenario() {
  have_scenario "$1" || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      [ "$MUT_DELAY" != 0 ] && sleep "$MUT_DELAY"
      "$2" "$r" "$k"
      run_once "$k" "$1" "${3:-tool_cmd}" "$k"
    done
  done
}
mut_none() { :; }

# ---------------------------- laporan --------------------------------------
report() {
  local sorted="$OUTDIR/.sorted.csv"
  awk -F, 'NR > 1' "$CSV" | sort -t, -k2,2 -k1,1 -k3,3n > "$sorted"
  awk -F, -v BW="$BW" -v WIDE="$WIDE" -v B="$C_BLD" -v D="$C_DIM" -v G="$C_GRN" -v Y="$C_YEL" -v R="$C_RED" -v Z="$C_RST" '
{
g = $2 SUBSEP $1
if (!(g in seen)) { seen[g] = 1; ng++; gs[ng] = g; gscen[ng] = $2; glab[ng] = $1 }
if ($4 == 0) { n[g]++; v[g, n[g]] = $3; ro[g] += $5; ra[g] += $6; rn[g] += $7 } else bad[g]++
}
END {
nsc = split("cold noop leaf bade mod touch sel dep", SC, " ")
desc["cold"]  = "build dari nol, 3 proyek"
desc["noop"]  = "tanpa perubahan"
desc["leaf"]  = "edit 1 source core (bade tak boleh ikut)"
desc["bade"]  = "edit 1 source bade"
desc["mod"]   = "edit 1 .rp rupamod -> arsip -> bade"
desc["touch"] = "touch 1 header bade, konten sama"
desc["sel"]   = "edit core+bade, build selektif -w core"
desc["dep"]   = "edit .rp+core, build selektif -w bade"
for (si = 1; si <= nsc; si++) {
s = SC[si]; k = 0; nf = 0
for (i = 1; i <= ng; i++) {
if (gscen[i] != s) continue; g = gs[i]
if (n[g] > 0) {
m = n[g]; med = (m % 2) ? v[g, (m + 1) / 2] : (v[g, m / 2] + v[g, m / 2 + 1]) / 2
k++; L[k] = glab[i]; M[k] = med; LO[k] = v[g, 1]; HI[k] = v[g, m]
OB[k] = ro[g] / m; AR[k] = ra[g] / m; BN[k] = rn[g] / m; BD[k] = bad[g] + 0
} else fl[++nf] = glab[i]
}
if (k == 0 && nf == 0) continue
for (a = 2; a <= k; a++) for (b = a; b > 1 && M[b] < M[b - 1]; b--) {
t = L[b]; L[b] = L[b - 1]; L[b - 1] = t; t = M[b]; M[b] = M[b - 1]; M[b - 1] = t
t = LO[b]; LO[b] = LO[b - 1]; LO[b - 1] = t; t = HI[b]; HI[b] = HI[b - 1]; HI[b - 1] = t
t = OB[b]; OB[b] = OB[b - 1]; OB[b - 1] = t; t = AR[b]; AR[b] = AR[b - 1]; AR[b - 1] = t
t = BN[b]; BN[b] = BN[b - 1]; BN[b - 1] = t; t = BD[b]; BD[b] = BD[b - 1]; BD[b - 1] = t
}
printf "\n%s%s%s %s%s%s\n", B, toupper(s), Z, D, desc[s], Z
mx = (k > 0) ? M[k] : 1; if (mx < 1) mx = 1; best = (k > 0) ? M[1] : 1; if (best < 1) best = 1
for (i = 1; i <= k; i++) {
len = int(M[i] / mx * BW + 0.5); if (len < 1) len = 1; br = ""
for (j = 1; j <= BW; j++) br = br (j <= len ? "█" : "░")
col = (i == 1) ? G : ""; mark = (i == 1) ? "★" : " "
printf "  %s%s %-6s %s %6d ms %5.2f×%s", col, mark, L[i], br, M[i], M[i] / best, Z
if (WIDE) printf "  %s(%d–%d) obj:%d arc:%d bin:%d%s", D, LO[i], HI[i], OB[i] + 0.5, AR[i] + 0.5, BN[i] + 0.5, Z
if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z; printf "\n"
}
for (i = 1; i <= nf; i++) printf "  %s✗ %-6s semua run gagal atau tidak sesuai harapan%s\n", R, fl[i], Z
}
}' "$sorted"
  rm -f "$sorted"
  echo
  echo "${C_DIM}median ms · ★ tercepat · × = rasio vs tercepat${C_RST}"
  echo "${C_DIM}Invalidasi yang diharapkan (pembanding ninja/make):${C_RST}"
  echo "${C_DIM}  leaf : obj 1, bin 1 (core saja) · bade: obj 1, bin 1${C_RST}"
  echo "${C_DIM}  mod  : arc 1, bin 1, obj 1 (.o embed arsip, bade relink)${C_RST}"
  echo "${C_DIM}  touch: tidak boleh ada recompile (semua tool)${C_RST}"
  echo "${C_DIM}  sel  : obj 1, bin 1 (hanya core; bade dibiarkan kotor)${C_RST}"
  echo "${C_DIM}  dep  : arc 1, bin 1, obj 1 (rupamod+bade; core kotor; .o embed)${C_RST}"
}

# ---------------------------- main -----------------------------------------
trap 'echo; echo "dibatalkan."; exit 130' INT
echo "${C_BLD}${C_CYN}▌ rbot workspace benchmark${C_RST} ${C_DIM}· ${#TOOLS[@]} tool · $NRUN run · median${C_RST}"
echo "  workspace: rupamod ($NMOD .rp) · core ($NSRC .c) · bade ($NSRC .c)"
echo "  graf     : bade depends_on rupamod (embed arsip), core <-> bade (circular)"
echo "  perintah : rbot -w (semua) · rbot -w <nama> (selektif)"
echo "  versi    :"
for k in "${TOOLS[@]}"; do printf '    %-6s %s\n' "$k" "${VER[$k]:-?}"; done
printf '    %-6s %s\n' gcc "$VER_GCC · $(uname -m)"
[ -n "$RBOT_ARGS" ] && echo "  rbot args: $RBOT_ARGS"
echo "  dir      : $WS"
echo

TOTAL=0
for s in cold noop leaf bade mod touch sel dep strace; do
  have_scenario "$s" && TOTAL=$((TOTAL + ${#TOOLS[@]} * NRUN))
done
[ "$TOTAL" -gt 0 ] || TOTAL=1

CUR="gen"
draw_progress
gen_ws
clear_line
T_START=$SECONDS
draw_progress

s_cold
run_scenario noop mut_none
run_scenario leaf mut_leaf
run_scenario bade mut_bade
run_scenario mod mut_mod
run_scenario touch mut_touch
run_scenario sel mut_sel sel_cmd
run_scenario dep mut_dep dep_cmd
s_strace

clear_line
echo
echo "${C_GRN}✔ selesai${C_RST} dalam $(fmt_t $((SECONDS - T_START)))"
report
echo
echo "hasil lengkap: $CSV"
[ "$HAVE_STRACE" = 1 ] && echo "syscall detail: $SYSCALL_CSV"
[ -s "$ERRLOG" ] && echo "stderr tool  : $ERRLOG"
rm -f "$MARK"
[ "$KEEP" = 1 ] || rm -rf "$WS"
exit 0
