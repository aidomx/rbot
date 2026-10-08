#!/usr/bin/env bash
# ============================================================================
# compare.sh — benchmark build tool: ninja vs make vs bear+make vs rbot
#                vs cmake-ninja vs meson-ninja vs xmake vs tup
#
# Membuat project sintetis (N source berantai header H, gaya /tmp/bench),
# menuliskan build.ninja + Makefile + Buildfile + CMakeLists.txt +
# meson.build + xmake.lua + Tupfile untuk project yang SAMA,
# lalu mengukur tiap tool pada skenario yang setara.
#
# Skenario:
#   cold   : build dari nol (cmake/meson TERMASUK langkah configure)
#   noop   : build tanpa perubahan apa pun (metrik utama overhead)
#   touch  : touch 1 header (konten sama) — rbot: 0 recompile (content-based)
#   edit   : edit isi 1 source — tepat 1 recompile + relink
#            + baris "gcc-floor" (gcc -c 1 file + link langsung, tanpa build
#              tool) sebagai lantai; selisih tool vs lantai = overhead tool
#   compdb : generate compile_commands.json (bear: butuh build bersih)
#   strace : hitung syscall no-op (jika strace tersedia)
#
# Metodologi (v2):
#   - Tiap tool diukur berurutan (tool-major) dan DIPRIME dulu: build bersih
#     + 1 run no-op yang tidak dihitung. Ini mencegah tool saling mengotori
#     state (.d, .ninja_log, objek) pada direktori build bersama.
#   - Tabel akhir memakai MEDIAN + min-max, bukan rerata.
#   - Kolom "obj" = jumlah .o yang benar-benar dikompilasi ulang (sanity check).
#   - results.csv memuat versi tiap tool (kolom "version"; rbot = git/hash biner).
#   - syscalls.csv memuat rincian syscall per tool (tool,syscall,calls,errors).
#
# Pemakaian:
#   ./bench/compare.sh                # default: 150 source, 30 header, 5 run
#   ./bench/compare.sh -n 40 -H 10    # project kecil, cepat
#   ./bench/compare.sh -s edit,touch  # subset skenario
#   NO_COLOR=1 ./bench/compare.sh     # tanpa warna / progress bar
#
# Catatan kejujuran: semua tool menghasilkan artefak yang setara, tapi
# fiturnya beda. Angka ini untuk melacak REGRESI rbot — bukan klaim
# bahwa satu tool "lebih baik" secara umum.
# ============================================================================
set -u

[ -n "${EPOCHREALTIME:-}" ] || { echo "butuh bash >= 5 (EPOCHREALTIME)" >&2; exit 1; }

# ---------------------------- konfigurasi ----------------------------------
NSRC=150      # -n : jumlah source t1..tN.c
NHEAD=30      # -H : jumlah header berantai h1->hH
NRUN=5        # -r : pengulangan per skenario (ambil median)
RBOT=bin/rbot # -b : binary rbot yang dipakai
KEEP=0        # -k : 1 = jangan hapus sys.tmp di akhir
ONLY=""       # -s : hanya skenario ini (comma-separated)
OUTDIR=""     # -o : direktori kerja benchmark (default sys.tmp di cwd)
NPROC=$(nproc 2>/dev/null || echo 4)

usage() { sed -n '2,/^# ====.*$/p' "$0"; exit 0; }
while getopts "n:H:r:b:t:ko:s:h" opt; do
  case $opt in
    n) NSRC=$OPTARG ;;
    H) NHEAD=$OPTARG ;;
    r) NRUN=$OPTARG ;;
    b) RBOT=$OPTARG ;;
    t) TARGETS=$OPTARG ;;
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

# Tool opsional
HAVE_BEAR=0;    command -v bear   >/dev/null 2>&1 && HAVE_BEAR=1
HAVE_STRACE=0;  command -v strace >/dev/null 2>&1 && HAVE_STRACE=1
HAVE_CMAKE=0;   command -v cmake  >/dev/null 2>&1 && HAVE_CMAKE=1
HAVE_MESON=0;   command -v meson  >/dev/null 2>&1 && HAVE_MESON=1
HAVE_XMAKE=0;   command -v xmake  >/dev/null 2>&1 && HAVE_XMAKE=1
HAVE_TUP=0;     command -v tup    >/dev/null 2>&1 && HAVE_TUP=1

