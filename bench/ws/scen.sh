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
mut_touch() { (cd "$WS" && touch library/a.h); }
mut_libtouch() { (cd "$WS" && touch library/ext.h); }
# Edit isi header internal (library/ext.h), diakses include relatif, bukan -I.
mut_libhdr() {
  local f="$WS/library/ext.h" v
  v=$(sed -n 's/^#define WS_EXT_VALUE \([0-9][0-9]*\)$/\1/p' "$f")
  sed -i "s/^#define WS_EXT_VALUE [0-9][0-9]*$/#define WS_EXT_VALUE $((v + 1))/" "$f"
}
# Edit ISI header pusat: konten berbeda (bukan sekadar mtime). Nilai tetap
# koheren karena kedua main membandingkan WS_SHARED_VALUE hasil kompilasi
# sendiri — build parsial yang lolos akan gagal runtime.
mut_hedit() {
  local f="$WS/library/a.h" v
  v=$(sed -n 's/^#define WS_SHARED_VALUE \([0-9][0-9]*\)$/\1/p' "$f")
  sed -i "s/^#define WS_SHARED_VALUE [0-9][0-9]*$/#define WS_SHARED_VALUE $((v + 1))/" "$f"
}

s_cold() {
  have_scenario cold || return 0
  local k r
  for k in "${TOOLS[@]}"; do
    for ((r = 1; r <= NRUN; r++)); do
      gen_ws # true cold, including .rbot cache/state
      run_once "$k" cold build_full "$k"
    done
  done
}

s_strace() {
  have_scenario strace || return 0
  [ "$HAVE_STRACE" != 1 ] && {
    clear_line
    printf '  %s! strace dilewati%s%s\n' "$C_YEL" "${STRACE_SKIP:+ ($STRACE_SKIP)}" "$C_RST"
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
    if ! prime_cached "$k"; then
      record_fail "$k" "$1"
      continue
    fi
    for ((r = 1; r <= NRUN; r++)); do
      [ "$MUT_DELAY" != 0 ] && sleep "$MUT_DELAY"
      "$2" "$r" "$k"
      run_once "$k" "$1" "${3:-tool_cmd}" "$k"
    done
  done
}
mut_none() { :; }

