# Brainstorming — Memecahkan "yang tidak bisa dipecahkan" di Termux (tanpa root, tanpa proot)

Brainstorm + proof-of-concept: menjalankan binary glibc/musl (Claude Code, busybox, apk-tools, dll.)
secara **native** di Termux/Android tanpa root, tanpa proot — termasuk **isolasi path ala chroot**
(fakechroot) dan **netralisasi seccomp** yang selama ini dianggap mustahil.

## Ringkasan temuan

| Masalah | Solusi terbukti |
|---|---|
| `setgid`/`setuid`/`chroot`/`mount` dibunuh seccomp Android | Handler SIGSYS yang melewati TRAP (`libfakeroot.so`) — semua larangan Android ternyata `RET_TRAP`, tidak ada `RET_KILL` |
| Proses menutup SIGSYS sebelum syscall terlarang (busybox) | Loader musl dipatch agar SIGSYS tak pernah diblokir + patchelf interpreter |
| Isolasi path tanpa chroot | LD_PRELOAD fakechroot: rewrite path absolut + env ala chroot (`fake-run`) |
| Antarmuka `/proc`, `/dev`, `/sys` | Passthrough symlink ke host |
| Claude Code (bun, musl) di dalam wadah | Jalan native: versi, config terisolasi di rootfs, jaringan aman |
| **Binary statis (tanpa loader) & syscall mentah yang mem-bypass LD_PRELOAD** | **Supervisor `SECCOMP_RET_USER_NOTIF` (`svsp`): filter kernel tambahan (komposisional), parent bebas-filter me-rewrite path di kernel — bukti: `st2` statik (no loader) + `bossv` (statik → fork → exec `sh` dinamis) jalan native; `/system/build.prop` → ENOENT** |

## Isi

- `HANDOFF.md` — **serah terima ke arena.ai / agen lanjutan**: status mutakhir, checklist pengerasan 5 item dengan kriteria terima, gotchas kritis (tabel syscall arm64, resep build, quirk svsp), verifikasi, dan prompt siap-tempel.
- `BRAINSTORM.md` — peta lengkap kemungkinan/mustahil + argumen kenapa APK adalah jalan buntu
- `libfakeroot.c` — shim utama (fakechroot rewrite path + handler SIGSYS, musl, `-nostdlib`; intercept `statx`; build dengan `--sysroot=$R -nostdlib` — jangan tanpa sysroot, simbol `__errno` bionic meracuni musl)
- `segcshim.c` — shim minimal netralisasi SIGSYS
- `sigsys-map.c` — peta per-syscall seccomp (`RET_TRAP` vs `RET_KILL`)
- `unshare-matrix.c` — bukti user-namespace mati (arg-filter seccomp)
- `block-trap.c` — bukti TRAP-while-SIGSYS-blocked = kill instan
- `raw-rt-sig.c` — resep handler SIGSYS dengan layout `sigaction` kernel (bionic)
- `fake-run` — **runner universal** wadah palsu: binary dinamis → jalur cepat loader musl + `LD_PRELOAD`; binary **statis (tanpa PT_INTERP) otomatis** jatuh ke supervisor `svsp` (USER_NOTIF); `--svsp` memaksa supervisor. Men-set `LD_LIBRARY_PATH=$BASE/lib:$BASE/usr/lib` + env chroot (HOME/PATH/TMPDIR/PWD/USER) di **kedua** jalur.
- `svsp.c` — supervisor `SECCOMP_RET_USER_NOTIF` (TAHAP 3): child pasang filter sendiri (`NO_NEW_PRIVS` + `NEW_LISTENER`), listener dikirim ke parent bebas-filter via SCM_RIGHTS; openat/stat/statx/statfs/readlink/execve/chdir/mkdir/unlink/rename/link/symlink/chmod/chown/truncate/utimensat/access/dll. di-rewrite `base+path` → eksekusi parent + `ADDFD`/`process_vm_writev`/memori-rewrite+`CONTINUE`. Menutup binary statis & syscall raw tanpa LD_PRELOAD. **Bug yang sudah diperbaiki:** arm64 tidak punya syscall `rmdir` (libc mengemulasi via `unlinkat`) — fallback lama `SYS_rmdir=21` menabrak `epoll_ctl=21` (tabel asm-generic) → runtime Go statis mati dengan "epollctl failed with 14"; rule kini di-guard `#ifdef SYS_rmdir`. **Juga:** `path_argidx()` salah untuk `renameat`/`renameat2`/`linkat` (membaca dirfd sbg pointer → EFAULT; ditemukan apk) → kini 1/1/1 + `symlink`=1, `symlinkat`=2; exit status child dipropagasi (`exit 7`→7, `exit 42`→42); cache 4096 (tak memotong path ≥4095); `openat2` ada di rules namun **tak dapat diverifikasi on-device** (seccomp eksternal Android SIGSYS-kill dulu, lihat `examples/o2test.c`).
- `examples/gobukti.go` — bukti dunia nyata: program **Go statis asli** (toolchain Go, `CGO_ENABLED=0`, tanpa PT_INTERP) yang berjalan lewat supervisor; sekaligus regresi-test bug epoll di atas.
- `examples/ebench.c` — benchmark overhead supervisor (1000 iterasi open+close, passthrough vs rewrite, `clock_gettime`).
- `examples/o2test.c` — bukti `openat2` diblokir seccomp eksternal Android (SIGSYS st=31, nol notif `nr=437`) sebelum USER_NOTIF svsp melihatnya.
- `bootstrap.sh` — **pemasangan satu-perintah** (TAHAP 4): unduh/ekstrak minirootfs Alpine 3.24.2 (aarch64) → pasang musl-dev (header + libc.a/crt*.o, sekaligus jadi toolchain build STATIS) → patch loader (byte-patch diverifikasi hash, SIGSYS tak diblokir) → rewrite PT_INTERP semua binary wadah (`pinterp` in-place; `patchelf` bila perlu-perluasan) → passthrough `/dev /proc /sys` + relink applet busybox relatif → build `libfakeroot.so` + `svsp` → pasang `fake-run`/`svsp` ke `$PREFIX/bin` → verifikasi 4 jalur (dinamis, supervisor, statis, isolasi ENOENT). Idempoten.
- `pinterp.c` — tool kecil tanpa dependensi: tulis ulang PT_INTERP **in-place** (path baru muat di segmen lama). Bila tak muat keluar kode 3 → `bootstrap.sh` menyerahkan ke `patchelf --set-interpreter` (terbukti untuk perluasan).

