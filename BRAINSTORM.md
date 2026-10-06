# BRAINSTORM — "Yang tak bisa dipecahkan": peta kemungkinan & jalan nyata

> Pertanyaan: *apakah yang tadinya dianggap **tidak bisa dipecahkan** bisa dipecahkan? mau itu bikin APK lagi atau apa?*
> Jawaban singkat: **Dua tembok besar sudah dipecahkan tanpa root & tanpa proot (native speed).**
> Satu tembok yang tersisa (syscall mentah / binary statis) punya jalan keluar yang tidak butuh APK: **supervisor seccomp USER_NOTIF**.

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
2. **Binary statis (Go/C statis)** — tidak punya loader dinamis → LD_PRELOAD tak bisa disuntik. Program melihat host.
3. **Identitas uid** — `getuid()` tetap 10386, bukan 0 (wajar: kita memang bukan root).

### Jalan keluar yang MENJANJIKAN untuk celah 1 & 2: supervisor seccomp USER_NOTIF

- `SECCOMP_RET_USER_NOTIF` **terbukti berfungsi** di Android (listener fd=3 diperoleh pada percobaan sebelumnya).
- Filter seccomp bersifat *komposisional* (AND-min) → kita **boleh menambahkan** filter sendiri yang meminta USER_NOTIF untuk kelas syscall path (`openat`, `execve`, `stat`, `mkdir`…) — ini **tidak melanggar** larangan firmware (kita hanya mengetatkan).
- Pola kerja: proses target dijalankan dengan filter tambahan; **supervisor daemon** menerima notifikasi → menulis ulang path sesuai rootfs → mengeksekusi syscall asli atas nama target → mengembalikan hasil.
- Kenapa ini penting: intervensi terjadi **di kernel**, bukan interposisi simbol → berlaku untuk **binary statis, Go, dan syscall raw sekaligus**. Ini "proot dengan bahasa lain": tanpa ptrace, tanpa fork-chroot, tetap native-ish (hanya syscall path yang melewati supervisor).
- Biaya: satu round-trip per syscall path (lebih lambat dari LD_PRELOAD), dan harus meniru semantik `ERESTART`/clone anak. Tapi ini **satu-satunya rute yang menutup seluruh kelas program**, dan **tidak butuh APK/root**.

Peta jalan yang jujur:

```
TAHAP 1 (SELESAI, terbukti): LD_PRELOAD fakechroot + SIGSYS shim + loader patched
  → dynamic musl: claude, busybox, apk-tools, shell penuh   [native speed]
TAHAP 2 (SUDAH dipakai Claude): env host-absolut + symlink /proc /dev
  → menutup syscall raw pada jalur HOME/TMPDIR/cwd
TAHAP 3 (peluang riset): supervisor USER_NOTIF
  → menutup binary statis + Go + seluruh syscall raw; wadah "universal"
TAHAP 4 (bukan prioritas): pembungkus APK hanya untuk distribusi/UX, bukan teknis
```

---

## 4. Rekomendasi

1. **Jangan bikin APK untuk memecahkan batasan** — secara teknis mustahil mengubah seccomp lewat APK; hanya menambah permukaan maintain.
2. Fokus riset berikutnya: **supervisor USER_NOTIF** = jawaban "yang tidak bisa dipecahkan" yang tersisa (binary statis & syscall raw), dengan dasar yang sudah terbukti hari ini.
3. Pakai hasil sekarang: `fake-run` + `libfakeroot.so` siap dipakai untuk **Claude Code native di dalam wadah Alpine** dan untuk menjalankan paket musl lain (busybox, apk, dll.) secara terisolasi.

> Kalimat penutup: *"Yang tak bisa dipecahkan" ternyata adalah soal memilih lapisan yang tepat — bukan kernel namespace (mati selamanya), bukan APK (jalan buntu), melainkan **trap seccomp + interposisi + (nanti) USER_NOTIF** — semua berjalan di kecepatan native, tanpa root, tanpa proot.*