#!/usr/bin/env bash
# ============================================================================
# compare.sh — benchmark build tool: ninja vs make vs bear+make vs satria
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
#   touch  : touch 1 header (konten sama) — satria: 0 recompile (content-based)
#   edit   : edit isi 1 source — tepat 1 recompile + relink
#            + baris "gcc-floor" (gcc -c 1 file + link langsung, tanpa build
#              tool) sebagai lantai; selisih tool vs lantai = overhead tool
#   compdb : generate compile_commands.json (bear: butuh build bersih)
#   strace : hitung syscall no-op (opsional: -s strace; jika tersedia)
#
# Metodologi (v2):
#   - Tiap tool diukur berurutan (tool-major) dan DIPRIME dulu: build bersih
#     + 1 run no-op yang tidak dihitung. Ini mencegah tool saling mengotori
#     state (.d, .ninja_log, objek) pada direktori build bersama.
#   - Tabel akhir memakai MEDIAN + min-max, bukan rerata.
#   - Kolom "obj" = jumlah .o yang benar-benar dikompilasi ulang (sanity check).
#   - results.csv memuat versi tiap tool (kolom "version"; satria = git/hash biner).
#   - syscalls.csv memuat rincian syscall per tool (tool,syscall,calls,errors).
#
# Pemakaian:
#   ./bench/compare.sh                # default: 150 source, 30 header, 5 run
#   ./bench/compare.sh -n 40 -H 10    # project kecil, cepat
#   ./bench/compare.sh -s edit,touch  # subset skenario
#   ./bench/compare.sh -s strace      # syscall profiling (opt-in)
#   NO_COLOR=1 ./bench/compare.sh     # tanpa warna / progress bar
#
# Catatan kejujuran: semua tool menghasilkan artefak yang setara, tapi
# fiturnya beda. Angka ini untuk melacak REGRESI satria — bukan klaim
# bahwa satu tool "lebih baik" secara umum.
# ============================================================================
set -u

[ -n "${EPOCHREALTIME:-}" ] || { echo "butuh bash >= 5 (EPOCHREALTIME)" >&2; exit 1; }

# ---------------------------- konfigurasi ----------------------------------
NSRC=150      # -n : jumlah source t1..tN.c
NHEAD=30      # -H : jumlah header berantai h1->hH
NRUN=5        # -r : pengulangan per skenario (ambil median)
SATRIA=bin/satria # -b : binary satria yang dipakai
AUDIT=1       # -A : disable untimed compile/link audit
KEEP=0        # -k : 1 = jangan hapus sys.tmp di akhir
ONLY=""       # -s : hanya skenario ini (comma-separated)
OUTDIR=""     # -o : direktori kerja benchmark (default sys.tmp di cwd)
NPROC=${NPROC:-$(nproc 2>/dev/null || echo 4)}
TARGETS=""

usage() { sed -n '2,/^# ====.*$/p' "$0"; exit 0; }
while getopts "n:H:r:b:t:ko:s:Ah" opt; do
  case $opt in
    n) NSRC=$OPTARG ;;
    H) NHEAD=$OPTARG ;;
    r) NRUN=$OPTARG ;;
    b) SATRIA=$OPTARG ;;
    t) TARGETS=$OPTARG ;;
    k) KEEP=1 ;;
    A) AUDIT=0 ;;
    o) OUTDIR=$OPTARG ;;
    s) ONLY=$OPTARG ;;
    h) usage ;;
    *) usage ;;
  esac
done

[ -x "$SATRIA" ] || { echo "satria tidak ditemukan/dieksekusi: $SATRIA" >&2; exit 1; }
SATRIA=$(cd "$(dirname "$SATRIA")" && pwd)/$(basename "$SATRIA")

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
TOOLS+=(satria)
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
    bear)  echo bear-make ;;   satria)  echo satria ;;
    cmake) echo cmake-ninja ;; meson) echo meson-ninja ;;
    xmake) echo xmake ;;       tup)   echo tup ;;
  esac
}

