# Device Feedback — Ronde 9 (getcwd konsisten host §28 + selftest penuh)

Tanggal: 2026-10-08 · Basis: `c5f9138` (§28 getcwd jawab kebenaran host +
escape hatch `FAKE_VIEW=container`)

**Verdict: 12/12 — MATRIKS PERTAMA KALI FULL HIJAU 🎉. Kasus 3 shim AKANNYA
TERATASI** (`LocationNotFoundError: /root` hilang, TUI render, spawn normal) —
hipotesis getcwd ronde 8 terbukti. **Selftest penuh: 0 FAIL** (tanpa regresi
apk/busybox). Satu temuan tambahan: **escape hatch `FAKE_VIEW=container` tidak
berfungsi lewat `fake-run`** (var dibuang `env -i` whitelist) — detail §4.

---

## 1. Build (tugas 1)

- `git pull` → RC=0 (`8d666c3..c5f9138`, §28).
- `./install.sh` → **RC=0**: `PT_INTERP: 24 file, 0 gagal`, quick-test hijau
  (`Test dinamis: 3.24.2`, `Isolasi: EACCES (benar)`).
  Log: `~/files/usr/tmp/opencode/install-r9.log`.

## 2. Selftest penuh (tugas 2 — WAJIB duluan, sesuai urutan brief)

`./selftest | tee device-feedback/selftest-ronde9.txt` → **RC=0, RINGKASAN: 0 FAIL**
(full log dilampirkan sebagai `device-feedback/selftest-ronde9.txt`).

Ringkasan jalur yang dijalankan (semua PASS):

- Fasilitas: fake-run, apk-doctor, base, shim libfakeroot.so, svsp, loader
  patched, resolv.conf (4 NS + options), DB apk-doctor bersih (flag f/s).
- **shim**: `apk --version`, `apk add acl` rc=0 tanpa error, `apk add busybox`
  (trigger) rc=0, `/data` passthrough.
- **svsp**: `apk add acl` rc=0, tanpa `failed to write database`, tanpa
  `not an absolute path`, tanpa error lain, `lib/apk/exec` kosong.
- **SBARGS** (busybox sh -c), cleanup purge acl, akhir: DB tetap bersih.

→ **Tidak ada FAIL akibat getcwd** — regresi yang ditakuti brief tidak terjadi.

## 3. Diagnostik getcwd (tugas 4)

`fake-run /bin/sh -c 'cd /root && pwd -P && pwd && readlink /proc/self/cwd'`:

```
/data/data/com.termux/files/home/alpine-rootfs/root   ← pwd -P (getcwd) = HOST ✅
/root                                                  ← pwd (logis)      = wadah
/data/data/com.termux/files/home/alpine-rootfs/root   ← readlink cwd     = HOST ✅
```

- **2 dari 3 kini host** — `pwd -P` (getcwd) dan `readlink /proc/self/cwd`
  konsisten ✅ (ronde 8: `pwd -P` = `/root`, silang-namespace).
- **`pwd` (logis) masih `/root`** — ini $PWD logis yang di-set shell saat
  `cd /root` (argumen yang diketik), bukan hasil getcwd; bukan perubahan
  libfakeroot. Baseline tanpa `cd /root` eksplisit: ketiganya host
  (`pwd -P`, `readlink`, `realpath .` semuanya `$B/root`). Catatan minor:
  komponen yang mempercayai env `$PWD` (bukan getcwd) setelah `cd /root`
  eksplisit masih bisa melihat `/root` — dampaknya nol utk opencode (kasus 3
  kini ✅), tapi saya catat apa adanya.

## 4. Escape hatch `FAKE_VIEW=container` — TIDAK BERFUNGSI lewat fake-run (temuan)

Bukan bagian tugas wajib (task 5 hanya bila selftest FAIL — tidak ada FAIL),
tapi saya sanity-check hatch-nya dan hasilnya perlu dilaporkan:

```
$ env -u LD_PRELOAD FAKE_VIEW=container fake-run /bin/sh -c 'echo FAKE_VIEW=$FAKE_VIEW; cd /root && pwd -P'
FAKE_VIEW=                                    ← KOSONG di anak
/data/.../alpine-rootfs/root                  ← tetap host (tak berubah)
```