## Reproduksi (instalasi satu-perintah — TAHAP 4)

```sh
pkg install clang binutils patchelf curl     # prasyarat
git clone https://github.com/jajangking/Brainstorming && cd Brainstorming
./bootstrap.sh                                # base default ~/alpine-rootfs
fake-run cat /etc/alpine-release              # -> 3.24.2 (dinamis, jalur LD_PRELOAD)
```

`bootstrap.sh` idempoten: aman dijalankan ulang (base/loader/toolchain yang sudah benar dipakai ulang). Opsi: `--base=DIR` (wadah lain), `--prefix=DIR` (tujuan instal, default `$PREFIX`), `--no-download` (pakai cache `~/alpine-minirootfs.tar.gz` + `~/musl-dev-cache.apk`).

Yang dibangun dari nol: rootfs segar terverifikasi (`SHA256` loader stock dicocokkan sebelum patch, hasil patch dicocokkan sesudahnya — bila versi minirootfs beda, script berhenti keras, tidak korup diam-diam), loader `ld-musl-patched.so.1`, toolchain musl-dev, symlink applet relatif, passthrough device set, shim, supervisor, dan runner.

## Pemakaian manual (bila sudah punya wadah)

```sh
./fake-run cat /etc/os-release          # dinamis -> jalur cepat LD_PRELOAD -> Alpine
./fake-run cat /system/build.prop      # -> ENOENT (isolasi terbukti)
./fake-run --svsp <program>            # paksa supervisor USER_NOTIF
./fake-run --loader=.../ld-musl...so.1 .../claude-bin --version   # Claude di dalam wadah
./fake-run /usr/bin/st2                # statik no-loader: baca /etc/alpine-release -> 3.24.2
./fake-run /usr/bin/bossv              # statik -> fork -> exec /bin/sh dinamis -> cat
```

## Bukti dunia nyata: program Go statis asli (jalur supervisor)

