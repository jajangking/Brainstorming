# RONDE 17 — §38 terverifikasi separuh; akar uv ternyata lain (bukan banner)

## TL;DR
- **§38.2 (install tak lagi O(semua berkas)): BERHASIL** — `./install.sh` dengan
  `.hermes` tetap di tempatnya selesai **48 detik** (rone 16: hang >7 menit).
- **§38.1 (banner loader): BERHASIL** — loader patched tanpa argumen kini mencetak
  `musl libc (aarch64) / Version 1.2.6`.
- **TAPI Task 4 GAGAL:** uv **tetap** `Failed to determine the libc` tanpa
  `UV_LIBC`. Diagnostik baru: uv tak pernah menyentuh loader musl sama sekali —
  ia mengeksekusi **`/system/bin/linker64` (linker Android)**. Penyebabnya:
  **uv statis** (tak ada `PT_INTERP`) sehingga **LD_PRELOAD shim tak berlaku
  padanya**, lalu ia membaca `/bin/sh` **host Android** (bionic), bukan `/bin/sh`
  wadah. **Jadi `UV_LIBC=musl` wajib dipertahankan** — bukan sekadar sabuk.

---

## Task 1 — `time ./install.sh` (dengan `.hermes` tetap)  ✅
```
INSTALL_RC=0   DETIK=48
[+] PT_INTERP di-set: 28 file, 0 gagal
[✓] Test dinamis: 3.24.2
[✓] Isolasi: /system/build.prop → EACCES
```
Rone 16 hang karena `find` menyapu 16.168 berkas `>64c` di `.hermes/.git`;
§38.2 (prune `.git`/`node_modules`/…, lewati >64 MB, pra-saring magic ELF)
menuntaskannya dalam ~48 s. Jumlah PT_INTERP wajar (28; rone 16 = 30). ✅

## Task 2 — selftest + TUI  ✅
- `./selftest` → `RINGKASAN: 0 FAIL`.
- `alpine` (login) → `opencode` TUI: `daemon 49474 HTTP=200`, `TUI render: 1`,
  RC=124 (keluar karena EOF stdin) — normal.

## Task 3 — loader patched tanpa argumen  ✅ (dugaan §38.1 benar)
```
$ alpine -c '/lib/ld-musl-patched.so.1 2>&1 | head -4'
musl libc (aarch64)
Version 1.2.6
Dynamic Program Loader
Usage: /lib/ld-musl-patched.so.1 [options] [--] pathname [args]
```
Bukan lagi usage BusyBox. Hipotesis Anda — `fk_fix_loader_argv()` menyisipkan
`/proc/self/exe` saat argv kosong — **terkonfirmasi sebagai penyebab keluaran
BusyBox**, dan §38.1 memperbaikinya. ✅

## Task 4 — uv deteksi libc TANPA `UV_LIBC`  ❌ (akar berbeda)

```
UV=$HOME/.hermes/tools/uv-0.12.3-linux-arm64-musl/uv
UV_LIBC=[musl]                              # dari fake-run (sabuk)
$ env -u UV_LIBC "$UV" python install 3.14 -v
DEBUG uv 0.12.3 (aarch64-unknown-linux-musl)
error: Failed to determine the libc used on the current platform
  Caused by: Could not detect either glibc version nor musl libc version, ...
```

### Mengapa — jejak strace
`strace -f -e trace=openat,readlink,execve`:
```
openat(AT_FDCWD, "/bin/sh", O_RDONLY|...) = 10
execve("/system/bin/linker64", ["/system/bin/linker64"], ...) = 0          # tanpa arg
readlinkat(AT_FDCWD, "/system/bin/linker64", "/apex/com.android.runtime/bin/linker64", 256) = 38
execve("/system/bin/linker64", ["/system/bin/linker64", "--version"], ...) = 0
```
uv (a) membaca **`/bin/sh`**, (b) mengambil interpretor ELF-nya, (c) menjalankan
interpretor itu. Yang didapat: **`/system/bin/linker64`** — linker **Android**,
bukan `ld-musl-patched.so.1`. uv lalu gagal mem-parse "musl libc / Version".

### Kenapa `/bin/sh` = Android?
```
# host (Android):
-rwxr-xr-x 1 root shell 334872 /bin/sh            # bionic; interpreter = /system/bin/linker64

# di wadah via busybox (shim AKTIF):
/bin/sh -> busybox
interp: /lib/ld-musl-patched.so.1                 # benar! shim menerjemahkan
```
Shim **menerjemahkan `/bin/sh` dengan benar** untuk program dinamis. Tapi uv:
```
readelf -l uv | grep INTERP   -> (kosong)          # uv STATIS
```
**uv adalah biner musl statis.** LD_PRELOAD tidak bisa masuk ke biner statis,
jadi **shim path-translation tidak berlaku pada uv**. Akibatnya `open("/bin/sh")`
dari uv mengenai **`/bin/sh` host Android** (bionic) → ia menjalankan
`/system/bin/linker64`. Inilah akar sebenarnya kegagalan deteksi libc uv —
**bukan** banner loader.

### Konsekuensi
- §38.1 tetap perlu (banner loader benar), tetapi **tidak cukup** untuk uv,
  karena uv tak pernah menjalankan loader musl di jalur ini.
- **`UV_LIBC=musl` bukan sabuk opsional — ia satu-satunya jalur yang bekerja**
  untuk uv statis. Mohon **tetap dibiarkan** (bahkan bisa dibilang perbaikan
  definitif untuk wadah ini).
- Kalau ingin uv benar-benar auto-deteksi, opsi (semuanya di sisi Arena):
  1. Beri uv statis sebuah mekanisme path-translation (mis. bungkus dengan
     binary dinamis kecil yang shim-able, atau arahkan probing ke wadah).
  2. Atau sediakan `/bin/sh` wadah lewat mount/bind (butuh root — tak mungkin).
  3. Atau cukup andalkan `UV_LIBC` (paling pragmatis).
- Catatan: `uv ... python list` membuka `/data/.../alpine-rootfs/root/.cache/uv`
  dan bekerja — jadi uv statis **memang** menjalankan operasinya di wadah untuk
  path absolut dari env; hanya path hardcoded seperti `/bin/sh` yang bocor ke host.

## Task 4 (lanjutan) — `UV_LIBC` terlihat uv
`alpine -c 'echo $UV_LIBC'` → `musl` (fake-run baris 155). ✅
Tidak ada error "libc" lagi saat `UV_LIBC` aktif (rone 16).

## Task 5 — Hermes selesai? (opsional) — belum, DNS
Iterasi rone 16 masih berhenti di unduh Python (`dns error`, `EAI_AGAIN`).
Tidak diulang lebih jauh di rone ini karena murni jaringan & opsional; bila DNS
membaik, jalur lain sudah siap (`uv ready`, libc beres via `UV_LIBC`).

---

## Ringkasan
| Item | Hasil |
|---|---|
| §38.2 install cepat | ✅ 48 s (`.hermes` tetap), PT_INTERP 28 |
| `selftest` | ✅ 0 FAIL |
| `alpine` → opencode TUI | ✅ render |
| §38.1 banner loader patched | ✅ `musl libc (aarch64) / Version 1.2.6` |
| uv auto-deteksi tanpa `UV_LIBC` | ❌ tetap gagal — uv **statis**, membaca `/bin/sh` host Android → `/system/bin/linker64` |
| `UV_LIBC=musl` | **wajib dipertahankan** (bukan opsional) |
| Loader diubah? | Tidak |

Log: `$PREFIX/tmp/opencode/install-r17*.log`, `hermes-r16*.log`,
`r17-uv-probe*.sh`.