# Generator workspace (source C, Buildfile.ws, pembanding ninja/make).
# ---------------------------- generator workspace --------------------------
gen_c() { # gen_c <proyek> <prefix>
  local proj=$1 pre=$2 h i
  mkdir -p "$WS/$proj/src"
  for ((h = 1; h <= NHEAD; h++)); do
    {
      echo "#pragma once"
      [ "$h" -gt 1 ] && echo "#include \"h$((h - 1)).h\""
      echo "static const int v$h = $h;"
    } > "$WS/$proj/src/h$h.h"
  done
  for ((i = 1; i <= NSRC; i++)); do
    {
      echo "#include \"h$((i % NHEAD + 1)).h\""
      echo "int ${pre}$i(void) { return v$((i % NHEAD + 1)) + $i; }"
    } > "$WS/$proj/src/${pre}$i.c"
  done
  {
    echo "#include \"h1.h\""
    echo "int ${pre}1(void);"
    echo "int main(void) { return ${pre}1() - 1; }"
  } > "$WS/$proj/src/main.c"
}

# Mode embed: `ld -r -b binary` bila linker mendukungnya (GNU ld); bila tidak
# (LLD di Termux), array C lewat embed_c.sh — fallback yang sama dengan rbot,
# sehingga pekerjaan kedua tool setara. BENCH_EMBED=carray|ld memaksa mode.
embed_mode() {
  [ -n "${BENCH_EMBED:-}" ] && {
    printf '%s' "$BENCH_EMBED"
    return
  }
  # ld -r -b binary adalah fitur GNU/Linux; jangan coba pada platform lain.
  if [ "$(uname -s)" != Linux ] || ! command -v ld >/dev/null 2>&1; then
    printf carray
    return
  fi
  local d
  d=$(mktemp -d 2> /dev/null) || {
    printf ld
    return
  }
  printf x > "$d/p.bin"
  if ld -r -b binary -o "$d/p.o" "$d/p.bin" > /dev/null 2>&1; then printf ld; else printf carray; fi
  rm -rf "$d"
}
EMBED_MODE=$(embed_mode)
# Kompilasi tambahan per embed (dihitung shim gcc) bila memakai array C.
embed_extra() { [ "$EMBED_MODE" = carray ] && echo 1 || echo 0; }

