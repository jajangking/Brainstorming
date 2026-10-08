# Device Feedback — Ronde 10 (verifikasi penutup, ringan)

Tanggal: 2026-10-08 · Basis: `66da3dc` (§29 penutupan OpenCode + FAKE_VIEW
lewat whitelist `env -i`)

**Verdict: SEMUA HIJAU — ronde penutup dikonfirmasi. 4/4 tugas hijau, tidak ada
temuan baru.**

## 1. Build (tugas 1)

- `git pull` → RC=0 (`35380b3..66da3dc`, §29).
- `./install.sh` → **RC=0**: `PT_INTERP: 24 file, 0 gagal`, quick-test hijau
  (`Test dinamis: 3.24.2`, `Isolasi: EACCES (benar)`).
  Log: `~/files/usr/tmp/opencode/install-r10.log`.

## 2. Escape hatch hidup (tugas 2)

```
$ env -u LD_PRELOAD FAKE_VIEW=container fake-run /bin/sh -c 'echo V=$FAKE_VIEW; cd /root && pwd -P'
V=container
/root                                      ← tampilan wadah lama pulih ✅

$ env -u LD_PRELOAD fake-run /bin/sh -c 'cd /root && pwd -P'
/data/data/com.termux/files/home/alpine-rootfs/root   ← default host ✅
```

- `V=container` + `pwd -P=/root` persis seperti harapan brief ✅; tanpa var →
  host ✅. Temuan §4 ronde 9 (var dibuang `env -i`) teratasi — pass-through ada
  di `fake-run:149` (`dbg_env+=("FAKE_VIEW=$FAKE_VIEW")`), sejajar SVSP_DEBUG.
- Catatan: variabel kustom di luar whitelist tetap dibuang (`MYVAR=[]`) — memang
  desain `env -i`, bukan masalah; hanya `FAKE_VIEW`/`SVSP_DEBUG`/`FK_DEBUG` yang
  diteruskan eksplisit.

## 3. Asap singkat (tugas 3)

| Tes | Hasil |
|---|---|
| `./selftest` | ✅ **0 FAIL** (RC=0), `akhir: DB tetap bersih` |
| `opencode --standalone` via **svsp** | ✅ TUI render (RC=124 = timeout mematikan TUI sehat) |
| `opencode --standalone` via **shim** | ✅ TUI render (RC=124), **0 baris error** |

Log: `~/files/usr/tmp/opencode/install-r10.log`, `r10-standalone-{svsp,shim}.log`.

## 4. Ringkas (tugas 4)

Semua hijau → **ronde penutup diterima dari sisi perangkat.** Tidak ada temuan
baru. Catatan arah brief: `FAKE_VIEW=container` tidak saya uji "memecahkan
sesuatu" lebih lanjut — hatch hanya jalan mundur; default host terbukti aman
(selftest 0 FAIL, matriks ronde 9 12/12).

## 5. Kejujuran & lingkungan

- Tidak ada perubahan config port round ini (49374 sejak ronde 9); host service
  utuh (401), 49474 kosong (000); tidak ada daemon tersisa (standalone pakai
  port privat, mati sendiri kena timeout); tidak ada file wadah yang diubah.
- Log mentah di luar repo; ringkasan tes = berkas ini.
- Riwayat lengkap: `ronde5.md` … `ronde9.md` + `selftest-ronde9.txt`.
