# Device Feedback — Ronde 14 (uji ulang gejala Hermes; bug SIGSYS lebih dalam)

Tanggal: 2026-10-08 · Basis: `3912529` (§34 `fk_sigsys` default `-ENOSYS`)

**Verdict: §34 MEMPERBAIKI kasus dasar (dulu kosong → kini terisi), tapi gejala
Hermes BELUM hilang.** Akar sebenarnya lebih dalam: **program yang memasang
handler SIGSYS-nya sendiri (mis. `bash` begitu ada `trap`) menimpa handler
libfakeroot**, sehingga konversi `-ENOSYS` tak pernah jalan dan `faccessat2` yang
diblokir Android langsung mematikan proses.

---

## 1. Tugas 1 — install

- `git pull` → RC=0 (`abb9b27..3912529`, §34).
- `./install.sh` → **RC=0** (`PT_INTERP: 24 file, 0 gagal`), config service
  `port 49474` dibiarkan utuh. Log: `install-r14.log`.

## 2. Tugas 2 — regresi (hijau)

| Tes | Hasil |
|---|---|
| `./selftest` | ✅ **0 FAIL** |
| `alpine` → `opencode` | ✅ **TUI render** (daemon 49474 connect) |

## 3. Tugas 3 — uji ulang gejala, DENGAN shim

### 3a. Kasus dasar: **FIXED** ✅

```
alpine -c 'uname -s; type uname; echo "[$(uname -s)]"'
Linux
uname is /bin/uname      <-- dulu KOSONG
[Linux]                  <-- dulu KOSONG
```

### 3b. Script Hermes penuh: **MASIH GAGAL** ❌

```
alpine -c 'bash /tmp/hermes-install.sh'
✗ unsupported platform: . On Windows use install.ps1.
Bad system call  (RC=159 / SIGSYS)
```

## 4. Tugas 4 — akar & `si_syscall`

strace t2 (skrip mini) menyisakan:

```
--- SIGSYS {si_code=SYS_SECCOMP, si_syscall=__NR_faccessat2} ---
```

Tapi yang menentukan bukan syscall-nya (itu sudah Anda perbaiki) melainkan
**siapa pemilik handler SIGSYS saat kejadian**.

**Repro minimal** (di dalam wadah, shim aktif):

| Skrip | Hasil |
|---|---|
| `set -u; echo "[$(uname -s)]"` | `[Linux]` ✅ |
| `set -u; trap ":" EXIT; echo "[$(uname -s)]"` | `[]` ❌ (anak mati SIGSYS) |
| `trap ":" INT` / `USR1` / `ERR` / `trap - EXIT` | `[Linux]` ✅ |
| `trap ":" EXIT; /bin/uname -s` (absolut) | jalan ✅ |
| `trap ":" EXIT; x=$(/bin/uname -s)` | `[Linux]` ✅ |
| `trap ":" EXIT; x=$(env)` | kosong ❌ (pencarian PATH) |

Jadi pemicunya **`trap ... EXIT` + perintah yang butuh pencarian PATH**
(`access()` → `faccessat2`).

**Bukti strace (sigaction):**

```
8:   rt_sigaction(SIGSYS, {sa_handler=0x72f6156608, ...}, NULL) = 0      <-- libfakeroot
61:  rt_sigaction(SIGSYS, {sa_handler=0x557ce5f9b0, ...}, {0x72f6156608...}) = 0  <-- BASH menimpanya (saat trap di-set)
108: [child] --- SIGSYS {si_code=SYS_SECCOMP, si_syscall=__NR_faccessat2} ---
115: [child] rt_sigaction(SIGSYS, {SIG_DFL}, {0x557ce5f9b0...}) = 0       <-- reset ke default
116: [child] kill(getpid(), SIGSYS) = 0
117: +++ killed by SIGSYS +++
```

→ `fk_sigsys(int ENOSYS)` hanya menolong bila handler libfakeroot **masih
terpasang**. Bash (dan program lain yang menginstall handler SIGSYS) menimpa-nya;
sejak itu seccomp-TRAP Android langsung mematikan proses. Itulah sebabnya
`uname -s` normal di kasus dasar tapi kosong di dalam script Hermes (yang punya
`trap ... EXIT`).

## 5. Bonus — jalur svsp juga belum menolong kasus ini

```
fake-run --svsp /bin/sh -c 'bash /tmp/t2.sh'   -> RC=159 (masih SIGSYS)
```

`SVSP_DEBUG=1` menunjukkan supervisor hanya mem-filter `newfstatat` (nr=79)
untuk path-rewrite, **`faccessat2` (nr=439) tidak ada di daftar** → lolos ke
seccomp → mati di tangan handler bash. (Tanpa trap, svsp biasa jalan:
`uname -s`/`sub=[Linux]` OK.)

## 6. Tugas 5 — perilaku yang BERUBAH karena ENOSYS

Tidak ada regresi yang teramati: `selftest` 0 FAIL, `alpine`→`opencode` TUI
render, `uname`/`type`/substitusi dasar normal. Belum ketemu program yang dulu
"jalan karena EPERM" lalu rusak. Bila ditemukan nanti, akan saya catat.

## 7. Rekomendasi (untuk Arena — bukan keputusan device)

1. **`faccessat2` (+kelas syscall stat/akses) masuk filter `svsp` USER_NOTIF** —
   jalur supervisor kebal terhadap masalah "program menimpa handler SIGSYS".
2. Atau: pertimbangkan melindungi handler SIGSYS shim agar tidak bisa ditimpa
   (mis. re-arm saat terdeteksi), walau ini rapuh.
3. Jalan pintas pengguna tetap: `env -u LD_PRELOAD ...` / jalur svsp tanpa shim.

## 8. Kejujuran & lingkungan

- Deteksi Termux di installer Hermes (`PREFIX`/`TERMUX_VERSION`) = kebijakan
  installer, **bukan** bug kita (saya sepakat dengan §34.3).
- Shutdown bersih: port tetap **49474** (baseline), host 49374 utuh (401),
  daemon uji tidak mengubah wadah. `strace` dipasang di wadah untuk diagnostik
  (`apk add strace`, 24.4 MiB) — dicatat sebagai perubahan isi wadah.
- Log: `install-r14.log`, `hermes-r14-shim.log`, `hermes-r14-svsp.log`,
  `hermes-dbg3.log`, `r14-alpine-opencode.log`, `t2-sig.log`, `svsp-t2.log`,
  `hermes-r14-strace.log` (di `~/files/usr/tmp/opencode/`).