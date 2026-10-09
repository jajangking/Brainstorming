# BRAINSTORM — "Yang tak bisa dipecahkan": peta kemungkinan & jalan nyata

> Pertanyaan: *apakah yang tadinya dianggap **tidak bisa dipecahkan** bisa dipecahkan? mau itu bikin APK lagi atau apa?*
> Jawaban singkat: **Tiga tembok besar sudah dipecahkan tanpa root & tanpa proot (native speed)** — termasuk yang terakhir: binary statis & syscall mentah, lewat **supervisor seccomp USER_NOTIF** (`svsp`).

---

## 1. Ringkasan: apa yang berubah dari "mustahil" menjadi "terbukti"

| Masalah | Status lama | Solusi yang TERBUKTI | Bukti |
|---|---|---|---|
| `setgid`/`setuid`/`chroot`/`mount` dll. dibunuh seccomp Android | **mati (SIGSYS), mustahil** | Neutralisasi TRAP: handler SIGSYS yang melewati syscall + mengembalikan sukses palsu (family `set*id`) / `EPERM` jujur (`chroot`, `mount`) | `libfakeroot.so`/`libsegcshim.so`, `sigsys-map.c` (SEMUA pembunuhan = TRAP yang bisa ditangkap, tanpa RET_KILL) |
| Proses yang **menutup SIGSYS** sebelum syscall terlarang (busybox: `rt_sigprocmask(BLOCK)` dulu) | **mati meski ada handler** | Loader musl **di-patch** agar SIGSYS tidak pernah diblokir | `lib/ld-musl-patched.so.1` — busybox hidup |
| Isolasi path seperti chroot **tanpa chroot** | **butuh proot/root** | `fakechroot` ala sendiri: LD_PRELOAD rewrite path absolut + env di-rewrite wrapper | Alpine `3.24.2` terlihat; `/system/build.prop` (ada di host) → **ENOENT** |
| **Claude Code** (bun, 241 MB, musl) di dalam wadah | pakai proot | Jalan **native** di wadah palsu: `--version` ✓, baca `/etc/os-release` Alpine ✓, **config masuk `rootfs/root/.claude`** ✓, jaringan ke gateway `127.0.0.1:20128` ✓ | `fake-run` + `libfakeroot.so` |
| Melihat `/proc` & `/dev` | perlu mount | **passthrough symlink**: `rootfs/proc → /proc`, `rootfs/sys → /sys`, `rootfs/dev/{null,zero,tty,urandom…} → /dev/*` | `ps` hidup, `/dev/null` OK |
| Rantai exec di dalam wadah (`sh -c 'cat …'`, pipe) | interpreter `/lib/ld-musl…` tak ada di host | **patchelf semua binary wadah** → PT_INTERP menunjuk loader nyata di host | 7 binary di-patch; `ls | head`, pipe, write bukti ✓ |

Ringkasan satu kalimat: **seccomp Android ternyata tidak membunuh tanpa syarat — semua larangannya adalah TRAP yang bisa dilewati handler, dan "chroot" bisa dipalsukan lewat interposisi simbol + rewrite env. Keduanya berjalan di kecepatan native.**

### TAHAP 3 (SELESAI): supervisor `SECCOMP_RET_USER_NOTIF` → binary statis & syscall mentah