# ---------------------------- versi tool -----------------------------------
first_ver() { grep -oE '[0-9]+(\.[0-9]+)+' | head -1; }
satria_id() {
    local d c h v_bin
    d=$(dirname "$SATRIA")

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
            v_bin=$($SATRIA version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
            if [ -z "$v_bin" ]; then
                v_bin=$(strings "$SATRIA" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
            fi
            c="release ${v_bin:-unknown}"
        fi
    fi

    # Hash unik untuk file binary (tetap berguna untuk melacak build yang sama)
    h=$(sha1sum "$SATRIA" 2>/dev/null | cut -c1-6)
    
    echo "${c} bin:${h:-n/a}"
}
declare -A VER
VER_GCC=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion 2>/dev/null)
VER[ninja]=$(ninja --version 2>&1 | first_ver)
VER[make]=$(make --version 2>&1 | first_ver)
VER[satria]=$(satria_id)
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
AUDITCSV="$OUTDIR/work-audit.csv"
AUDITLOG="$OUTDIR/work-audit.log"
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

rule_line() {
  local w=$1 s
  printf -v s '%*s' "$w" ''
  printf '%s%s%s\n' "$C_DIM" "${s// /─}" "$C_RST"
}
box_width() {
  local w=$(( COLS > 72 ? 70 : COLS - 2 ))
  (( w < 40 )) && w=40
  printf '%s' "$w"
}
rules_header() {
  echo 'Legend :'
  echo '  ★    : Tercepat pada skenario ini'
  echo '  ×    : Rasio waktu terhadap yang tercepat'
  echo '  ✗    : Eksekusi gagal (exit non-zero)'
  echo
  echo 'Metrics :'
  echo '  obj   : Jumlah .o yang berubah (mtime)'
  echo '  ms    : Durasi eksekusi; laporan memakai median'
  echo
  echo 'Correctness (Validation Rules) :'
  echo '  cold  : build bersih; jumlah .o dicatat'
  echo '  noop  : build tanpa perubahan'
  echo '  touch : header di-touch tanpa mengubah konten'
  echo '  edit  : satu source diubah; gcc-floor diukur terpisah'
  echo '  compdb: compile_commands.json harus dihasilkan'
  echo '  Audit : compile/link diuji terpisah tanpa memengaruhi timed runs'
}
validation_report() {
  echo
  echo "${C_BLD}[Detailed Validation Log]${C_RST}"
  echo
  awk -F, -v TOOLS="${TOOLS[*]}" '
    NR == 1 { next }
    {
      key = $2 SUBSEP $1
      seen[key] = 1
      if ($4 != 0) failed[key]++
      else passed[key]++
    }
    END {
      nsc = split("cold noop touch edit compdb", SC, " ")
      nt = split(TOOLS, TL, " ")
      w = 12
      printf "%-*s | %-9s | %s\n", w, "tool", "execution", "status"
      for (si = 1; si <= nsc; si++) {
        for (ti = 1; ti <= nt; ti++) {
          t = TL[ti]
          name = t == "bear" ? "bear-make" : t == "cmake" ? "cmake-ninja" : t == "meson" ? "meson-ninja" : t
          k = SC[si] SUBSEP name
          if (!(k in seen)) continue
          printf "%-*s | %-9s | %s\n", w, name, SC[si], failed[k] ? "Failed" : "Passed (exit status)"
        }
      }
    }
  ' "$CSV"
}

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
# Rebuild Ninja graph from the real source tree. Cold measures this work.
# No precomputed source/object list is reused between cold runs.
gen_ninja_graph() {
  local src obj
  local -a sources=() objects=()
  # Discover actual files instead of using NSRC as a precomputed manifest.
  mapfile -d '' -t sources < <(cd "$PROJ" && find src -type f -name '*.c' -print0 | sort -z)
  [ "${#sources[@]}" -gt 0 ] || return 1
  {
    echo 'cflags = -O2 -std=gnu11 -Iinclude'
    echo
    echo 'rule cc'
    echo '  command = gcc $cflags -MMD -MP -MF $out.d -c $in -o $out'
    echo '  depfile = $out.d'
    echo '  deps = gcc'
    echo 'rule link'
    echo '  command = gcc $in -o $out'
    echo
    for src in "${sources[@]}"; do
      obj="build/${src#src/}"
      obj="${obj%.c}.o"
      objects+=("$obj")
      printf 'build %s: cc %s\n' "$obj" "$src"
    done
    printf 'build bin/app: link'
    printf ' %s' "${objects[@]}"
    printf '\n\ndefault bin/app\n'
  } >"$PROJ/build.ninja"
}

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
  cp "$PROJ/src/t1.c" "$PROJ/src/t1.c.baseline"

  gen_ninja_graph

  # ---- Makefile ----
  {
    echo "CC      := gcc"
    echo "CFLAGS  := -O2 -std=gnu11 -Iinclude"
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

  # ---- Buildfile (satria) ----
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
  ( cd "$PROJ" && "$SATRIA" init >/dev/null 2>&1 )

  # ---- CMakeLists.txt (untuk CMake + Ninja) ----
  {
    echo "cmake_minimum_required(VERSION 3.12)"
    echo "project(app C)"
    echo
    echo "set(CMAKE_C_STANDARD 11)"
    echo "set(CMAKE_C_EXTENSIONS ON)"
    echo "set(CMAKE_C_STANDARD_REQUIRED ON)"
    echo "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)"
    echo "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \${CMAKE_BINARY_DIR}/bin)"
    echo
    echo "file(GLOB SOURCES CONFIGURE_DEPENDS \"\${CMAKE_CURRENT_SOURCE_DIR}/src/*.c\")"
    echo "list(SORT SOURCES)"
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
    echo "CFLAGS = -O2 -std=gnu11 -Iinclude -MMD -MP -MF \$(@).d"
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
    ninja) ARGV=(ninja -j"$NPROC") ;;
    make)  ARGV=(make -j"$NPROC") ;;
    bear)  ARGV=(bear -- make -j"$NPROC") ;;
    satria)  ARGV=("$SATRIA" -j"$NPROC") ;;
    cmake) ARGV=(ninja -C build-cmake -j"$NPROC") ;;
    meson) ARGV=(ninja -C build-meson -j"$NPROC") ;;
    xmake) ARGV=(xmake -y -j"$NPROC") ;;
    tup)   ARGV=(tup) ;;
  esac
}
tool_cmd() { tool_argv "$1"; "${ARGV[@]}"; }

