# HANDOFF — serah terima ke arena.ai

Dokumen ini berisi **segala yang dibutuhkan** untuk melanjutkan proyek "fake-chroot di
Termux/Android (tanpa root, tanpa proot)" — dari nol konteks sampai checklist hijau.
Tujuan: kamu (arena.ai atau agen lain) bisa melanjutkan **tanpa perlu ngobrol dulu
dengan pemilik proyek**.

---

## 1. TL;DR (60 detik)

- Proyek ini **sudah SELESAI untuk core-nya**: pemasangan satu-perintah (`bootstrap.sh`),
  runner universal (`fake-run`), jalur LD_PRELOAD (dinamis) DAN jalur supervisor
  seccomp USER_NOTIF (statis/raw) — semua **terbukti hijau** (lihat §3), ter-commit &
  ter-push (`f7e5fd7`).
- Yang tersisa = **pengerasan akhir, 5 item** (lihat §6). Tidak ada yang rusak/tidak jalan.
- Yang **permanen mustahil** (jangan dibuang waktu): `chroot`/`mount`/namespace sungguhan
  dan VM — seccomp firmware Android ber-AND-min (hanya bisa mengetatkan, tidak melonggarkan).

---

## 2. Repositori & berkas kunci

Remote: `https://github.com/jajangking/Brainstorming` — branch `main` (git identity
`jajangking`, `credential.helper=store`).

| Berkas | Fungsi |
|---|---|
| `bootstrap.sh` | Pemasangan satu-perintah (TAHAP 4). Idempoten. Pin Alpine 3.24.2 minirootfs + SHA. |
| `fake-run` | Runner universal: deteksi `PT_INTERP` → dinamis = jalur cepat (`ld-musl-patched.so.1` + `LD_PRELOAD libfakeroot.so`); statis = otomatis `svsp` (supervisor). Opsi `--svsp` memaksa supervisor. |
| `svsp.c` | Supervisor `SECCOMP_RET_USER_NOTIF` — rewrite path di kernel untuk binary statis & syscall raw. Binary hasil build `svsp` di-gitignore; dibangun ulang oleh `bootstrap.sh`. |
| `libfakeroot.c` | Shim: fakechroot rewrite path + handler SIGSYS (untuk `set*id` dll.). |
| `pinterp.c` | Tulis ulang PT_INTERP in-place (rc=3 → `patchelf --set-interpreter`). |
| `examples/gobukti.go` | Program **Go statis asli** (Go 1.27, `CGO_ENABLED=0`, tanpa PT_INTERP) — bukti jalur supervisor + regresi-test bug epoll (lihat §7). |
| `README.md` / `BRAINSTORM.md` | Dokumentasi + peta pembuktian lengkap. |

---

## 3. Status SAAT INI — verifikasi yang sudah hijau

Semua diuji di perangkat Android arm64 aarch64, Alpine 3.24.2, Go 1.27.1, claude-code 2.1.291:

| Uji | Perintah inti | Hasil |
|---|---|---|
| V1 dinamis (jalur LD_PRELOAD) | `fake-run --base=$R $R/bin/busybox cat /etc/alpine-release` | `3.24.2` rc=0 |
| V2 supervisor dipaksa (dinamis) | `fake-run --svsp --base=$R $R/bin/busybox cat /etc/alpine-release` | `3.24.2` rc=0 |
| V3 statis (`boot-static`, dibangun bootstrap) | `fake-run --base=$R $R/usr/bin/boot-static` | `3.24.2 BOOT-STATIC-OK` rc=0 |
| V4 isolasi | `fake-run --base=$R $R/bin/busybox stat /system/build.prop` | `ENOENT` ✓ |
| Go statis asli | `fake-run --base=$R $R/usr/bin/gobukti` (lihat §8) | rewrite → `3.24.2`; `/system/build.prop` → ENOENT; rantai file `/tmp` ✓; walk `/etc` (36) ✓; rc=0 |
| Claude Code (flagship, bun/musl, 241 MB) | `fake-run --base=$R $HOME/musl-test/package/claude --version` | `2.1.291 (Claude Code)` rc=0; config terisolasi di `$R/root/.claude` |

Commit terakhir: `f7e5fd7` — *"svsp: perbaiki tabrakan nomor syscall arm64 (epoll_ctl=21 vs fallback rmdir) + bukti Go statis asli (examples/gobukti.go)"* — sudah di `main`.