| Pembuktian | Hasil nyata |
|---|---|
| Filter seccomp **komposisional** (AND-min): kita boleh *menambah* filter USER_NOTIF sendiri di atas larangan firmware | `svsp` berjalan tanpa root; child pasang filter pada dirinya sendiri, listener fd dikirim ke parent **bebas-filter** via `socketpair` + `SCM_RIGHTS` |
| Arch-check BPF yang benar (bug klasik: `JEQ(ARCH, jt=1, jf=0)`; jt/jf terbalik = SIGSYS instan) | tercatat di `svsp.c` |
| Rewrite-openat: parent membuka `base+path`, hasil fd disuntik via `SECCOMP_IOCTL_NOTIF_ADDFD` | busybox `cat /etc/alpine-release` → `3.24.2` — **tanpa LD_PRELOAD** |
| Rewrite-stat/readlink/statfs: parent mengeksekusi, hasil ditulis ke memori child via `process_vm_writev` | `ls -l`, script yang stat/readlink ✓ |
| Rewrite-execve: tulis ulang path di memori child + `CONTINUE` (kernel eksekusi ulang) | `sh -c` rantai: pipe, write/rm `/root/x.txt`, `ls /sbin` ✓ |
| **Binary STATIS (tanpa loader)** — LD_PRELOAD tak relevan | `st2` (78 KB, `-nostdlib`, raw syscall): baca `/etc/alpine-release` via ADDFD = `3.24.2` ✓ |
| **Boss test: statik → fork → exec `/bin/sh` dinamis → `cat`** | `bossv`: `SH-OK` + `3.24.2` ✓ — satu proses pohon campur statis/dinamis |
| Isolasi terbalik | `cat /system/build.prop` → **ENOENT** (bukan EACCES host) ✓; `/dev/null`, `/sys` tetap passthrough ✓ |
| Kematian target tidak membangunkan `RECV` yang terblokir (bug kernel Android) | loop `poll(listener, 200ms)` + `waitpid(WNOHANG)` → supervisor keluar bersih (rc=0), tanpa hang |
| **Go statis asli** (toolchain Go 1.27, `CGO_ENABLED=0`, tanpa PT_INTERP) di bawah USER_NOTIF — runtime netpoll (epoll/eventfd), `openat`/`stat`/`getdents64`, tanpa LD_PRELOAD | `examples/gobukti.go`: `/etc/alpine-release`→`3.24.2`, `/system/build.prop`→**ENOENT**, rantai file `/tmp`, walk `/etc` (36) ✓, `readlink /proc/self/exe`→path base ✓, rc=0 |
| Bug yang ditemukan: arm64 **tidak punya syscall `rmdir`** (tabel asm-generic; libc mengemulasi via `unlinkat`) — fallback `SYS_rmdir=21` menabrak **`epoll_ctl=21`** (nomor 21 di arm64 BUKAN rmdir!) → `epoll_ctl` Go di-*notify* sebagai "rmdir" → path = fd (bukan pointer) → `process_vm_readv` gagal → balas **-EFAULT** → "runtime: epollctl failed with 14", runtime mati | Diperbaiki: hapus fallback, rule+case di-guard `#ifdef SYS_rmdir`. Bukti: `eptest` (epoll C statis) `epoll_ctl=0`; `gobukti` lolos penuh; V1–V4 + claude tetap hijau |
| Batas jujur | execve path yang memerlukan rewrite tapi berada di memori **read-only** (.rodata) tidak bisa diubah → **ENOENT** (aman, tidak bocor ke host); shell/program normal membangun path di stack/heap (writable) → tetap jalan |
| Batas yang tetap (tetap butuh shim/loader patched) | `set*id` dll.: `RET_TRAP` firmware (0x30000) menang atas `USER_NOTIF` (0x7fc00000) via AND-min → SIGSYS-shim `libfakeroot.so` tetap hidup berdampingan |

---

## 2. Peta ketidakmungkinan — apa yang memang TIDAK akan pernah bisa

| Hal | Alasan final (bukan tebakan) | Vonis |
|---|---|---|
| **chroot/mount/namespace sungguhan** | seccomp Android menumpuk dengan operasi **AND-min** pada hasil tiap filter → kita hanya bisa *mengetatkan*, tidak pernah *melonggarkan*. Firmware menolak `mount`/`chroot`/`unshare` (user-namespace: `EINVAL` lewat arg-filter). | **Permanen mustahil** — seburuk apa pun APK-nya |
| **VM** | dimatikan di level firmware (sudah dikonfirmasi) | **Permanen mustahil** |
| **APK baru** | APK apa pun berjalan di *domain keamanan yang sama*: `untrusted_app`, seccomp yang sama, SELinux yang sama. APK tidak menaikkan CAP_SYS_ADMIN, tidak mengubah filter kernel. | **Jalan buntu** — biaya besar, keuntungan nol pada kemampuan |

Mengapa APK mustahil membantu (argumen penuh):
- seccomp melekat pada **UID/domain**, bukan pada kemasan APK. APK yang kita tandatangani sendiri tetap `untrusted_app` dengan SELinux `domain=untrusted_app`, `seccomp=global` yang sama persis dengan Termux.
- Satu-satunya hal yang bisa berubah dengan APK: UX (launcher, izin `QUERY_ALL_PACKAGES`, dsb.) — bukan seccomp, bukan mount, bukan uid 0.
- Konklusi: **APK hanya relevan sebagai pembungkus distribusi, bukan pemecah batasan. Buang ide itu untuk tujuan teknis.**

---

## 3. Sisa celah (jujur) & jalan keluar yang menjanjikan

Yang masih BELUM 100% tertutup oleh solusi LD_PRELOAD + loader patched:

1. **Syscall mentah** — bun/node/zig (dan Go) memanggil sebagian syscall **langsung**, tanpa lewat simbol libc → rewrite path terlewat. (Terbukti: `mkdirat` claude kena rewrite, tapi `openat` raw tidak.)
   - Mitigasi sekarang: env `HOME/TMPDIR/PWD` diarahkan ke *path host-absolut yang memang sudah berada di dalam rootfs* → jalur ini masuk wadah **bahkan lewat syscall raw** (karena path-nya memang ada di sana). Inilah trik yang membuat Claude benar-benar jalan.
2. **Binary statis (Go/C statis)** — tidak punya loader dinamis → LD_PRELOAD tak bisa disuntik. **TERTUTUP oleh supervisor `svsp`** (TAHAP 3): rewrite path terjadi di kernel, berlaku untuk statis & syscall raw. **Terbukti dengan program Go statis asli** (`examples/gobukti.go`, Go 1.27, `CGO_ENABLED=0`, tanpa PT_INTERP) yang berjalan penuh lewat `fake-run` (auto → supervisor): rewrite `/etc/alpine-release` → `3.24.2`, isolasi `/system/build.prop` → ENOENT, rc=0.
3. **Identitas uid** — `getuid()` tetap 10386, bukan 0 (wajar: kita memang bukan root).

