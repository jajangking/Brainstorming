# Device Feedback — Ronde 6 (verifikasi fix O_PATH svsp + /proc/self/exe shim, HANDOFF §25)

Tanggal: 2026-10-08 · Basis: `ca64cb4` (§25 O_PATH lolos ADDFD + FAKEROOT_EXE)
Target brief: ulangi matriks 4 kasus × 3 jalur + repro O_PATH + 2 sudut §25.1.

**Verdict: 10/12 ✅. Kedua kegagalan svsp (kasus 3 & 4) TERPERBAIKI — fix O_PATH
terkonfirmasi di perangkat. Shim masih ❌ di 2 kasus spawn ("cannot load serve"
persis seperti ronde 5 — fix /proc/self/exe tidak menjangkau `process.execPath`
saat bun melakukan spawn). Kedua sudut §25.1 MASIH gagal (dibuktikan, §5).**

---

## 1. Build (brief tugas 1)

- `git pull` → RC=0 (`a0f575a..ca64cb4`, §25).
- `./install.sh` → **RC=0 langsung, tanpa intervensi** — 52 file `.codex` 0555
  sudah bersih sejak `chmod u+w` ronde 5 (persisten), jadi abort bootstrap tidak
  terpicu. Quick-test hijau: `Test dinamis: 3.24.2`, `Isolasi: /system/build.prop → EACCES (benar)`.
  Log: `~/files/usr/tmp/opencode/install-r6.log`.
- Catatan utk Arena: perbaikan bootstrap (ronde 5 §1, `bootstrap.sh:214`) **belum
  dikerjakan** sesuai brief — hanya tidak terpicu karena chmod ronde 5 masih ada.

## 2. Matriks (tugas 2) — perintah persis & hasil

Variabel: `B=$HOME/alpine-rootfs`, `OC=$B/root/.opencode/bin/opencode`.
Jalur natif: `env -u LD_PRELOAD LD_LIBRARY_PATH=$B/lib:$B/usr/lib HOME=$B/root $OC ...`
Jalur svsp: `env -u LD_PRELOAD fake-run --svsp $OC ...`
Jalur shim: `env -u LD_PRELOAD fake-run $OC ...`

| # | Kasus | natif | svsp | shim |
|---|-------|-------|------|------|
| 1 | `--help` (timeout 20) | ✅ RC=0 | ✅ RC=0 | ✅ RC=0 |
| 2 | `serve` manual (timeout 14 + `curl /` port 4096) | ✅ 200 | ✅ 200 | ✅ 200 |
| 3 | spawn serve polos, `</dev/null` (timeout 35–40) | ✅ TUI render | ✅ **FIX** — TUI render **+ spawn sendiri** (49474: 000→200) | ❌ **RC=1, `cannot load serve`** |
| 4 | `--standalone` (timeout 20) | ✅ TUI render | ✅ **FIX** — TUI render (ronde 5: 500) | ❌ **RC=1, exited before readiness** |

Detail:

- **Kasus 1:** ketiga jalur RC=0, argumen lengkap keluar.
- **Kasus 2:** ketiga jalur `server listening on http://127.0.0.1:4096` + probe
  `curl /` → HTTP 200.
- **Kasus 3 natif:** `49474 bebas? HTTP=000` dibuktikan dulu → run → TUI render,
  RC=124 (timeout mematikan TUI sehat).
- **Kasus 3 svsp (RETEST keadaan bersih):** daemon dipastikan mati (kill pid dari
  `service.json`, port → 000) → run → `render TUI: 1`, RC=124, dan
  **port 49474 sesudah run = 200** → anak benar-benar spawn sendiri lewat svsp.
  O_PATH/ADDFD tak lagi mematikan server.
- **Kasus 3 shim (RETEST keadaan bersih) — teks error persis:**

  ```
  Starting background server...
  Error: Server process exited with code 1
  /data/data/com.termux/files/home/alpine-rootfs/lib/ld-musl-patched.so.1: cannot load serve: No such file or directory
  ```

  Port 49474 sesudah run = 000 (anak tak pernah naik). RC=1, `render TUI: 0`.
  Log: `~/files/usr/tmp/opencode/r6-bare-shim2.log`.
- **Kasus 4 svsp:** RC=124 (TUI hidup s/d timeout), `render TUI: 1` — dulu RC=1
  `UnexpectedStatus: 500`. Log: `r6-standalone-svsp.log`.