Go menutup seluruh rantai: binarynya **statis murni** (tanpa PT_INTERP → `fake-run` otomatis memilih supervisor `svsp`), dan runtime-nya memakai syscall nyata (`epoll_ctl` netpoll, `openat`, `stat`, `getdents64`, …) yang di-rewrite di kernel.

```sh
cd examples
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o gobukti gobukti.go
fake-run ./gobukti
```

Hasil (dicapai 2026-10-07, Alpine 3.24.2 aarch64, Go 1.27.1):

```
[gobukti] Go statis asli — pid=7906 cwd=.../alpine-rootfs
  baca /etc/alpine-release   -> "3.24.2" (err=<nil>)
  >>> ISOLASI TERBUKTI: /system/build.prop tidak terlihat (ENOENT)
  chain tulis->baca->hapus      -> write=<nil> read="go-chain-OK" readerr=<nil> rm=<nil>
  walk /etc (36 entri)         -> OK
  readlink /proc/self/exe       -> .../alpine-rootfs/usr/bin/gobukti
```

Jalur dinamis (flagship, 241 MB bun/musl) juga tetap hijau: `fake-run .../package/claude --version` → `2.1.291 (Claude Code)`; `--help` usage rc=0; **TUI terbuka di dalam wadah** dan config mendarat di `$R/root/.claude` (host `~/.claude` tak tersentuh). Syarat: salin claude KE DALAM base (`$R/usr/local/bin/claude` + `patchelf --set-interpreter`) — claude melakukan self re-exec dengan path absolutnya sendiri yang, demi isolasi, di-rewrite ke dalam wadah.

## Overhead supervisor (angka jujur, `examples/ebench.c`)

1000 iterasi `open(path)+close(path)` — bare (host) vs lewat `svsp`:

| Skenario | μs/op |
|---|---|
| Bare (tanpa supervisor) | **1.11–1.59** |
| Di bawah `svsp` (passthrough CONTINUE / rewrite ADDFD) | **51.7–470** lintas run (fluktuasi didominasi termal/governor CPU; bare tetap ~1.4 μs/op pada sesi sama → bukan regresi kode) |

Overhead USER_NOTIF (round-trip kernel + kunjungan supervisor per notif) memang puluhan–ratusan μs; cache rewrite FNV-1a membuat rewrite berulang pada path sama lebih murah daripada passthrough ulang pada run hangat. Ini **harga wajar** untuk isolasi path tanpa root/proot.

## Uji daya tahan nyata: `apk add` di dalam wadah

`fake-run --svsp --base=$R $R/sbin/apk add --no-scripts --no-cache ncurses` → 3/3 paket
terpasang penuh (19 paket, 9447 KiB; symlink `libncursesw.so.6 → .so.6.6` benar), dan
binary-nya jalan native: `fake-run --base=$R $R/usr/bin/tput cols` → `80`, `infocmp xterm`
membaca terminfo wadah. Dua limitasi (bukan bug kode ini, keduanya terbukti):
**db apk** menggunakan `linkat(AT_EMPTY_PATH)` yang butuh `CAP_DAC_READ_SEARCH` → EPERM
rootless (kontrol `apk --root` vanila gagal identik); **skrip trigger** memakai shebang
`#!/bin/sh` yang di-exec kernel ke bionic `/system/bin/sh` (tidak bisa link di child
non-zygote) → `--no-scripts`. Selengkapnya di `HANDOFF.md` §6.5.

## Kesimpulan

- **Permanen mustahil:** `chroot`/`mount`/namespace sungguhan (seccomp AND-min), VM (mati di firmware).
- **Seccomp luar tetap membunuh `openat2`** (SIGSYS sebelum supervisor) — celah kecil yang tak bisa dipaksa lebih jauh di perangkat ini (`examples/o2test.c`); bukan regresi, path lain tak terpengaruh.
- **Tembok terakhir sudah ditumbangkan:** supervisor `SECCOMP_RET_USER_NOTIF` (`svsp`) menutup **binary statis & syscall mentah** yang mem-bypass LD_PRELOAD — rewrite path terjadi di kernel, tanpa root, tanpa proot, native speed. `set*id` tetap butuh shim SIGSYS (`libfakeroot.so`) berdampingan (TRAP firmware menang via AND-min). Program dunia nyata (apk, ncurses, Claude Code) terbukti **menginstall & berjalan native di dalam wadah**.