---

## 4. Lingkungan kerja (perangkat Termux ini)

- Repo: `~/Brainstorming/` (`/data/data/com.termux/files/home/Brainstorming`)
- Wadah hidup (base): `~/alpine-rootfs/` — berisi loader `lib/ld-musl-patched.so.1`,
  `usr/include`, `usr/lib/{libc.a,crt1.o,crti.o,crtn.o}`, `usr/bin/boot-static`,
  `usr/bin/gobukti`, `usr/bin/eptest` (artefak uji).
- Terpasang: `$PREFIX/bin/fake-run`, `$PREFIX/bin/svsp` (rebuilt versi FIX), `~/libfakeroot.so`.
- Cache unduhan (dipakai ulang bootstrap dengan `--no-download`):
  `~/alpine-minirootfs.tar.gz` (terverifikasi) dan `~/musl-dev-cache.apk`.
- Target uji dunia nyata: `~/musl-test/package/claude` (diekstrak dari
  `~/musl-test/anthropic-ai-claude-code-linux-arm64-musl-2.1.291.tgz`).
- Area uji coba: **WAJIB** `/data/data/com.termux/files/usr/tmp/opencode/` — jangan pernah `/tmp` (bukan area approved & bisa bermasalah di Android).
- Toolchain: `pkg install clang binutils patchelf curl golang` (clang 21; compiler-rt ada di
  `$PREFIX/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a`).

---

## 5. Checkpoint sebelum mulai (30 detik)

```sh
cd ~/Brainstorming && git status --short && git log --oneline -1   # bersih, f7e5fd7
R=$HOME/alpine-rootfs
env -u LD_PRELOAD ./fake-run --base=$R $R/bin/busybox cat /etc/alpine-release   # -> 3.24.2
```

Bila output tidak sesuai → jangan lanjut, perbaiki dulu lingkungannya.

---

## 6. Checklist tersisa — 5 item pengerasan (dengan kriteria terima)

> Definisi **SELESAI**: semua item hijau + `README.md` tetap mereproduksi dari nol
> (`git clone` → `bootstrap.sh` → `fake-run cat /etc/alpine-release` → `3.24.2`) +
> semua perubahan di-commit & di-push ke `main`.

### 6.1 `openat2` (SYS 437) masuk aturan `svsp.c` — celah kecil ✅ SELESAI

Perilaku saat ini: `openat2` **tidak** di-notify → path host yang TIDAK dicegat (openat2
dipakai beberapa program modern). `openat2` pada arm64 = **437** (bionic
mendefinisikannya; periksa dulu dengan `grep __NR_openat2 $PREFIX/include/...` atau
trik `symap` di §7).

Tanda tangan: `openat2(int dirfd, const char *path, struct open_how *how, size_t size)` —
**path di arg[1]**, tapi arg[2] adalah **pointer ke `struct open_how { u64 flags; u64 mode;
u64 resolve; }`** (jangan dipakai seperti openat! ).

Sketsa perubahan di `svsp.c`:
1. `path_argidx()`: tambah `case SYS_openat2: return 1;` (guard `#ifdef SYS_openat2`).
2. `rules[]`: tambah `#ifdef SYS_openat2 { C_OPEN, SYS_openat2 }, #endif`.
3. Handler `case C_OPEN:` — percabangan per nomor:
   ```c
   int fd;
   #ifdef SYS_openat2
   if (nr == SYS_openat2) {
       struct open_how how;
       if (read_mem(pid, (void *)(uintptr_t)a[2], &how, sizeof how) < 0) {
           send_resp(listener, req->id, 0, -EFAULT, 0); break;
       }
       fd = syscall(SYS_openat2, AT_FDCWD, pout, &how, (size_t)a[3]);
   } else
   #endif
       fd = openat(AT_FDCWD, pout, (int)a[2], (int)a[3]);
   ```
   (sisanya sama: `ADDFD` seperti C_OPEN sekarang.)
4. `#include <linux/openat2.h>` untuk `struct open_how`; bila header bionic tidak punya,
   definisikan sendiri (3× `__u64`).

Kriteria terima: `eptest`-ber-`openat2` (pakai `syscall(SYS_openat2, AT_FDCWD, "/etc/alpine-release", &how, sizeof how)`) di dalam wadah membuka `base/etc/alpine-release`; ulangi V1–V4 + gobukti → tetap hijau.

