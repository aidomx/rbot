# Harapan (except) per skenario + state verdict. Dimuat dari ws.sh.
EXPECT_OBJ=$((2 * (NSRC + 1) + 1 + (NSRC == 1 ? 2 : 0))) # core + bade + embedded object
EXPECT_CC=$((2 * (NSRC + 1) + 2 * (NSRC == 1 ? 1 : 0))) # kompilasi C pada cold
EXPECT_DEPS=6 # dependen library/a.h: r1,r2,main (core) + k1,k2,main (bade)

# Cache prime per tool: build penuh + gate hanya sekali per tool; skenario
# berikutnya memulihkan salinan hasil prime (cp -a) ke path WS yang sama.
declare -gA PRIME_OK=() PRIME_FAIL=() # -g: global walau dimuat lewat ws_source()
MISMATCH_RUNS=0 # run gagal (rc != 0) atau prime/gate gagal
NOT_ACC=0 # run (semua tool) yang kerjanya tidak sesuai harapan -> Not in accordance
EXPECT_MISS=""
exp_chk() { [ "$2" -eq "$3" ] || EXPECT_MISS+=" $1=$2(harap $3)"; }
# Harapan kerja per skenario. obj/arc/lib/bin berasal dari snapshot artefak,
# cc dari shim gcc (bukti kompilasi aktual). bin di-hard-check juga pada
# leaf/bade/mod (bin 2): kedua binary me-relink oleh siklus core <-> bade dan
# under-relink bin 1 pernah jadi bug (problems/ws-benchmark-2026-10-15.md
# Temuan 2). Divergensi lib antar tool TIDAK di-hard-check pada leaf/bade/mod:
# relink .a/.so berlebih/kurang adalah temuan peluang optimasi yang dilaporkan
# di ringkasan, bukan gagal ukur.
# Selain men-flag EXPECT_MISS (temuan di stderr.log), eksplisit mencatat
# harapan ke EXP_* untuk laporan view (problems/ws.md "View"): -1 = field
# TIDAK di-hard-check (tampil '*' di blok except — divergensi lib antar tool
# pada leaf/bade/mod adalah temuan, bukan gagal ukur).
expect_check() { # <tool> <scen> <obj> <arc> <lib> <bin> <cc>
  local k=$1 s=$2 o=$3 a=$4 l=$5 b=$6 c=$7
  EXPECT_MISS=""
  EXP_OBJ=-1 EXP_ARC=-1 EXP_LIB=-1 EXP_BIN=-1 EXP_CC=-1
  case $s in
    cold)
      # CMake melakukan compiler identification/probe saat configure cold.
      # Probe itu pekerjaan nyata, bukan kompilasi source workspace.
      local cmake_probe=0
      [ "$k" = cmake ] && cmake_probe=1
      EXP_OBJ=$EXPECT_OBJ EXP_ARC=1 EXP_LIB=4 EXP_BIN=2 EXP_CC=$((EXPECT_CC + $(embed_extra) + cmake_probe))
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    noop)
      EXP_OBJ=0 EXP_ARC=0 EXP_LIB=0 EXP_BIN=0 EXP_CC=0
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    leaf | bade)
      # 1 source berubah -> tepat 1 kompilasi ulang di proyeknya sendiri.
      # bin wajib 2: kedua binary me-relink karena siklus core <-> bade.
      # lib tidak di-hard-check (relink .a/.so antar tool boleh berbeda).
      EXP_OBJ=1 EXP_ARC=0 EXP_CC=1 EXP_BIN=2
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    mod)
      # 1 .rp -> arsip -> 1 .o embed; nol kompilasi C.
      # bin wajib 2: arsip -> lib bade -> kedua binary ikut me-relink.
      EXP_OBJ=1 EXP_ARC=1 EXP_CC=$(embed_extra) EXP_BIN=2
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    touch)
      # Konten header sama: satria wajib 0 (content-hash); CMake (mtime)
      # wajib membangun ulang seluruh dependen library/a.h.
      if [ "$k" = satria ]; then
        EXP_OBJ=0 EXP_ARC=0 EXP_LIB=0 EXP_BIN=0 EXP_CC=0
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk arc "$a" "$EXP_ARC"
        exp_chk lib "$l" "$EXP_LIB"
        exp_chk bin "$b" "$EXP_BIN"
        exp_chk cc "$c" "$EXP_CC"
      else
        EXP_OBJ=$EXPECT_DEPS EXP_CC=$EXPECT_DEPS
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk cc "$c" "$EXP_CC"
      fi
      ;;
    hedit)
      # Edit isi header pusat: kedua tool wajib mengompilasi ulang persis
      # dependen library/a.h (r1,r2,main core; k1,k2,main bade), memperbarui
      # keempat library dan kedua binary. Runtime check memastikan tidak ada
      # object basi yang lolos: nilai WS_SHARED_VALUE mengalir ke kedua main.
      EXP_OBJ=$EXPECT_DEPS EXP_ARC=0 EXP_LIB=4 EXP_BIN=2 EXP_CC=$EXPECT_DEPS
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    sel)
      # Kesetaraan selektif: satria -w core dan CMake <SEL_TARGETS> wajib
      # mengerjakan pekerjaan yang sama — compile r1.c + k1.c, 4 library,
      # 2 binary, nol arsip.
      EXP_OBJ=2 EXP_ARC=0 EXP_LIB=4 EXP_BIN=2 EXP_CC=2
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    dep)
      # .rp + core source: arsip 1, compile r1.c saja (cc 1), obj 2 (r1.o +
      # .o embed), 4 library, 2 binary.
      EXP_OBJ=2 EXP_ARC=1 EXP_LIB=4 EXP_BIN=2 EXP_CC=$((1 + $(embed_extra)))
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    libtouch)
      # Header internal (library/ext.h) dengan konten sama: satria wajib 0
      # (content-hash). CMake (mtime) membangun ulang 2 TU yang meng-include-nya.
      if [ "$k" = satria ]; then
        EXP_OBJ=0 EXP_ARC=0 EXP_LIB=0 EXP_BIN=0 EXP_CC=0
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk arc "$a" "$EXP_ARC"
        exp_chk lib "$l" "$EXP_LIB"
        exp_chk bin "$b" "$EXP_BIN"
        exp_chk cc "$c" "$EXP_CC"
      else
        EXP_OBJ=2 EXP_CC=2
        exp_chk obj "$o" "$EXP_OBJ"
        exp_chk cc "$c" "$EXP_CC"
      fi
      ;;
    libhdr)
      # Edit isi header internal: r2.c + k2.c (yang meng-include ext.h) dikompilasi
      # ulang, 4 library, 2 binary — sama untuk semua tool.
      EXP_OBJ=2 EXP_ARC=0 EXP_LIB=4 EXP_BIN=2 EXP_CC=2
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk arc "$a" "$EXP_ARC"
      exp_chk lib "$l" "$EXP_LIB"
      exp_chk bin "$b" "$EXP_BIN"
      exp_chk cc "$c" "$EXP_CC"
      ;;
    rel)
      # Release diukur terpisah: hanya packaging, tanpa compile ulang.
      EXP_OBJ=0 EXP_CC=0
      exp_chk obj "$o" "$EXP_OBJ"
      exp_chk cc "$c" "$EXP_CC"
      ;;
  esac
}

