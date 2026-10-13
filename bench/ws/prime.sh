# Prime tool + gate correctness + skenario release.
prime_cached() { # prime_cached <tool>: prime sekali per tool, lalu cp -a
  local k=$1 snap="$OUTDIR/.prime/$1"
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
    *" rbot "*) : ;;
    *)
      printf '  %s! rel: rbot tidak dipilih; dilewati%s\n' "$C_YEL" "$C_RST"
      return 0
      ;;
  esac
  local r
  if ! prime_cached rbot; then
    record_fail rbot rel
    return 0
  fi
  for ((r = 1; r <= NRUN; r++)); do
    run_once rbot rel rel_cmd "0.0.$r"
  done
}

reset_all() {
  (cd "$WS" && rm -rf core/build core/bin bade/build bade/bin \
    rupamod/build rupamod/modules modules .rbot core/lib bade/lib .ninja_log .ninja_deps)
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
  [ -s "$archive" ] || { echo "  ✗ $1: archive tidak ditemukan: $archive" >&2; return 1; }
  tar -tf "$archive" > "$list" 2> /dev/null
  grep -Eq '(^|/)m1[.]rp$' "$list" || { echo "  ✗ $1: archive tidak berisi m1.rp" >&2; return 1; }
  for f in "$WS/core/lib/libcore.so" "$WS/bade/lib/libbade.so"; do
    [ -s "$f" ] || { echo "  ✗ $1: shared library hilang: $f" >&2; return 1; }
  done
  # Pastikan resource benar-benar berada dalam binary, bukan sekadar file .o.
  nm "$WS/bade/bin/bade" > "$OUTDIR/.nm.list" 2> /dev/null
  grep -Eq '_binary_.*rupa_modules_tar_start' "$OUTDIR/.nm.list" || {
    echo "  ✗ $1: embedded archive tidak ter-link ke binary bade" >&2
    return 1
  }
  # Keberhasilan link saja tidak membuktikan circular symbol teratasi.
  if ! "$WS/core/bin/rupa" || ! "$WS/bade/bin/bade"; then
    printf '  ✗ %s: runtime circular dependency gagal\n' "$1" >&2
    return 1
  fi
  # Uji negatif memastikan graph bukan circular semu. Tanpa linker group,
  # setidaknya satu dari dua urutan library wajib gagal.
  # LLD (Termux) menyelesaikan archive sirkular tanpa group: uji ini tidak
  # bermakna di sana (linker-dependent). Graf tetap dibuktikan oleh runtime
  # check di atas; build ninja memakai --start-group seperti rbot.
  if ld --version 2>/dev/null | grep -qi lld; then
    echo "  i $1: uji negatif circular dilewati (linker LLD)" >&2
    return 0
  fi
  local no_group_core=0 no_group_bade=0
  (cd "$WS" && gcc core/build/main.o core/build/libcore.a bade/build/libbade.a -o "$OUTDIR/.nogroup-core") >/dev/null 2>&1 || no_group_core=1
  (cd "$WS" && gcc bade/build/main.o bade/build/libbade.a core/build/libcore.a -o "$OUTDIR/.nogroup-bade") >/dev/null 2>&1 || no_group_bade=1
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

