# design/

Dokumen desain rbot.

> **Catatan provenance (3 Okt 2026):** direktori `design/` asli hilang bersama
> penghapusan tak sengaja direktori rbot dan tidak ikut dalam backup. Isi di
> sini adalah **rekonstruksi** dari arsitektur kode aktif (`src/`) dan
> keputusan yang tercatat pada sesi kerja — bukan salinan persis dokumen lama.
> Bila ada perbedaan, kode adalah sumber kebenaran.

## Daftar dokumen

| Dokumen | Isi |
|---|---|
| [architecture.md](architecture.md) | Peta modul, alur build, state/cache, kinerja, lingkungan khusus |
| [buildfile.md](buildfile.md) | Semantik Buildfile: dua format, alias, workspace, `-xf`, cache konfigurasi |
| [packaging.md](packaging.md) | Desain packaging `pack.*` (tarball + deb + checksum) |

## Prinsip yang dipegang

- Satu binary, tanpa dependensi build wajib di luar compiler C — bootstrap
  lewat `install.sh --dev` (compiler C langsung, tanpa cmake; Windows:
  `install.ps1 --dev`).
- Buildfile deklaratif; dua format (legacy & `use alias`) didukung bersamaan
  dan tidak saling mengganggu.
- Portabel sampai MSVC: fitur inti tidak bergantung tool UNIX-only
  (contoh: .deb dirakit manual dari C, tanpa `dpkg-deb`).
- Berkas source < 500 baris; modular per subdirektori `src/<tema>/`.
- Termux/proot: hemat syscall; tidak pernah hardcode `/tmp`
  (lihat [architecture.md](architecture.md) § Lingkungan khusus).
