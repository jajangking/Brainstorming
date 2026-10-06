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

## Isi

- `BRAINSTORM.md` — peta lengkap kemungkinan/mustahil + argumen kenapa APK adalah jalan buntu
- `libfakeroot.c` — shim utama (fakechroot rewrite path + handler SIGSYS, musl, `-nostdlib`)
- `segcshim.c` — shim minimal netralisasi SIGSYS
- `sigsys-map.c` — peta per-syscall seccomp (`RET_TRAP` vs `RET_KILL`)
- `unshare-matrix.c` — bukti user-namespace mati (arg-filter seccomp)
- `block-trap.c` — bukti TRAP-while-SIGSYS-blocked = kill instan
- `raw-rt-sig.c` — resep handler SIGSYS dengan layout `sigaction` kernel (bionic)
- `fake-run` — wrapper wadah palsu (rewrite env, resolve program, jalankan via loader musl)

## Reproduksi (ringkas)

```sh
# 1) rootfs Alpine + loader musl patched
# 2) kompilasi shim (perlu musl-dev sebagai sysroot)
clang --target=aarch64-alpine-linux-musl --sysroot=$ROOTFS \
  -fPIC -shared -O2 -nostdlib -o libfakeroot.so libfakeroot.c

# 3) normalisasi symlink rootfs (absolut -> relatif) + passthrough /dev /proc /sys
# 4) patchelf semua binary wadah agar PT_INTERP menunjuk loader nyata di host
# 5) jalankan
./fake-run cat /etc/os-release          # -> Alpine, bukan host
./fake-run cat /system/build.prop      # -> ENOENT (isolasi terbukti)
./fake-run --loader=.../ld-musl...so.1 .../claude-bin --version   # Claude di dalam wadah
```

## Kesimpulan

- **Permanen mustahil:** `chroot`/`mount`/namespace sungguhan (seccomp AND-min), VM (mati di firmware).
- **Jalan buntu:** APK baru — domain keamanan (`untrusted_app`) dan seccomp-nya identik.
- **Jalan berikutnya:** supervisor `SECCOMP_RET_USER_NOTIF` (listener sudah terbukti) untuk menutup
  binary statis/Go dan syscall mentah yang mem-bypass LD_PRELOAD.