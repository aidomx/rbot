#!/usr/bin/env bash
# scale-build.sh — full-build scaling benchmark for the synthetic graph from graph.sh.
#
# It generates each workspace, removes previous build artifacts, then measures
# full rbot builds. Each run therefore measures compilation + graph resolution.
#
# Usage:
#   ./bench/scale-build.sh
#   ./bench/scale-build.sh -n 100,500,1000,5000 -e 1,5 -r 3
#   ./bench/scale-build.sh -n 1000,5000 -e 1000,5000 -r 5 -b ./bin/rbot
#
set -u

RBOT="bin/rbot"
NODES="100,500,1000,5000"
EDGES="1,5"
RUNS=5
OUTDIR="scale.tmp"
KEEP=0

usage() {
  sed -n '2,/^# ====.*$/p' "$0"
  exit 0
}

while getopts "b:n:e:r:o:kh" opt; do
  case "$opt" in
    b) RBOT=$OPTARG ;;
    n) NODES=$OPTARG ;;
    e) EDGES=$OPTARG ;;
    r) RUNS=$OPTARG ;;
    o) OUTDIR=$OPTARG ;;
    k) KEEP=1 ;;
    h) usage ;;
    *) usage ;;
  esac
done

[[ "$RUNS" =~ ^[0-9]+$ && "$RUNS" -gt 0 ]] || { echo "runs harus > 0" >&2; exit 2; }
[ -x "$RBOT" ] || { echo "rbot tidak ditemukan/dieksekusi: $RBOT" >&2; exit 1; }
RBOT=$(cd "$(dirname "$RBOT")" && pwd)/$(basename "$RBOT")
GRAPH="$PWD/bench/graph.sh"
[ -x "$GRAPH" ] || { echo "bench/graph.sh tidak ditemukan" >&2; exit 1; }

mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)
CSV="$OUTDIR/results.csv"
printf 'nodes,edges,headers,run,ms,rc,objects,graph_edges\n' > "$CSV"

IFS=, read -r -a NS <<< "$NODES"
IFS=, read -r -a ES <<< "$EDGES"

now_ns() {
  if [ -n "${EPOCHREALTIME:-}" ]; then
    # EPOCHREALTIME is Bash >= 5 and avoids spawning date for every sample.
    printf '%d\n' "${EPOCHREALTIME/./}000"
  else
    date +%s%N
  fi
}

median() {
  printf '%s\n' "$@" | sort -n | awk 'NF{a[NR]=$1} END{if(NR%2)print a[(NR+1)/2];else print (a[NR/2]+a[NR/2+1])/2}'
}

for n in "${NS[@]}"; do
  for e in "${ES[@]}"; do
    ws="$OUTDIR/n${n}-e${e}"
    echo
    echo "========================================"
    echo "  graph nodes=$n edges=$e"
    echo "========================================"

    "$GRAPH" -n "$n" -e "$e" -o "$ws" >/dev/null || exit $?

    H=$(( (e + n - 1) / n )); (( H > 0 )) || H=1
    graph_edges=$(wc -l < "$ws/graph.tsv")
    [ "$graph_edges" -eq "$e" ] || { echo "edge count mismatch: $graph_edges != $e" >&2; exit 1; }

    times=()
    for ((run=1; run<=RUNS; run++)); do
      start=$(now_ns)
      rc=0
      (
        cd "$ws"
        rm -rf .rbot build bin
        "$RBOT" >/dev/null
      ) || rc=$?
      end=$(now_ns)
      ms=$(( (end - start) / 1000000 ))
      obj=$(find "$ws/build" -type f -name '*.o' 2>/dev/null | wc -l)
      printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$n" "$e" "$H" "$run" "$ms" "$rc" "$obj" "$graph_edges" >> "$CSV"
      times+=("$ms")
      printf '  run=%d  %d ms  obj=%d  rc=%d\n' "$run" "$ms" "$obj" "$rc"
      [ "$rc" -eq 0 ] || exit "$rc"
    done

    med=$(median "${times[@]}")
    printf '  median=%s ms  nodes=%s edges=%s\n' "$med" "$n" "$e"
  done
done

echo
echo "========================================"
echo "  RESULT"
echo "========================================"
cat "$CSV"

if (( KEEP == 0 )); then
  # Keep CSV but remove generated workspaces; this is important on Termux.
  find "$OUTDIR" -mindepth 1 -maxdepth 1 -type d -name 'n*-e*' -exec rm -rf {} +
fi
