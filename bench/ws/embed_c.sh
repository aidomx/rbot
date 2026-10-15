#!/bin/sh
# Embed fallback tanpa `ld -r -b binary` (linker LLD di Termux): array C dari
# berkas biner, dengan simbol <sym>_start dan <sym>_size seperti embed satria.
# Pemakaian: embed_c.sh <in> <out.c> <sym>
in=$1; out=$2; sym=$3
n=$(wc -c < "$in" | tr -d ' ')
{
  printf 'const unsigned char %s_start[] = {\n' "$sym"
  od -An -v -tx1 "$in" | awk '{ for (i = 1; i <= NF; i++) printf "0x%s,", $i; printf "\n" }'
  printf '};\nconst unsigned long %s_size = %s;\n' "$sym" "$n"
} > "$out"