# Daftar tool aktif (kunci internal); label tampilan lewat label_of()
TOOLS=(ninja make)
[ "$HAVE_BEAR"  = 1 ] && TOOLS+=(bear)
TOOLS+=(rbot)
[ "$HAVE_CMAKE" = 1 ] && TOOLS+=(cmake)
[ "$HAVE_MESON" = 1 ] && TOOLS+=(meson)
[ "$HAVE_XMAKE" = 1 ] && TOOLS+=(xmake)
[ "$HAVE_TUP"   = 1 ] && TOOLS+=(tup)
CDB_TOOLS=()   # make & tup tidak punya compdb native (make => lihat bear)
# Filter tool benchmark jika -t diberikan.
# Urutan TOOLS mengikuti urutan yang diminta user.
if [ -n "$TARGETS" ]; then
  OLD_TOOLS=("${TOOLS[@]}")
  TOOLS=()
  IFS=',' read -r -a REQUESTED_TOOLS <<< "$TARGETS"
  for req in "${REQUESTED_TOOLS[@]}"; do
    found=0
    for k in "${OLD_TOOLS[@]}"; do
      if [ "$k" = "$req" ]; then
        TOOLS+=("$k")
        found=1
        break
      fi
    done
    [ "$found" = 1 ] || { echo "target tool tidak tersedia: $req" >&2; exit 1; }
  done
  [ "${#TOOLS[@]}" -gt 0 ] || { echo "target tool kosong" >&2; exit 1; }
fi

CDB_TOOLS=()   # make & tup tidak punya compdb native (make => lihat bear)
for k in "${TOOLS[@]}"; do
  case $k in make|tup) ;; *) CDB_TOOLS+=("$k") ;; esac
done

label_of() {
  case $1 in
    ninja) echo ninja ;;       make)  echo make ;;
    floor) echo gcc-floor ;;
    bear)  echo bear-make ;;   rbot)  echo rbot ;;
    cmake) echo cmake-ninja ;; meson) echo meson-ninja ;;
    xmake) echo xmake ;;       tup)   echo tup ;;
  esac
}

