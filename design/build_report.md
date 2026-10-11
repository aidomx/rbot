# Build report

## No-op report
```
> Project   : rupamod  (archive pass)

> Summary
Archive  : ../modules/rupa_modules.tar
Status   : Success

> Project   : bade  (library pass)
> Library   : lib/libbade.a (up-to-date)
> Library   : lib/libbade.so (up-to-date)

> Summary
Library  : lib/libbade.a (432.9KB)
Library  : lib/libbade.so (271.4KB)
Compiled : 0
Skipped  : 36
Status   : Success

> Project   : core  (library pass)
> Library   : lib/libcore.a (up-to-date)
> Library   : lib/libcore.so (up-to-date)

> Summary                                       Library  : lib/libcore.a (1168.4KB)
Library  : lib/libcore.so (713.9KB)             Compiled : 0
Skipped  : 176
Status   : Success
                                                > Project   : rupamod  (skip: selesai di fase library)

> Project   : bade                              > Build with cached state (fingerprint unchanged)
> Linking   : bin/bade (up-to-date)             > Library   : lib/libbade.a (up-to-date)
> Library   : lib/libbade.so (up-to-date)
> Summary                                       Target   : bin/bade
Size     : 626.3KB                              Library  : lib/libbade.a (432.9KB)
Library  : lib/libbade.so (271.4KB)
Compiled : 0
Skipped  : 36
Status   : Success
                                                > Project   : core
> Build with cached state (fingerprint unchanged)
> Linking   : bin/rupa (up-to-date)             > Library   : lib/libcore.a (up-to-date)
> Library   : lib/libcore.so (up-to-date)
                                                > Summary
Target   : bin/rupa                             Size     : 838.2KB
Library  : lib/libcore.a (1168.4KB)             Library  : lib/libcore.so (713.9KB)
Compiled : 0                                
Skipped  : 176
Status   : Success
> Release    : rupa (workspace, format deb)     > Package   : dist/rupa-v0.2.2.tar.gz (up-to-date)
> Checksum  : dist/rupa-v0.2.2.tar.gz.sha256 (up-to-date)
> Package   : dist/rupa-v0.2.2.deb (up-to-date)
> Checksum  : dist/rupa-v0.2.2.deb.sha256 (up-to-date)
```

Design report di atas memang sudah cukup bagus, tapi kalau kurang jeli/teliti memang gampang salah paham.

Design baru menggunakan symbol | (unicode) fallback > kalau tidak mendukung.

```
# cold build
| Project : rupamod
| Fase    : archive
| Summary :
    Archive  : ../modules/rupa_modules.tar
    Skipped  : true # hanya muncul di no-op
    Status   : Success
```
 