- **Kasus 4 shim:** RC=1, `render TUI: 0`, `Error: Standalone server exited before
  reporting readiness`; dengan `--print-logs` (log: `r6-standalone-shim-logs.log`):

  ```
  message="spawning process" command=/data/data/com.termux/files/home/alpine-rootfs/lib/ld-musl-patched.so.1 args=["serve","--stdio","--port","0"] cwd=/ role=cli
  /data/data/com.termux/files/home/alpine-rootfs/lib/ld-musl-patched.so.1: cannot load serve: No such file or directory
  level=ERROR ... message="cli process failed" cause="... Standalone server exited before reporting readiness"
  ```

  → anak di-spawn dengan **command = loader**, argv `["serve","--stdio","--port","0"]`
  → loader mencoba memuat argumen pertama "serve" sebagai berkas → gagal.
  Ini varian langsung dari keluarga bug §20.1 (argv/loader), jalur LD_PRELOAD.

### Konfound yang saya temukan & perbaiki (jujur, dicatat)

Pass pertama kasus 3: shim **terlihat** `render TUI: 1` — TIDAK VALID. Urutannya
natif → svsp → shim; natif sudah menaikkan daemon di 49474, sehingga svsp & shim
pass-1 **CONNECT ke daemon natif**, bukan spawn. Semua angka kasus 3/4 di tabel
di atas diambil dari **retest keadaan bersih** (daemon dibunuh + port dibuktikan
000 sebelum tiap run). Hasil svsp pass-1 = hasil retest (✅, tetap sah); shim
pass-1 dibatalkan → ❌.

## 3. Repro O_PATH (tugas 3) — natif vs svsp, dilampirkan

`clang -O0 repro-svsp.c` (sumber ronde 5, `~/files/usr/tmp/opencode/repro-svsp.c`):

```
--- natif:
lstat('/') OK mode=40755 dev=fe13
open(O_PATH)('/') OK fd=3
open(O_RDONLY|O_DIRECTORY)('/') GAGAL: Permission denied (errno=13)
open(O_RDONLY|O_NOFOLLOW)('/') GAGAL: Permission denied (errno=13)
stat('/') OK mode=40755

--- svsp:
lstat('/') OK mode=40700 dev=fe53
open(O_PATH)('/') OK fd=3            ← RONDE 5: EACCES (errno=13) — KINI OK ✅
open(O_RDONLY|O_DIRECTORY)('/') OK fd=3
open(O_RDONLY|O_NOFOLLOW)('/') OK fd=3
stat('/') OK mode=40700
```

- `open(O_PATH)('/')` di svsp **kini OK fd=3** — fix ADDFD §25 terkonfirmasi di
  perangkat (dulu `EACCES`, kernel `fget()` menolak fd `FMODE_PATH`).
- Dua baris GAGAL di natif adalah izin root Android pada `/` (wajar, bukan bug;
  svsp malah menjawab OK karena supervisor membuka via wadah).
- `SVSP_DEBUG=1` memunculkan jalur fix-nya:

  ```
  [svsp] nr=56 [/bin] -> [/data/data/com.termux/files/home/alpine-rootfs/bin]
  [svsp] ADDFD nr=56 -> fd=3 errno=2 (O_PATH didegradasi ke O_RDONLY)
  ```

  Catatan kosmetik: `errno=2` pada baris sukses itu **stale** (sisa errno syscall
  sebelumnya, fd=3 = sukses) — bisa dibersihkan di DBG agar tidak menyesatkan.

## 4. Diagnosa jalur yang masih ❌ (tugas 4)

- **svsp:** N/A — tidak ada lagi yang gagal (kasus 1–4 ✅ semua), jadi
  `grep ADDFD|O_PATH|execve` utk kasus gagal tidak dijalankan; bukti DBG ada di §3.
- **shim** — dua wawancara:

  1. Env anak: `env -u LD_PRELOAD fake-run /bin/sh -c 'echo FAKEROOT_EXE=$FAKEROOT_EXE; readlink /proc/self/exe'` →

     ```
     FAKEROOT_EXE=
     /data/data/com.termux/files/home/alpine-rootfs/bin/busybox
     ```

     `readlink(/proc/self/exe)` via libc **menjawab program asli** ✅ (fix bekerja),
     tetapi **`FAKEROOT_EXE` kosong di env anak** — tidak terlihat di lingkungan
     yang diwariskan ke proses anak (mungkin dipakai internal lalu dilepas; kalau
     memang sengaja, mohon dkonfirmasi — saya hanya melaporkan apa adanya).
  2. Penyebab gagal spawn: log `--print-logs` (§2) menunjukkan `process.execPath`
     yang dipakai bun = **loader**, padahal `readlink(/proc/self/exe)` via libc
     sudah benar → dugaan: bun membaca `/proc/self/exe` tanpa melewati wrapper
     libc (syscall langsung), sehingga interposisi LD_PRELOAD tidak menangkap,
     dan/atau `execPath` di-cache dari jalur lain. Konsekuensi: spawn selalu
     `command=ld-musl-patched.so.1 args=["serve",...]` → `cannot load serve`.
  - Saran perbaikan (utk Arena, saya tidak menyentuh kode): interposisi jalur
    baca execPath yang dipakai bun (bila syscall langsung → perlu jalur lain),
    atau pastikan loader menerima argv layaknya program (terima argumen pertama
    sebagai berkas program — keluarga §20.1).