# ---------------------------- versi tool -----------------------------------
first_ver() { grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
rbot_id() {
    local d c h v_bin
    d=$(dirname "$RBOT")

    # 1. PRIORITAS: Cek apakah ini persis di Git Tag (Release Resmi)
    c=$(git -C "$d" describe --tags --exact-match 2>/dev/null)
    if [ -n "$c" ]; then
        c="release ${c}"
    else
        # 2. FALLBACK 1: Build Development (ada commit setelah tag, atau dirty)
        c=$(git -C "$d" describe --tags --always --dirty 2>/dev/null)
        if [ -n "$c" ]; then
            c="dev git:${c}"
        else
            # 3. FALLBACK 2: Binary sudah di-install (tidak ada folder .git)
            # Coba ambil versi dari output binary itu sendiri, atau dari strings
            v_bin=$($RBOT version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
            if [ -z "$v_bin" ]; then
                v_bin=$(strings "$RBOT" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
            fi
            c="release ${v_bin:-unknown}"
        fi
    fi

    # Hash unik untuk file binary (tetap berguna untuk melacak build yang sama)
    h=$(sha1sum "$RBOT" 2>/dev/null | cut -c1-6)
    
    echo "${c} bin:${h:-n/a}"
}
declare -A VER
VER_GCC=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion 2>/dev/null)
VER[ninja]=$(ninja --version 2>&1 | first_ver)
VER[make]=$(make --version 2>&1 | first_ver)
VER[rbot]=$(rbot_id)
VER[gcc-floor]="gcc $VER_GCC"
[ "$HAVE_BEAR"  = 1 ] && VER[bear-make]="bear $(bear --version 2>&1 | first_ver) + make ${VER[make]}"
[ "$HAVE_CMAKE" = 1 ] && VER[cmake-ninja]="cmake $(cmake --version 2>&1 | first_ver) + ninja ${VER[ninja]}"
[ "$HAVE_MESON" = 1 ] && VER[meson-ninja]="meson $(meson --version 2>&1 | first_ver) + ninja ${VER[ninja]}"
[ "$HAVE_XMAKE" = 1 ] && VER[xmake]=$(xmake --version 2>&1 | first_ver)
[ "$HAVE_TUP"   = 1 ] && VER[tup]=$(tup --version 2>&1 | first_ver)

# ---------------------------- direktori kerja ------------------------------
if [ -z "$OUTDIR" ]; then OUTDIR="$PWD/sys.tmp"; fi
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
PROJ="$OUTDIR/proj"
CSV="$OUTDIR/results.csv"
MARK="$OUTDIR/.mark"
ERRLOG="$OUTDIR/stderr.log"
rm -f "$CSV" "$ERRLOG"
echo 'label,scenario,ms,rc,rebuilt,version' >"$CSV"
SYSCSV="$OUTDIR/syscalls.csv"

# ---------------------------- tampilan -------------------------------------
TTY=0; [ -t 1 ] && TTY=1
if [ "$TTY" = 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RST=$'\033[0m'; C_BLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_GRN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_CYN=$'\033[36m'
else
  C_RST=""; C_BLD=""; C_DIM=""; C_GRN=""; C_RED=""; C_YEL=""; C_CYN=""
fi
COLS=$(tput cols 2>/dev/null || echo 80)
[ "$COLS" -gt 0 ] 2>/dev/null || COLS=80
if (( COLS >= 80 )); then WIDE=1; BW=$(( COLS - 60 )); else WIDE=0; BW=$(( COLS - 38 )); fi
(( BW > 24 )) && BW=24
(( BW < 6 ))  && BW=6
PBW=$(( COLS - 30 ))
(( PBW > 30 )) && PBW=30
(( PBW < 8 ))  && PBW=8

STEP=0; TOTAL=1; CUR=""; T_START=$SECONDS

fmt_t() { printf '%d:%02d' $(( $1 / 60 )) $(( $1 % 60 )); }

bar() {  # bar <selesai> <total> <lebar>
  local d=$1 t=$2 w=$3 f i s=""
  (( t > 0 )) || t=1
  f=$(( d * w / t ))
  for ((i = 0; i < w; i++)); do
    if (( i < f )); then s+="█"; else s+="░"; fi
  done
  printf '%s' "$s"
}

clear_line() { [ "$TTY" = 1 ] && printf '\r\033[K'; return 0; }

draw_progress() {
  [ "$TTY" = 1 ] || return 0
  local el=$(( SECONDS - T_START )) eta=0 pct
  pct=$(( STEP * 100 / TOTAL ))
  (( STEP > 0 )) && eta=$(( el * (TOTAL - STEP) / STEP ))
  printf '\r\033[K  %s%s%s %3d%% %s%d/%d ETA %s%s' \
    "$C_CYN" "$(bar "$STEP" "$TOTAL" "$PBW")" "$C_RST" "$pct" \
    "$C_DIM" "$STEP" "$TOTAL" "$(fmt_t "$eta")" "$C_RST"
  if (( COLS >= 90 )); then printf '  %s▸ %s%s' "$C_YEL" "$CUR" "$C_RST"; fi
}

# ---------------------------- generator project ----------------------------
gen_project() {
  rm -rf "$PROJ"
  mkdir -p "$PROJ/src" "$PROJ/include" "$PROJ/bin" "$PROJ/build"

  # Header berantai h1 -> h2 -> ... -> hH
  local h
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      [ "$h" -gt 1 ] && echo "#include \"h$((h - 1)).h\""
      echo "static const int v$h = $h;"
    } >"$PROJ/include/h$h.h"
  done

  # Source files
  local i
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int t$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } >"$PROJ/src/t$i.c"
  done

  # main.c
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
    echo -n "build bin/app: link build/main.o "
    for ((i = 1; i <= NSRC; i++)); do printf 'build/t%d.o ' "$i"; done
    echo
    echo
    echo "default bin/app"
  } >"$PROJ/build.ninja"

  # ---- Makefile ----
  {
    echo "CC      := gcc"
    echo "CFLAGS  := -O2 -Iinclude"
    echo -n "OBJS    := build/main.o "
    for ((i = 1; i <= NSRC; i++)); do printf 'build/t%d.o ' "$i"; done
    echo
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
  ( cd "$PROJ" && "$RBOT" init >/dev/null 2>&1 )

  # ---- CMakeLists.txt (untuk CMake + Ninja) ----
  {
    echo "cmake_minimum_required(VERSION 3.10)"
    echo "project(app C)"
    echo
    echo "set(CMAKE_C_STANDARD 11)"
    echo "set(CMAKE_C_STANDARD_REQUIRED ON)"
    echo "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)"
    echo "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \${CMAKE_BINARY_DIR}/bin)"
    echo
    echo "file(GLOB SOURCES \"src/*.c\")"
    echo "add_executable(app \${SOURCES})"
    echo "target_include_directories(app PRIVATE include)"
    echo "target_compile_options(app PRIVATE -O2 -MMD -MP)"
  } >"$PROJ/CMakeLists.txt"

  # ---- meson.build (untuk Meson + Ninja) ----
  {
    echo "project('app', 'c',"
    echo "        version : '1.0',"
    echo "        default_options : ['c_std=gnu11', 'warning_level=0', 'optimization=2'])"
    echo
    echo "inc = include_directories('include')"
    echo "src = files("
    echo -n "  'src/main.c'"
    for ((i = 1; i <= NSRC; i++)); do
      echo ","
      echo -n "  'src/t$i.c'"
    done
    echo
    echo ")"
    echo
    echo "executable('app', src,"
    echo "           include_directories : inc,"
    echo "           install : false)"
  } >"$PROJ/meson.build"

  # ---- xmake.lua (untuk XMake) ----
  {
    echo "set_project(\"app\")"
    echo "set_version(\"1.0.0\")"
    echo
    echo "set_languages(\"gnu11\")"
    echo "set_warnings(\"none\")"
    echo "set_optimize(\"fast\")"
    echo
    echo "add_includedirs(\"include\")"
    echo
    echo "target(\"app\")"
    echo "    set_kind(\"binary\")"
    echo "    add_files(\"src/*.c\")"
  } >"$PROJ/xmake.lua"

  # ---- Tupfile (untuk Tup) ----
  {
    echo "CFLAGS = -O2 -Iinclude -MMD -MF \$(@).d"
    echo
    echo "foreach src in src/*.c"
    echo "  obj = \$(src:t=o)"
    echo "  : foreach src |> gcc \$(CFLAGS) -c %f -o %o |> build/\$(obj)"
    echo "end"
    echo
    echo -n ": foreach build/*.o |> gcc %f -o %o |> bin/app"
    echo
  } >"$PROJ/Tupfile"
  # Tup butuh inisialisasi database
  if [ "$HAVE_TUP" = 1 ]; then
    ( cd "$PROJ" && tup init >/dev/null 2>&1 || true )
  fi
}

# ---------------------------- perintah per tool ----------------------------
# ARGV diisi dengan argv build inkremental "biasa" tiap tool (dipakai juga
# oleh strace, yang tidak bisa mengeksekusi fungsi bash).
ARGV=()
tool_argv() {
  case $1 in
    ninja) ARGV=(ninja) ;;
    make)  ARGV=(make -j"$NPROC") ;;
    bear)  ARGV=(bear -- make -j"$NPROC") ;;
    rbot)  ARGV=("$RBOT") ;;
    cmake) ARGV=(ninja -C build-cmake) ;;
    meson) ARGV=(ninja -C build-meson) ;;
    xmake) ARGV=(xmake -y) ;;
    tup)   ARGV=(tup) ;;
  esac
}
tool_cmd() { tool_argv "$1"; "${ARGV[@]}"; }

