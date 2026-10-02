#!/bin/sh

set -u

JOBS="4 8 16 32 64 88 128"
OUT="ws.tmp/jobs.csv"

mkdir -p ws.tmp
printf 'jobs,real,user,sys\n' > "$OUT"

for j in $JOBS; do
    echo
    echo "========================================"
    echo "  rbot -j$j"
    echo "========================================"

    rm -rf ruka/bin ruka/build
    rm -rf rupamod/build
    rm -rf rupa/bin rupa/build
    rm -rf .rbot

    START=$(date +%s%N)

    rbot -j"$j"

    RC=$?

    END=$(date +%s%N)
    REAL_NS=$((END - START))
    REAL=$(awk "BEGIN { printf \"%.3f\", $REAL_NS / 1000000000 }")

    printf '%s,%s,%s,%s\n' "$j" "$REAL" "-" "-" >> "$OUT"

    echo
    echo "jobs=$j real=${REAL}s rc=$RC"

    [ "$RC" -ne 0 ] && exit "$RC"
done

echo
echo "========================================"
echo "  RESULT"
echo "========================================"
cat "$OUT"
