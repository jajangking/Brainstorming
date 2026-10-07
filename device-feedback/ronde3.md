# Feedback Device — RONDE 3 (checklist HANDOFF §20.4)

> Diisi agent lokal Termux; aturan main di AGENT-BRIEF-DEVICE.md (ronde 3).

## Lingkungan
- Commit yang dipakai: checklist §20.4 dijalankan di `05f71c9`; lalu pull
  `5b80b86` (selftest) dan ULANGI uji penentu — hasil identik. Diff
  `05f71c9..5b80b86` hanya AGENT-BRIEF/HANDOFF/selftest (nol perubahan kode),
  jadi hasil kode berlaku untuk keduanya.
- Patch lokal SYS_unlink sudah dibuang/ditimpa? **ya** — `git checkout
  svsp.c` sebelum pull; guard resmi kini ada di `svsp.c:622-626` (build
  bersih tanpa patch lokal = kriteria lingkungan terpenuhi).
- `./install.sh`: **SUKSES** (RC=0, build svsp + libfakeroot + PT_INTERP OK) —
  catatan: quick-test mencetak `[!] Isolasi: hasil tidak terduga:
  cat: can't open '/system/build.prop': Permission denied` (lihat Temuan 4).

## Kriteria utama
- Persiapan (transaksi harus nyata; acl sempat terpasang sisa ronde 2):
  `fake-run --svsp apk del --no-scripts acl ca-certificates openssl` → rc=0,
  lalu `./apk-doctor --clear-broken` → rc=0 "bersih".
- `fake-run --svsp apk add --no-cache acl` → **rc=1**, output penuh:
  ```
  (1/2) Installing acl-libs (2.3.2-r1)
  (2/2) Installing acl (2.3.2-r1)
  Executing busybox-1.37.0-r31.trigger
  * busybox: 'bin/busybox' is not an absolute path
  ERROR: lib/apk/exec/busybox-1.37.0-r31.trigger: exited with error 1
  1 error; 14.0 MiB in 28 packages
  ```
  → **Kriteria lulus utama TIDAK terpenuhi** (harus rc=0 tanpa baris error).
  Diulang di `5b80b86` (transaksi nyata, acl dipastikan absen via grep db):
  rc=1, error sama persis.
- `fake-run --svsp apk add --no-cache ca-certificates openssl` → **rc=2**,
  output:
  ```
  (1/2) Installing ca-certificates (20260909-r0)
  (2/2) Installing openssl (3.5.9-r0)
  Executing busybox-1.37.0-r31.trigger
  * busybox: 'bin/busybox' is not an absolute path
  ERROR: lib/apk/exec/busybox-1.37.0-r31.trigger: exited with error 1
  Executing ca-certificates-20260909-r0.trigger
  2 errors; 15.1 MiB in 30 packages
  ```
  (ada 1 baris ERROR tapi "2 errors" — kemungkinan error kedua = hitung senyap
  flag f/s basi §18.2a dari transaksi acl sebelumnya; interpretasi, bukan fakta
  langsung.)

## Kriteria lanjutan
- `fake-run apk add busybox` → **rc=1**, output: `1 error; 15.1 MiB in 30
  packages` (senyap, tanpa baris ERROR — pola §18.2a: flag `f:s` basi busybox
  bekas kegagalan svsp dihitung sbg 1 error).
- `./apk-doctor` sesudahnya: **rc=1**, `busybox f:s` — flag basi terkonfirmasi.
- `fake-run --svsp apk add --no-cache attr` → **rc=2**:
  ```
  (1/2) Installing libattr (2.5.2-r2)
  (2/2) Installing attr (2.5.2-r2)
  Executing busybox-1.37.0-r31.trigger
  * busybox: 'bin/busybox' is not an absolute path
  ERROR: lib/apk/exec/busybox-1.37.0-r31.trigger: exited with error 1
  2 errors; 15.3 MiB in 32 packages
  ```
