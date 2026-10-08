# Device Feedback — Ronde 8 (realpath namespace shim kasus 3 + bootstrap 0555, HANDOFF §27)

Tanggal: 2026-10-08 · Basis: `56bfa20` (§27 realpath jawab sesuai namespace +
bootstrap 0555 tak lagi fatal)

**Verdict: 11/12 — TIDAK NAIK dari ronde 7. Kasus 3 shim MASIH ❌ dengan error
IDENTIK (`LocationNotFoundError: Location not found: /root`) meski tambalan
realpath terpasang. Tapi uji mekanisme §27 sendiri BEKERJA di level libc (bukti
di §3), dan saya menemukan kontradiksi kuat utk Arena: `getcwd` (di-interposisi)
masih menjawab namespace wadah (`/root`) padahal `readlink /proc/self/cwd` =
path host — kemungkinan jalur bocornya `/root` ke Bun (lihat §3, bukti `pwd -P`).**

---

## 1. Build (tugas 1)

- `git pull` → RC=0 (`d4040b0..56bfa20`, §27).
- `./install.sh` → **RC=0** (dua run, keduanya): `PT_INTERP: 24 file, 0 gagal`,
  quick-test hijau (`Test dinamis: 3.24.2`, `Isolasi: EACCES (benar)`).
  Log: `~/files/usr/tmp/opencode/install-r8.log`, `install-r8b.log`.
- Tidak ada baris log khusus tentang peminjahan bit tulis (fix senyap — tidak
  ada pesan "read-only/pinjam/dipulih" di output; hanya RC=0 yang membuktikan).

## 2. Uji bootstrap 0555 sengaja (tugas 4)

Dua ELF `.codex` yang sengaja dibuat read-only, lalu `./install.sh`:

| Berkas | Mode asli | Sesudah `chmod a-w` saya (= sebelum install) | Sesudah install |
|---|---|---|---|
| `.../0.161.0-.../bin/codex` (ELF, 252 MB) | **755** (tercatat langsung) | 555 | **555 ✅ dipulihkan** |
| `.../codex-resources/voice/lib/gstreamer-1.0/libgstapp.so` (ELF) | 755 (inferensi: 4 saudara sefolder semua 755) | 555 | **555 ✅ dipulihkan** |
| `.../standalone/install.lock` (non-ELF, bonus) | 644 (inferensi: saudara `app-server-daemon/install.lock` = 644) | 444 | **444 ✅ dipulihkan** |

- **Install RC=0** dengan kedua ELF 555 — ronde 5 (52 file 0555 → hard-abort
  RC=1) kini **tidak terulang**. `PT_INTERP 24 file, 0 gagal` — rewrite jalan
  penuh di file read-only (bit dipinjam lalu mode dikembalikan persis).
- "Kembali seperti semula" saya artikan = seperti kondisi saat install mulai
  (555/444) → terpenuhi. Mode asli sebelum intervensi tes saya (755/644) saya
  **kembalikan sendiri** sebagai higienitas lingkungan (angka di kolom 2–3 tetap
  dilaporkan apa adanya).
- **Utang ronde 5 (bootstrap hard-abort) terbukti lunas.**

## 3. Kasus 3 shim (fokus, tugas 3) — masih ❌, bukti lengkap

Protokol keadaan bersih penuh (kill daemon → port dibuktikan `000` → run →
000/200 dicatat). Run `--print-logs` dari port 000 (bukan connect):

```
timestamp=... level=INFO run=4592aaca message="background service starting" reason=missing previousVersion=undefined role=cli
timestamp=... level=ERROR run=4592aaca message="cli process failed"
  cause="Cause([Fail(UnknownError: An error occurred in Effect.tryPromise
  (cause: LocationNotFoundError: Location not found: /root))])" args="[\"--print-logs\"]" role=cli
```

- RC=1, `render TUI: 0`, **49474 sesudah run = 200** → anak SPAWN NORMAL naik.
- **Baris `spawning process` TIDAK MUNCUL** di jalur service background (yang
  muncul hanya `background service starting reason=missing`) — untuk perbandingan,
  jalur `--standalone` mencatatnya (ronde 7: `command=` = program asli). Spawn
  tetap terbukti dari 000→200.
- **Server sehat** — inspeksi daemon (`opencode serve --service`):

  ```
  HOME    = $B/root                    ✅
  FAKEROOT_EXE = $B/root/.opencode/bin/opencode  ✅
  cwd kernel (/proc/PID/cwd) = $B/root ✅
  API: /session, /config → HTTP 200 (SPA); /api/location, /api/fs/list → 401
       {"_tag":"UnauthorizedError"}    ← server merespons, tapi butuh auth
  ```

  → sisi mati = **klien TUI**, bukan server (konsisten ronde 7).

### Diagnostik tambahan sesuai permintaan brief