Akar: **`fake-run` melakukan `exec env -i` dengan whitelist eksplisit**
(`fake-run:167` & `:192` — hanya HOME/PATH/LD_LIBRARY_PATH/dsb.; `SVSP_DEBUG`
pun diteruskan khusus, komentar `fake-run:140`). Variabel kustom apa pun
dibuang — terbukti juga dengan `MYVAR=hello` → `MYVAR=[]` di anak.
`libfakeroot.c:1085` membaca `FAKE_VIEW` via `getenv`, tapi var itu **tidak
pernah sampai ke proses anak** saat dipakai seperti anjuran brief
(`FAKE_VIEW=container fake-run ...`).

Saran utk Arena: tambahkan `FAKE_VIEW` ke daftar pass-through `env -i` di
kedua titik exec (baris 167 & 192), sama seperti `SVSP_DEBUG`. (Saya tidak
mengubah kode — hanya melaporkan.)

## 5. Matriks (tugas 3) — protokol keadaan bersih, FOKUS kasus 3 shim

Port wadah → 49474 (49374 = host), dipulihkan §7. Tiap run kasus 3 didahului
kill daemon + port dibuktikan `000`; 000→200 dicatat sesudah run.

| # | Kasus | natif | svsp | shim |
|---|-------|-------|------|------|
| 1 | `--help` | ✅ RC=0 | ✅ RC=0 | ✅ RC=0 |
| 2 | `serve` manual + `curl /` | ✅ 200 | ✅ 200 | ✅ 200 |
| 3 | spawn polos (keadaan bersih) | ✅ TUI, 000→200 | ✅ TUI, 000→200 | ✅ **FIX** — TUI render, RC=124, 000→200, **0 baris error** |
| 4 | `--standalone` | ✅ TUI RC=124 | ✅ TUI RC=124 | ✅ TUI RC=124 |

**12/12** — ronde 7: 11/12, ronde 8: 11/12.

Detail kasus 3 shim (fokus): log `r9-bare-shim.log` — `Starting background
server...` → TUI render penuh → hidup s/d timeout 40s (RC=124), **tanpa satu
pun baris `Error|failed`** (grep: 0), port 49474 sesudah = 200 (anak spawn
sendiri lewat jalur shim, `command=` program asli sejak §26).

## 6. Regresi svsp

**4/4 ✅** — tanpa perubahan sejak ronde 6 (kasus 1 RC=0, kasus 2 200, kasus 3
TUI+spawn 000→200 tanpa baris Error/500, kasus 4 TUI RC=124).

## 7. Catatan kejujuran & perubahan lingkungan

- Urutan brief diikuti persis: install → selftest (WAJIB) → matriks →
  diagnostik → penilaian task 5 (N/A, tak ada FAIL).
- Task 5 (`FAKE_VIEW=container` utk tes yang FAIL): **N/A — 0 FAIL di
  selftest**; sanity-check hatch tetap saya jalankan → hasil §4 (tidak
  berfungsi lewat fake-run).
- Perubahan lingkungan dipulihkan: port config 49474 → **49374** (RC=0);
  49474 = 000; host service (49374) utuh → 401; daemon uji (pid 28499, 28686,
  + 1 daemon kasus 3 shim) di-kill setelah port dibuktikan 200; tidak ada pid
  lain disentuh. Tak ada file wadah yang dimodifikasi round ini.
- Log mentah `~/files/usr/tmp/opencode/`: `install-r9.log`,
  `r9-serve-*.log`, `r9-bare-{natif,svsp,shim}.log`, `r9-standalone-*.log`.
- Full selftest log: `device-feedback/selftest-ronde9.txt` (ikut di-commit,
  diminta brief).

## 8. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| Selftest penuh (urutan wajib) | ✅ **0 FAIL** — tanpa regresi getcwd di apk/busybox |
| Diagnostik getcwd: ketiga sama (host) | ⚠️ **2/3 host** — `pwd -P` ✅ & `readlink` ✅; `pwd` logis = `/root` ($PWD shell setelah `cd /root`, bukan getcwd — penjelasan §3) |
| Kasus 3 shim | ✅ **TERATASI** — error hilang, TUI render, 0 baris error |
| Matriks 4×3 | ✅ **12/12** (pertama kali full hijau) |
| Regresi svsp | ✅ 4/4 |
| Escape hatch (task 5) | N/A utk tes (0 FAIL); tapi **hatch tidak berfungsi** lewat fake-run (§4) — saran pass-through `env -i` |
| Catatan arah brief | Kasus 3 shim ✅ → **tidak perlu berhenti menambal shim** — shim kini 4/4 juga |