- Isi `$BASE/lib/apk/exec/`: **kosong** (0 entri — tetap bersih walau trigger
  gagal; tidak ada `.orig-svsp` yatim).
- Lanjutan: setelah kegagalan svsp tsb, `fake-run apk del --no-scripts acl`
  ikut **rc=1 senyap** (flag basi) — sesudah `./apk-doctor --clear-broken`
  (backup `installed.bak-doctor.10522`), doctor final **bersih, rc=0**;
  acl terpurge (grep db = 0).

## Shim /data passthrough
- `fake-run /bin/busybox test -e /data/data/com.termux/files/home` →
  **PASSTHROUGH-OK** (rc=0, tanpa output).

## Temuan lain / usul fix berikutnya

### 1. KOREKSI ROOT CAUSE §20.1 (bukti dari device — ini temuan utama)
Hipotesis §20.1 ("busybox baca `/proc/self/exe`, gagal krn akses
`/proc/pid/exe` dibatasi Android") **tidak cocok dengan device**:

- `strings $BASE/bin/busybox | grep -x '/bin/busybox|/proc/self/exe'` →
  hanya `/bin/busybox`; string `/proc/self/exe` **tidak ada** di binary
  busybox Alpine 1.37.0-r31 ini.
- Log `SVSP_DEBUG` (pid satu-satunya utk proses `--install`): notifikasi
  `readlinkat` (nr=78) datang DENGAN PATH `/bin/busybox`
  (`nr=78 [/bin/busybox] -> [$BASE/bin/busybox]`) → `bb_busybox_exec_path`
  build ini = `/bin/busybox`, bukan `/proc/self/exe`.
- Sumber busybox (`libbb/appletlib.c`, diambil dari upstream):
  `busybox = xmalloc_readlink(bb_busybox_exec_path); if (!busybox) { if
  (argv[0][0] != '/') bb_error_msg_and_die("'%s' is not an absolute path",
  argv[0]); busybox = argv[0]; }`
- `$BASE/bin/busybox` = **file reguler** (bukan symlink, `ls -la` = `-rwxr-xr-x`)
  → readlink = EINVAL → selalu NULL. (Tanpa rewrite supervisor pun tetap gagal:
  `/bin/busybox` tidak ada di host Android → ENOENT.) Jadi readlink **tidak
  pernah bisa sukses** untuk path ini di wadah — regardless §20.2.
- Yang menentukan adalah **fallback argv[0]**: di dalam rantai wrapper,
  argv[0] = `bin/busybox` **relatif** (rewrite fallback C_EXEC: buffer argv[0]
  asli cuma 13 byte, path penuh 46 byte tak muat → ditulis relatif) → mati.
  Bukti: top-level `fake-run --svsp /bin/busybox --install -s` → **rc=0**
  (argv[0] = `$BASE/bin/busybox`, absolut).
- Fix §20.2 (`/proc/self/*` CONTINUE) **sendiri terbukti bekerja**: `readlink
  /proc/self/exe` di bawah svsp (proses sh maupun child busybox, exec absolut
  maupun relatif) kini selalu balik absolut — tapi itu jalur yang TIDAK dipakai
  busybox build ini, jadi tak menyelesaikan trigger.

**Usul fix (arah ke Arena):** fallback eksekusi jangan pernah menulis argv[0]
relatif. Opsi termudah: bila path penuh tak muat di buffer argv[0], **biarkan
argv[0] asli** (`/bin/busybox` — sudah absolut & muat), atau tulis path penuh
ke memori anak di alamat lain (process_vm_writev ke stack/ekor env). Dengan
argv[0] absolut, busybox `--install` jalan walau readlink selalu NULL.
(Catatan: argv[0] asli non-absolut hanya bila dipanggil via PATH tanpa
slash — itu pun di Alpine asli juga akan mati, jadi tidak lebih buruk.)

### 2. `./selftest` (5b80b86) — dijalankan 2×, RC=1 keduanya
File mentah: `selftest-2026-10-07.txt` (perintah brief persis) dan
`selftest-2026-10-07-run2.txt` (ulangan setelah acl terpurge, agar shim add =
transaksi nyata). Keduanya: **1 FAIL** — identik:

```
./selftest: line 58: /tmp/.doctor-pre.8475: Permission denied
[FAIL] apk-doctor: ada flag f/s basi —
```

- **Temuan 2a (bug selftest di device):** baris 58 hardcode
  `/tmp/.doctor-pre.$$` — `/tmp` tak tertulis di Termux → redirect gagal →
  `if` selalu masuk else → **FAIL ini deterministik, tak peduli kondisi DB**
  (detail grep juga ikut gagal → pesan kosong). Fix: `mktemp` / `$PREFIX/tmp`.
  Konsekuensi: exit code selftest tak akan pernah 0 di device ini.
- **Temuan 2b (kriteria svsp VAKUUS):** step 5 (`shim: apk add $PKG`) selalu
  mendahului step 6 (`svsp: apk add $PKG`) dengan paket sama → saat step 6,
  paket SELALU sudah terpasang → transaksi no-op → trigger tidak pernah
  dijalankan → `[PASS] svsp: tanpa 'not an absolute path'` **tidak
  membuktikan apa-apa** (baik run1 maupun run2). Bukti: transaksi NYATA
  `fake-run --svsp apk add --no-cache acl` di commit sama → **rc=1**, error
  `'bin/busybox' is not an absolute path`. Usul: purge `$PKG` antara step 5
  dan 6, atau pakai paket berbeda utk jalur svsp.
- Semua PASS lain valid: artefak/resolv/doctor/SBARGS/passthrough/akhir bersih
  + **jalur shim add acl rc=0 tanpa error (transaksi nyata di run2)** — selaras
  dgn §21 (shim = jalur utama, hijau penuh).

### 3. Stale-flag chain §18.2a masih berulang (bukan bug baru — sudah didok.)
Setiap kegagalan trigger svsp menulis `f:s` di busybox → transaksi shim
berikutnya dapat rc=1 senyap → `./apk-doctor --clear-broken` = obatnya
(terbukti lagi: del rc=1 senyap → clear-broken → doctor bersih rc=0).
Kalau Plan C §21 dikejar, `--no-scripts` otomatis untuk svsp akan memutus
rantai ini juga.

### 4. V4 isolasi `/system` — dokumen & quick-test bertentangan dgn §20.2
`fake-run stat /system/build.prop` (jalur shim) → **rc=0, tembus** (file
0600 milik root: stat sukses, `cat` EACCES). Ini yang membuat quick-test
`install.sh` (yang mengharapkan ENOENT) mencetak warning "hasil tidak
terduga". HANDOFF §8 V4 masih menuliskan `ENOENT (benar!)`, sedangkan §20.2
menambahkan prefix `/system` ke passthrough shim. Putuskan salah satu:
perbarui §8 + quick-test (terima tembus), atau buang `/system` dari
passthrough. (Diuji jalur shim; jalur svsp tidak diuji utk butir ini.)

### 5. Anomsi kecil yang perlu dicatat apa adanya
- Probe pipeline saya (script test agent, bukan kode repo) pernah sekali
  keluar **RC=139 (SIGSEGV)**; 4× percobaan ulang selanjutnya selalu RC=2.
  Penyebab RC=2 ternyata script saya sendiri: `${PIPESTATUS[0]}` **tidak
  didukung** busybox ash di wadah ini (`sh: syntax error: bad substitution`,
  diverifikasi terpisah). RC=139 satu-satunya itu **tidak berulang dan tidak
  terjelaskan** — kemungkinan transient; tidak cukup bukti utk menyebutnya
  bug svsp, tapi saya catat.
