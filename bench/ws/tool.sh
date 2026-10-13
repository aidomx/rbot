# Perintah per tool (rbot/ninja/make) dan mode selektif/release.
# ---------------------------- perintah per tool ----------------------------
tool_cmd() {
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -w ;;
    ninja) ninja ;;
    make) make -j"$NPROC" ;;
  esac
}
# Cakupan selektif harus SETARA antar tool. Karena core <-> bade circular
# (library.cdeps dua arah), cakupan rbot -w core / -w bade adalah kedua
# library + kedua binary; ninja/make diberi target keluaran yang sama persis
# agar waktunya layak dibandingkan (audit problems/ws.md butir 5).
SEL_TARGETS="core/bin/rupa bade/bin/bade core/lib/libcore.so bade/lib/libbade.so"
sel_cmd() { # build selektif cakupan core
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -w core ;;
    ninja) ninja $SEL_TARGETS ;;
    make) make -j"$NPROC" $SEL_TARGETS ;;
  esac
}
dep_cmd() { # build selektif cakupan bade
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -w bade ;;
    ninja) ninja $SEL_TARGETS ;;
    make) make -j"$NPROC" $SEL_TARGETS ;;
  esac
}
rel_cmd() { # release terpisah: tar + checksum (docs/guide/releases.md)
  # Validasi packaging ikut menentukan rc run: artefak + checksum harus nyata
  # dan cocok (sha256sum -c), bukan sekadar exit 0 dari release.
  local vf=$1 tarf
  "$RBOT" $RBOT_ARGS -w release -- "format=tar,version=$vf,checksum=sha256" || return 1
  tarf="$WS/dist/wstest-v$vf.tar.gz"
  [ -s "$tarf" ] && [ -s "$tarf.sha256" ] || {
    echo "rbot/rel: artefak atau checksum hilang: $tarf" >&2
    return 1
  }
  (cd "$WS/dist" && sha256sum -c "wstest-v$vf.tar.gz.sha256") > /dev/null 2>&1 || {
    echo "rbot/rel: checksum tidak cocok: $tarf.sha256" >&2
    return 1
  }
}
build_full() {
  case $1 in
    rbot) "$RBOT" $RBOT_ARGS -j"$NPROC" -w ;;
    *) tool_cmd "$1" ;;
  esac
}