# Build dari nol termasuk langkah configure (dipakai cold & prime).
build_full() {
  case $1 in
    rbot)  "$RBOT" -j"$NPROC" ;;
    cmake) cmake -G Ninja -B build-cmake -DCMAKE_BUILD_TYPE=Release && ninja -C build-cmake ;;
    meson) meson setup build-meson --buildtype=release && ninja -C build-meson ;;
    xmake) xmake -y -j"$NPROC" ;;
    *)     tool_cmd "$1" ;;
  esac
}

# ---------------------------- util pengukuran ------------------------------
run_once() {
  local label=$1 scen=$2; shift 2
  local t0 t1 rc ms nobj
  CUR="$label/$scen"; draw_progress
  touch "$MARK"
  t0=${EPOCHREALTIME/[.,]/}
  ( cd "$PROJ" && "$@" ) >/dev/null 2>>"$ERRLOG"
  rc=$?
  t1=${EPOCHREALTIME/[.,]/}
  ms=$(( (t1 - t0 + 500) / 1000 ))
  nobj=$(find "$PROJ" -name '*.o' -newer "$MARK" 2>/dev/null | wc -l)
  nobj=${nobj//[[:space:]]/}
  if [ "$rc" -eq 0 ] && [ -n "${CHECK_COMPDB:-}" ]; then
    find "$PROJ" -name compile_commands.json -newer "$MARK" -size +2c 2>/dev/null \
      | grep -q . || rc=98
  fi
  clear_line
  if [ "$rc" -ne 0 ]; then
    local why="exit $rc"
    [ "$rc" -eq 98 ] && why="compile_commands.json tidak dihasilkan"
    printf '  %s✗ %-12s %-7s GAGAL (%s)%s\n' "$C_RED" "$label" "$scen" "$why" "$C_RST"
    ms=-1
  else
    printf '  %s✓%s %-12s %-7s %6s ms  %s%s obj%s\n' \
      "$C_GRN" "$C_RST" "$label" "$scen" "$ms" "$C_DIM" "$nobj" "$C_RST"
  fi
  local ver=${VER[$label]:-}; ver=${ver//,/ }
  printf '%s,%s,%s,%s,%s,%s\n' "$label" "$scen" "$ms" "$rc" "$nobj" "$ver" >>"$CSV"
  STEP=$(( STEP + 1 )); draw_progress
}

report() {
  local sorted="$OUTDIR/.sorted.csv"
  awk -F, 'NR > 1' "$CSV" | sort -t, -k2,2 -k1,1 -k3,3n >"$sorted"
  awk -F, -v BW="$BW" -v WIDE="$WIDE" -v B="$C_BLD" -v D="$C_DIM" \
      -v G="$C_GRN" -v Y="$C_YEL" -v R="$C_RED" -v Z="$C_RST" '
    {
      g = $2 SUBSEP $1
      if (!(g in seen)) { seen[g] = 1; ng++; gs[ng] = g; gscen[ng] = $2; glab[ng] = $1 }
      if ($4 == 0) { n[g]++; v[g, n[g]] = $3; rb[g] += $5 } else bad[g]++
    }
    END {
      nsc = split("cold noop touch edit compdb", SC, " ")
      desc["cold"]   = "build dari nol (cmake/meson incl. configure)"
      desc["noop"]   = "tanpa perubahan"
      desc["touch"]  = "touch 1 header, konten sama"
      desc["edit"]   = "edit 1 source (vs lantai gcc)"
      desc["compdb"] = "compile_commands.json (bear = build penuh)"
      for (si = 1; si <= nsc; si++) {
        s = SC[si]; k = 0; nf = 0; hasfl = 0
        for (i = 1; i <= ng; i++) {
          if (gscen[i] != s) continue
          g = gs[i]
          if (n[g] > 0) {
            m = n[g]
            med = (m % 2) ? v[g, (m + 1) / 2] : (v[g, m / 2] + v[g, m / 2 + 1]) / 2
            if (glab[i] == "gcc-floor") { FL = med; FLLO = v[g, 1]; FLHI = v[g, m]; hasfl = 1; continue }
            k++; L[k] = glab[i]; M[k] = med; LO[k] = v[g, 1]; HI[k] = v[g, m]
            RB[k] = rb[g] / m; BD[k] = bad[g] + 0
          } else fl[++nf] = glab[i]
        }
        if (k == 0 && nf == 0 && !hasfl) continue
        for (a = 2; a <= k; a++)
          for (b = a; b > 1 && M[b] < M[b - 1]; b--) {
            t = L[b];  L[b]  = L[b - 1];  L[b - 1]  = t
            t = M[b];  M[b]  = M[b - 1];  M[b - 1]  = t
            t = LO[b]; LO[b] = LO[b - 1]; LO[b - 1] = t
            t = HI[b]; HI[b] = HI[b - 1]; HI[b - 1] = t
            t = RB[b]; RB[b] = RB[b - 1]; RB[b - 1] = t
            t = BD[b]; BD[b] = BD[b - 1]; BD[b - 1] = t
          }
        printf "\n%s%s%s %s%s%s\n", B, toupper(s), Z, D, desc[s], Z
        mx = (k > 0) ? M[k] : 1; if (mx < 1) mx = 1
        best = (k > 0) ? M[1] : 1; if (best < 1) best = 1
        for (i = 1; i <= k; i++) {
          len = int(M[i] / mx * BW + 0.5); if (len < 1) len = 1
          br = ""; for (j = 1; j <= BW; j++) br = br (j <= len ? "█" : "░")
          col = (i == 1) ? G : ""
          mark = (i == 1) ? "★" : " "
          printf "  %s%s %-12s %s %6d ms %5.2f×%s", col, mark, L[i], br, M[i], M[i] / best, Z
          if (WIDE) printf "  %s(%d–%d) obj:%d%s", D, LO[i], HI[i], RB[i] + 0.5, Z
          if (WIDE && hasfl) printf " %s%+dms%s", D, M[i] - FL, Z
          if (s == "touch" && RB[i] < 0.5 && L[i] != "rbot") printf " %s⚠ header?%s", Y, Z
          if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z
          printf "\n"
        }
        if (hasfl) printf "  %s┄ %-12s %6d ms  lantai: gcc -c + link (+N = overhead)%s\n", D, "gcc-floor", FL, Z
        for (i = 1; i <= nf; i++) printf "  %s✗ %-12s semua run gagal%s\n", R, fl[i], Z
      }
    }' "$sorted"
  rm -f "$sorted"
  echo
  echo "${C_DIM}angka = median ms · ★ tercepat · × = rasio vs tercepat${C_RST}"
  echo "${C_DIM}obj = rata-rata .o yang dikompilasi ulang. touch: obj 0 pada tool${C_RST}"
  echo "${C_DIM}selain rbot (⚠) = header tidak terlacak; rbot 0 = content-hash.${C_RST}"
}

# ---------------------------- skenario -------------------------------------
have_scenario() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

reset_build() {
  ( cd "$PROJ" && rm -rf build bin/app )
}

reset_all() {
  # Reset semua artefak termasuk configure cache
  ( cd "$PROJ" && rm -rf build bin/app build-cmake build-meson .xmake .tup compile_commands.json )
  if [ "$HAVE_TUP" = 1 ]; then ( cd "$PROJ" && tup init >/dev/null 2>&1 || true ); fi
}

# Siapkan state bersih & konsisten untuk SATU tool: build penuh + 1 no-op
# (tidak dihitung). Dipanggil sebelum noop/touch/edit/compdb/strace.
prime_tool() {
  CUR="prime $(label_of "$1")"; draw_progress
  reset_all
  if ! ( cd "$PROJ" && build_full "$1" && tool_cmd "$1" ) >/dev/null 2>>"$ERRLOG"; then
    clear_line
    printf '  %s! prime gagal: %s (lihat %s)%s\n' "$C_YEL" "$(label_of "$1")" "$ERRLOG" "$C_RST"
  fi
}

touch_header() { ( cd "$PROJ" && touch "include/h$(( NHEAD / 2 + 1 )).h" ); }

# ---- COLD: build dari nol -------------------------------------------------
s_cold() {
  have_scenario cold || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      reset_all
      run_once "$(label_of "$k")" cold build_full "$k"
    done
  done
}

# ---- NOOP: build tanpa perubahan -----------------------------------------
s_noop() {
  have_scenario noop || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      run_once "$(label_of "$k")" noop tool_cmd "$k"
    done
  done
}

# ---- TOUCH: touch 1 header (konten sama) ---------------------------------
s_touch() {
  have_scenario touch || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      touch_header
      run_once "$(label_of "$k")" touch tool_cmd "$k"
    done
  done
}

# ---- EDIT: edit 1 source (file & jumlah edit sama untuk semua tool) -------
floor_run() {  # lantai: compile 1 file + link, tanpa build tool
  gcc -O2 -Iinclude -MMD -MP -c src/t1.c -o build/t1.o && gcc build/*.o -o bin/app
}
s_edit() {
  have_scenario edit || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    for ((r = 1; r <= NRUN; r++)); do
      ( cd "$PROJ" && printf '\n/* edit %s-%s */\n' "$k" "$r" >>src/t1.c )
      run_once "$(label_of "$k")" edit tool_cmd "$k"
    done
  done
  # lantai (gcc langsung) — objek disiapkan lewat make
  prime_tool make
  for ((r = 1; r <= NRUN; r++)); do
    ( cd "$PROJ" && printf '\n/* edit floor-%s */\n' "$r" >>src/t1.c )
    run_once gcc-floor edit floor_run
  done
}

# ---- COMPDB: generate compile_commands.json ------------------------------
# Hasil divalidasi: file harus benar-benar ada & tidak kosong.
compdb_run() {
  case $1 in
    ninja) ninja -t compdb cc >compile_commands.json ;;
    bear)  bear -- make -j"$NPROC" ;;
    rbot)  "$RBOT" -g compdb ;;
    cmake) cmake -G Ninja -B build-cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ;;
    meson) meson setup --reconfigure build-meson --buildtype=release ;;
    xmake) xmake project -k compile_commands ;;
  esac
}