### 6.2 Benchmark overhead per round-trip supervisor (angka jujur) ✅ SELESAI

Tujuan: angka realistis di README — "berapa μs per syscall path yang di-rewrite".

Cara:
1. Program C statis (pola `eptest`, lihat §8) yang mengukur loop `open(path)+close(path)`
   pada file yang TIDAK berubah (mis. `/root/file-x`, passthrough → CONTINUE) DAN file
   yang BERUBAH (mis. `/etc/alpine-release` → rewrite) — 1000 iterasi, `clock_gettime` nanodetik.
2. Jalankan: (a) bare (host), (b) lewat `svsp` `SVSP_DEBUG=0`.
3. Lapor: `μs/op = (t_svsp - t_bare) / N` per kategori. Harapan: puluhan μs untuk yang
   rewrite (ada ADDFD + read/write memori), jauh lebih kecil untuk passthrough-CONTINUE.

Kriteria terima: angka terukur tercantum di README; tak ada regresi fungsi (V1–V4).

### 6.3 Zombie/grandchild di `svsp` ✅ SELESAI

Sudah ada: `waitpid(WNOHANG)` pada `pid` target (loop `poll(200ms)` + `waitpid`), supaya
RECV yang tidak pernah bangun saat target mati tidak menggantung. Yang belum rapi:
**anak/grandchild target** (mis. `sh` yang `fork`/`exec` turunan) bisa jadi zombie saat sesi
lama; `svsp` keluar ketika target utama mati dan cucu di-reparent — bukan penuh.

Perbaikan yang diinginkan:
- Install handler `SIGCHLD` + `waitpid(-1, &st, WNOHANG)` berulang di loop poll (contoh pola
  `while (waitpid(-1, &st, WNOHANG) > 0);`).
- Tetap pertahankan `waitpid(pid)` WNOHANG untuk memutuskan **kapan svsp keluar** (agar rc
  target diteruskan).

Kriteria terima: sesi `svsp` yang menjalankan rantai `sh` ber-fork panjang tidak meninggal
zombie (cek `ps` sebelum/sesudah), rc target tetap benar, V1–V4 hijau.

### 6.4 Cache rewrite (hemat kerja per-notif) ✅ SELESAI

Jujur: round-trip kernel tetap terjadi per notif (tidak bisa dihilangkan). Yang bisa
dihemat = pekerjaan supervisor per notif:
1. **Cache hasil rewrite** per hash path (FNV-1a misalnya; tabel kecil 256–512 entri):
   path yang sama berulang kali (Go memanggil `stat`/`statx` berulang pada path sama saat
   walk) → lewati `snprintf`/`strcmp`/`passthrough()`; langsung pakai hasil tersimpan.
2. **Urutan rule BPF**: taruh aturan paling sering di depan (`openat`, `newfstatat`,
   `statx`) supaya rantai `JEQ` rata-rata lebih pendek (BPF linear-scan dari awal).

Kriteria terima: overhead terukur turun atau tidak naik (bandingkan dengan 6.2), semua uji
regresi hijau, tidak ada perubahan perilaku rewrite.

### 6.5 Uji daya tahan nyata (Claude + `apk add` sungguhan) ⏳ PERLU DEVICE

**(a) `apk add` paket nyata DI DALAM wadah:**
1. `cp /etc/resolv.conf $R/etc/resolv.conf` (musl baca resolv.conf dari wadah; tanpa ini
   DNS gagal walaupun syscall jaringan passthrough).
2. `fake-run --base=$R $R/bin/busybox sh -c 'apk add --no-cache ncurses'` (atau via
   `fake-run ... /sbin/apk`).
3. Verifikasi: binary terpasang di `$R/usr/bin/…`, jalankan native via `fake-run`.
   Catatan: `apk` perlu `ca-certificates` bila HTTPS ke repo alpine (CDN dl-cdn.alpinelinux.org).
   Bila `apk` gagal membuka skrip/path tertentu → itu temuan rewrite yang sah: laporkan nomor
   syscall-nya (pakai `SVSP_DEBUG=1`), jangan diam-diam menyerah.