# Cold includes Ninja graph generation and CMake/Meson configure (also used for prime).
build_full() {
  case $1 in
    satria)  "$SATRIA" -j"$NPROC" ;;
    ninja)  gen_ninja_graph && ninja -j"$NPROC" ;;
    cmake) CC=gcc cmake -G Ninja -B build-cmake -DCMAKE_BUILD_TYPE= -DCMAKE_C_FLAGS="-O2 -std=gnu11" && ninja -C build-cmake -j"$NPROC" ;;
    meson) CC=gcc meson setup build-meson --buildtype=plain -Dc_args="-O2 -std=gnu11" && ninja -C build-meson -j"$NPROC" ;;
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
  # Check actual outputs, not merely process exit status.
  if [ "$rc" -eq 0 ] && [ -z "${CHECK_COMPDB:-}" ]; then
    local binary="$PROJ/bin/app"
    case "$label" in
      cmake-ninja) binary="$PROJ/build-cmake/bin/app" ;;
      meson-ninja) binary="$PROJ/build-meson/app" ;;
      xmake) binary="" ;; # xmake output location is tool-dependent
    esac
    if [ -n "$binary" ] && [ ! -x "$binary" ]; then rc=97; fi
    if [ "$rc" -eq 0 ]; then
      case "$scen" in
        cold) [ "$nobj" -eq "$((NSRC+1))" ] || rc=96 ;;
        noop) [ "$nobj" -eq 0 ] || rc=96 ;;
        edit) [ "$nobj" -eq 1 ] || rc=96 ;;
      esac
    fi
  fi
  if [ "$rc" -eq 0 ] && [ -n "${CHECK_COMPDB:-}" ]; then
    find "$PROJ" -name compile_commands.json -newer "$MARK" -size +2c 2>/dev/null \
      | grep -q . || rc=98
  fi
  clear_line
  if [ "$rc" -ne 0 ]; then
    local why="exit $rc"
    [ "$rc" -eq 98 ] && why="compile_commands.json tidak dihasilkan"
    [ "$rc" -eq 97 ] && why="binary tidak dihasilkan"
    [ "$rc" -eq 96 ] && why="jumlah objek tidak sesuai"
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
      desc["cold"]   = "build dari nol (Ninja graph gen, CMake/Meson configure included)"
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
          if (s == "touch" && RB[i] < 0.5 && L[i] != "satria") printf " %s⚠ header?%s", Y, Z
          if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z
          printf "\n"
        }
        if (hasfl) printf "  %s┄ %-12s %6d ms  lantai: gcc -c + link (+N = overhead)%s\n", D, "gcc-floor", FL, Z
        for (i = 1; i <= nf; i++) printf "  %s✗ %-12s semua run gagal%s\n", R, fl[i], Z
      }
    }' "$sorted"
  rm -f "$sorted"
  echo
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
  ( cd "$PROJ" && rm -rf build bin/app build-cmake build-meson .xmake .tup .satria .rbot .ninja_log .ninja_deps compile_commands.json )
  if [ "$HAVE_TUP" = 1 ]; then ( cd "$PROJ" && tup init >/dev/null 2>&1 || true ); fi
}