`fake-run /bin/sh -c 'cd /root && pwd && realpath . && echo HOME=$HOME'`:

```
/root                                              ← cd /root berhasil (stat di-interposisi)
/data/data/com.termux/files/home/alpine-rootfs/root ← realpath . jawab HOST ✅ (§27)
HOME=/data/data/com.termux/files/home/alpine-rootfs/root
```

Varian: `realpath "$HOME"` → `$B/root` (host→host ✅); `realpath /root` → `/root`
(wadah→wadah ✅, sesuai desain §27). **Perilaku realpath = sesuai desain, tapi
error klien tidak berubah sama sekali** → jalur Bun tidak melewati realpath
libfakeroot, ATAU `/root` datang dari sumber lain. Sumber paling kuat:

```
fake-run /bin/sh -c 'cd /root && pwd -P && pwd && echo PWD=$PWD && readlink /proc/self/cwd'
/root                        ← pwd -P (getcwd)     = NAMESPACE WADAH
/root                        ← pwd                 = namespace wadah
PWD=/root
/data/.../alpine-rootfs/root ← readlink /proc/self/cwd = TRUTH HOST (kernel)
```

**getcwd di-interposisi untuk menjawab `/root` sementara kernel cwd = `$B/root`**
— brief §27 menyebut "`getcwd()` tidak diubah" → memang masih menyilangkan
namespace di dalam proses yang sama. Bila Bun/opencode memakai getcwd (libc)
untk lokasi proyek lalu men-stat-nya dengan syscall mentah (tanpa translasi
libfakeroot) → ENOENT → `LocationNotFoundError: /root`. Ini hipotesis kerja,
bukan fakta final — saya tidak menyentuh kode.

### Limitation (jujur)

- `/api/location` menjawab 401 (auth) → **N/A**: saya tidak bisa membaca isi
  respons lokasi server tanpa token. Bila Arena ingin saya bedah sisi server
  vs klien, mohon sediakan cara ambil password/token-nya.

## 4. Matriks (tugas 2) — protokol keadaan bersih

Port wadah dipindah ke 49474 (49374 = host), dipulihkan di §6.

| # | Kasus | natif | svsp | shim |
|---|-------|-------|------|------|
| 1 | `--help` | ✅ RC=0 | ✅ RC=0 | ✅ RC=0 |
| 2 | `serve` manual + `curl /` | ✅ 200 | ✅ 200 | ✅ 200 |
| 3 | spawn polos (keadaan bersih) | ✅ TUI, 000→200 | ✅ TUI, 000→200 | ❌ **RC=1, `LocationNotFoundError: /root`** (anak spawn, 000→200; klien mati) |
| 4 | `--standalone` | ✅ TUI RC=124 | ✅ TUI RC=124 | ✅ TUI RC=124 |

**11/12** — identik ronde 7.

## 5. Regresi svsp (tugas 5)

**4/4 ✅** — kasus 1 (RC=0), kasus 2 (200), kasus 3 (TUI render, spawn sendiri
000→200, tanpa baris Error/500 di log), kasus 4 (TUI RC=124). Tidak ada regresi
sejak ronde 6, sesuai catatan brief "jalur svsp sudah 4/4 sejak ronde 6".

## 6. Catatan kejujuran & perubahan lingkungan

- Semua run kasus 3 memakai protokol keadaan bersih: kill via pid
  `service.json` **setelah** port dibuktikan 200; port dibuktikan 000 sebelum
  run. Daemon uji (pid 18469, 19182, + 2 daemon run print-logs) di-kill semua;
  tidak ada pid lain disentuh; host service (49374) utuh → 401.
- Perubahan lingkungan, semua dipulihkan: port config 49474 → **49374** (RC=0);
  49474 = 000; mode 3 file `.codex` dikembalikan (755/755/644 — angka tes
  sebelum/sesudah tetap dilaporkan penuh di §2).
- Run print-logs pertama saya lakukan saat port masih 200 (connect, bukan
  spawn) → saya ulangi dari 000; hasil ulang yang dilaporkan.
- Log mentah `~/files/usr/tmp/opencode/`: `install-r8.log`, `install-r8b.log`,
  `r8-serve-*.log`, `r8-bare-{natif,svsp,shim}.log`, `r8-bare-shim-logs{,2}.log`,
  `r8-standalone-*.log`, `r8-api_*.out`.

## 7. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| Bootstrap 0555 → RC=0 + mode kembali | ✅ terverifikasi (tabel §2) — utang ronde 5 lunas |
| Kasus 3 shim teratasi oleh fix realpath | ❌ **TIDAK** — error identik; mekanisme realpath sendiri teruji sesuai desain (§3) → bocor `/root` dari jalur lain; kandidat kuat: getcwd silang-namespace (bukti `pwd -P`) |
| Regresi svsp | ✅ 4/4 bersih |
| Matriks keseluruhan | 11/12 (stagnan ronde 7) |