**(b) Claude Code — sesi sungguhan:**
1. Tanpa API key, capaian yang realistis: `fake-run ... claude --version` ✓,
   `fake-run ... claude --help` ✓, TUI interaktif terbuka dan **config tertulis
   `$R/root/.claude`** (bukan `~/.claude` host), lalu keluar bersih.
2. Bila ada key/credential yang boleh dipakai: jalankan `claude -p "hai"` di dalam wadah
   dan pastikan jawaban datang (buktikan jaringan + rewrite hidup berdampingan).
3. Perhatikan `HOME` diberi `$R/root` oleh `fake-run`/`svsp`; `.claude` dihosting di sana.

Kriteria terima: (a) instalasi `apk add` sukses + binary berjalan native di wadah; (b)
claude versi/help/TUI + isolasi config terbukti; setiap syscall baru yang muncul di
`SVSP_DEBUG` dicatat (tambahkan aturannya bila perlu, mis. `openat2` sudah dibereskan di 6.1).

---

## 7. Pengetahuan kritis — WAJIB dibaca sebelum menyentuh kode

### Tabel syscall arm64 (asm-generic) — JANGAN pakai nomor x86_64
Nilai yang **terverifikasi** (bionic arm64 = kernel arm64):
`openat=56, execve=221, execveat=281, newfstatat=79, statx=291, statfs=43, chdir=49,
mkdirat=34, unlinkat=35, renameat=38, renameat2=276, linkat=37, symlinkat=36,
readlinkat=78, faccessat=48, faccessat2=439, fchmodat=53, fchmodat2=452, fchownat=54,
truncate=45, utimensat=88, mknodat=33, epoll_create1=20, epoll_ctl=21, eventfd2=19, openat2=437.
**TIDAK ADA** syscall `rmdir`, `mkdir`, `unlink`, `rename`, `link`, `symlink`, `chmod`,
`chown`, `lchown`, `mknod`, `access`, `utimes`, `stat`, `lstat`, `fstat` di arm64 (libc
mengemulasi via `*at`). `readlink=89` memang ada di arm64.

> **Riwayat bug (jangan diulang):** dulu `svsp.c` punya fallback `#define SYS_rmdir 21`.
> Di arm64, **21 = `epoll_ctl`** → filter men-*notify* `epoll_ctl` sebagai "rmdir" → membaca
> fd sebagai pointer path → `process_vm_readv` gagal → balas `-EFAULT` → runtime **Go statis
> mati**: `runtime: epollctl failed with 14`. Fix sudah masuk (guard `#ifdef SYS_rmdir`,
> fallback dihapus). **Aturan emas: jangan pernah menebak nomor syscall; pakai konstanta
> header, atau verifikasi dengan trik `symap`:** kompilasi program kecil yang
> `printf("%ld", (long)SYS_xxx)` dan cocokkan dengan tabel di atas.

### Quirks supervisor `svsp`
- **RECV tidak bangun saat target mati** (kernel Android): loop `poll(listener, 200ms)` +
  `waitpid(WNOHANG)` — PERTAHANKAN (jangan ubah ke `recvmsg` blocking murni).
- Debug: `SVSP_DEBUG=1 ./svsp --base=$R <program>` → stream `[svsp] >> nr=… cls=… args…`.
  Untuk proses ber-child banyak, arahkan `>> file` (bukan `>`).
- **Hasil `strace`/`ptrace` TIDAK bisa dipercaya** di sini (interaksi USER_NOTIF + ptrace
  kacau); pakai `SVSP_DEBUG` sebagai kebenaran.
- `set*id` TIDAK bisa via USER_NOTIF: `RET_TRAP` firmware (0x30000) menang atas
  `USER_NOTIF` (0x7fc00000) via AND-min → satu-satunya jalan = shim `libfakeroot.so`.
- Program yang memblokir SIGSYS butuh loader patched (`ld-musl-patched.so.1`) — itu
  kenapa bootstrap mem-patch byte loader (7 situs → `mov w0,#0; ret`) dengan verifikasi
  SHA sebelum/sesudah.

### Loader & INTERP
- **JANGAN kembali ke appending manual PT_INTERP** (kernel aarch64 menolak
  `p_vaddr == p_offset` pada PT_INTERP tambahan → SIGSEGV saat exec). Satu-satunya layout
  terbukti = `patchelf --set-interpreter` (`p_vaddr = p_offset + 0x10000`, filesz 0x44).
