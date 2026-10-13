# Desain Output Benchmark Workspace

Dokumen ini merinci desain antarmuka CLI (Command Line Interface) untuk skrip benchmark workspace (`bench/ws.sh`). Tujuannya adalah menyajikan data yang sangat rinci (validasi per-run, metrik, aturan) namun tetap mudah dibaca (*scannable*) dan tidak melelahkan mata.

## Filosofi Desain
1. **Terstruktur & Hierarkis:** Menggunakan pemisah visual (`─`) dan blok log yang rapi untuk memisahkan fase Header, Eksekusi, Validasi, dan Laporan.
2. **Detail Tetap Utuh:** Tidak menghilangkan data validasi (`summary` vs `except`), tetapi dikemas dalam blok `[VALIDATION]` yang ringkas.
3. **Visual yang Tenang:** Menghindari warna yang berlebihan. Menggunakan simbol (`★`, `[!]`) sebagai *anchor* visual.
4. **Konsolidasi Aturan:** Menggabungkan bagian `benchmark except`, `rules`, dan `computate` yang terpisah-pisah menjadi satu bagian **Validation Rules & Metrics** yang padu di akhir.

---

## Contoh Output (Implementasi)

```text
▌ rbot workspace benchmark · 2 tools · 1 run · median
──────────────────────────────────────────────────────────────────────
Workspace : rupamod (40 .rp) · core (1 .c) · bade (1 .c)
Graph     : bade depends_on rupamod (embed), core <-> bade (circular)
Directory : /home/aidomx/projects/tools/rbot/ws.tmp/wstest

Versions  :
  ninja : 1.12.1
  rbot  : dev git:v0.2.3-3-g10521b0-dirty (bin:78e771)
  gcc   : 14.2.0 · aarch64

[Execution Stream]
> [ninja cold ] 1881ms | obj:7 cc:6 lib:4 bin:2
> [rbot  cold ] 3083ms | obj:7 cc:6 lib:4 bin:2
> [ninja noop ]   62ms | obj:0 cc:0 lib:0 bin:0
> [rbot  noop ]   70ms | obj:0 cc:0 lib:0 bin:0
> [ninja leaf ] 1233ms | obj:1 cc:1 lib:2 bin:2
> [rbot  leaf ] 1209ms | obj:1 cc:1 lib:2 bin:1
... (dilanjutkan untuk skenario lainnya)

[Detailed Validation Log]
┌─ VALIDATION: ninja in cold ──────────────────────────────────────┐
│ SUMMARY  │ time=1881ms | obj=7 | cc=6 | lib=4 | bin=2 | arc=1   │
│ EXPECTED │ obj=7 | arc=1 | lib=4 | bin=2 | cc=6                 │
│ STATUS   │ ✅ In accordance                                      │
└──────────────────────────────────────────────────────────────────┘

┌─ VALIDATION: rbot in touch ──────────────────────────────────────┐
│ SUMMARY  │ time=212ms | obj=0 | cc=0 | lib=0 | bin=0 | arc=0    │
│ EXPECTED │ obj=0 | arc=* | lib=* | bin=* | cc=0                 │
│ STATUS   │ ✅ In accordance                                      │
└──────────────────────────────────────────────────────────────────┘
*(Blok validasi ini dicetak untuk setiap skenario & tool untuk memastikan keakuratan data)*

✔ Selesai dalam 1:12
──────────────────────────────────────────────────────────────────────
BENCHMARK REPORT (Median)
──────────────────────────────────────────────────────────────────────

COLD   build dari nol, 3 proyek
  ★ ninja   ██████████░░░░░░   1881 ms   1.00×
    rbot    ████████████████   3083 ms   1.64×

NOOP   tanpa perubahan
  ★ ninja   ██████████████░░     62 ms   1.00×
    rbot    ████████████████     70 ms   1.13×

TOUCH  touch library/a.h pusat, konten sama
  ★ rbot    ██░░░░░░░░░░░░░░    212 ms   1.00×
    [!]
    ninja   ████████████████   1538 ms   7.25×
    [!]

SEL    edit core+bade, build selektif cakupan -w core
  ★ rbot    ████████████░░░░   2774 ms   1.00×
    ninja   ████████████████   3668 ms   1.32×

──────────────────────────────────────────────────────────────────────
VALIDATION RULES & METRICS
──────────────────────────────────────────────────────────────────────
Success Rate:
  ninja : 11/11 scenarios passed in workspace concept
  rbot  : 12/12 scenarios passed in workspace concept
  strace : dilewati (proot terdeteksi — ptrace bentrok)

Legend:
  ★   = Tercepat pada skenario ini
  ×   = Rasio waktu terhadap yang tercepat
  [!] = Pekerjaan berbeda antar tool (waktu tidak setara 1:1, bandingkan dengan hati-hati)
  *   = Tidak di-hard-check pada skenario ini (lihat EXPECTED di blok validasi)

Metrics:
  obj = .o berubah · cc = kompilasi gcc aktual (shim) · lib = lib .a/.so
  bin = binary berubah · arc = arsip .rp berubah

Harapan Correctness (Validation Rules):
  cold  : obj penuh, arc 1, lib 4, bin 2 · noop: semua nol
  leaf  : cc 1 — dependensi compile terbukti via shim
  touch : rbot 0 (content-hash); ninja/make 6 (mtime, dependen a.h)
  sel   : cakupan disamakan — obj cc 2, lib 4, bin 2
  rel   : artefak tar + sha256 dicek sha256sum -c (rbot-only)

Artifacts:
   hasil lengkap : /home/aidomx/projects/tools/rbot/ws.tmp/results.csv
  view data     : /home/aidomx/projects/tools/rbot/ws.tmp/view.csv
  bukti kerja   : /home/aidomx/projects/tools/rbot/ws.tmp/work.log
```

## Perubahan Utama dari Desain Lama
1. **Penggantian Blok `│` yang Repetitif:** 
   Blok `│ summary : ... │ except : ...` yang sebelumnya memakan banyak baris vertikal dikemas menjadi blok `[VALIDATION]` yang padat dan menggunakan format `key=value` yang lebih mudah dipindai mata.
2. **Pembersihan Bar Chart:** 
   Peringatan panjang seperti `! lib berbeda antar tool — bandingkan waktu dengan hati-hati` dihapus dari sebelah bar chart. Cukup diberi tanda `[!]` dan penjelasannya dipindahkan ke bagian **Legend**.
3. **Konsolidasi Footer:** 
   Bagian `benchmark except`, `benchmark rules`, dan `benchmark computate` yang terpisah digabung menjadi satu bagian **VALIDATION RULES & METRICS** yang rapi, menghilangkan duplikasi dan membuat dokumen terlihat lebih profesional.

# Noted

Symbol `-` usahakan mendukung layar kecil, misal cek dengan :

```bash
COLS=$(tput cols 2>/dev/null || echo 80)
# Fallback ke 80 jika tput gagal (misal saat di-pipe)
echo $COLUMNS
```

Jadi baik tabel maupun symbol `-` bisa mengikuti lebar dari layar