### Jalan keluar yang sudah TERBUKTI untuk celah 1 & 2: supervisor seccomp USER_NOTIF (`svsp`)

- `SECCOMP_RET_USER_NOTIF` **terbukti berfungsi penuh** di Android (lihat TAHAP 3 di atas): rewrite path untuk binary statis DAN syscall raw **tanpa LD_PRELOAD**.
- Filter seccomp bersifat *komposisional* (AND-min) → kita **boleh menambahkan** filter sendiri yang meminta USER_NOTIF untuk kelas syscall path (`openat`, `execve`, `stat`, `mkdir`…) — ini **tidak melanggar** larangan firmware (kita hanya mengetatkan).
- Pola kerja: child pasang filter pada dirinya sendiri (`NO_NEW_PRIVS` + `NEW_LISTENER`), kirim listener ke **parent bebas-filter** via SCM_RIGHTS; parent menulis ulang path sesuai rootfs → mengeksekusi syscall atas nama target (sendiri untuk side-effect, atau memori-rewrite + `CONTINUE` untuk execve/chdir, atau `ADDFD` untuk open).
- Kenapa ini penting: intervensi terjadi **di kernel**, bukan interposisi simbol → berlaku untuk **binary statis, Go, dan syscall raw sekaligus**. Ini "proot dengan bahasa lain": tanpa ptrace, tanpa fork-chroot, tetap native-ish (hanya syscall path yang melewati supervisor).
- Biaya: satu round-trip per syscall path (lebih lambat dari LD_PRELOAD), `execve` path di memori read-only tak bisa di-rewrite (ENOENT), dan `set*id` tetap butuh shim SIGSYS (TRAP firmware menang via AND-min). Tapi ini **satu-satunya rute yang menutup seluruh kelas program**, dan **tidak butuh APK/root**.

Peta jalan yang jujur:

```
TAHAP 1 (SELESAI, terbukti): LD_PRELOAD fakechroot + SIGSYS shim + loader patched
  → dynamic musl: claude, busybox, apk-tools, shell penuh   [native speed]
TAHAP 2 (SUDAH dipakai Claude): env host-absolut + symlink /proc /dev
  → menutup syscall raw pada jalur HOME/TMPDIR/cwd
TAHAP 3 (SELESAI, terbukti): supervisor USER_NOTIF (svsp)
  → binary statis + syscall raw: openat/stat/execve/readlink/mkdir/… di-rewrite di kernel
  → st2 & bossv (statik → fork → exec sh dinamis) jalan native; isolasi ENOENT terbukti
TAHAP 4 (bukan prioritas): pembungkus APK hanya untuk distribusi/UX, bukan teknis
```

### Batas baru (2026-10-10, Infinix X6855 / Android 16 SDK 36): USER_NOTIF ditolak

- `svsp` gagal total di perangkat ini: `seccomp: Resource busy` (EBUSY, bukan
  EINVAL) bahkan dari host bersih — firmware (`Seccomp_filters: 2`) menolak
  `NEW_LISTENER` bagi `untrusted_app`. TAHAP 3 tetap valid di perangkat yang
  mengizinkan, tapi bukan jaminan universal.
- Konsekuensi: celah 2 (binary statis) TERBUKA KEMBALI di perangkat ini —
  LD_PRELOAD tak berlaku untuk statis, dan supervisor tak bisa dipasang.
- Mitigasi yang sudah jalan (`fake-run`, commit `2eacd33`): probe `svsp`
  sebelum exec; gagal → dinamis/`sh`/`node` otomatis ke LD_PRELOAD; statis →
  error jujur. Bukan penutup celah, tapi kegagalan yang anggun.

---

## 4. Rekomendasi

1. **Jangan bikin APK untuk memecahkan batasan** — secara teknis mustahil mengubah seccomp lewat APK; hanya menambah permukaan maintain.
2. Supervisor `USER_NOTIF` **sudah jadi**: `svsp` + `libfakeroot.so` + `fake-run` menutup rantai penuh — binary dinamis (LD_PRELOAD, cepat) maupun statis/raw (supervisor, kernel-level) — untuk **Claude Code native** dan paket musl lain secara terisolasi, tanpa root, tanpa proot.
3. Pengembangan wajar berikutnya: gabungkan `svsp` di bawah `fake-run` (pilih jalur cepat LD_PRELOAD untuk dinamis, otomatis jatuh ke supervisor untuk statis/raw), dan tambahkan handler `set*id` di sisi supervisor untuk kasus yang memblokir SIGSYS.

> Kalimat penutup: *"Yang tak bisa dipecahkan" ternyata adalah soal memilih lapisan yang tepat — bukan kernel namespace (mati selamanya), bukan APK (jalan buntu), melainkan **trap seccomp + interposisi + USER_NOTIF (kernel-level path supervisor)** — semuanya berjalan di kecepatan native, tanpa root, tanpa proot.*