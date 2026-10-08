# Device Feedback — Ronde 11 (TANPA BRIEF — temuan pemakaian nyata: `FAKEROOT_EXE` basi di nested shell)

Tanggal: 2026-10-08 · Basis: `66da3dc` (§29, sama dgn ronde 10)
Pemicu: paste error dari sesi pemakaian langsung (user masuk shell wadah →
ketik `opencode`):

```
Starting background server...
Error: Server process exited with code 2
/data/data/com.termux/files/home/alpine-rootfs/bin/sh: can't open 'serve': No such file or directory
```

**Verdict: REPRODUKSI PERSIS, akar masalah terbukti — `FAKEROOT_EXE` basi
diwarisi dari shell fake-run. Matriks ronde 9/10 tidak menangkapnya karena
semua tes menjalankan `fake-run $OC` langsung (var = opencode, benar); jalur
pemakaian alami "masuk shell wadah → ketik opencode" tidak pernah ada di brief.**

---

## 1. Reproduksi (identik baris demi baris)

```
$ env -u LD_PRELOAD fake-run /bin/sh -c '$HOME/.opencode/bin/opencode </dev/null'
FAKEROOT_EXE=/data/.../alpine-rootfs/bin/sh        ← di dalam shell: menunjuk SHELL
Starting background server...
Error: Server process exited with code 2
/data/.../alpine-rootfs/bin/sh: can't open 'serve': No such file or directory
RC=1
```

`--print-logs` menegaskan (log `r11-repro-logs.log`):

```
message="background service starting" reason=missing
message="cli process failed" cause="... Server process exited with code 2
  /data/.../alpine-rootfs/bin/sh: can't open 'serve': No such file or directory"
```

(`sh serve ...` = shell mencoba menjalankan skrip bernama "serve" → exit 2.)

## 2. Bukti kausalitas (kontrol A/B)

| Variabel di dalam shell fake-run | `opencode --standalone` |
|---|---|
| `FAKEROOT_EXE=` (dikosongkan) | ✅ **RC=124, TUI render** |
| `FAKEROOT_EXE=.../bin/sh` (basi, bawaan) | ❌ RC=1, TUI 0, `Standalone server exited before reporting readiness` |

Satu-satunya variabel yang dibedakan = `FAKEROOT_EXE` → **penyebab terbukti.**

## 3. Mekanisme (untuk Arena)

1. `fake-run` meluncurkan shell wadah → men-set `FAKEROOT_EXE=/.../bin/sh`
   (benar **utk proses itu**).
2. User di dalam shell menjalankan `opencode` → **mewarisi `FAKEROOT_EXE=bin/sh`**
   yang tidak pernah di-refresh saat nested `execve(opencode)`.
3. `fk_self_exe()` mempercayai env (prioritas) → `process.execPath` = `bin/sh`
   → spawn: `command=bin/sh args=["serve", ...]` → sh: `can't open 'serve'` → exit 2.
4. Jaring pengaman §26 **tidak menangkap** — net hanya aktif bila
   `command=` **loader**; di sini command = `bin/sh` (executable valid) → net diam.

Data pendukung:

- Di dalam shell: `readlink /proc/self/exe` (via libc, ter-interposisi) →
  `bin/sh` — hook sudah menjawab benar **untk shell**; masalahnya nilai env yang
  basi dipakai apa adanya untuk proses berbeda (opencode).
- cmdline shell = `ld-musl-patched.so.1 /bin/sh -c ...` (argv[0] loader) —
  fallback `cmdline[1]` §26 memang didesain utk kasus exe=loader.
- Proses nested opencode: env `LD_PRELOAD` juga diwarisi (shim tetap aktif) —
  hanya `FAKEROOT_EXE` yang salah sasaran.

## 4. Kandidat perbaikan (laporan saja — saya tidak menyentuh kode)

Urut dari paling bersih:

1. **`execve` yang di-interposisi oleh libfakeroot**: saat anak mengeksekusi
   ELF baru, refresh status self-exe di dalam hook (state internal per-proses;
   env parent tak bisa diubah, tapi jawaban hook bisa diprioritaskan di atas
   env).
2. **Urutan prioritas `fk_self_exe()`**: jawaban hook `/proc/self/exe`
   (kernel/milik hook) dulu; env `FAKEROOT_EXE` hanya fallback bila hook tak
   punya jawaban — validasi: bila env ≠ hasil readlink yang di-interposisi,
   buang env.
3. Perluas jaring pengaman §26: `command=bin/sh` (atau command bukan ELF yang
   sesuai) juga disisipi program — paling rapuh, opsi terakhir.

## 5. Workaround (untuk user, sementara)

Di dalam shell wadah sebelum menjalankan opencode, kosongkan variabelnya:

```sh
FAKEROOT_EXE= opencode
```

(terbukti di §2) — atau jalankan langsung dari luar shell: `fake-run /root/.opencode/bin/opencode`
(FAKEROOT_EXE diset fake-run = opencode, benar — jalur yang dipakai matriks
ronde 9/10).

## 6. Kejujuran & lingkungan

- **Kenapa matriks ronde 9/10 12/12 tapi pemakaian nyata gagal:** semua tes brief
  memakai `fake-run $OC` langsung. Kasus "nested: shell fake-run → opencode"
  tidak pernah ada dalam matriks brief mana pun (bukan regresi ronde 10 —
  bug laten sejak `FAKEROOT_EXE` diperkenalkan §26, tertutup oleh gaya tes
  langsung). Saya laporkan apa adanya.
- Tidak ada perubahan config/lingkungan: port 49374 tak tersentuh (repro mati
  sebelum port; kontrol pakai `--standalone` port privat), host service pid 6486
  utuh (401), 49474 = 000, tidak ada proses tes tersisa (scan `/proc`).
- Log: `~/files/usr/tmp/opencode/r11-repro.log`, `r11-repro-logs.log`,
  `r11-kontrol-standalone.log`, `r11-basi-standalone.log`.
