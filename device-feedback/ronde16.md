# Ronde 16 — Laporan Device

Ringkas: **§37 (`UV_LIBC=musl`) berhasil** — uv kini tak lagi "Failed to determine
the libc", Hermes lolos ke tahap unduh Python dan berhenti hanya karena **DNS
jaringan** (bukan bug wadah). **Dugaan Anda soal loader terkonfirmasi**: loader
**stock** mencetak banner `musl libc (aarch64) / Version 1.2.6`, loader
**patched TIDAK** (malah mencetak usage BusyBox). Plus satu temuan baru:
**`./install.sh` nyaris macet (hang) bila di dalam `$BASE` ada klon `.hermes`.**

---

## 1. `git pull && ./install.sh` + `./selftest`

- `git pull` → HEAD sekarang **9bd93da** (§37) — sudah termasuk §36 penutupan SIGSYS.
- **TEMUAN BARU (penting): `./install.sh` praktis HANG bila ada `.hermes`.**
  Penyebabnya langkah `rewrite_interp`:
  ```
  while IFS= read -r f; do ... pinterp "$f" ... done < <(find "$BASE" -type f -size +64c)
  ```
  `find` menyapu **seluruh `$BASE`** — termasuk `root/.hermes/hermes-agent/.git`
  milik Anda. Di wadah ini jumlah berkas `>64c` di `.hermes` = **16.168**, dan
  `find` total base jauh lebih besar; tiap berkas dipanggil `pinterp` satu per satu.
  Akibatnya install berjalan >7 menit tanpa penanda selesai (dua kali `timeout`
  600s habis). Ini bukan hang mutlak, tapi O(semua berkas) yang tak praktis.
  - **Saran perbaikan (bukan saya kerjakan):** prune `.git`/`objects` dan/atau
    batasi `find` ke direktori biner (`bin`, `sbin`, `usr/bin`, `lib`), atau lewati
    berkas > ambang (mis. > 8 MB) — objek git/pack tidak pernah ELF ber-`INTERP`.
- **Workaround untuk uji ini:** `.hermes` dipindah sebentar ke luar `$BASE`, lalu:
  ```
  INSTALL_RC=0
  [+] PT_INTERP di-set: 30 file
  ```
  setelah itu `.hermes` dipulihkan ke tempat semula.
- **`./selftest` → `RINGKASAN: 0 FAIL`.** Tidak ada regresi dari §37.

## 2. Diagnostik akar loader (yang Anda minta) — **dugaan Anda BENAR**

Dijalankan di dalam wadah, tanpa argumen program, `stdin=/dev/null`, `timeout 5`:

```
=stock no-arg=   /lib/ld-musl-aarch64.so.1
rc=1
musl libc (aarch64)
Version 1.2.6
Dynamic Program Loader
Usage: /lib/ld-musl-aarch64.so.1 [options] [--] pathname [args]

=patched no-arg= /lib/ld-musl-patched.so.1
rc=0
BusyBox v1.37.0 (2026-01-10 15:38:28 UTC) multi-call binary.
BusyBox is copyrighted by many authors between 1998-2015.
Licensed under GPLv2. ...
```

- **Stock loader** mencetak `musl libc (aarch64)` + `Version 1.2.6` — persis yang
  digrep uv.
- **Patched loader TIDAK mencetak banner itu.** Ia justru berhenti `rc=0` dan
  mencetak **usage BusyBox** (aneh, tapi terulang konsisten 3×).

Catatan tambahan:
- `PT_INTERP /bin/busybox` (di wadah) = `/lib/ld-musl-patched.so.1`, jadi jalur
  "jalankan loader dari PT_INTERP tanpa argumen" memang mengarah ke loader patched.