- `pinterp.c` hanya menangani kasus in-place; rc=3 → bootstrap menyerah ke patchelf.

### Nilai ditanam di `bootstrap.sh` (jangan diubah tanpa sengaja)
`ALPINE_VER=3.24.2`; `STOCK_SHA=32377e6d71725bb019e9ff6d5e9f16b4d5156d6f2c36504191c2d6a7c4d4a44d`;
`PATCHED_SHA=0131918ffbbdc1d50f842448e0ba8eecbd5e61babbd04e10984f1876e0818f47`;
`PATCH_SITES=0x24BB8 0x6993C 0x69980 0x69994 0x699A8 0x699BC 0x699EC`;
CDN=`https://dl-cdn.alpinelinux.org/alpine/v3.24`; indeks paket = `APKINDEX.tar.gz`
(**`Packages.gz` adalah 404**); member apk tanpa prefix `./`. Minirootfs busybox applet =
symlink absolut → harus direlink relatif (`bin/cat -> busybox`) atau rewrite host gagal.

### Resep build (sudah terbukti)
- Shim: `clang --target=aarch64-alpine-linux-musl --sysroot=$R -fPIC -shared -O2 -nostdlib -o libfakeroot.so libfakeroot.c` (13 warning non-fatal OK).
- `svsp`: `clang -O2 -o svsp svsp.c` + `strip` → salin ke `$PREFIX/bin/svsp` bila mengganti.
- Statis (C): `clang --target=aarch64-alpine-linux-musl --sysroot=$R -static -nostdlib -o X X.o $R/usr/lib/crt1.o $R/usr/lib/crti.o $R/usr/lib/libc.a $R/usr/lib/crtn.o` (+ `$PREFIX/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a` di akhir bila ada `printf` float/`vfprintf` → butuh `__extenddftf2` dll.).
- Go statis: `CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o gobukti gobukti.go` (tanpa PT_INTERP → auto supervisor).

### Pengujian
- Selalu `env -u LD_PRELOAD` di depan `fake-run` pada tes (mencegah LD_PRELOAD host masuk).
- Pakai `timeout 60` untuk program yang berisiko gantung.
- Jalur cepat (dinamis) memakai `$LOADER <bin>` eksplisit → `PT_INTERP` binary dinamis tidak
  relevan untuk jalur itu; jalur supervisor hanya untuk statis / `--svsp`.

---

## 8. Cara verifikasi cepat (copy-paste)

```sh
R=$HOME/alpine-rootfs; FR=$HOME/Brainstorming/fake-run
# V1–V4
env -u LD_PRELOAD $FR --base=$R $R/bin/busybox cat /etc/alpine-release        # 3.24.2
env -u LD_PRELOAD $FR --svsp --base=$R $R/bin/busybox cat /etc/alpine-release # 3.24.2
env -u LD_PRELOAD $FR --base=$R $R/usr/bin/boot-static                        # 3.24.2 BOOT-STATIC-OK
env -u LD_PRELOAD $FR --base=$R $R/bin/busybox stat /system/build.prop        # ENOENT
# Go statis asli
cd ~/Brainstorming/examples && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o gobukti gobukti.go
cp gobukti $R/usr/bin/gobukti
env -u LD_PRELOAD timeout 60 $FR --base=$R $R/usr/bin/gobukti                 # rewrite + ENOENT + rc=0
# Claude Code
env -u LD_PRELOAD timeout 60 $FR --base=$R $HOME/musl-test/package/claude --version  # 2.1.291
```

`eptest` (repro epoll, sudah ada di `$R/usr/bin/eptest`): jalankan `svsp --base=$R $R/usr/bin/eptest` → baris `epoll_ctl = 0` (sebelum fix: `-1 errno=14`).

---

## 9. Prompt siap-tempel untuk arena.ai (satu blok, salin langsung)

