# RONDE 20 — Uji aplikasi glibc di wadah musl: hasil & batas kerasnya

## Pertanyaan

Bisa tidak aplikasi yang butuh **glibc** jalan di wadah (repo ini musl-only)?
Diuji dengan `busybox` Debian bookworm arm64 (dynamis glibc, `GLIBC_2.34`).

## Hasil: TIDAK BISA — bukan karena 설정, tapi karena Android

Tiga hurdles, semuanya di luar kendali userspace:

### 1. `gcompat` (shim libc6-compat Alpine) — parsial, tidak cukup

```
$ fake-run apk add gcompat          # 96.2 MiB, 47 paket
$ fake-run /root/busybox-glibc echo hi
Error relocating .../busybox-glibc: re_search: symbol not found
Error relocating .../busybox-glibc: re_compile_pattern: symbol not found
Error relocating .../busybox-glibc: mallopt: symbol not found
```
gcompat menyediakan `libc.so.6` shim tapi tidak mengimplementasikan simbol
regex (`re_search`, `re_compile_pattern`, `re_syntax_options`), `mallopt`,
`fcntl64`. Binary glibc modern butuh ≥ GLIBC_2.34; cakupan gcombatrolya
sebagian besar simbol itu.

### 2. glibc asli (ekstrak `libc6` Debian) — loader bisa jalan, tapi…

```
$ ld.so (Debian GLIBC 2.36-9+deb12u14) stable release version 2.36   # OK di host
$ fake-run --svsp ... /opt/glibc/lib/ld-linux-aarch64.so.1 --list busybox
--- SIGSYS {si_code=SYS_SECCOMP, si_syscall=__NR_set_robust_list} ---
+++ killed by SIGSYS +++
```
Loader glibc diekstrak ke `<base>/opt/glibc` (3,5 MiB) dan berhasil
`--version` di host. Hung: semua proses glibc memanggil
`set_robust_list()` saat init TLS, dan Android **membunuh proses itu tanpa
bisa di-catch** — bahkan `GLIBC_TUNABLES=glibc.pthread.robust_list=0`
hanya melewati yang pertama, yang kedua tetap terjadi.

### 3. Trap-nya tidak deliverable — handler in-process tak menolong

Probe musl statis yang memanggil `syscall(SYS_set_robust_list, …)` juga mati
meski shim `libfakeroot.so` aktif (shim menjawab trap yang deliverable seperti
`faccessat2`, `io_uring_setup`). Untuk `set_robust_list`, kernel tidak
menyerahkan sinyal ke handler sama sekali — proses langsung dibunuh.

## `sigguard.c` (percobaan yang gagal sebagian)

Untuk glibc, shim musl tak bisa di-preload (`CANNOT LINK`), jadi handler
SIGSYS harus libc-agnostic. `sigguard.c` = guard freestanding (`-nostdlib`,
tanpa libc) yang:

- memasang handler `rt_sigaction(SIGSYS, SA_SIGINFO|SA_RESTORER)` via syscall
  mentah,
- membaca nomor syscall dari `ucontext` (offset ABI aarch64: `regs[8]`),
  melewati instruksi `svc` (`pc += 4`), menjawab ENOSYS / `io_uring_setup`→fd
  `/dev/null` / `set*id`→0.

Hasil: **constructor-nya tidak dijalankan loader glibc** (file di-`openat`
terbaca, tapi `DT_INIT_ARRAY` tak dieksekusi → tidak ada `rt_sigaction` di
trace). Builds Android-bionic maupun musl sama sama tidakdieksekusi. Selain
itu `-nostdlib` dari clang Termux menyuntik `RUNPATH` ke lib Android → harus
`patchelf --remove-rpath` (sudah ada di catatan build).

Tetap disimpan di repo karena pola (guard libc-agnostic, offset ABI) berguna
untuk target lain; **bukan**_GLBC enable_ untuk repo ini.

## Revisi §45: LD_LIBRARY_PATH jangan dibuang svsp

Sambil menguji glibc, ketemu bug di svsp: `svsp` melakukan
`unsetenv("LD_LIBRARY_PATH")`, jadi biner glibc mencari libc di
`<base>/lib/` (shim gcompat/musl) alih-alih `<base>/opt/glibc/lib/...`
→ `no version information available`. Perubahan: **pertahankan**
`LD_LIBRARY_PATH` dari pemanggil (fake-run tetap menyetelnya ke
`<base>/lib:<base>/usr/lib`). Tidak merusak jalur musl (loader musl mencari
libc sendiri di direktorinya).

## Batas keras & rekomendasi

- **glibc mustahil di wadah fake-chroot ini** selama Android memblokir
  `set_robust_list` secara non-deliverable. Bukan sesuai konfigurasi — dan
  bukan bug fake-chroot.
- Butuh glibc → pakai **proot-distro** (ptrace; glibc utuh dari rootfs
  Debian/Ubuntu) atau **Termux `pkg install glibc-repo`** (bionic, bukan
  glibc penuh tapi utilitas umum jalan).
- Wadah tetap andal untuk **musl**: Alpine package, Node/Python/uv/OpenCode/
  Claude Code/Hermes Agent.

## Jejak bersih

Sisa percobaan glibc (loader+libc Debian, 3,5 MiB) saya tinggalkan di
`<base>/opt/glibc` untuk referensi — hapus dengan `rm -rf <base>/opt/glibc`
bila tak diperlukan.
Artefak uji (`busybox-glibc`, probe, `fatest*`, `npmtest*`) sudah dihapus.