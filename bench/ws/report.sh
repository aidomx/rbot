# Laporan (design/bench_ws_output.md): chart median, blok [Detailed Validation Log],
# dan footer VALIDATION RULES & METRICS. Dimuat dari ws.sh.
# ---------------------------- utilitas -------------------------------------
rule_line() {
  local w=$1 s=""
  s=$(printf '%*s' "$w" '')
  printf '%s%s%s\n' "$C_DIM" "${s// /─}" "$C_RST"
}
plural() { [ "$1" = 1 ] && printf '%s %s' "$1" "$2" || printf '%s %ss' "$1" "$2"; }
box_width() {
  local tw=$((COLS > 72 ? 70 : COLS - 2))
  ((tw < 40)) && tw=40
  printf '%s' "$tw"
}

# ---------------------------- chart median ---------------------------------
report() {
  local sorted="$OUTDIR/.sorted.csv"
  awk -F, 'NR > 1' "$CSV" | sort -t, -k2,2 -k1,1 -k3,3n > "$sorted"
  awk -F, -v BW="$BW" -v B="$C_BLD" -v D="$C_DIM" -v G="$C_GRN" -v Y="$C_YEL" -v R="$C_RED" -v Z="$C_RST" '
{
g = $2 SUBSEP $1
if (!(g in seen)) { seen[g] = 1; ng++; gs[ng] = g; gscen[ng] = $2; glab[ng] = $1 }
if ($4 == 0) { n[g]++; v[g, n[g]] = $3; ro[g] += $5; ra[g] += $6; rl[g] += $7; rn[g] += $8; rcc[g] += $9 } else bad[g]++
}
END {
nsc = split("cold noop leaf bade mod touch hedit sel dep libtouch libhdr rel", SC, " ")
desc["cold"]  = "build dari nol, 3 proyek"
desc["noop"]  = "tanpa perubahan"
desc["leaf"]  = "edit 1 source core (bade tak boleh ikut)"
desc["bade"]  = "edit 1 source bade"
desc["mod"]   = "edit 1 .rp rupamod -> arsip -> bade"
desc["touch"] = "touch include/a.h pusat, konten sama"
desc["hedit"] = "edit isi include/a.h pusat (konten berbeda)"
desc["sel"]   = "edit core+bade, build selektif cakupan -w core"
desc["dep"]   = "edit .rp+core, build selektif cakupan -w bade"
desc["libtouch"] = "touch include/ext.h (header internal), konten sama"
desc["libhdr"]   = "edit isi include/ext.h (header internal, include relatif)"
desc["rel"]   = "release tar+sha256 (rbot-only, diukur terpisah)"
for (si = 1; si <= nsc; si++) {
s = SC[si]; k = 0; nf = 0
for (i = 1; i <= ng; i++) {
if (gscen[i] != s) continue; g = gs[i]
if (n[g] > 0) {
m = n[g]; med = (m % 2) ? v[g, (m + 1) / 2] : (v[g, m / 2] + v[g, m / 2 + 1]) / 2
k++; L[k] = glab[i]; M[k] = med; LB[k] = rl[g] / m; BN[k] = rn[g] / m; BD[k] = bad[g] + 0
} else fl[++nf] = glab[i]
}
if (k == 0 && nf == 0) continue
for (a = 2; a <= k; a++) for (b = a; b > 1 && M[b] < M[b - 1]; b--) {
t = L[b]; L[b] = L[b - 1]; L[b - 1] = t; t = M[b]; M[b] = M[b - 1]; M[b - 1] = t
t = LB[b]; LB[b] = LB[b - 1]; LB[b - 1] = t; t = BN[b]; BN[b] = BN[b - 1]; BN[b - 1] = t
t = BD[b]; BD[b] = BD[b - 1]; BD[b - 1] = t
}
printf "\n%s%s%s %s%s%s\n", B, toupper(s), Z, D, desc[s], Z
mx = (k > 0) ? M[k] : 1; if (mx < 1) mx = 1; best = (k > 0) ? M[1] : 1; if (best < 1) best = 1
div = (k > 1 && (diverges(LB, k) || diverges(BN, k)))
for (i = 1; i <= k; i++) {
len = int(M[i] / mx * BW + 0.5); if (len < 1) len = 1; br = ""
for (j = 1; j <= BW; j++) br = br (j <= len ? "█" : "░")
col = (i == 1) ? G : ""; mark = (i == 1) ? "★" : " "
printf "  %s%s %-6s %s %6d ms %5.2f×%s", col, mark, L[i], br, M[i], M[i] / best, Z
if (BD[i] > 0) printf " %s✗%d%s", R, BD[i], Z
printf "\n"
# Divergensi pekerjaan antar tool: waktu bukan perbandingan setara (audit ws.md butir 5).
if (div) printf "    %s[!]%s\n", Y, Z
}
for (i = 1; i <= nf; i++) printf "  %s✗ %-6s semua run gagal atau tidak sesuai harapan%s\n", R, fl[i], Z
}
}
function diverges(arr, cnt,  i, mn, mx) {
mn = arr[1]; mx = arr[1]
for (i = 2; i <= cnt; i++) { if (arr[i] < mn) mn = arr[i]; if (arr[i] > mx) mx = arr[i] }
return mx - mn > 0.5
}
' "$sorted"
  rm -f "$sorted"
}

