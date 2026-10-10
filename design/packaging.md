# Packaging `pack.*` — desain & spesifikasi

Status: **selesai diimplementasi & teruji** (2 Okt 2026). 

## Tujuan

- Hasil build bisa dikemas: tarball (tar.gz/xz/bz2/tar) dan `.deb` — tanpa
  dependensi `dpkg-deb` maupun binary `ar`, agar portabel sampai MSVC.
- `.deb` dirakit **manual**: header ar ditulis di C; staging control/data via
  `tar`+`gzip` shell yang tersedia di semua environment target (Termux
  termasuk).

## Aktifasi & bentuk output

- `pack.files` / `pack.output` menandai pack aktif (`pack.requested`).
- Section `pack` mandiri di Buildfile, ATAU key bare via
  `projects.<n>.pack as pack` di workspace (prefix `projects.<n>.` di-strip
  saat sintesis).
- Proyek tanpa sources otomatis `output.binary=false` (pack-only); proyek
  binary tetap bisa pack (hybrid) — kedua jalur memanggil pack setelah build
  sukses.
- `pack.format = deb` **menambah** `.deb` di samping tarball (dua artefak);
  bila template output berakhiran `.deb` → mode deb-only (tarball tidak
  dibuat).
- Template nama: `{name} {version} {os} {arch}`. Ekstensi tar dikenal
  (`.tar.gz/.tar.xz/.tar.bz2/.tgz/.tar`) diganti `.deb` untuk nama deb.

## Key

| Key | Arti | Default |
|---|---|---|
| `pack.name` | nama paket | o.binaryName (dari finalize) |
| `pack.version` | versi | `0.0.0` |
| `pack.output` | template artefak | `dist/{name}-v{version}.tar.gz` |
| `pack.compress` | `gzip\|none\|xz\|bz2` (`"bzip2"` → `bz2`) | dari ekstensi output |
| `pack.checksum` | `sha256` | — |
| `pack.format` | `tar\|none\|deb` (lain → error) | `tar` |
| `pack.files` | entri yang dikemas (dir/file) | — |
| `pack.deb.*` | `maintainer`, `description`, `prefix`, `architecture` | `rbot <rbot@localhost>`, pack.name, `/usr/local`, host-derived |

## Deb

- Pemetaan: tiap entri `pack.files` dipasang di `<install_prefix>/<entri>`.
- Mode file: `0755` untuk komponen pertama `bin/` atau `*.so`; selain itu bit
  +x sumber dipertahankan (`fsGetMode`).
- Control: `Package`/`Version`/`Architecture`
  (`amd64/arm64/i386/riscv64/armhf/all`, override `pack.deb.architecture`) /
  `Maintainer`/`Installed-Size`/`Description`.
- Layout ar: `!<arch>\n` + member `debian-binary` (`"2.0\n"`),
  `control.tar.gz`, `data.tar.gz`; header member 60 byte
  `%-16.16s%-12ld%-6s%-6s%-8s%-10lld\`\n`; member ganjil di-pad `\n`.
- Staging di `.rbot/pack/` — bukan `/tmp`, aman di Termux
  (lihat architecture.md § Lingkungan khusus).

## Freshness & checksum

- Rebuild hanya bila entri terbaru dari `pack.files` (folder di-walk rekursif,
  mtime direktori ikut dihitung) > mtime artefak; aturan sama untuk file
  checksum.
- Checksum: SHA-256 murni C (FIPS 180-4, `pack_sha256.c`); file berisi
  `<hex>  <basename>` (dua spasi, kompatibel `sha256sum -c` — terverifikasi
  cocok coreutils).

## clean

Menghapus staging `.rbot/pack`; **tidak** menyentuh `dist/` (bisa dirujuk
`embedded.*` proyek lain).

## Hasil uji (2 Okt 2026)

- Workspace alias+bare → `dist/wspkg-v3.1.4-linux-aarch64.deb` valid
  (`dpkg-deb --contents`: `usr/local/app/bin/app` 0755).
- Pack-only section form, prefix `/opt/solo`.
- Hybrid binary+pack + `sha256sum -c` OK.
- No-op up-to-date; touch → rebuild; file hilang → pesan error jelas;
  clean menghapus staging, bukan `dist/`.
- Self-host rc=0 (29 file); build 0 warning; regresi `~/rupa/v0.2.2`
  61 PASS / 0 FAIL.

## Catatan terbuka

- Template tanpa `{version}` → nama file tanpa versi (diamankan, bukan error).
- Windows/MSVC belum diuji nyata (jalur `_chmod` sudah disiapkan).
- Ide lanjut: `pack.deb.depends`/`section`, mode deb-only eksplisit via key,
  penandatanganan `.deb`.