gen_ws() {
  rm -rf "$WS"
  mkdir -p "$WS/rupamod"
  local i
  gen_c core r
  gen_c bade k
  # Header publik milik workspace: kedua proyek memakai pusat library/.
  mkdir -p "$WS/library" "$WS/core/include" "$WS/bade/include" "$WS/modules"
  printf '#pragma once\n#define WS_SHARED_VALUE 7\n' > "$WS/library/a.h"
  printf '#pragma once\n#include "a.h"\n#define WS_BADE_VALUE WS_SHARED_VALUE\n' > "$WS/library/b.h"
  # Header bersama: library/ dipantau lewat headers workspace;
  # ext.h tetap di-include secara relatif tanpa symlink.
  printf '#pragma once\n#define WS_EXT_VALUE 3\n' > "$WS/library/ext.h"
  ln -s ../../library/a.h "$WS/core/include/a.h"
  ln -s ../../library/b.h "$WS/bade/include/b.h"
  ln -s ../../library/a.h "$WS/bade/include/a.h"
  for ((i = 1; i <= NMOD; i++)); do
    printf 'module m%s\nexport f%s\n' "$i" "$i" > "$WS/rupamod/m$i.rp"
  done

  # Simbol benar-benar circular: core_entry -> bade_entry -> core_helper.
  cat > "$WS/core/src/r1.c" << 'EOF'
#include "../include/a.h"
int bade_entry(void);
int core_entry(void) { return bade_entry(); }
EOF
  cat > "$WS/core/src/r2.c" << 'EOF'
#include "../include/a.h"
#include "../../library/ext.h"
int ws_ext_value = WS_EXT_VALUE;
int core_helper(void) { return WS_SHARED_VALUE + ws_ext_value - WS_EXT_VALUE; }
EOF
  cat > "$WS/bade/src/k1.c" << 'EOF'
#include "../include/b.h"
int core_helper(void);
int bade_entry(void) { return core_helper() + (WS_BADE_VALUE - WS_SHARED_VALUE); }
EOF
  # Jalur balik independen: bade -> core -> bade.
  cat > "$WS/bade/src/k2.c" << 'EOF'
#include "../include/b.h"
#include "../../library/ext.h"
int core_entry(void);
int bade_ext_value = WS_EXT_VALUE;
int bade_reverse(void) { return core_entry() + (WS_BADE_VALUE - WS_SHARED_VALUE) + bade_ext_value - WS_EXT_VALUE; }
EOF
  # Runtime invariant: nilai dari header pusat (library/a.h) mengalir ke
  #kedua main, sehingga object basi setelah edit header pusat membuat
  #binary gagal (exit != 0) dan terdeteksi sebagai kegagalan build.
  cat > "$WS/core/src/main.c" << 'EOF'
#include "../include/a.h"
int core_entry(void);
int main(void) { return core_entry() == WS_SHARED_VALUE ? 0 : 1; }
EOF
  cat > "$WS/bade/src/main.c" << 'EOF'
#include "../include/b.h"
int bade_reverse(void);
int main(void) { return bade_reverse() == WS_BADE_VALUE ? 0 : 1; }
EOF

  # -n 1 tetap memiliki dua translation unit untuk menguji siklus simbol.
  # Source tambahan khusus ini tidak mengubah arti -n (jumlah source beban).
  # ---- Buildfile.ws (Sesuai desain terbaru) ----
  {
    extract_tmp="${TMPDIR:-${PREFIX:+$PREFIX/tmp}}"
    extract_tmp="${extract_tmp:-/tmp}"
    echo "use workspace"
    echo "projects.rupamod as mod"
    echo "projects.core as core"
    echo "projects = bade, core, rupamod"
    echo "compiler = $COMPILERS"
    echo "flags = Wall, Wextra, MMD, MP, fPIC"
    echo "headers = (core, bade, I.), library"
    echo "std = gnu11"
    echo "mod.output.binary = false"
    echo "mod.archive.src = ."
    echo "mod.archive.name = rupa_modules"
    echo "mod.archive.with = tar"
    echo "mod.archive.dir = modules"
    echo "mod.archive.exclude = build, dist, LICENSE, README.md, self_archive"
    echo "core.sources = src"
    echo "core.exclude = main.c"
    echo "core.output.binaryName = rupa"
    echo "core.output.libraryName = core"
    echo "core.output.libraryShared = true"
    echo "bade.sources = manager, src"
    echo "bade.exclude = manager/main.c"
    echo "bade.output.binaryName = bade"
    echo "bade.output.libraryName = bade"
    echo "bade.output.libraryShared = true"
    echo "bade.depends_on = rupamod"
    echo "bade.embedded.modules.file = modules/rupa_modules.tar"
    echo "bade.embedded.modules.variable = MODULES"
    echo "bade.embedded.modules.extract = ${extract_tmp}/rupa-system"
    echo "bade.embedded.modules.pattern = .rp"
    echo "bade.library.cdeps = core"
    echo "core.library.cdeps = bade"
  } > "$WS/Buildfile.ws"

  # ---- pembanding: build.ninja (FAIR: Static Libs + Circular Deps) ----
  {
    echo "cflags = -Wall -Wextra -fPIC -std=gnu11"
    echo "rule cc"
    echo "  command = mkdir -p \$\$(dirname \$out) && gcc \$cflags -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "rule shared"
    echo "  command = mkdir -p \$\$(dirname \$out) && gcc -shared -o \$out \$in"
    echo "rule ar"
    echo "  command = mkdir -p \$\$(dirname \$out) && ar rcs \$out \$in"
    echo "rule link_group"
    echo "  command = mkdir -p \$\$(dirname \$out) && gcc \$main -Wl,-u,_binary_modules_rupa_modules_tar_start -Wl,--start-group \$in -Wl,--end-group -o \$out"
    echo "rule pack"
    echo "  command = mkdir -p \$\$(dirname \$out) && tar -cf \$out -C rupamod --exclude=build --exclude=dist --exclude=LICENSE --exclude=README.md --exclude=self_archive ."
    echo "rule embed"
    if [ "$EMBED_MODE" = carray ]; then
      echo "  command = mkdir -p \$\$(dirname \$out) && sh $WS_LIB/embed_c.sh \$in \$out.c _binary_modules_rupa_modules_tar && gcc -c -fPIC -o \$out \$out.c"
    else
      echo "  command = mkdir -p \$\$(dirname \$out) && ld -r -b binary -o \$out \$in"
    fi

    local p pre
    for p in core:r bade:k; do
      pre=${p#*:}
      p=${p%%:*}
      echo "build $p/build/main.o: cc $p/src/main.c"
      for ((i = 1; i <= NSRC; i++)); do echo "build $p/build/$pre$i.o: cc $p/src/$pre$i.c"; done
    done

    echo -n "build modules/rupa_modules.tar: pack"
    for ((i = 1; i <= NMOD; i++)); do printf ' rupamod/m%d.rp' "$i"; done
    echo

    echo "build bade/build/embed.o: embed modules/rupa_modules.tar"

    if ((NSRC == 1)); then
      echo "build core/build/r2.o: cc core/src/r2.c"
      echo "build bade/build/k2.o: cc bade/src/k2.c"
    fi
    echo -n "build core/build/libcore.a: ar"
    for ((i = 1; i <= NSRC; i++)); do printf ' core/build/r%d.o' "$i"; done
    if ((NSRC == 1)); then printf " core/build/r2.o"; fi
    echo

    echo -n "build bade/build/libbade.a: ar"
    for ((i = 1; i <= NSRC; i++)); do printf ' bade/build/k%d.o' "$i"; done
    if ((NSRC == 1)); then printf " bade/build/k2.o"; fi
    echo " bade/build/embed.o"

    # Shared libraries dibangun dari object yang sama, tanpa melewatkan embed.
    echo -n "build core/lib/libcore.so: shared"
    for ((i = 1; i <= NSRC; i++)); do printf ' core/build/r%d.o' "$i"; done
    if ((NSRC == 1)); then printf ' core/build/r2.o'; fi
    echo
    echo -n "build bade/lib/libbade.so: shared"
    for ((i = 1; i <= NSRC; i++)); do printf ' bade/build/k%d.o' "$i"; done
    if ((NSRC == 1)); then printf ' bade/build/k2.o'; fi
    echo " bade/build/embed.o"

    # Circular dependency diselesaikan dengan linker group
    echo "build core/bin/rupa: link_group core/build/libcore.a bade/build/libbade.a | core/build/main.o"
    echo "  main = core/build/main.o"
    echo "build bade/bin/bade: link_group core/build/libcore.a bade/build/libbade.a | bade/build/main.o"
    echo "  main = bade/build/main.o"

    echo "default core/bin/rupa bade/bin/bade core/lib/libcore.so bade/lib/libbade.so"
  } > "$WS/build.ninja"

  # ---- pembanding: Makefile (FAIR: Static Libs + Circular Deps) ----
  {
    echo "CFLAGS := -Wall -Wextra -fPIC -std=gnu11"
    echo -n "CORE_OBJS :="
    for ((i = 1; i <= NSRC; i++)); do printf ' core/build/r%d.o' "$i"; done
    if ((NSRC == 1)); then printf " core/build/r2.o"; fi
    echo
    echo -n "BADE_OBJS :="
    for ((i = 1; i <= NSRC; i++)); do printf ' bade/build/k%d.o' "$i"; done
    if ((NSRC == 1)); then printf " bade/build/k2.o"; fi
    echo " bade/build/embed.o"
    echo -n "MODS :="
    for ((i = 1; i <= NMOD; i++)); do printf ' rupamod/m%d.rp' "$i"; done
    echo

    echo "all: core/bin/rupa bade/bin/bade core/lib/libcore.so bade/lib/libbade.so"

    # Linking dengan --start-group untuk circular dependency
    printf 'core/bin/rupa: core/build/main.o core/build/libcore.a bade/build/libbade.a\n\t@mkdir -p $(@D)\n\tgcc $< -Wl,-u,_binary_modules_rupa_modules_tar_start -Wl,--start-group core/build/libcore.a bade/build/libbade.a -Wl,--end-group -o $@\n'
    printf 'bade/bin/bade: bade/build/main.o core/build/libcore.a bade/build/libbade.a\n\t@mkdir -p $(@D)\n\tgcc $< -Wl,-u,_binary_modules_rupa_modules_tar_start -Wl,--start-group core/build/libcore.a bade/build/libbade.a -Wl,--end-group -o $@\n'

    printf 'core/build/libcore.a: $(CORE_OBJS)\n\tar rcs $@ $^\n'
    printf 'bade/build/libbade.a: $(BADE_OBJS)\n\tar rcs $@ $^\n'

    printf 'modules/rupa_modules.tar: $(MODS)\n\t@mkdir -p $(@D)\n\ttar -cf $@ -C rupamod --exclude=build --exclude=dist --exclude=LICENSE --exclude=README.md --exclude=self_archive .\n'
    if [ "$EMBED_MODE" = carray ]; then
      printf 'bade/build/embed.o: modules/rupa_modules.tar\n\t@mkdir -p $(@D)\n\tsh %s $< $@.c _binary_modules_rupa_modules_tar && gcc -c -fPIC -o $@ $@.c\n' "$WS_LIB/embed_c.sh"
    else
      printf 'bade/build/embed.o: modules/rupa_modules.tar\n\t@mkdir -p $(@D)\n\tld -r -b binary -o $@ $<\n'
    fi

    printf 'core/lib/libcore.so: $(CORE_OBJS)\n\t@mkdir -p $(@D)\n\tgcc -shared -o $@ $^\n'
    printf 'bade/lib/libbade.so: $(BADE_OBJS)\n\t@mkdir -p $(@D)\n\tgcc -shared -o $@ $^\n'

    printf 'core/build/%%.o: core/src/%%.c\n\t@mkdir -p $(@D)\n\tgcc $(CFLAGS) -MMD -MP -c $< -o $@\n'
    printf 'bade/build/%%.o: bade/src/%%.c\n\t@mkdir -p $(@D)\n\tgcc $(CFLAGS) -MMD -MP -c $< -o $@\n'

    echo '-include $(CORE_OBJS:.o=.d) $(BADE_OBJS:.o=.d) core/build/main.d bade/build/main.d'
  } > "$WS/Makefile"
}
