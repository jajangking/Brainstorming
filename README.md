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

- `BRAINSTORM.md` — peta lengkap kemungkinan/mustahil + argumen kenapa APK adalah jalan buntu
- `libfakeroot.c` — shim utama (fakechroot rewrite path + handler SIGSYS, musl, `-nostdlib`)
- `segcshim.c` — shim minimal netralisasi SIGSYS
- `sigsys-map.c` — peta per-syscall seccomp (`RET_TRAP` vs `RET_KILL`)
- `unshare-matrix.c` — bukti user-namespace mati (arg-filter seccomp)
- `block-trap.c` — bukti TRAP-while-SIGSYS-blocked = kill instan
- `raw-rt-sig.c` — resep handler SIGSYS dengan layout `sigaction` kernel (bionic)
- `fake-run` — **runner universal** wadah palsu: binary dinamis → jalur cepat loader musl + `LD_PRELOAD`; binary **statis (tanpa PT_INTERP) otomatis** jatuh ke supervisor `svsp` (USER_NOTIF); `--svsp` memaksa supervisor.
- `svsp.c` — supervisor `SECCOMP_RET_USER_NOTIF` (TAHAP 3): child pasang filter sendiri (`NO_NEW_PRIVS` + `NEW_LISTENER`), listener dikirim ke parent bebas-filter via SCM_RIGHTS; openat/stat/statx/statfs/readlink/execve/chdir/mkdir/unlink/rename/link/symlink/chmod/chown/truncate/utimensat/access/dll. di-rewrite `base+path` → eksekusi parent + `ADDFD`/`process_vm_writev`/memori-rewrite+`CONTINUE`. Menutup binary statis & syscall raw tanpa LD_PRELOAD.
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

## Kesimpulan

- **Permanen mustahil:** `chroot`/`mount`/namespace sungguhan (seccomp AND-min), VM (mati di firmware).
- **Jalan buntu:** APK baru — domain keamanan (`untrusted_app`) dan seccomp-nya identik.
- **Tembok terakhir sudah ditumbangkan:** supervisor `SECCOMP_RET_USER_NOTIF` (`svsp`) menutup **binary statis & syscall mentah** yang mem-bypass LD_PRELOAD — rewrite path terjadi di kernel, tanpa root, tanpa proot, native speed. `set*id` tetap butuh shim SIGSYS (`libfakeroot.so`) berdampingan (TRAP firmware menang via AND-min).