# Prime tool + gate correctness + skenario release.
prime_cached() { # prime_cached <tool>: prime sekali per tool, lalu cp -a
  local k=$1 snap="$OUTDIR/.prime/$1"
  select_ws "$k" || return 1
  [ -n "${PRIME_FAIL[$k]:-}" ] && return 1
  if [ -n "${PRIME_OK[$k]:-}" ]; then
    rm -rf "$WS"
    cp -a "$snap" "$WS" || return 1
    return 0
  fi
  if prime_tool "$k"; then
    mkdir -p "$OUTDIR/.prime"
    rm -rf "$snap"
    cp -a "$WS" "$snap" || return 1
    PRIME_OK[$k]=1
    return 0
  fi
  PRIME_FAIL[$k]=1
  return 1
}

s_release() {
  have_scenario rel || return 0
  case " ${TOOLS[*]} " in
    *" satria "*) : ;;
    *)
      printf '  %s! rel: satria tidak dipilih; dilewati%s\n' "$C_YEL" "$C_RST"
      return 0
      ;;
  esac
  local r
  if ! prime_cached satria; then
    record_fail satria rel
    return 0
  fi
  for ((r = 1; r <= NRUN; r++)); do
    run_once satria rel rel_cmd "0.0.$r"
  done
}

reset_all() {
  # root build/bin/lib/dist = output root-centric satria (design/
  # workspace_v1.md); sisanya layout nested CMake + state satria.
  (cd "$WS" && rm -rf core/build core/bin bade/build bade/bin \
    rupamod/build rupamod/modules modules .satria core/lib bade/lib .ninja_log .ninja_deps \
    build bin lib dist .cmake-build)
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
  select_ws "$1" || return 1
  CUR="prime $1"
  draw_progress
  gen_ws # isolated workspace per tool, including cache and state
  if ! (cd "$WS" && build_full "$1" && tool_cmd "$1") > /dev/null 2>> "$ERRLOG"; then
    clear_line
    printf '  %s! prime gagal: %s (lihat %s)%s\n' "$C_YEL" "$1" "$ERRLOG" "$C_RST"
    tail -n 3 "$ERRLOG" 2> /dev/null | sed 's/^/      /'
    return 1
  fi
  check_artifacts "$1"
  prime_gate "$1"
}

# Gate correctness: artefak nyata, runtime circular berjalan, dan uji negatif
# membuktikan graph memang circular. Hindari pola `cmd | grep -q`: di bawah
# pipefail, grep yang selesai lebih dulu membuat tar/nm kena SIGPIPE dan gate
# gagal palsu secara acak (terukur 73 dari 300 percobaan). Output ditampung
# ke file dulu.
prime_gate() {
  local archive="$WS/modules/rupa_modules.tar" f list="$OUTDIR/.gate.list"
  # Path keluaran per tool: satria root-centric (bin/, lib/, build/<proyek>/
  # di root workspace — design/workspace_v1.md); CMake layout nested.
  local rupa_bin bade_bin core_so bade_so core_main bade_main core_a bade_a
  case $1 in
    satria)
      rupa_bin="bin/rupa"; bade_bin="bin/bade"
      core_so="lib/libcore.so"; bade_so="lib/libbade.so"
      core_main="build/core/main.o"; bade_main="build/bade/main.o"; core_a="lib/libcore.a"; bade_a="lib/libbade.a" ;;
    cmake)
      rupa_bin="core/bin/rupa"; bade_bin="bade/bin/bade"
      core_so="core/lib/libcore.so"; bade_so="bade/lib/libbade.so"
      core_main=".cmake-build/CMakeFiles/rupa.dir/core/src/main.c.o"; bade_main=".cmake-build/CMakeFiles/bade.dir/bade/src/main.c.o"
      core_a="core/build/libcore.a"; bade_a="bade/build/libbade.a" ;;
  esac
  [ -s "$archive" ] || { echo "  ✗ $1: archive tidak ditemukan: $archive" >&2; return 1; }
  tar -tf "$archive" > "$list" 2> /dev/null
  grep -Eq '(^|/)m1[.]rp$' "$list" || { echo "  ✗ $1: archive tidak berisi m1.rp" >&2; return 1; }
  for f in "$WS/$core_so" "$WS/$bade_so"; do
    [ -s "$f" ] || { echo "  ✗ $1: shared library hilang: $f" >&2; return 1; }
  done
  # Pastikan resource benar-benar berada dalam binary, bukan sekadar file .o.
  nm "$WS/$bade_bin" > "$OUTDIR/.nm.list" 2> /dev/null
  grep -Eq '_binary_.*rupa_modules_tar_start' "$OUTDIR/.nm.list" || {
    echo "  ✗ $1: embedded archive tidak ter-link ke binary bade" >&2
    return 1
  }
  # Keberhasilan link saja tidak membuktikan circular symbol teratasi.
  if ! "$WS/$rupa_bin" || ! "$WS/$bade_bin"; then
    printf '  ✗ %s: runtime circular dependency gagal\n' "$1" >&2
    return 1
  fi
  # Uji negatif memastikan graph bukan circular semu. Tanpa linker group,
  # setidaknya satu dari dua urutan library wajib gagal.
  # LLD (Termux) menyelesaikan archive sirkular tanpa group: uji ini tidak
  # bermakna di sana (linker-dependent). Graf tetap dibuktikan oleh runtime
  # check di atas; build ninja memakai --start-group seperti satria.
  if ld --version 2>/dev/null | grep -qi lld; then
    echo "  i $1: uji negatif circular dilewati (linker LLD)" >&2
    return 0
  fi
  local no_group_core=0 no_group_bade=0
  (cd "$WS" && gcc "$core_main" "$core_a" "$bade_a" -o "$OUTDIR/.nogroup-core") >/dev/null 2>&1 || no_group_core=1
  (cd "$WS" && gcc "$WS/$bade_main" "$bade_a" "$core_a" -o "$OUTDIR/.nogroup-bade") >/dev/null 2>&1 || no_group_bade=1
  if (( no_group_core == 0 && no_group_bade == 0 )); then
    echo "  ✗ $1: circular semu (kedua link tanpa group berhasil)" >&2
    return 1
  fi
  return 0
}

# Prime/gate gagal dicatat sebagai baris gagal (rc 95), TIDAK dilewati diam-diam.
record_fail() { # record_fail <tool> <scen>
  local k=$1 s=$2 ver=${VER[$1]:-}
  ver=${ver//,/ }
  expect_check "$k" "$s" 0 0 0 0 0
  MISMATCH_RUNS=$((MISMATCH_RUNS + 1))
  printf '  %s✗ %-6s %-6s GAGAL (prime/gate, lihat %s)%s\n' "$C_RED" "$k" "$s" "$ERRLOG" "$C_RST"
  printf '%s,%s,-1,95,0,0,0,0,0,0,%s\n' "$k" "$s" "$ver" >> "$CSV"
  printf '%s,%s,95,-1,0,0,0,0,0,%s,%s,%s,%s,%s,1\n' \
    "$k" "$s" "$EXP_OBJ" "$EXP_ARC" "$EXP_LIB" "$EXP_BIN" "$EXP_CC" >> "$VIEW_CSV"
}