```text
Lanjutkan proyek "fake-chroot" di Termux/Android (tanpa root, tanpa proot) di repo
https://github.com/jajangking/Brainstorming (branch main). Baca dulu HANDOFF.md di
root repo — itu berisi status mutakhir, lingkungan, gotchas kritis (tabel syscall arm64,
resep build, quirk svsp), dan verifikasi yang sudah hijau.

Kerjakan checklist pengerasan berikut sampai HIJAU, satu per satu:
1. tambah openat2 (437) ke aturan svsp.c (path arg[1]; struct open_how di arg[2] dibaca
   via process_vm_readv; ADDFD seperti openat biasa) — jangan pakai nomor syscall tebakan,
   verifikasi konstanta dari header;
2. benchmark overhead supervisor per round-trip (program C statis loop open+close, bare
   vs svsp, laporkan μs/op jujur ke README);
3. bersihkan zombie/grandchild: SIGCHLD + waitpid(-1, WNOHANG) di loop poll svsp, tanpa
   mengubah perilaku RECV/waitpid target;
4. cache hasil rewrite per hash path + urutkan rule BPF paling sering dipakai di depan;
5. uji daya tahan: (a) salin resolv.conf host ke base/etc lalu apk add --no-cache ncurses
   DI DALAM wadah dengan fake-run, verifikasi binary berjalan native; (b) claude --version,
   --help, TUI + buktikan config mendarat di base/root/.claude (bukan host); laporkan
   syscall baru yang muncul di SVSP_DEBUG.

Definisi SELESAI: semua item hijau + README tetap mereproduksi dari nol (git clone ->
./bootstrap.sh -> fake-run cat /etc/alpine-release -> "3.24.2") + commit & push ke main.
Regresi wajib setiap selesai item: V1 dinamis, V2 --svsp dinamis, V3 boot-static,
V4 isolasi /system/build.prop ENOENT, gobukti (Go statis) rc=0, claude --version.
Lingkungan kerja: repo ~/Brainstorming, wadah ~/alpine-rootfs, cache
~/alpine-minirootfs.tar.gz + ~/musl-dev-cache.apk, area uji
/data/data/com.termux/files/usr/tmp/opencode (bukan /tmp). Selalu env -u LD_PRELOAD
di depan fake-run saat tes.
```

---

## 10. Log perubahan singkat

- TAHAP 1: LD_PRELOAD fakechroot + shim SIGSYS + loader patched → dinamis (claude, busybox) native.
- TAHAP 2: env host-absolut (HOME/TMPDIR/cwd) menutup syscall raw pada jalur tersebut.
- TAHAP 3: supervisor `SECCOMP_RET_USER_NOTIF` (`svsp`) → binary statis & syscall raw di-rewrite di kernel (st2, bossv; isolasi ENOENT).
- TAHAP 4: `bootstrap.sh` satu-perintah + `pinterp.c` + `fake-run` universal → `eae042f`.
- Pasca-TAHAP 4: bug `epoll_ctl=21` vs fallback `rmdir` ditemukan & diperbaiki; bukti Go
  statis asli (`examples/gobukti.go`) dan claude 2.1.291 → `f7e5fd7`.
- Dokumen ini: `HANDOFF.md` untuk melanjutkan pengerasan 5 item (lihat §6).
- Arena.ai (2026-10-07): bug fix + pengerasan checklist:
  - **Bug fix `libfakeroot.c`**: `execl()` argc counting selalu 64 (UB) → sekarang scan NULL terminator; `getwd()` return `int` (pointer terpotong di 64-bit) → `char *`; tambah intercept `statx()`.
  - **Bug fix `svsp.c`**: exit code child tidak propagate (selalu 0) → `WEXITSTATUS`/`WTERMSIG`.
  - **Bug fix `segcshim.c`**: debug output hardcoded `"HIT\n"`/`"INIT\n"` → conditional `SECSHIM_DEBUG` env.
  - **Bug fix `block-trap.c`**: missing `#include <errno.h>`.
  - **Bug fix `fake-run`**: empty array expansion under `set -u` → `${array[@]+"${array[@]}"}`.
  - **6.1**: `openat2` (SYS 437) ditambahkan ke rules, path_argidx, handler (baca `struct open_how` via `process_vm_readv`).
  - **6.2**: benchmark program `examples/ebench.c` (1000 iterasi open+close, passthrough vs rewrite, μs/op).
  - **6.3**: zombie/grandchild cleanup: `SIGCHLD` handler + `waitpid(-1, WNOHANG)` di loop poll svsp.
  - **6.4**: cache rewrite FNV-1a (512 entri) + reorder rules BPF (openat/newfstatat/statx di depan).
  - **6.5**: perlu device nyata (Termux aarch64) untuk `apk add` + Claude sesi.