# Desain Bootstrap & Dispatching CLI `rbot`

## Tujuan
Memisahkan logika parsing CLI, mutasi environment, dan evaluasi state dari eksekusi perintah. 
`rbotRun` saat ini adalah fungsi monolitik yang melakukan parsing argumen, mutasi direktori (`chdir`), konversi file (`-xf`), dan dispatching sekaligus. 

Desain ini mengusulkan pemisahan menjadi tiga fase yang jelas:
1. **Parse:** Membaca `argv` dan mengekstrak state ke dalam struct `Bootstrap`.
2. **Apply:** Melakukan mutasi environment (chdir, konversi file) dan memetakan state cache (`mmap`).
3. **Dispatch:** `rbotRun` bertindak murni sebagai *router* yang memanggil handler yang tepat.

## Prinsip Inti
- **`rbotRun` adalah Consumer Murni:** Tidak boleh ada logika parsing `argv`, `strcmp` bercabang, atau mutasi filesystem di dalam `rbotRun`. Ia hanya membaca `Bootstrap` dan melakukan `switch`.
- **Single Source of Truth:** Struct `Bootstrap` adalah satu-satunya representasi state CLI setelah parsing.
- **Enum over String:** Dispatching menggunakan `enum Command`, bukan `strcmp`, untuk performa (jump table) dan keamanan tipe (compiler warning jika ada case yang terlewat).

## Struktur Data

### 1. Perintah (`Command`)
```c
typedef enum {
    CMD_NONE = 0,
    CMD_INIT,
    CMD_BUILD,
    CMD_CLEAN,
    CMD_HELP,
    CMD_VERSION,
    CMD_RELEASE,
    CMD_PROFILE,
    CMD_INTERACTIVE_INIT
} Command;
```

### 2. Status Build & Cache (`BootstrapStatus`)
Struct ini mengevaluasi state build secara cepat (menggunakan `mmap` jika memungkinkan) untuk mendukung *fast-path* (seperti `noop` atau `cold`).

```c
typedef struct {
    bool cold;       // True jika ini adalah cold build (tidak ada state sebelumnya)
    bool noop;       // True jika tidak ada yang perlu di-build (fingerprint cocok)
    bool dirty;      // True jika ada perubahan pada Buildfile atau source
    bool has_state;  // True jika file state (.rbot/build.state) berhasil dibaca
    
    // Pointer ke memory-mapped state (jika mmap diaktifkan)
    void *state_map; 
    size_t state_size;
    
} BootstrapStatus;
```

### 3. Struct Utama (`Bootstrap`)
```c
typedef struct {
    // Perintah utama
    Command cmd;
    
    // Opsi Global
    const char *buildfile;  // Default: "Buildfile"
    int jobs;               // Default: cpuCount()
    
    // Flags
    bool want_workspace;
    bool interactive;       // init -p
    bool generate_compdb;   // -g compdb
    
    // Workspace specific
    const char *ws_only;    // -w <nama>
    const char *ws_rel_sel; // -- key=value
    
    // Profile specific
    int profile_argc;
    const char **profile_argv;
    
    // Konversi Ninja (-xf / -xcf)
    const char *conv_file;
    bool conv_keep;         // true jika -xcf, false jika -xf
    
    // Status Evaluasi (Diisi pada Fase Apply)
    BootstrapStatus status; 
    
} Bootstrap;
```

## Fase 1: Parsing (`bootstrapParse`)

```c
int bootstrapParse(int argc, const char *argv[], Bootstrap *bs);
```

**Tanggung Jawab:**
- Menginisialisasi `bs` dengan nilai default.
- Melakukan loop `argv` untuk mengekstrak opsi (`-w`, `-f`, `-j`, `-xf`, `--`, dll).
- Mengidentifikasi `Command` utama dan menyimpannya di `bs->cmd`.
- **Aturan Profile:** Jika menemukan `profile`, simpan sisa `argv` ke `bs->profile_argc` dan `bs->profile_argv`, lalu hentikan parsing global.
- **Validasi:** Memeriksa kombinasi opsi yang tidak valid dan mengembalikan kode error + pesan yang jelas.
- **Batasan:** Tidak boleh melakukan mutasi environment (tidak ada `chdir`, tidak ada `fopen`, tidak ada `mmap`).

## Fase 2: Persiapan Environment & State (`bootstrapApply`)

```c
int bootstrapApply(Bootstrap *bs);
```

**Tanggung Jawab:**
- Menangani mutasi filesystem dan environment berdasarkan state yang sudah diparsing.
- **`-f` Path Resolution:** Jika `buildfile` mengandung direktori, lakukan `fsSetCwd` dan perbarui `bs->buildfile` menjadi nama file polos.
- **Konversi Ninja (`-xf` / `-xcf`):** Membaca file `.ninja`, mengonversinya menjadi `Buildfile` (atau file sementara `XF_TMP`), dan menangani konfirmasi interaktif.
- **Evaluasi State & `mmap`:** 
  1. Cek keberadaan `.rbot/build.state`. Jika tidak ada, set `bs->status.cold = true`.
  2. Jika ada, gunakan `mmap` (atau `MapViewOfFile` di Windows via `portability.h`) untuk memetakan file state ke `bs->status.state_map` sebagai *Read-Only*.
  3. Bandingkan fingerprint konfigurasi saat ini dengan state di memori. Set `bs->status.noop` atau `bs->status.dirty` berdasarkan hasil perbandingan.