## 5. Dua sudut §25.1 (tugas 5) — MASIH GAGAL, terbukti

Repro baru `repro-sudut2.c` (sumber dilampirkan; uji pada **file fisik yang sama**
di kedua sisi — natif membuka path asli tanpa rewrite, svsp membuka path di-rewrite):

```
--- NATIF:
O_PATH|O_NOFOLLOW <B>/bin/sh: fd=3 -> SYMLINK (S_ISLNK=1 S_ISDIR=0)
O_PATH <B>/tmp/norwx: OK (errno=0)
O_RDONLY <B>/tmp/norwx: Permission denied (errno=13)

--- SVSP (/bin/sh & /tmp/norwx di-rewrite ke wadah):
O_PATH|O_NOFOLLOW /bin/sh: GAGAL Too many symbolic links encountered (errno=40)
O_PATH /tmp/norwx: Permission denied (errno=13)
O_RDONLY /tmp/norwx: Permission denied (errno=13)
```

1. **Symlink + `O_NOFOLLOW`:** natif `O_PATH` pada symlink (`$B/bin/sh → busybox`)
   → OK, fd menunjuk SYMLINK; svsp → **ELOOP (errno=40)**. Supervisor membuka
   ulang tanpa O_PATH (O_NOFOLLOW dipertahankan) → kernel menolak symlink →
   divergensi nyata. **Sudut ini belum terpecahkan** (sesuai §25.1).
   (Catatan: uji awal memakai path `/bin` wadah yang ternyata **direktori**
   `drwx------`, bukan symlink — tidak valid sbg uji symlink; diganti `/bin/sh`
   yang symlink asli.)
2. **File tanpa izin baca (mode 000, milik sendiri):** natif `O_PATH` → **OK**
   (kernel tidak memeriksa izin baca utk O_PATH); svsp → **EACCES (errno=13)**
   (supervisor membuka dengan O_RDONLY → ditolak). **Sudut ini belum terpecahkan**
   (sesuai §25.1). O_RDONLY-nya memang EACCES di kedua sisi (konsisten) — yang
   beda hanya O_PATH.
3. Sudut O_PATH pada direktori via O_NOFOLLOW: natif OK, svsp OK (uji `/bin`
   direktori di §5 catatan) — aman.

## 6. Catatan kejujuran & perubahan lingkungan

- Konfounded pass-1 kasus 3 (shim/svsp connect ke daemon natif) — didokumentasikan
  di §2; semua angka tabel dari retest bersih.
- Jalur shim utk repro C **N/A**: biner repro = bionic host, tak bisa dijalankan
  lewat ldpreload (ronde 5 §5, RC=127 `Error relocating`) — shim divalidasi hanya
  lewat biner opencode (dinamis musl) di matriks §2.
- Perubahan lingkungan (semua dipulihkan): port config wadah 49374 → 49474 saat
  uji spawn → **dikembalikan ke 49374** (`service set port` RC=0); file uji
  `$B/tmp/norwx` mode 000 **dibuat & dihapus** (chmod 644 dulu utk bisa unlink);
  semua daemon uji sudah mati (49474 → 000); host service pid 6486 utuh
  (49374 → 401).
- `chmod u+w` ronde 5 tidak perlu diulang (install RC=0 tanpa intervensi).
- Log mentah di luar repo: `~/files/usr/tmp/opencode/` — `install-r6.log`,
  `r6-serve-{natif,svsp,shim}.log`, `r6-bare-{natif,svsp,svsp2,shim,shim2}.log`,
  `r6-standalone-{natif,svsp,shim}.log`, `r6-standalone-shim-logs.log`,
  `repro-svsp.c/.bin`, `repro-sudut.c/.bin`, `repro-sudut2.c/.bin`.

## 7. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| Fix O_PATH svsp (kasus 3 & 4) | ✅ **TERVERIFIKASI** di perangkat — matriks svsp 4/4 ✅, repro O_PATH OK fd=3 |
| Fix /proc/self/exe shim menghapus `cannot load serve` | ❌ **TIDAK** — kasus 3 & 4 shim masih `cannot load serve`; `execPath` bun = loader saat spawn |
| Sudut §25.1 (symlink O_NOFOLLOW, file tanpa izin baca) | ❌ **MASIH gagal keduanya** — bukti angka di §5 (sesuai ekspektasi brief, "masih belum terpecahkan") |
