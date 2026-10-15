# Mutator dan runner skenario.
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
mut_touch() { (cd "$WS" && touch include/a.h); }
mut_libtouch() { (cd "$WS" && touch include/ext.h); }
# Edit isi header internal (include/ext.h), diakses include relatif, bukan -I.
mut_libhdr() {
  local f="$WS/include/ext.h" v
  v=$(sed -n 's/^#define WS_EXT_VALUE \([0-9][0-9]*\)$/\1/p' "$f")
  sed -i "s/^#define WS_EXT_VALUE [0-9][0-9]*$/#define WS_EXT_VALUE $((v + 1))/" "$f"
}
# Edit ISI header pusat: konten berbeda (bukan sekadar mtime). Nilai tetap
# koheren karena kedua main membandingkan WS_SHARED_VALUE hasil kompilasi
# sendiri — build parsial yang lolos akan gagal runtime.
mut_hedit() {
  local f="$WS/include/a.h" v
  v=$(sed -n 's/^#define WS_SHARED_VALUE \([0-9][0-9]*\)$/\1/p' "$f")
  sed -i "s/^#define WS_SHARED_VALUE [0-9][0-9]*$/#define WS_SHARED_VALUE $((v + 1))/" "$f"
}

# Urutan ABBA per repetisi (untuk 2 tool): A B B A A B ...
# Setiap run cold memanggil gen_ws sehingga output/state dibersihkan.
scenario_order() {
  local r=$1
  if [ "$RUN_ORDER" = abba ] && [ "${#TOOLS[@]}" -eq 2 ] && (( r % 2 == 1 )); then
    printf '%s\n' "${TOOLS[1]}" "${TOOLS[0]}"
  else
    printf '%s\n' "${TOOLS[@]}"
  fi
}
wait_run() { [ "$RUN_DELAY" = 0 ] || sleep "$RUN_DELAY"; }

s_cold() {
  have_scenario cold || return 0
  local k r
  if [ "$RUN_ORDER" = grouped ]; then
    for k in "${TOOLS[@]}"; do
      for ((r = 1; r <= NRUN; r++)); do
        gen_ws
        wait_run
        run_once "$k" cold build_full "$k"
      done
    done
  else
    for ((r = 0; r < NRUN; r++)); do
      while IFS= read -r k; do
        gen_ws
        wait_run
        run_once "$k" cold build_full "$k"
      done < <(scenario_order "$r")
    done
  fi
}

s_strace() {
  have_scenario strace || return 0
  # Catatan dilewati dicetak sekali di footer Success Rate
  # (design/bench_ws_output.md) — tanpa baris terpisah di sini.
  [ "$HAVE_STRACE" != 1 ] && return 0
  local k r report total_sec total_calls total_err rank
  local pct sec usecs calls errs syscall
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= 1; r++)); do
      reset_all
      CUR="strace/$k#$r"
      draw_progress
      report="$OUTDIR/workspace.strace.${k}.${r}.txt"
      rm -f "$report"
      if [ "$k" = satria ]; then
        tcmd=("$SATRIA" $SATRIA_ARGS -j"$NPROC" -w)
      elif [ "$k" = ninja ]; then
        tcmd=(ninja -j"$NPROC")
      elif [ "$k" = cmake ]; then
        tcmd=(cmake --build .cmake-build --parallel "$NPROC")
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
  local k r snap
  if [ "$RUN_ORDER" = grouped ]; then
    for k in "${TOOLS[@]}"; do
      if ! prime_cached "$k"; then record_fail "$k" "$1"; continue; fi
      for ((r = 1; r <= NRUN; r++)); do
        [ "$MUT_DELAY" = 0 ] || sleep "$MUT_DELAY"
        "$2" "$r" "$k"
        wait_run
        run_once "$k" "$1" "${3:-tool_cmd}" "$k"
      done
    done
  else
    # Simpan workspace masing-masing tool antar giliran. Salinan dilakukan
    # DI LUAR pengukuran; tidak menghapus state incremental yang sedang diuji.
    for ((r = 0; r < NRUN; r++)); do
      while IFS= read -r k; do
        snap="$OUTDIR/.order-state/$1/$k"
        if [ "$r" -eq 0 ]; then
          if ! prime_cached "$k"; then record_fail "$k" "$1"; continue; fi
        else
          [ -d "$snap" ] || continue
          rm -rf "$WS"
          cp -a "$snap" "$WS" || return 1
        fi
        [ "$MUT_DELAY" = 0 ] || sleep "$MUT_DELAY"
        "$2" "$((r + 1))" "$k"
        wait_run
        run_once "$k" "$1" "${3:-tool_cmd}" "$k"
        mkdir -p "$(dirname "$snap")"
        rm -rf "$snap"
        cp -a "$WS" "$snap" || return 1
      done < <(scenario_order "$r")
    done
    rm -rf "$OUTDIR/.order-state/$1"
  fi
}
mut_none() { :; }
