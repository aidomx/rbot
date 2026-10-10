# CLI rbot: self-upgrade dan self-uninstall

`rbot` adalah build tool sistem, bukan package manager. Karena itu, CLI
rbot tidak menyediakan registry package atau mekanisme untuk memasang,
memperbarui, dan menghapus dependency eksternal. Pengelolaan dependency
sistem tetap menjadi tanggung jawab package manager OS; source dependency
proyek tetap dikelola oleh proyek dan penggunanya.

Untuk saat ini, perintah pengelolaan instalasi global yang dirancang khusus
hanya:

- `rbot self-upgrade` — memperbarui program rbot itu sendiri.
- `rbot self-uninstall` — menghapus instalasi rbot yang dikelola installer
  resmi beserta state global milik rbot.

## 1. `rbot self-upgrade`

```bash
rbot self-upgrade
```

Perintah ini hanya memperbarui rbot, bukan dependency proyek atau package
sistem. Tidak diperlukan registry rbot terpisah. Informasi versi terbaru
diambil langsung dari GitHub Releases API repository resmi:

`https://api.github.com/repos/aidomx/rbot/releases/latest`

### Alur

1. Ambil metadata release terbaru dari GitHub.
2. Validasi respons dan `tag_name`; respons yang hilang atau tidak valid
   dianggap gagal.
3. Bandingkan tag release dengan versi yang tertanam pada binary aktif.
4. Jika versinya sama, laporkan bahwa rbot sudah terbaru dan berhenti.
5. Jika berbeda, gunakan installer distribusi resmi untuk memasang release
   yang sesuai dengan tag tersebut.
6. Laporkan hasil installer. Jika pemeriksaan, pengunduhan, atau instalasi
   gagal, jangan menyatakan upgrade berhasil.

### Batasan dan keamanan

- Jika binary berjalan dari working tree pengembangan, jangan menimpanya
  otomatis. Beri instruksi untuk memperbarui source dan membangun ulang.
- Jika binary berada pada lokasi custom yang tidak dikenali sebagai target
  instalasi resmi, jangan mengganti binary secara otomatis; tampilkan
  instruksi manual.
- Versi/tag harus diteruskan sebagai data yang divalidasi, bukan disisipkan
  sebagai perintah shell mentah.
- Pemeriksaan GitHub yang gagal tidak boleh mengubah instalasi lokal.
- Validasi artefak, pergantian binary, dan pemulihan jika instalasi gagal
  merupakan tanggung jawab installer resmi.
- **Windows:** upgrade otomatis hanya untuk binary resmi di
  `%LOCALAPPDATA%\rbot\rbot.exe`. Karena executable yang sedang berjalan
  terkunci, rbot membuat helper PowerShell sementara di direktori temp,
  menjalankannya di console terpisah, lalu keluar. Helper menunggu proses rbot
  berakhir, memeriksa GitHub Releases API, dan menjalankan `install.ps1`
  dari tag release yang sudah divalidasi. Lokasi custom dan source checkout
  ditolak; jika helper tidak dapat dijalankan, instalasi tidak diubah.
- **POSIX:** upgrade menggunakan `install.sh` resmi setelah pemeriksaan versi
  selesai. Binary di lokasi custom atau source checkout tidak diganti otomatis.

## 2. `rbot self-uninstall [--yes|-y]`

```bash
rbot self-uninstall
rbot self-uninstall --yes
```

Perintah ini menghapus instalasi rbot yang dikelola installer resmi dan
state global rbot. Perintah ini tidak menghapus dependency sistem, source
proyek, maupun state build setiap proyek.

### Cakupan

- Binary rbot hanya jika lokasinya dikenali sebagai target instalasi yang
  dikelola installer resmi dan penghapusannya aman.
- Direktori state global `$RBOT_HOME`, dengan default `~/.rbot/`.
- Hanya file dan direktori yang secara eksplisit menjadi milik instalasi
  global rbot.

`RBOT_HOME` dapat digunakan untuk mengubah lokasi state global. Sebelum
menghapusnya, rbot harus memvalidasi path agar tidak menerima root direktori,
`.` atau `..`, dan tidak mengikuti symlink untuk menghapus data di luarnya.

### Konfirmasi

- Pada terminal interaktif (TTY), minta konfirmasi sebelum menghapus.
- Jika input bukan terminal, tolak secara default. Pengguna harus memberi
  `--yes` atau `-y` secara eksplisit.
- Jika binary aktif tidak dapat dihapus langsung pada platform tertentu,
  gunakan mekanisme tertunda yang aman hanya jika platform mendukungnya;
  jika tidak, hapus state yang aman dihapus dan jelaskan langkah manual
  yang tersisa.
- Jangan melaporkan penghapusan penuh jika binary atau state yang dimiliki
  rbot masih tertinggal.

Contoh konfirmasi:

```text
This will remove rbot and its globally managed state (~/.rbot/).
Are you sure? [y/N]
```

### Yang tidak boleh dihapus

- `project/.rbot/`, karena merupakan state build proyek.
- `project/third/` atau direktori source lain yang disediakan pengguna.
- Binary atau file yang kepemilikannya tidak dapat dipastikan sebagai milik
  instalasi rbot.
- Package sistem yang dipasang melalui `apt`, `pacman`, `dnf`, `brew`, atau
  package manager OS lainnya.

## 3. Prinsip desain

1. **Tanggung jawab terbatas:** rbot mengelola dirinya sendiri, bukan package
   sistem atau dependency eksternal.
2. **Tanpa registry package:** versi rbot diperiksa langsung melalui GitHub
   Releases API; tidak diperlukan metadata registry tersendiri.
3. **Kepemilikan eksplisit:** hanya hapus atau ganti resource yang diketahui
   milik instalasi rbot.
4. **Aman saat gagal:** kegagalan pemeriksaan versi atau upgrade tidak boleh
   merusak instalasi yang sedang berjalan.
5. **Non-interaktif aman:** jangan menunggu input pada stdin non-TTY; operasi
   uninstall membutuhkan `--yes` atau `-y`.
6. **Validasi, bukan tebakan:** validasi versi dan path sebelum bertindak,
   dan jangan mengklaim operasi berhasil sebelum hasilnya dipastikan.
