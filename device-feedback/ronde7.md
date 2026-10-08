# Device Feedback — Ronde 7 (verifikasi 3 tambalan shim + O_PATH mode-000, HANDOFF §26)

Tanggal: 2026-10-08 · Basis: `b9d1cdb` (§26: FAKEROOT_EXE di fake-run, fallback
cmdline[1], jaring pengaman spawn, O_PATH mode-000, DBG tanpa errno basi)

**Verdict: 11/12 ✅ (ronde 6: 10/12). Kasus 4 shim kini ✅ — `cannot load serve`
HILANG total, spawn `command=` kini program asli. Satu-satunya gagalan: kasus 3
shim (bare spawn) → `LocationNotFoundError: Location not found: /root`.
Tiga tambalan §26 yang bisa diverifikasi semuanya TERKONFIRMASI, kecuali bug
baru itu. O_PATH mode-000 kini OK di svsp ✅; symlink O_NOFOLLOW tetap ELOOP ✅
(ekspektasi brief).**

---

## 1. Build (tugas 1)

- `git pull` → RC=0 (`a85196e..b9d1cdb`, §26).
- `./install.sh` → **RC=0 langsung**: `PT_INTERP: 24 file, 0 gagal`, quick-test
  hijau (`Test dinamis: 3.24.2`, `Isolasi: EACCES (benar)`).
  Log: `~/files/usr/tmp/opencode/install-r7.log`.
- **Tugas 5:** bootstrap 0555 **tidak terpicu** (chmod ronde 5 masih persisten)
  → tidak ada yang dilaporkan selain "belum terpicu". Perbaikan bootstrap memang
  belum dikerjakan sesuai brief.

## 2. Matriks (tugas 2) — protokol keadaan bersih (port dibuktikan 000 sebelum tiap run)

Variabel & jalur sama ronde 6. Port wadah dipindah ke **49474** (49374 = service
host session ini), dipulihkan di §6.

| # | Kasus | natif | svsp | shim |
|---|-------|-------|------|------|
| 1 | `--help` (timeout 20/30) | ✅ RC=0 | ✅ RC=0 | ✅ RC=0 |
| 2 | `serve` manual (timeout 14 + `curl /` 4096) | ✅ 200 | ✅ 200 | ✅ 200 |
| 3 | spawn serve polos, `</dev/null` (keadaan bersih) | ✅ TUI, 000→200 | ✅ TUI, 000→200 (spawn sendiri) | ❌ **RC=1, `LocationNotFoundError: /root`** (anak spawn & daemon sehat, klien TUI mati) |
| 4 | `--standalone` (timeout 20) | ✅ TUI | ✅ TUI | ✅ **FIX** — TUI render, RC=124 (ronde 6: `cannot load serve`) |

Detail:

- **Kasus 3 natif:** port 000 → run → TUI render (RC=124) → 49474 = 200 (anak
  spawn sendiri). Log: `r7-bare-natif.log`.
- **Kasus 3 svsp:** kill daemon (pid 3390, port dibuktikan 000) → run → TUI
  render (RC=124), tanpa baris Error/500 di log → 49474 = 200 (000→200 = spawn
  lewat svsp sendiri). Log: `r7-bare-svsp.log`.
- **Kasus 3 shim — teks error persis** (tugas 3, log `r7-bare-shim.log` &
  `r7-bare-shim-logs.log`):

  ```
  Starting background server...
  timestamp=... level=INFO run=f31cb46f message="background service starting" reason=missing previousVersion=undefined role=cli
  timestamp=... level=ERROR run=f31cb46f message="cli process failed"
    cause="Cause([Fail(UnknownError: An error occurred in Effect.tryPromise
    (cause: LocationNotFoundError: Location not found: /root))])" args="[\"--print-logs\"]" role=cli
  UnknownError: An error occurred in Effect.tryPromise
    [cause]: LocationNotFoundError: Location not found: /root
  ```

  RC=1, `render TUI: 0`.
- **Kasus 4 shim (dgn `--print-logs`):** RC=124, `render TUI: 1` — baris
  `spawning process` kini:

  ```
  message="spawning process" command=/data/data/com.termux/files/home/alpine-rootfs/root/.opencode/bin/opencode args=["serve","--stdio","--port","0"] cwd=/ role=cli
  ```

  `command=` **program asli** (dulu `ld-musl-patched.so.1`) → jaring pengaman
  §26 bekerja, `cannot load serve` tidak muncul lagi. Ada satu warning non-fatal
  di log: `FileSystemError ... readDirectoryEntries: EACCES ... scandir '/'`
  (scandir root host, terisolasi §20.2) — TUI tetap render penuh.
  Log: `r7-standalone-shim.log`.

## 3. Diagnosa jalur shim kasus 3 (tugas 3 — semua yang diminta dilampirkan)

1. **Baris `spawning process`:** TIDAK MUNCUL di run kasus 3 (`--print-logs`)
   — klien mencatat `background service starting reason=missing` lalu **5 detik
   kemudian** mati `LocationNotFoundError: /root`. Namun anak/jaring pengaman
   terbukti jalan di run kasus 3 sebelumnya: 49474 sesudah run = **200**, dan
   inspeksi daemon (pid 4455) via `/proc`:

   ```
   cmdline : $B/root/.opencode/bin/opencode serve --service      ← program asli, bukan loader
   HOME    = $B/root
   PWD     = $B
   FAKEROOT_EXE = $B/root/.opencode/bin/opencode                 ← env kini ter-set ✅
   LD_PRELOAD   = $HOME/libfakeroot.so
   ```

   → **sisi server sehat**; yang mati sisi **klien TUI**.
