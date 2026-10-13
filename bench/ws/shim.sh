# Shim gcc/ld: mencatat compile/link nyata per run (bukti kerja).
# ------------------------ shim gcc/ld (bukti kerja) ------------------------
# rbot/ninja/make semuanya memanggil gcc/ld lewat PATH (posix_spawnp/direct),
# jadi shim ini mencatat setiap kompilasi (C), link binary (L), link shared
# (S), embed biner (E), dan link ld lain (D) tanpa mengubah hasil build.
# Log per run dipakai untuk membuktikan compile/relink yang benar-benar
# terjadi dan dependensi mana yang terinvalidasi (audit problems/ws.md butir 4).
REAL_GCC=$(command -v gcc)
REAL_LD=$(command -v ld 2> /dev/null || echo /usr/bin/ld)
WRAPDIR="$OUTDIR/.wrap"
mkdir -p "$WRAPDIR"
cat > "$WRAPDIR/gcc" <<'WRAP_EOF'
#!/usr/bin/env bash
log=${RB_CC_LOG:-/dev/null}
out="" is_cc=0 is_shared=0 next=0
for a in "$@"; do
  if [ "$next" = 1 ]; then out=$a; next=0; continue; fi
  case $a in
    -c) is_cc=1 ;;
    -shared) is_shared=1 ;;
    -o) next=1 ;;
  esac
done
if [ -n "$out" ]; then
  if [ "$is_cc" = 1 ]; then printf 'C|%s\n' "$out" >> "$log"
  elif [ "$is_shared" = 1 ]; then printf 'S|%s\n' "$out" >> "$log"
  else printf 'L|%s\n' "$out" >> "$log"
  fi
fi
exec "$RB_REAL_GCC" "$@"
WRAP_EOF
cat > "$WRAPDIR/ld" <<'WRAP_EOF'
#!/usr/bin/env bash
log=${RB_CC_LOG:-/dev/null}
out="" kind=D next=0 is_bin=0
for a in "$@"; do
  if [ "$next" = 1 ]; then out=$a; next=0; continue; fi
  case $a in
    -o) next=1 ;;
    -b) is_bin=1 ;;
  esac
done
if [ -n "$out" ]; then
  [ "$is_bin" = 1 ] && kind=E
  printf '%s|%s\n' "$kind" "$out" >> "$log"
fi
exec "$RB_REAL_LD" "$@"
WRAP_EOF
chmod +x "$WRAPDIR/gcc" "$WRAPDIR/ld"
export RB_REAL_GCC="$REAL_GCC" RB_REAL_LD="$REAL_LD"
export RB_CC_LOG="$OUTDIR/.cc.log"
export PATH="$WRAPDIR:$PATH"
WORK_LOG="$OUTDIR/work.log"
rm -f "$WORK_LOG"

