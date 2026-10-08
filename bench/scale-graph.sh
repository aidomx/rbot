#!/usr/bin/env bash
# scale-graph.sh — graph/invalidation scaling benchmark.
#
# Each workspace is built once (prime, excluded from timing), then rbot is
# invoked repeatedly with an unchanged workspace. The measured path is the
# graph/build-state inspection needed to prove that nothing must be rebuilt.
# No source object is intentionally recompiled during measured runs.
#
# Usage:
#   ./bench/scale-graph.sh
#   ./bench/scale-graph.sh -n 100,500,1000,5000 -e 100,1000 -r 5
#   ./bench/scale-graph.sh -n 10000 -e 10000 -r 3 -b ./bin/rbot
#
set -u

RBOT="bin/rbot"
NODES="100,500,1000,5000"
EDGES="100,1000"
RUNS=5
OUTDIR="graph-scale.tmp"
KEEP=0

usage() { sed -n '2,/^# ====.*$/p' "$0"; exit 0; }
while getopts "b:n:e:r:o:kh" opt; do
  case "$opt" in
    b) RBOT=$OPTARG ;; n) NODES=$OPTARG ;; e) EDGES=$OPTARG ;; r) RUNS=$OPTARG ;; o) OUTDIR=$OPTARG ;; k) KEEP=1 ;; h) usage ;; *) usage ;;
  esac
done
[[ "$RUNS" =~ ^[0-9]+$ && "$RUNS" -gt 0 ]] || { echo "runs harus > 0" >&2; exit 2; }
[ -x "$RBOT" ] || { echo "rbot tidak ditemukan/dieksekusi: $RBOT" >&2; exit 1; }
RBOT=$(cd "$(dirname "$RBOT")" && pwd)/$(basename "$RBOT")
GRAPH="$PWD/bench/graph.sh"
[ -x "$GRAPH" ] || { echo "bench/graph.sh tidak ditemukan" >&2; exit 1; }
mkdir -p "$OUTDIR"; OUTDIR=$(cd "$OUTDIR" && pwd)
CSV="$OUTDIR/results.csv"
printf 'nodes,edges,headers,run,ms,rc,objects,graph_edges,rebuilt\n' > "$CSV"
IFS=, read -r -a NS <<< "$NODES"; IFS=, read -r -a ES <<< "$EDGES"
now_ns() { if [ -n "${EPOCHREALTIME:-}" ]; then printf '%d\n' "${EPOCHREALTIME/./}000"; else date +%s%N; fi; }
median() { printf '%s\n' "$@" | sort -n | awk 'NF{a[NR]=$1} END{if(NR%2)print a[(NR+1)/2];else print (a[NR/2]+a[NR/2+1])/2}'; }

for n in "${NS[@]}"; do for e in "${ES[@]}"; do
  ws="$OUTDIR/n${n}-e${e}"
  echo; echo "========================================"; echo "  graph nodes=$n edges=$e"; echo "========================================"
  "$GRAPH" -n "$n" -e "$e" -o "$ws" >/dev/null || exit $?
  ( cd "$ws"; rm -rf .rbot build bin; "$RBOT" >/dev/null ) || { echo "prime gagal: n=$n e=$e" >&2; exit 1; }
  H=$(( (e + n - 1) / n )); (( H > 0 )) || H=1
  graph_edges=$(wc -l < "$ws/graph.tsv")
  [ "$graph_edges" -eq "$e" ] || { echo "edge count mismatch: $graph_edges != $e" >&2; exit 1; }
  before=$(find "$ws/build" -type f -name '*.o' 2>/dev/null | wc -l)
  times=()
  for ((run=1; run<=RUNS; run++)); do
    start=$(now_ns); rc=0
    ( cd "$ws"; "$RBOT" >/dev/null ) || rc=$?
    end=$(now_ns); ms=$(( (end-start)/1000000 ))
    after=$(find "$ws/build" -type f -name '*.o' 2>/dev/null | wc -l)
    rebuilt=0; (( after != before )) && rebuilt=1
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$n" "$e" "$H" "$run" "$ms" "$rc" "$after" "$graph_edges" "$rebuilt" >> "$CSV"
    times+=("$ms"); printf '  run=%d  %d ms  obj=%d  rebuilt=%d  rc=%d\n' "$run" "$ms" "$after" "$rebuilt" "$rc"
    [ "$rc" -eq 0 ] || exit "$rc"
  done
  med=$(median "${times[@]}"); printf '  median=%s ms  nodes=%s edges=%s\n' "$med" "$n" "$e"
done; done

echo; echo "========================================"; echo "  RESULT"; echo "========================================"; cat "$CSV"
if (( KEEP == 0 )); then find "$OUTDIR" -mindepth 1 -maxdepth 1 -type d -name 'n*-e*' -exec rm -rf {} +; fi
