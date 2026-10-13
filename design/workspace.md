# Workspace

Seluruh kinerja workspace untuk saat ini sudah sangat bagus, tapi setelah audit terbaru ada masalah yang ditemukan.

## [√] workspace state

Saat di mode workspace akan menghasilkan `.rbot/workspace` tapi tidak menghasilkan `.rbot/workspace.state` yang dimana ini sangat berguna untuk pengecekan di `src/bootstrap.c` untuk mode no-op yang saat ini `.rbot/build.state` saja yang di cek.

Simulasi struktur mode workspace :

```
/wsproject
  .rbot
    workspace # direktori yang saat ini memang sudah dihasilkan
    workspace.state
```

## [] workspace flatten

> Eksperimen ada di `/tmp/rbot_flat`

Hanya sampingan bukan utama tapi kalau hasilnya lebih baik bisa diterapkan ke source asli.

> Eksekusi: Tidak

## [] workspace build

Saat mode workspace proses build harusnya terjadj didalam direktori workspace bukan didalam per-proyek.

Aturan workspace dari rbot masih sangat ketat, karena percuma jika sebuah projek sudah masuk ke workspace menjalankan per-proyek tidak akan menghasilkan apapun.

Karena workspace dari rbot sudah menyediakan `rbot -w project_name` untuk selektif.

Begitu juga dengan `compile_commands.json` yang harusnya berada di direktori workspace bukan per-proyek.

Logikanya jika sudah menggunakan workspace otomatis akan lebih mudah membukanya langsung di direktori workspace tersebut alih-alih masuk ke proyek masing-masing.

Dan jika `compile_commands.json` tidak ada, text editor akan kesulitan membaca.

Proyek hanya menyediakan source untuk workspace, karena aturan ketat dari workspace untuk saat ini.