2. **`fake-run /bin/sh -c 'env | grep FAKEROOT'`** (persis permintaan brief):

   ```
   HOME=/data/data/com.termux/files/home/alpine-rootfs/root
   FAKEROOT_EXE=/data/data/com.termux/files/home/alpine-rootfs/bin/sh
   PWD=/data/data/com.termux/files/home/alpine-rootfs
   ```

   `FAKEROOT_EXE` kini **ADA** di env anak ✅ (ronde 6: kosong) — tambalan
   pertama §26 terverifikasi.
3. **Akar masalah sisa (dugaan, saya tidak menyentuh kode):** klien menyelesaikan
   lokasi proyek menjadi literal `/root` lalu gagal lookup. `/root` = `$B/root`
   **tanpa prefix `$B`** → dugaan: ada terjemahan path ganda (host→container)
   di jalur resolve HOME/lokasi klien di bawah shim, atau klien jatuh ke fallback
   `passwd` (`pw_dir=/root`) saat `realpath(HOME)` di-interposisi. Perbedaan
   dengan kasus 4 (sukses): standalone tidak melewati koneksi service background
   / resolusi lokasi yang sama. Untuk Arena.

## 4. Repro sudut §25.1 (tugas 4)

`repro-sudut2.c` (sumber ronde 6, file fisik sama di kedua sisi):

```
--- NATIF (path asli):
O_PATH|O_NOFOLLOW $B/bin/sh: fd=3 -> SYMLINK (S_ISLNK=1 S_ISDIR=0)
O_PATH $B/tmp/norwx (mode 000): OK (errno=0)
O_RDONLY $B/tmp/norwx: Permission denied (errno=13)

--- SVSP (/bin/sh & /tmp/norwx di-rewrite):
O_PATH|O_NOFOLLOW /bin/sh: GAGAL Too many symbolic links encountered (errno=40)
O_PATH /tmp/norwx (mode 000): OK (errno=0)      ← RONDE 6: EACCES — KINI OK ✅
O_RDONLY /tmp/norwx: Permission denied (errno=13)
```

- **File mode-000: OK ✅** — persis sasaran brief; `O_RDONLY` tetap EACCES di
  kedua sisi (konsisten).
- **Symlink O_NOFOLLOW: ELOOP (errno=40) ✅** — dikonfirmasi masih gagal, sesuai
  ekspektasi brief (batas arsitektural, bukan regresi).
- `SVSP_DEBUG=1`: `ADDFD nr=56 -> fd=3 errno=0 (O_PATH didegradasi ke O_RDONLY)`
  → **errno basi hilang** ✅ (ronde 6 masih cetak `errno=2` stale) — tambalan
  DBG §26 terverifikasi.

## 5. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| `FAKEROOT_EXE` di-set oleh fake-run | ✅ terverifikasi (env anak + env daemon) |
| Cadangan `cmdline[1]` / jaring pengaman spawn → `cannot load serve` hilang | ✅ terverifikasi — kasus 4 shim ✅, `command=` = program asli; kasus 3 spawn juga sampai naik (200) |
| O_PATH file mode-000 di svsp OK | ✅ terverifikasi (OK vs ronde 6 EACCES) |
| DBG tanpa errno basi | ✅ `errno=0` |
| Matriks 4×3 keadaan bersih | 11/12 ✅ — **❌ baru: kasus 3 shim `LocationNotFoundError: /root`** (detail §3) |
| Symlink O_NOFOLLOW tetap ELOOP | ✅ dikonfirmasi (batas arsitektural) |
| Bootstrap 0555 | tidak terpicu (install RC=0) |

## 6. Catatan kejujuran & perubahan lingkungan

- Protokol keadaan bersih dipakai penuh: sebelum tiap run kasus 3, port 49474
  dibuktikan `HTTP=000` (daemon lama di-kill via pid di `service.json` setelah
  port dicek `200`); 000→200 sesudah run dibuktikan per jalur. Tidak ada konfound
  seperti pass-1 ronde 6.
- Tiga daemon uji (pid 3390 natif, 3747 svsp, 4455 shim) di-kill; tidak ada
  pid lain yang disentuh. Host service pid 6486 utuh (49374 → 401).
- Perubahan lingkungan, semua dipulihkan: port config 49474 → **49374** (RC=0);
  file uji `$B/tmp/norwx` mode 000 dibuat & dihapus (chmod 644 dulu);
  49474 sesudah tes = 000.
- Log mentah di luar repo `~/files/usr/tmp/opencode/`: `install-r7.log`,
  `r7-serve-{natif,svsp,shim}.log`, `r7-bare-{natif,svsp,shim}.log`,
  `r7-bare-shim-logs.log`, `r7-standalone-{natif,svsp,shim}.log`,
  `repro-sudut2.c` + biner.
- Kasus 4 shim difokuskan pada `--print-logs` (diminta brief utk baris spawn);
  varian tanpa print-logs tidak dijalankan terpisah — TUI RC=124 dari run
  print-logs itu sendiri sudah membuktikan kerja.
