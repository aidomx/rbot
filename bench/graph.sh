#!/usr/bin/env bash
# graph.sh — generate a synthetic C dependency graph for rbot scaling tests.
#
# The graph is bipartite: N source nodes depend on generated header nodes.
# E controls the number of source -> header dependency edges.  The generated
# Buildfile is intentionally small; the expensive part is the dependency graph
# seen by the compiler/rbot, not shell bookkeeping.
#
# Usage:
#   ./bench/graph.sh -n 1000 -e 5000 -o ws.tmp/graph
#   ./bench/graph.sh -n 10000 -e 20000 -o ws.tmp/graph
#
# Output:
#   <out>/Buildfile
#   <out>/src/*.c
#   <out>/include/*.h
#   <out>/graph.tsv       # exact edge list: source<TAB>header
#
set -u

N=100
E=200
OUT="graph.tmp"

usage() {
  sed -n '2,/^# ====.*$/p' "$0"
  exit 0
}

while getopts "n:e:o:h" opt; do
  case "$opt" in
    n) N=$OPTARG ;;
    e) E=$OPTARG ;;
    o) OUT=$OPTARG ;;
    h) usage ;;
    *) usage ;;
  esac
done

[[ "$N" =~ ^[0-9]+$ && "$N" -gt 0 ]] || { echo "N harus > 0" >&2; exit 2; }
[[ "$E" =~ ^[0-9]+$ ]] || { echo "E harus >= 0" >&2; exit 2; }

# Keep the graph sparse enough that every source can be compiled cheaply.
# One header per source is enough for E <= N; for denser graphs we create
# ceil(E/N) headers.  Thus max E is N * H, with H growing only as needed.
H=$(( (E + N - 1) / N ))
(( H > 0 )) || H=1
MAX=$(( N * H ))
(( E <= MAX )) || { echo "E terlalu besar: maksimum saat ini $MAX (N=$N, H=$H)" >&2; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT/src" "$OUT/include" "$OUT/bin" "$OUT/build"

# Header nodes.  No header includes another header: this keeps E an explicit
# edge count instead of silently multiplying it through transitive includes.
for ((h=1; h<=H; h++)); do
  printf '#pragma once\nstatic const int gv_%d = %d;\n' "$h" "$h" > "$OUT/include/h$h.h"
done

: > "$OUT/graph.tsv"

# Deterministic permutation without $RANDOM: repeatable across shells/devices.
# Each source gets edges to distinct headers.  The step is coprime to H when
# possible, then a second offset guarantees coverage for dense graphs.
for ((i=1; i<=N; i++)); do
  src="$OUT/src/s$i.c"
  printf '#include "h1.h"\n' > "$src"
  sum='gv_1'

  # Number of edges for this source: distribute the remainder over the first
  # sources so total E is exact.
  base=$(( E / N ))
  rem=$(( E % N ))
  cnt=$base
  (( i <= rem )) && cnt=$((cnt + 1))
  (( cnt > H )) && cnt=$H

  # h1 is already included above when cnt >= 1. For the remaining edges use
  # a deterministic permutation of header indexes.
  if (( cnt > 1 )); then
    for ((k=2; k<=cnt; k++)); do
      h=$(( ((i - 1) * 37 + (k - 1) * 17) % H + 1 ))
      # Avoid duplicate h1 and previous headers. H is normally much larger
      # than cnt; dense cases use a simple linear scan to guarantee uniqueness.
      seen=0
      for ((q=2; q<k; q++)); do
        prev=$(( ((i - 1) * 37 + (q - 1) * 17) % H + 1 ))
        (( h == prev )) && { seen=1; break; }
      done
      if (( h == 1 || seen )); then
        for ((h=1; h<=H; h++)); do
          used=0
          (( h == 1 )) && used=1
          for ((q=2; q<k; q++)); do
            prev=$(( ((i - 1) * 37 + (q - 1) * 17) % H + 1 ))
            (( h == prev )) && { used=1; break; }
          done
          (( used == 0 )) && break
        done
      fi
      printf '#include "h%d.h"\n' "$h" >> "$src"
      printf 's%d\th%d\n' "$i" "$h" >> "$OUT/graph.tsv"
      sum="$sum + gv_$h"
    done
  fi

  # Record h1 only if this source has at least one edge.
  if (( cnt >= 1 )); then
    printf 's%d\th1\n' "$i" >> "$OUT/graph.tsv"
    sum="$sum + 0"
  else
    # E may be 0: remove the unused h1 include.
    sed -i '1d' "$src"
    sum='0'
  fi

  {
    printf 'int s%d(void) { return %s; }\n' "$i" "$sum"
  } >> "$src"
done

# main.c is deliberately tiny; only s1 is linked into the final binary.
cat > "$OUT/src/main.c" <<'EOF_MAIN'
int s1(void);
int main(void) { return s1() == -1; }
EOF_MAIN

cat > "$OUT/Buildfile" <<'EOF_BUILD'
use project

root = .
sources = src
headers = include, I., build
compiler = gcc
flags = Wall, Wextra, O0, MMD, MP
std = gnu11

output as o
o.binaryName = graph
o.binaryDir = bin
o.buildDir = build
EOF_BUILD

printf 'nodes=%d edges=%d headers=%d\n' "$N" "$E" "$H"
printf 'out=%s\n' "$OUT"
printf 'graph=%s\n' "$OUT/graph.tsv"