# Siapkan state bersih & konsisten untuk SATU tool: build penuh + 1 no-op
# (tidak dihitung). Dipanggil sebelum noop/touch/edit/compdb/strace.
prime_tool() {
  CUR="prime $(label_of "$1")"; draw_progress
  reset_all
  # Each tool starts from the exact same source content.
  [ ! -f "$PROJ/src/t1.c.baseline" ] || cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"
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
  gcc -O2 -std=gnu11 -Iinclude -MMD -MP -c src/t1.c -o build/t1.o && gcc build/*.o -o bin/app
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
    satria)  "$SATRIA" -g compdb ;;
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
    [ "$k" = satria ] && ( cd "$PROJ" && sed -i 's/^o.compileCommands = false/o.compileCommands = auto/' Buildfile )
    for ((r = 1; r <= NRUN; r++)); do
      ( cd "$PROJ" && rm -f compile_commands.json build-cmake/compile_commands.json build-meson/compile_commands.json )
      [ "$k" = bear ] && { reset_all; cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"; }
      CHECK_COMPDB=1 run_once "$(label_of "$k")" compdb compdb_run "$k"
    done
    [ "$k" = satria ] && ( cd "$PROJ" && sed -i 's/^o.compileCommands = auto/o.compileCommands = false/' Buildfile )
  done
}

# ---- STRACE: hitung syscall no-op ----------------------------------------
s_strace() {
  # strace hanya berjalan bila diminta eksplisit melalui -s strace.
  [ -n "$ONLY" ] || return 0
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

# -------------------- untimed actual-work audit -----------------------------
# GCC shim records the actual compiler driver invocations. The shim delegates
# to the real compiler; no wrapper is present during measured benchmark runs.
# Compare source coverage, compile/link counts, and important command options.
# Each tool gets an isolated clean project state. Non-GCC toolchains are marked
# unsupported rather than silently claiming equivalence.
audit_work() {
  [ "$AUDIT" = 1 ] || return 0
  local k scen label auditbin realgcc log rc summary
  realgcc=$(command -v gcc)
  auditbin="$OUTDIR/.audit-bin"
  mkdir -p "$auditbin"
  cat >"$auditbin/gcc" <<'SHIM'
#!/usr/bin/env bash
printf '%s\0' "$@" >>"$SATRIA_AUDIT_LOG"
printf '\n' >>"$SATRIA_AUDIT_LOG"
exec "$SATRIA_REAL_GCC" "$@"
SHIM
  chmod +x "$auditbin/gcc"
  : >"$AUDITCSV"
  echo 'tool,scenario,status,compiles,links,unique_sources,details' >"$AUDITCSV"
  : >"$AUDITLOG"
  echo
  echo "${C_BLD}[Actual Work Audit · untimed]${C_RST}"
  local audit_failed=0
  for k in "${TOOLS[@]}"; do
    label=$(label_of "$k")
    # Only tools whose compiler can be redirected to gcc are auditable here.
    case "$k" in
      ninja|make|bear|satria|cmake|meson) ;;
      *) echo "$label,cold,UNSUPPORTED,0,0,0,compiler interception not verified" >>"$AUDITCSV"; continue ;;
    esac
    for scen in cold noop edit; do
      reset_all
      cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"
      log="$OUTDIR/.audit-$k-$scen.argv"
      : >"$log"
      # Build outside the observed window for incremental scenarios.
      if [ "$scen" != cold ]; then
        ( cd "$PROJ" && PATH="$auditbin:$PATH" SATRIA_REAL_GCC="$realgcc" SATRIA_AUDIT_LOG="$log" build_full "$k" ) >>"$AUDITLOG" 2>&1 || {
          echo "$label,$scen,PRIME_FAILED,0,0,0,initial build failed" >>"$AUDITCSV"; continue;
        }
        : >"$log"
        if [ "$scen" = edit ]; then
          # A semantic source change, not a comment-only edit.
          sed -i -E 's/(return v[0-9]+ \+ )[0-9]+;/\199999;/' "$PROJ/src/t1.c"
        fi
      fi
      if [ "$scen" = cold ]; then
        ( cd "$PROJ" && PATH="$auditbin:$PATH" SATRIA_REAL_GCC="$realgcc" SATRIA_AUDIT_LOG="$log" build_full "$k" ) >>"$AUDITLOG" 2>&1
      else
        ( cd "$PROJ" && PATH="$auditbin:$PATH" SATRIA_REAL_GCC="$realgcc" SATRIA_AUDIT_LOG="$log" tool_cmd "$k" ) >>"$AUDITLOG" 2>&1
      fi
      rc=$?
      python3 - "$log" "$AUDITCSV" "$label" "$scen" "$rc" "$NSRC" <<'PY_AUDIT'
import csv, os, sys
path, out, tool, scenario, rc, nsrc = sys.argv[1:]
nsrc = int(nsrc)
raw = open(path, 'rb').read().split(b'\n')
cmds = [[v.decode(errors='replace') for v in line.split(b'\0') if v]
        for line in raw if line]

def output_of(cmd):
    try:
        return cmd[cmd.index('-o') + 1]
    except (ValueError, IndexError):
        return ''

def source_of(cmd):
    return [x for x in cmd if x.endswith('.c') and
            (x.startswith('src/') or '/src/' in x)]

def includes_project(cmd):
    paths = []
    for i, arg in enumerate(cmd):
        if arg == '-I' and i + 1 < len(cmd):
            paths.append(cmd[i + 1])
        elif arg.startswith('-I') and len(arg) > 2:
            paths.append(arg[2:])
        elif arg == '-isystem' and i + 1 < len(cmd):
            paths.append(cmd[i + 1])
    return any(os.path.normpath(p) == 'include' or
               os.path.normpath(p).endswith('/include') for p in paths)

compiles, links, badflags = [], [], []
for cmd in cmds:
    output = output_of(cmd)
    sources = source_of(cmd)
    if '-c' in cmd and sources:
        compiles.extend(sources)
        if '-O2' not in cmd or '-std=gnu11' not in cmd:
            badflags.append('compile flags')
        if not includes_project(cmd):
            badflags.append('include path')
    elif '-c' not in cmd and output and os.path.basename(output) == 'app':
        # Only the project executable counts. CMake's compiler checks
        # (cmTC_*, CMakeFiles/CMakeScratch) are configure work, not app links.
        links.append(cmd)

expected = nsrc + 1 if scenario == 'cold' else (1 if scenario == 'edit' else 0)
expected_links = 0 if scenario == 'noop' else 1
srcset = {os.path.basename(x) for x in compiles}
if scenario == 'cold':
    expected_sources = {'main.c'} | {f't{i}.c' for i in range(1, nsrc + 1)}
elif scenario == 'edit':
    expected_sources = {'t1.c'}
else:
    expected_sources = set()
issues = []
if int(rc):
    issues.append('build exit ' + rc)
if len(compiles) != expected:
    issues.append(f'compile {len(compiles)} != {expected}')
if len(links) != expected_links:
    issues.append(f'link {len(links)} != {expected_links}')
if srcset != expected_sources:
    issues.append('source coverage mismatch')
if badflags:
    issues.append('flags: ' + '/'.join(sorted(set(badflags))))
if any(len([x for x in cmd if x.endswith('.o')]) != nsrc + 1 for cmd in links):
    issues.append('link object inputs mismatch')
status = 'PASS' if not issues else 'FAIL'
with open(out, 'a', newline='') as f:
    csv.writer(f).writerow([tool, scenario, status, len(compiles),
                            len(links), len(srcset), '; '.join(issues)])
PY_AUDIT
      summary=$(tail -n 1 "$AUDITCSV")
      case "$summary" in *",PASS,"*) printf '  ✓ %-12s %-5s compile/link validated\n' "$label" "$scen" ;; *) audit_failed=1; printf '  ✗ %-12s %-5s %s\n' "$label" "$scen" "$(printf '%s' "$summary" | cut -d, -f7-)" ;; esac
    done
  done
  reset_all
  cp "$PROJ/src/t1.c.baseline" "$PROJ/src/t1.c"
  rm -rf "$auditbin" "$OUTDIR"/.audit-*.argv
  if [ "$audit_failed" -ne 0 ]; then
    echo "  ${C_RED}Audit gagal; benchmark tidak dijalankan. Periksa $AUDITCSV dan $AUDITLOG${C_RST}"
    return 1
  fi
  echo "  ${C_GRN}Audit lulus; benchmark dimulai.${C_RST}"
}

# ---------------------------- main -----------------------------------------
trap 'echo; echo "dibatalkan."; exit 130' INT

SKIPPED=""
[ "$HAVE_BEAR"  = 0 ] && SKIPPED+=" bear"
[ "$HAVE_CMAKE" = 0 ] && SKIPPED+=" cmake"
[ "$HAVE_MESON" = 0 ] && SKIPPED+=" meson"
[ "$HAVE_XMAKE" = 0 ] && SKIPPED+=" xmake"
[ "$HAVE_TUP"   = 0 ] && SKIPPED+=" tup"

echo "${C_BLD}${C_CYN}▌ satria benchmark${C_RST} ${C_DIM}· ${#TOOLS[@]} tool · $NRUN run · median${C_RST}"
rule_line "$(box_width)"
echo "Project   : $NSRC source · $NHEAD header"
echo "Workers   : $NPROC (all tools with configurable -j)"
echo "Compiler  : gcc · -O2 · gnu11 (generator-dependent extra flags may remain)"
echo "Caveat    : cold includes Ninja graph generation and CMake/Meson configure; Bear compiles for compdb"
echo "Directory : $PROJ"
echo
echo 'Versions  :'
for k in "${TOOLS[@]}"; do
  printf '  %-12s : %s\n' "$(label_of "$k")" "${VER[$(label_of "$k")]:-?}"
done
printf '  %-12s : %s\n' gcc "$VER_GCC · $(uname -m)"
[ -n "$SKIPPED" ] && echo "${C_DIM}Skipped   :$SKIPPED${C_RST}"
echo
rules_header
echo
# total langkah untuk progress bar
TOTAL=0
for s in cold noop touch edit; do
  have_scenario "$s" && TOTAL=$(( TOTAL + ${#TOOLS[@]} * NRUN ))
done
have_scenario edit && TOTAL=$(( TOTAL + NRUN ))   # baris gcc-floor
have_scenario compdb && TOTAL=$(( TOTAL + ${#CDB_TOOLS[@]} * NRUN ))
if [ -n "$ONLY" ] && have_scenario strace && [ "$HAVE_STRACE" = 1 ]; then TOTAL=$(( TOTAL + ${#TOOLS[@]} )); fi
[ "$TOTAL" -gt 0 ] || TOTAL=1

gen_project
if ! audit_work; then
  exit 1
fi
echo
echo "${C_BLD}[Execution Stream]${C_RST}"
T_START=$SECONDS
CUR="gen"; draw_progress

s_cold
s_noop
s_touch
s_edit
s_compdb
s_strace

clear_line
echo
echo "${C_GRN}✔ selesai${C_RST} dalam $(fmt_t $(( SECONDS - T_START )))"
validation_report
echo
echo "${C_BLD}BENCHMARK REPORT (Median)${C_RST}"
rule_line "$(box_width)"
report
echo
echo "${C_BLD}Artifacts :${C_RST}"
echo "  hasil lengkap : $CSV"
[ -f "$AUDITCSV" ] && echo "  work audit    : $AUDITCSV"
[ -s "$AUDITLOG" ] && echo "  audit log     : $AUDITLOG"
[ -f "$SYSCSV" ] && echo "  syscall detail: $SYSCSV"
[ -s "$ERRLOG" ] && echo "  stderr tool   : $ERRLOG"

rm -f "$MARK"
[ "$KEEP" = 1 ] || { rm -rf "$PROJ" "$OUTDIR"/strace.*.txt; }
exit 0