# ---------------------------- [Detailed Validation Log] --------------------
# Satu blok per skenario+tool: SUMMARY (median aktual), EXPECTED (harapan),
# STATUS. Tanpa border kanan: padding awk berbasis byte, dan simbol ✔/✗ akan
# membuat garis kanan tidak sejajar.
view_report() {
  [ -s "$VIEW_CSV" ] || return 0
  local tw; tw=$(box_width)
  echo
  echo "${C_BLD}[Detailed Validation Log]${C_RST}"
  awk -F, -v TOOLS="${TOOLS[*]}" -v TW="$tw" -v SUCC="$OUTDIR/.success.txt" -v B="$C_BLD" -v D="$C_DIM" -v G="$C_GRN" -v R="$C_RED" -v Z="$C_RST" '
function med(key, f,   i, j, n, a, t) {
  n = cnt[key, f]
  if (n == 0) return -999
  for (i = 1; i <= n; i++) a[i] = val[key, f, i]
  for (i = 2; i <= n; i++) { t = a[i]; j = i - 1
    while (j >= 1 && a[j] > t) { a[j + 1] = a[j]; j-- }
    a[j + 1] = t }
  return (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
}
function fmtv(v) {
  if (v == -999) return "n/a"
  if (v == -1) return "*"
  return (v == int(v)) ? sprintf("%d", v) : sprintf("%g", v)
}
function fmte(x) { if (x + 0 == -1) return "*"; return x }
function push(key, f, v) { cnt[key, f]++; val[key, f, cnt[key, f]] = v }
function dashes(n,   s, i) { s = ""; for (i = 0; i < n; i++) s = s "─"; return s }
{
  k = $2 SUBSEP $1
  seen[k] = 1
  rc = $3 + 0; ms = $4 + 0
  # Metrik hanya dari run yang berhasil; run gagal tidak masuk median.
  if (rc == 0) {
    if (ms >= 0) push(k, "ms", ms)
    push(k, "obj", $5 + 0); push(k, "arc", $6 + 0); push(k, "lib", $7 + 0)
    push(k, "bin", $8 + 0); push(k, "cc", $9 + 0)
  }
  eobj[k] = $10; earc[k] = $11; elib[k] = $12; ebin[k] = $13; ecc[k] = $14
  # In accordance hanya bila SEMUA run rc 0 DAN kerja sesuai harapan (miss = 0).
  if (rc != 0 || $15 != 0) acc[k] = 0
  else if (!(k in acc)) acc[k] = 1
}
END {
  nsc = split("cold noop leaf bade mod touch hedit sel dep libtouch libhdr rel", SC, " ")
  nt = split(TOOLS, TL, " ")
  for (si = 1; si <= nsc; si++) {
    s = SC[si]
    for (ti = 1; ti <= nt; ti++) {
      t = TL[ti]; k = s SUBSEP t
      if (!(k in seen)) continue
      tms = med(k, "ms")
      lab = "VALIDATION: " t " in " s " "
      printf "\n┌─ %s%s\n", lab, dashes(TW - length(lab) - 3)
      tval = (tms == -999) ? "n/a" : sprintf("%sms", fmtv(tms))
      printf "│ %-8s │ time=%s | obj=%s | cc=%s | lib=%s | bin=%s | arc=%s\n", "SUMMARY", tval, fmtv(med(k, "obj")), fmtv(med(k, "cc")), fmtv(med(k, "lib")), fmtv(med(k, "bin")), fmtv(med(k, "arc"))
      printf "│ %-8s │ obj=%s | arc=%s | lib=%s | bin=%s | cc=%s\n", "EXPECTED", fmte(eobj[k]), fmte(earc[k]), fmte(elib[k]), fmte(ebin[k]), fmte(ecc[k])
      if (acc[k]) { printf "│ %-8s │ ✔ In accordance\n", "STATUS"; okc[t]++ }
      else        { printf "│ %-8s │ ✗ Not in accordance\n", "STATUS"; badc[t]++ }
      printf "└%s\n", dashes(TW - 1)
    }
  }
  # Ringkasan sukses per tool dipakai footer VALIDATION RULES & METRICS.
  for (ti = 1; ti <= nt; ti++) print TL[ti], okc[TL[ti]] + 0, badc[TL[ti]] + 0 > SUCC
  close(SUCC)
}
' "$VIEW_CSV"
}

# ---------------------------- VALIDATION RULES & METRICS -------------------
rules_footer() {
  local succ="$OUTDIR/.success.txt" w=0 tw
  tw=$(box_width)
  echo
  echo "${C_BLD}VALIDATION RULES & METRICS${C_RST}"
  rule_line "$tw"
  echo "Success Rate:"
  if [ -s "$succ" ]; then
    w=$(awk '{ if (length($1) > w) w = length($1) } END { print w + 0 }' "$succ")
    awk -v W="$w" '{ printf "  %-*s : %d/%d scenarios passed in workspace concept\n", W, $1, $2, $2 + $3 }' "$succ"
  fi
  [ "$HAVE_STRACE" = 1 ] || echo "  strace : dilewati${STRACE_SKIP:+ ($STRACE_SKIP)}"
  rm -f "$succ"
  echo
  echo "Legend:"
  echo "  ★   = Tercepat pada skenario ini"
  echo "  ×   = Rasio waktu terhadap yang tercepat"
  echo "  [!] = Pekerjaan berbeda antar tool (waktu tidak setara 1:1, bandingkan dengan hati-hati)"
  echo "  *   = Tidak di-hard-check pada skenario ini (lihat EXPECTED di blok validasi)"
  echo
  echo "Metrics:"
  echo "  obj = .o berubah · cc = kompilasi C aktual (shim) · lib = lib .a/.so"
  echo "  bin = binary berubah · arc = arsip .rp berubah"
  echo
  echo "Harapan Correctness (Validation Rules):"
  echo "  cold  : obj penuh, arc 1, lib 4, bin 2 · noop: semua nol"
  echo "  leaf/bade: cc 1 — dependensi compile terbukti via shim"
  echo "  mod   : cc 0 — arsip berubah, kompilasi tidak perlu ulang"
  echo "  touch : rbot 0 (content-hash); ninja/make 6 (mtime, dependen a.h)"
  echo "  hedit : SEMUA tool obj cc 6 (edit konten header pusat), lib 4, bin 2"
  echo "  sel   : cakupan disamakan (SEL_TARGETS) — obj cc 2, lib 4, bin 2"
  echo "  dep   : cakupan disamakan — obj cc 1, lib 4, bin 2"
  echo "  rel   : artefak tar + sha256 dicek sha256sum -c (rbot-only)"
}
