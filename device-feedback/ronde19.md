# RONDE 19 — Hermes Agent v0.21.6 terinstall PENUH di wadah (installer rc=0)

## Hasil utama

`https://hermes-agent.nousresearch.com/install.sh` **selesai dengan sukses**
(`rc=0`) di dalam wadah, setelah 9 kali percobaan. Yang tadinya mustahil
(ronde 14: `unsupported platform:` + SIGSYS) kini melewati semua tahap:

```
✓ prerequisites ok (git, curl)
✓ Hermes Agent cloned
✓ uv ready (uv 0.12.3 aarch64-unknown-linux-musl)
✓ bootstrap Python ready
✓ node  ✓ npm  ✓ python  ✓ ripgrep
✓ venv
✓ Installing Python dependencies
✓ config prepared in ~/.hermes
✓ app products and hermes command ready
✓ Hermes Agent install complete.
```
```
$ fake-run --svsp bash -lc 'export PATH=$PATH:/root/.hermes/hermes-agent/.hermes/bin; hermes --version'
Hermes Agent v0.21.6+130.g25a71a7 (2026.9.24) · upstream 25a71a74
Python: 3.14.7 · OpenAI SDK: 2.24.0 · Up to date
```

Cara menjalankan (setup/gateway sengaja dilewati: `--non-interactive`):
```bash
alpine                                                   # shell wadah
export PATH=$PATH:/root/.hermes/hermes-agent/.hermes/bin # sudah ada di .bashrc/.profile
hermes setup                                            # pairing/token API (interaktif)
```

## Lima bug yang di hurdles (semua sudah di-push ke main)

### §40 — supervised dynamic TAK memuat shim → `bash` mati SIGSYS

`fake-run --svsp bash -c 'uname -s'` → rc=159 (`Bad system call`), padahal
`fake-run --svsp uname -s` → `Linux`. Bukti (strace):

```
faccessat(AT_FDCWD, "/bin/uname", X_OK, AT_EACCESS) = -1 ENETDOWN (Network is down)
--- SIGSYS {si_signo=SIGSYS, si_code=SYS_SECCOMP, si_syscall=__NR_faccessat2} ---
+++ killed by SIGSYS +++
```

Supervisor **tak pernah menerima notif** untuk nr=439: trap seccomp eksternal
Android (RET_TRAP) dievaluasi SEBELUM filter svsp — sama seperti `openat2`
(HANDOFF §6.1). Handle-nya harus in-process: handler `fk_sigsys` milik shim
(§34/§35) menjawab ENOSYS → libc jatuh ke `faccessat`. Tapi `svsp` melakukan
`unsetenv("LD_PRELOAD")` sehingga supervised process berjalan tanpa shim.

Fix: `fake-run` menyuntik `LD_PRELOAD=$SHIM` di cabang `--svsp`; `svsp` tidak
lagi meng-unset-nya. Biner statis mengabaikan LD_PRELOAD (tak efek).

### §42 — Node.js 26 abort: `uv__close: fd > STDERR_FILENO`

Tahap `pm install` selalu gagal di `npm: self-install exited -6`. Repro minimal:
`fake-run <node> -e 'console.log(1)'` → **abort**. Bukan stdin, bukan NMR.
Strace menunjukkan urutan persisnya:

```
epoll_create1(EPOLL_CLOEXEC)      = 3
io_uring_setup(256, {...})        = (trap)
--- SIGSYS ... si_syscall=__NR_io_uring_setup ---
writev(2, "Assertion failed: fd > STDERR_FILENO (../deps/uv/src/unix/core.c: uv__close: 646)")
+++ killed by SIGABRT +++
```

libuv (bundled node) memanggil `io_uring_setup` begitu kernel ≥ 5.1 (device:
6.1.157-android14). Jawaban ENOSYS membuat libuv masuk **fail path** dan menutup
`ringfd` yang tak pernah valid (-1/0) → `assert(fd > 2)` → abort.
`UV_USE_IO_URING=0` **tidak menolong**: `uv__use_io_uring()` di build ini
`return 1` sebelum membaca env bila flags bukan SQPOLL.