- `grep` string di **berkas host** `$BASE/lib/ld-musl-patched.so.1` masih
  mengandung `musl libc` dan `Dynamic Program Loader` (ukuran identik 723480 B
  dengan stock). Jadi banner-nya masih ada di dalam biner, tapi **jalur
  eksekusi tanpa-argumen tidak mencetaknya** — ada indikasi patch SIGSYS in-place
  ikut mengubah/menggeser alur saat `argv[1]` kosong. **Mekanisme persisnya saya
  serahkan ke Anda** (brief: jangan ubah loader).
- Kesimpulan untuk uv: langkah "deteksi libc = jalankan loader PT_INTERP, cari
  `Version x.y`" **gagal pada loader patched** karena banner tak muncul. Karena itu
  `UV_LIBC=musl` adalah obat yang tepat — dan §37 memakai jalur itu.

## 3. Ulang Hermes — **§37 BERHASIL**, tahap Python lewat deteksi libc

`UV_LIBC` terlihat di dalam wadah: `UV_LIBC=[musl]` (`fake-run` baris 155:
`UV_LIBC=${UV_LIBC:-musl}`).

```
✓ prerequisites ok (git, curl)
✓ uv ready (uv 0.12.3 (aarch64-unknown-linux-musl))
→ Downloading Python 3.14
error: Failed to install cpython-3.14.7-linux-aarch64-musl
  Caused by: Request failed after 3 retries in 45.6s
  Caused by: ... cpython-3.14.7+20260807-aarch64-unknown-linux-musl-...tar.gz
  Caused by: client error (Connect)
  Caused by: dns error
  Caused by: failed to lookup address information: Try again
✗ bootstrap Python installation failed
```

Tafsir:
- `Failed to determine the libc used on the current platform` **SUDAH HILANG.**
  uv melaporkan `uv 0.12.3 (aarch64-unknown-linux-musl)` dengan lancar → **§37
  terkonfirmasi memperbaiki akar yang Anda duga.** ✅
- Kegagalan sekarang murni **jaringan**: `dns error` → `EAI_AGAIN` saat unduh
  Python dari `github.com/astral-sh/...` (redirect ke `objects.githubusercontent`).
- Diuji 2×, hasil sama. Menarik: `git fetch origin/main` **berhasil**
  (github.com resolvable via musl), dan `nslookup github.com` di wadah juga
  menjawab (via 1.1.1.1) — tapi resolver yang dipakai uv (`getaddrinfo` musl)
  mengembalikan `Try again`. Ini **isu jaringan/resolver lingkungan**, bukan bug
  fake-chroot; besar kemungkinan transient (andai diulang saat DNS stabil, unduhan
  mungkin lanjut).
- Bila Anda ingin memastikan, jalankan `uv python install 3.14 -v` di wadah saat
  jaringan sehat; baris tracing libc **sudah tidak muncul** sebagai error.

## 4. Ringkasan status

| Item | Hasil |
|---|---|
| `install.sh` | RC=0 **setelah** `.hermes` disingkirkan; **sebelumnya hang** (find menyapu 16k+ berkas `.git`) |
| `selftest` | **0 FAIL** |
| Loader stock | cetak banner `musl libc (aarch64) / Version 1.2.6` |
| Loader patched (tanpa arg) | **tak cetak banner**; cetak usage BusyBox, rc=0 → akar kegagalan uv **terkonfirmasi** |
| `UV_LIBC` di wadah | `musl` ✓ |
| Hermes | lolos deteksi libc (uv ready); **hanya** tersangkut DNS unduh Python |
| Loader diubah? | **Tidak** (sesuai brief) |

## 5. Catatan lingkungan

- Di wadah kini ada artefak uji pihak pemilik: `root/.hermes/hermes-agent` (klon
  git + `uv` 0.12.3) dan `git` terpasang. Itu yang memicu temuan #1.
- `$PREFIX/tmp/opencode/`: `install-r16.log`, `install-r16b.log`, `install-r16c.log`,
  `hermes-r16.log`, `hermes-r16b.log`, `r16-loader-test.sh`.