s_compdb() {
  have_scenario compdb || return 0
  local k r
  for k in "${CDB_TOOLS[@]}"; do
    # bear hanya merekam compile yang benar-benar jalan => butuh build bersih
    [ "$k" = bear ] || prime_tool "$k"
    [ "$k" = rbot ] && ( cd "$PROJ" && sed -i 's/^o.compileCommands = false/o.compileCommands = auto/' Buildfile )
    for ((r = 1; r <= NRUN; r++)); do
      ( cd "$PROJ" && rm -f compile_commands.json build-cmake/compile_commands.json build-meson/compile_commands.json )
      [ "$k" = bear ] && reset_all
      CHECK_COMPDB=1 run_once "$(label_of "$k")" compdb compdb_run "$k"
    done
    [ "$k" = rbot ] && ( cd "$PROJ" && sed -i 's/^o.compileCommands = auto/o.compileCommands = false/' Buildfile )
  done
}

# ---- STRACE: hitung syscall no-op ----------------------------------------
s_strace() {
  have_scenario strace || return 0
  [ "$HAVE_STRACE" = 1 ] || { echo "  (strace tidak ada — lewati)"; return 0; }
  local k n f lbl top err
  echo
  echo "${C_BLD}SYSCALL${C_RST} ${C_DIM}no-op, strace -c -f${C_RST}"
  echo 'tool,syscall,calls,errors' >"$SYSCSV"
  for k in "${TOOLS[@]}"; do
    prime_tool "$k"
    lbl=$(label_of "$k")
    CUR="strace $lbl"; draw_progress
    f="$OUTDIR/strace.$k.txt"
    tool_argv "$k"
    ( cd "$PROJ" && strace -c -f "${ARGV[@]}" >/dev/null 2>"$f" )
    n=$(awk '$NF == "total" {print $4}' "$f")
    echo "$lbl,TOTAL,${n:-0},0" >>"$SYSCSV"
    awk -v t="$lbl" '$1 ~ /^[0-9]+\.[0-9]+$/ && $NF != "total" && NF >= 5 {
      print t "," $NF "," $4 "," ((NF == 6) ? $5 : 0) }' "$f" >>"$SYSCSV"
    top=$(awk -F, -v t="$lbl" '$1 == t && $2 != "TOTAL"' "$SYSCSV" | sort -t, -k3,3nr | head -4 \
          | awk -F, '{printf "%s%s=%s", (NR > 1 ? " " : ""), $2, $3}')
    err=$(awk -F, -v t="$lbl" '$1 == t && $2 != "TOTAL" {s += $4} END {print s + 0}' "$SYSCSV")
    clear_line
    printf '  %-12s %10s syscall  %serr=%s%s\n' "$lbl" "${n:-?}" "$C_DIM" "$err" "$C_RST"
    printf '  %s    %s%s\n' "$C_DIM" "$(printf '%s' "$top" | cut -c1-$(( COLS - 8 )))" "$C_RST"
    STEP=$(( STEP + 1 )); draw_progress
  done
}