## Fase 3: Dispatching (`rbotRun`)

```c
int rbotRun(const Bootstrap *bs);
```

**Tanggung Jawab:**
- Bertindak sebagai *dispatcher* murni menggunakan `switch(bs->cmd)`.
- Menggunakan `bs->status` untuk *fast-path* (misal: langsung keluar jika `noop`).

**Contoh Implementasi:**
```c
int rbotRun(const Bootstrap *bs) {
    // 1. Handle Profile (Non-mutating, standalone)
    if (bs->cmd == CMD_PROFILE) {
        return cmdProfile(bs->profile_argv, bs->profile_argc);
    }

    // 2. Fast-Path untuk No-Op (Tidak ada yang berubah)
    if (bs->cmd == CMD_BUILD && !bs->status.cold && bs->status.noop) {
        printf("> rbot      : nothing to do (noop)\n");
        return 0;
    }

    // 3. Handle Workspace
    if (bs->want_workspace || workspaceAutoDetect()) {
        return handleWorkspaceRun(bs);
    }

    // 4. Dispatch Perintah Standar
    switch (bs->cmd) {
        case CMD_NONE:
        case CMD_BUILD:
            if (bs->generate_compdb) return cmdCompdbGenerate(bs->buildfile);
            return cmdBuild(bs->jobs, bs->buildfile, &bs->status);
            
        case CMD_INIT:
            return bs->interactive ? cmdInteractiveInit() : cmdInit(bs->buildfile);
            
        case CMD_CLEAN:
            return cmdClean(bs->buildfile);
            
        case CMD_HELP:
            showHelp();
            return 0;
            
        // ... case CMD_VERSION, CMD_RELEASE, dst ...
        
        default:
            fprintf(stderr, "rbot: unknown command\n");
            showHelp();
            return 1;
    }
}
```

## Alur Eksekusi Baru di `main.c`

```c
int main(int argc, const char *argv[]) {
    Bootstrap bs = {0};
    
    // Fase 1: Parse Argumen
    int rc = bootstrapParse(argc, argv, &bs);
    if (rc != 0) return rc;
    
    // Fase 2: Apply Environment & Map State
    rc = bootstrapApply(&bs);
    if (rc != 0) return rc;
    
    // Fase 3: Dispatch & Execute
    rc = rbotRun(&bs);
    
    // Cleanup (Unmap mmap, hapus file sementara -xf, free strdup)
    bootstrapCleanup(&bs); 
    
    return rc;
}
```

## Aturan dan Batasan Desain

1. **Kepemilikan Memori:** `Bootstrap` tidak boleh melakukan `malloc` untuk string opsional jika tidak perlu. Gunakan pointer ke `argv` yang sudah ada. Jika harus `strdup`, dokumentasikan bahwa `bootstrapCleanup` harus dipanggil di akhir.
2. **Cross-Platform mmap:** Penggunaan `mmap` untuk state harus dibungkus di `portability.h` (menggunakan `CreateFileMapping` untuk Windows, dan `mmap` untuk POSIX).
3. **Read-Only Mapping:** State build harus di-*map* sebagai **Read-Only** (`PROT_READ` / `FILE_MAP_READ`). Modifikasi state hanya boleh dilakukan oleh modul `build` yang sah setelah proses build selesai.
4. **Fallback Aman:** Jika `mmap` gagal (misal file terlalu besar atau dibatasi OS), `bootstrapApply` harus memiliki *fallback* untuk membaca file secara tradisional, namun tetap mengisi `BootstrapStatus` dengan benar.
5. **Ekstensibilitas:** Menambahkan perintah baru hanya memerlukan: menambah 1 `enum` di `Command`, dan 1 `case` di `switch` `rbotRun`. Tidak perlu menyentuh logika parsing global jika tidak ada opsi baru.

## Manfaat Refactoring Ini
1. **Kode `rbotRun` menyusut drastis** (dari ~250 baris menjadi ~50 baris).
2. **Membaca alur program menjadi instan:** Developer baru bisa langsung melihat perintah apa saja yang didukung `rbot` hanya dengan melihat `switch` di `rbotRun`.
3. **Performa No-Op Instan:** Mengecek `noop` tidak lagi membutuhkan pembacaan file disk yang lambat, melainkan hanya perbandingan memori dari `mmap`.
4. **Mencegah Regresi:** Memisahkan `bootstrapApply` (mutasi) dari `bootstrapParse` (parsing) mencegah bug di mana file terkonversi atau direktori berubah sebelum argumen selesai divalidasi.
