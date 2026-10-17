# Tampilan terminal: warna, bar, progres. Dimuat dari ws.sh.
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
tool_display() { case "$1" in cmake|cninja) printf cninja ;; *) printf "%s" "$1" ;; esac; }
draw_progress() {
  [ "${AUDIT_QUIET_PROGRESS:-0}" = 1 ] && return 0
  [ "$TTY" = 1 ] || return 0
  local el=$((SECONDS - T_START)) eta=0 pct
  pct=$((STEP * 100 / TOTAL))
  ((STEP > 0)) && eta=$((el * (TOTAL - STEP) / STEP))
  printf '\r\033[K  %s%s%s %3d%% %s%d/%d ETA %s%s' \
    "$C_CYN" "$(bar "$STEP" "$TOTAL" "$PBW")" "$C_RST" "$pct" \
    "$C_DIM" "$STEP" "$TOTAL" "$(fmt_t "$eta")" "$C_RST"
  if ((COLS >= 90)); then printf '  %s▸ %s%s' "$C_YEL" "$CUR" "$C_RST"; fi
}