# ---------------------------- main -----------------------------------------
trap 'echo; echo "dibatalkan."; exit 130' INT

SKIPPED=""
[ "$HAVE_BEAR"  = 0 ] && SKIPPED+=" bear"
[ "$HAVE_CMAKE" = 0 ] && SKIPPED+=" cmake"
[ "$HAVE_MESON" = 0 ] && SKIPPED+=" meson"
[ "$HAVE_XMAKE" = 0 ] && SKIPPED+=" xmake"
[ "$HAVE_TUP"   = 0 ] && SKIPPED+=" tup"

echo "${C_BLD}${C_CYN}▌ rbot benchmark${C_RST} ${C_DIM}· ${#TOOLS[@]} tool · $NRUN run · median${C_RST}"
echo "  project : $NSRC source, $NHEAD header"
echo "  versi   :"
for k in "${TOOLS[@]}"; do
  printf '    %-12s %s\n' "$(label_of "$k")" "${VER[$(label_of "$k")]:-?}"
done
printf '    %-12s %s\n' gcc "$VER_GCC · $(uname -m)"
[ -n "$SKIPPED" ] && echo "  ${C_DIM}dilewati (tidak terpasang):$SKIPPED${C_RST}"
echo "  dir     : $PROJ"
echo

# total langkah untuk progress bar
TOTAL=0
for s in cold noop touch edit; do
  have_scenario "$s" && TOTAL=$(( TOTAL + ${#TOOLS[@]} * NRUN ))
done
have_scenario edit && TOTAL=$(( TOTAL + NRUN ))   # baris gcc-floor
have_scenario compdb && TOTAL=$(( TOTAL + ${#CDB_TOOLS[@]} * NRUN ))
if have_scenario strace && [ "$HAVE_STRACE" = 1 ]; then TOTAL=$(( TOTAL + ${#TOOLS[@]} )); fi
[ "$TOTAL" -gt 0 ] || TOTAL=1

CUR="gen"; draw_progress
gen_project
clear_line
T_START=$SECONDS
draw_progress

s_cold
s_noop
s_touch
s_edit
s_compdb
s_strace

clear_line
echo
echo "${C_GRN}✔ selesai${C_RST} dalam $(fmt_t $(( SECONDS - T_START )))"
report

echo
echo "hasil lengkap: $CSV"
[ -f "$SYSCSV" ] && echo "syscall      : $SYSCSV"
[ -s "$ERRLOG" ] && echo "stderr tool  : $ERRLOG"

rm -f "$MARK"
[ "$KEEP" = 1 ] || { rm -rf "$PROJ" "$OUTDIR"/strace.*.txt; }
exit 0

