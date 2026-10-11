# Workspace

Seluruh kinerja workspace untuk saat ini sudah sangat bagus, tapi setelah audit terbaru ada masalah yang ditemukan.

## workspace state

Saat di mode workspace akan menghasilkan `.rbot/workspace` tapi tidak menghasilkan `.rbot/workspace.state` yang dimana ini sangat berguna untuk pengecekan di `src/bootstrap.c` untuk mode no-op yang saat ini `.rbot/build.state` saja yang di cek.

Simulasi struktur mode workspace :

```
/wsproject
  .rbot
    workspace # direktori yang saat ini memang sudah dihasilkan
    workspace.state
```

## workspace speed build (cold build)

Masih tahap ide untuk mempercepat proses build yang saat ini masih terasa lambat, tidak perlu di eksekusi.

```
# Buildfile.ws.tmp
src_a = a, b, c, d, e
src_b = f, g, h, i, j

srcs = src_a, src_b
```

Semua source yang dari `Buildfile.ws` akan diratakan terlebih dahulu, ini berguna untuk mempercepat proses source menjadi object, e.g: `a.o`.

`Buildfile.ws.tmp` adalah salinan dari `Buildfile.ws` tetapi sourcenya sudah diratakan, untuk pengaturan lain tetap sama.

### Lalu bagaimana dengan fase library?

Fase library tetap melewati tahap build source menjadi object, karena source sudah menjadi object tugas selanjutnya sebenarnya sederhana.

Lakukan pencocokan source asli dengan object, contoh :

```
src_a/a == build/a
```

Jika source dan object cocok, ambil object tersebut kemudian ratakan kembali :

```
obj_a = a, b, c, d, e
```

Kemudian lanjutkan proses fase library, dan fase binary menjadi sangat cepat karena tidak lagi membuild source menjadi object.

Pembagian fase tetap sama tidak ada yang berubah.
