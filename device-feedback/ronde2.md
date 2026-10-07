# Feedback Device — RONDE 2 (ceklist HANDOFF §19.4)

> Diisi agent lokal Termux; aturan main di AGENT-BRIEF-DEVICE.md (ronde 2).

## Lingkungan
- `git pull` commit terakhir yang dipakai: `09462af` ("Ronde 2: shebang-in-container
  svsp, passthrough Android, fix SBARGS/env-i fake-run"), branch
  `arena/a172be3f-brainstorming`, fast-forward bersih dari `11b8db2`.
- `./install.sh`: **GAGAL pertama (RC=1)** — build svsp mati:
  ```
  /data/data/com.termux/files/home/Brainstorming/svsp.c:619:55: error: use of undeclared identifier 'SYS_unlink'
    619 |     if (cls == C_SIDE && (nr == SYS_unlinkat || nr == SYS_unlink)) {
        |                                                       ^~~~~~~~~~~~
  1 error generated.
  [x] build svsp gagal
  ```
  Penyebab: `SYS_unlink` tidak ada di arm64 (persis kelas bug §7) — di-guard di
  baris 383/824 tapi **lupa di baris 619**. **Saya patch LOKAL saja** (bukan
  commit saya, working tree `svsp.c` masih modified & TIDAK ikut di-commit ini):
  ```c
  #ifdef SYS_unlink
      if (cls == C_SIDE && (nr == SYS_unlinkat || nr == SYS_unlink)) {
  #else
      if (cls == C_SIDE && (nr == SYS_unlinkat)) {
  #endif
  ```
  Sesudah patch: `./install.sh` **SUKSES (RC=0)** — shim+svsp+fake-run terpasang,
  quick test dinamis 3.24.2 + isolasi ENOENT hijau. `fake-run` terpasang identik
  dgn repo (fix env-i/SBARGS aktif).

## 1. Trigger svsp rc=0?

- `./apk-doctor --clear-broken` (pra-kondisi): `flag f/s dibersihkan. Backup lama:
  .../installed.bak-doctor.<pid>` → re-check `bersih` (RC=0).
  (Persiapan checklist: `fake-run --svsp apk del --no-scripts attr acl busybox`
  → RC=0; busybox tak bisa dilepas — `not removed due to: busybox: ca-certificates
  alpine-baselayout` — lalu `attr`/`acl` ter-purge.)
- `fake-run --svsp apk add --no-cache acl` → **RC=1** (**KRITERIA LULUS TIDAK TERPENUHI**), output:
  ```
  (1/2) Installing acl-libs (2.3.2-r1)
  (2/2) Installing acl (2.3.2-r1)
  Executing busybox-1.37.0-r31.trigger
  * busybox: 'bin/busybox' is not an absolute path
  ERROR: lib/apk/exec/busybox-1.37.0-r31.trigger: exited with error 1
  1 error; 15.1 MiB in 30 packages
  ```
- `fake-run --svsp apk add --no-cache ca-certificates openssl` → **RC=2**, output:
  ```
  (1/2) Installing ca-certificates (20260909-r0)
  (2/2) Installing openssl (3.5.9-r0)
  Executing busybox-1.37.0-r31.trigger
  * busybox: 'bin/busybox' is not an absolute path
  ERROR: lib/apk/exec/busybox-1.37.0-r31.trigger: exited with error 1
  Executing ca-certificates-20260909-r0.trigger
  2 errors; 15.1 MiB in 30 packages
  ```
  Catatan: "2 errors" = 1 error trigger busybox + **1 SILENT dari flag `f:s` basi**
  yang sudah tertulis oleh transaksi acl sebelumnya (lihat §2). Trigger
  **ca-certificates kini JALAN tanpa `CANNOT LINK /bin/sh`** — wrap §19.2.1
  bekerja untuk kasus itu (di ronde 1 masih error linker bionic).
- Isi `$BASE/lib/apk/exec/` sesudahnya: **KOSONG** ✅ (ls hanya `.`/`..`;
  tidak ada `.orig-svsp` yatim — pembersihan unlink-pasangan bekerja).

### Reproduksi & tangkapan wrapper (bonus, seperti diminta brief)

Probe persisten `#!/bin/busybox sh` di `$BASE/usr/bin/probe-trg.sh` dijalankan
sebagai anak di bawah svsp (`fake-run --svsp /bin/busybox sh -c '...'`), file TIDAK
dihapus apk → bisa dibaca:

```
#!/system/bin/sh
# svsp-shebang-wrapper v1 (dibuat otomatis oleh svsp; jangan diedit)
exec "/data/data/com.termux/files/home/alpine-rootfs/lib/ld-musl-patched.so.1" "/data/data/com.termux/files/home/alpine-rootfs/bin/busybox" "sh" "/data/data/com.termux/files/home/alpine-rootfs/usr/bin/probe-trg.sh.orig-svsp" "$@"
```

→ bentuk wrapper **benar** (absolut, arg shebang `sh` utuh, `"$@"` diteruskan,
`.orig-svsp` berisi isi asli). Isi asli jalan (echo argv0 absolut, rc=0).

TAPI begitu script memanggil `/bin/busybox --install -s` (inti trigger busybox):
`busybox: 'bin/busybox' is not an absolute path` → **RC=1**, deterministik.
Pembanding: `fake-run --svsp /bin/busybox --install -s` **langsung (top-level) → RC=0**,
dan `fake-run /bin/busybox --install -s` (jalur shim) → **RC=0**. Jadi error hanya
muncul dalam rantai wrapper (bionic sh → loader patched → busybox). Dugaan belum
terverifikasi: yang dilihat busybox utk path dirinya (`/proc/self/exe`/argv) jadi
relatif `bin/busybox` — perlu di-strace-free (pakai SVSP_DEBUG) di sisi Arena.

## 2. "1 error" tidak kembali

- `fake-run apk add busybox` → **RC=1**, output (TANPA baris error — SILENT):
  ```
  1 error; 15.1 MiB in 30 packages
  ```
  Sumbernya flag `f:s` busybox yang ditulis ulang SETIAP trigger svsp gagal —
  rantai §18.2a masih hidup selama butir 1 gagal.
- `./apk-doctor` sesudahnya:
  ```
  apk-doctor: paket dgn flag 'f'(broken_files)/'s'(broken_script) basi:
    busybox                      f:s
  RC=1
  ```
- Setelah `./apk-doctor --clear-broken` (backup `installed.bak-doctor.31152`),
  transaksi shim DENGAN trigger penuh: `fake-run apk fix --reinstall busybox` →
  **RC=0 bersih**:
  ```
  (1/1) Reinstalling busybox (1.37.0-r31)
    Executing busybox-1.37.0-r31.post-upgrade
  Executing busybox-1.37.0-r31.trigger
  OK: 15.1 MiB in 30 packages
  ```
- `./apk-doctor` akhir: `bersih — tidak ada flag broken_files/broken_script di DB.` RC=0.
  → mekanisme obat tetap bekerja; "1 error" hanya kembali karena flag basi yang
  ditulis ulang oleh kegagalan trigger svsp (butir 1).

## 3. Regresi fake-run

- `fake-run /bin/busybox sh -c 'echo SBARGS-ok'` → output `SBARGS-ok`, **RC=0** ✅
  (regresi SBARGS dari ce108b9 beres — arg shebang tak lagi hilang).
- Uji trigger manual via fake-run (§19.4 baris terakhir):
  - Persis resep (`env -i APK_SCRIPT=... bash fake-run --ldpreload <trigger> /bin`,
    TANPA HOME) → **RC=1**: `fake-run: interpreter tak ada: /bin/busybox`.
    Penyebab: fix env-i memakai `: "${HOME:=$PREFIX/home}"` → base jadi
    `$PREFIX/home/alpine-rootfs` (TIDAK ADA; home Termux =
    `/data/data/com.termux/files/home`) → base tak ditemukan.
  - Dengan `HOME` eksplisit + trigger di dalam wadah
    (`env -i HOME=$HOME APK_SCRIPT=trigger APK_PACKAGE=busybox bash fake-run
    --ldpreload $BASE/lib/apk/exec/test.trigger /bin`) → **RC=0, output bersih** ✅
    (env-i + SBARGS + shebang shim semua bekerja; trigger `--install -s` sukses
    di jalur shim).
  - Catatan: trigger asli ada di `/data/...` (host tmp) tak bisa dipakai langsung
    via jalur shim — `fake-run /bin/busybox test -e /data/data/.../file` → RC=1
    (path `/data/*` host di-rewrite ke dlm base). svsp sudah passthrough `/data`
    (§19.2.2); **shim (libfakeroot.c) belum**.

## Temuan lain / pesan aneh

1. **Build device gagal tanpa patch** (`SYS_unlink` tak di-guard di svsp.c:619) —
   lihat Lingkungan. Patch lokal saya, belum di-commit; silakan terapkan resmi.
2. **Kriteria lulus utama belum terpenuhi**: trigger busybox di bawah wrapper svsp
   gagal `--install -s` dengan pesan baru `'bin/busybox' is not an absolute path`
   (rc=1); dulu rc=127 ENOENT — jadi wrap menggerakkan eksekusi (kemajuan), tapi
   path yang dilihat busybox masih salah.
3. `lib/apk/exec/` selalu **kosong** sesudah transaksi (kriteria pendamping ✅),
   tak ada `.orig-svsp` yatim; `apk-doctor --clear-broken` juga membersihkan
   backup/wrap yatim sesuai §19.3.
4. Default `HOME`/`PREFIX` fake-run (`$PREFIX/home`) tak cocok dgn layout device —
   resep `env -i` murni tetap gagal meski "tahan env-i" sudah ditambahkan.
5. Jalur shim belum passthrough `/data` (svsp sudah) → uji file host via fake-run
   tak bisa.
6. Tidak ada warning xattr/"failed to preserve" apa pun di seluruh ronde ini;
   trigger ca-certificates bebas error linker (CANNOT LINK hilang ✅).
7. Lock apk (RC=99, §19.2.6) tidak kena sama sekali di ronde ini.

## Usul fix berikutnya

1. **(utama)** Benahi `'bin/busybox' is not an absolute path` pada rantai wrapper
   svsp — lokasi pasti: `busybox --install` (butuh path absolut dirinya) hanya
   gagal setelah dieksekusi via wrapper `/system/bin/sh` → loader → busybox;
   top-level svsp & jalur shim RC=0. Kriteria lulus ronde 3 = `--svsp apk add
   --no-cache acl` rc=0 murni.
2. Guard `#ifdef SYS_unlink` di svsp.c:619 (patch di Lingkungan) supaya
   `./install.sh` device jalan mulai dari checkout bersih.
3. Default `HOME` fake-run: arahkan ke `/data/data/com.termux/files/home`
   (atau pilih base yang benar-benar ada) supaya resep `env -i` §19.4 jalan
   tanpa tambahan variabel.
4. Shim: passthrough `/data` agar konsisten dgn svsp §19.2.2 (uji manual & artefak
   host bisa diakses).
5. Putus rantai "1 error" silent di sumbernya: saat trigger gagal karena
   environment wrapper, jangan persist flag `f:`/`s:` (atau auto-clear pasca
   transaksi) — selama butir 1 gagal, setiap transaksi berikutnya masih
   tercemar 1 error sunyi.
