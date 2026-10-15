# Design ws output

```
▌ rbot workspace benchmark · 2 tools · 1 run · median
────────────────────────────────────────────────────
Workspace : rupamod (40 .rp) · core (1 .c) · bade (1 .c)
Graph     : bade depends_on rupamod (embed), core <-> bade (circular)
Directory : /home/aidomx/projects/tools/rbot/ws.tmp/wstest

Versions  :
  rbot  : dev git:v0.2.3-17-g65169f7-dirty bin:5be295
  ninja : 1.12.1
  gcc   : 14.2.0 · aarch64

Legend :
  ★    : Tercepat pada skenario ini
  ×    : Rasio waktu terhadap yang tercepat
  [!]  : Pekerjaan berbeda antar tool (waktu tidak setara 1:1, bandingkan dengan hati-hati)
  *    : Tidak di-hard-check pada skenario ini (lihat EXPECTED di blok validasi)

Metrics :
  obj   : .o berubah
  cc    : kompilasi C aktual (shim)
  lib   : lib .a/.so
  bin   : binary berubah
  arc   : arsip .rp berubah

Correctness (Validation Rules) :
  cold      : obj penuh, arc 1, lib 4, bin 2 · noop: semua nol
  leaf/bade : cc 1 — dependensi compile terbukti via shim
  mod       : cc 0 — arsip berubah, kompilasi tidak perlu ulang
  touch     : rbot 0 (content-hash); ninja/make 6 (mtime, dependen a.h)
  hedit     : SEMUA tool obj cc 6 (edit konten header pusat), lib 4, bin 2
  sel       : cakupan disamakan (SEL_TARGETS) — obj cc 2, lib 4, bin 2
  dep       : cakupan disamakan — obj cc 1, lib 4, bin 2
  rel       : artefak tar + sha256 dicek sha256sum -c (rbot-only)

[Execution Stream]
> [rbot  cold    ]  2964ms | obj:7 cc:6 lib:4 bin:2
> [ninja cold    ]  1830ms | obj:7 cc:6 lib:4 bin:2
> [rbot  noop    ]    59ms | obj:0 cc:0 lib:0 bin:0
> [ninja noop    ]    48ms | obj:0 cc:0 lib:0 bin:0
> [rbot  leaf    ]  1482ms | obj:1 cc:1 lib:2 bin:2
> [ninja leaf    ]  1143ms | obj:1 cc:1 lib:2 bin:2
> [rbot  bade    ]  1370ms | obj:1 cc:1 lib:2 bin:2
> [ninja bade    ]  1167ms | obj:1 cc:1 lib:2 bin:2
> [rbot  mod     ]  1353ms | obj:1 cc:0 lib:2 bin:2
> [ninja mod     ]  1041ms | obj:1 cc:0 lib:2 bin:2
> [rbot  touch   ]   169ms | obj:0 cc:0 lib:0 bin:0
> [ninja touch   ]  1608ms | obj:6 cc:6 lib:4 bin:2
> [rbot  hedit   ]  2194ms | obj:6 cc:6 lib:4 bin:2
> [ninja hedit   ]  1608ms | obj:6 cc:6 lib:4 bin:2
> [rbot  sel     ]  1898ms | obj:2 cc:2 lib:4 bin:2
> [ninja sel     ]  1375ms | obj:2 cc:2 lib:4 bin:2
> [rbot  dep     ]  1880ms | obj:2 cc:1 lib:4 bin:2
> [ninja dep     ]  1393ms | obj:2 cc:1 lib:4 bin:2
> [rbot  libtouch]   136ms | obj:0 cc:0 lib:0 bin:0
> [ninja libtouch]  1380ms | obj:2 cc:2 lib:4 bin:2
> [rbot  libhdr  ]  1908ms | obj:2 cc:2 lib:4 bin:2
> [ninja libhdr  ]  1271ms | obj:2 cc:2 lib:4 bin:2
> [rbot  rel     ]   228ms | obj:0 cc:0 lib:0 bin:0

[Detailed Validation Log]

tool | execution | status
rbot | cold      | In accordance

✔ selesai dalam 0:55

BENCHMARK REPORT (Median)
────────────────────────────────────────────────────

COLD build dari nol, 3 proyek
  ★ ninja  ██████████░░░░░░   1830 ms  1.00×
    rbot   ████████████████   2964 ms  1.62×

NOOP tanpa perubahan
  ★ ninja  █████████████░░░     48 ms  1.00×
    rbot   ████████████████     59 ms  1.23×

LEAF edit 1 source core (bade tak boleh ikut)
  ★ ninja  ████████████░░░░   1143 ms  1.00×
    rbot   ████████████████   1482 ms  1.30×

BADE edit 1 source bade
  ★ ninja  ██████████████░░   1167 ms  1.00×
    rbot   ████████████████   1370 ms  1.17×

MOD edit 1 .rp rupamod -> arsip -> bade
  ★ ninja  ████████████░░░░   1041 ms  1.00×
    rbot   ████████████████   1353 ms  1.30×

TOUCH touch include/a.h pusat, konten sama
  ★ rbot   ██░░░░░░░░░░░░░░    169 ms  1.00× [!]
    ninja  ████████████████   1608 ms  9.51× [!]

HEDIT edit isi include/a.h pusat (konten berbeda)
  ★ ninja  ████████████░░░░   1608 ms  1.00×
    rbot   ████████████████   2194 ms  1.36×

SEL edit core+bade, build selektif cakupan -w core
  ★ ninja  ████████████░░░░   1375 ms  1.00×
    rbot   ████████████████   1898 ms  1.38×

DEP edit .rp+core, build selektif cakupan -w bade
  ★ ninja  ████████████░░░░   1393 ms  1.00×
    rbot   ████████████████   1880 ms  1.35×

LIBTOUCH touch include/ext.h (header internal), konten sama
  ★ rbot   ██░░░░░░░░░░░░░░    136 ms  1.00× [!]
    ninja  ████████████████   1380 ms 10.15× [!]

LIBHDR edit isi include/ext.h (header internal, include relatif)
  ★ ninja  ███████████░░░░░   1271 ms  1.00×
    rbot   ████████████████   1908 ms  1.50×

REL release tar+sha256 (rbot-only, diukur terpisah)
  ★ rbot   ████████████████    228 ms  1.00×

Success Rate:
  rbot  : 12/12 scenarios passed in workspace concept
  ninja : 11/11 scenarios passed in workspace concept
  strace : dilewati (proot terdeteksi — strace (ptrace) bentrok & pernah force close)

Artifacts :
  hasil lengkap : /home/aidomx/projects/tools/rbot/ws.tmp/results.csv
  view data     : /home/aidomx/projects/tools/rbot/ws.tmp/view.csv
  bukti kerja   : /home/aidomx/projects/tools/rbot/ws.tmp/work.log
  ```