Fix: di handler SIGSYS, `io_uring_setup` dijawab **fd `/dev/null` asli**
(dibuka via `svc` mentah — aman di signal handler, tanpa lock libc; fd 0–2
dihindari). Parameter hasil tetap nol → libuv mundur anggun ke epoll.
Bukti: `node -e` OK, event-loop I/O OK, `npm install --offline` OK.

### §43 — ELF ber-INTERP stock mati ENOENT saat parent statis

`pm` (statis) men-spawn python hasil unduhan → `uv venv` gagal
`No interpreter found`. Biner itu dinamis dengan PT_INTERP `/lib/ld-musl-…`;
kernel host tak punya `/lib`, dan karena parent statis **tak ada re-exec shim**
yang mengarahkan ke loader patched. Manual `patchelf` sempat berhasil, tapi
pm **re-extract** lalu menolak (`No interpreter found`) — jadi fix-nya harus
tidak menyentuh file (hash pm diverifikasi).

Fix: supervisor mendeteksi ELF+INTERP-stock saat `execve`, membuat **wrapper
stabil** `~/.svsp-elfwrap/<hash-path>` (isi diverifikasi tiap pakai → upgrade
paket tak membuat wrapper basi) berisi
`#!/$BASE/bin/busybox sh` + `exec LOADER REAL "$@"`, lalu redirect path di
memori child (write_mem). Biner host/bionic dikecualikan.

### §43b — shebang wrapper + shim = CANNOT LINK

Setelah §40, wrapper shebang (`#!/system/bin/sh`) **rusak**: sh host itu
bionic, preload shim itu musl → `CANNOT LINK EXECUTABLE "/system/bin/sh":
cannot locate symbol "__errno_location"`. NOL `.orig-svsp` di device, jadi
regresi ini belum pernah tereksekusi. Fix: default `#!/$BASE/bin/busybox sh`
(busybox musl + shim = cocok; argumen `sh` wajib untuk mode multicall).
`fake-run` juga kini melewati `LD_PRELOAD` untuk target host-bionic langsung.

### §44 — syscall dua-path merusak path RELATIF (bug umum, bukan khusus Hermes)

Tahap `uv sync` gagal: setuptools menulis egg-info lalu `os.replace(tmp, 'rel/PKG-INFO')`
→ ENOENT. Penyebabnya di shim:

```c
if (fk_is_abs(a) || fk_is_abs(b2)) {
    fk_rewrite(x, sizeof x, a);      /* <-_relative_ ikut di-prefix base! */
    fk_rewrite(y, sizeof y, b2);
```
`fk_rewrite()` tidak mengecek absolut — hasilnya
`$BASE` + `egtest/PKG-INFO` (tanpa separator) → kernel/parent receives
`/…/alpine-rootfsegtest/PKG-INFO` (terlihat di strace). Fix: `fk_rewrite1()`
(absolut → rewrite, relatif → apa adanya) untuk `rename`, `renameat`,
`renameat2`, `link`, `linkat`.

## Catatan operasional (bukan bug)

- **Jaringan seluler**: run #3–#4 gagal `Connection aborted` di 81,9% unduhan
  Node.js → transient. `curl -sSI` ke URL yang sama = HTTP 200; install ulang
  langsung lanjut (installer resume, tidak mengulang dari nol).
- **Batas scan PT_INTERP** (§40 commit): opencode 195 MB dulu lolos dari
  rewrite karena `find ... -size -64M`. Sekarang 512M; pra-saring magic ELF
  4 byte membuat sapuan tetap murah.
- **Paket dasar untuk fresh install**: `libstdc++`+`libgcc` (tanpa itu
  opencode gagal `Error loading shared library libstdc++.so.6`), serta
  `curl bash git` (wadah fresh hanya punya busybox ash).
- **Port opencode wadah**: `service.json` port 49474 dibuat oleh `alpine` bila
  opencode sudah ada tapi config belum — opencode yang dipasang *setelah*
  bootstrap lolos dari `setup_opencode_port` (§32 hanya jalan saat bootstrap).

## Yang masih belum terverifikasi

- `hermes setup` / `hermes gateway install` (butuh TTY + input) — belum diuji.
- Hermes menjalankan tool interaktif (TUI) di dalam wadah.
- glibc binary di wadah (repo ini musl-only; `gcompat` belum dicoba).