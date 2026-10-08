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
| Claude Code (flagship, bun/musl, 241 MB) | `fake-run --base=$R $R/usr/local/bin/claude --version` (salinan di dalam wadah) | `2.1.291 (Claude Code)` rc=0; `--help` usage rc=0; **TUI terbuka** via pty (`Welcome to Claude Code v2.1.291`); config **terisolasi**: `$R/root/.claude/{projects,sessions}` dibuat, `~/.claude` host tak tersentuh |
| ncurses asli dipasang apk | `fake-run --base=$R $R/usr/bin/tput cols` | `80` rc=0 (libncursesw.so.6 dimuat via LD_LIBRARY_PATH wadah) |
| terminfo dari wadah | `fake-run --base=$R $R/usr/bin/infocmp xterm` | rekonstruksi dari `/etc/terminfo/x/xterm` wadah ✓ |
| `apk add` nyata | `fake-run --svsp --base=$R $R/sbin/apk add --no-scripts --no-cache ncurses` | 3/3 paket terpasang penuh (19 pkg, 9447 KiB, symlink benar); **db-commit EPERM = limitasi apk-tools rootless** (linkat `AT_EMPTY_PATH` butuh CAP_DAC_READ_SEARCH; terbukti via kontrol `apk --root` vanila — bukan bug kita) |

Commit terakhir: `2b135d3` (merge arena `c7d6ccb0-brainstorming`) + pengerjaan lanjutan di working tree (belum di-push saat HANDOFF ditulis; §10).

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
cd ~/Brainstorming && git status --short && git log --oneline -1   # bersih, 2b135d3 (merge arena) + push final
R=$HOME/alpine-rootfs
env -u LD_PRELOAD ./fake-run --base=$R $R/bin/busybox cat /etc/alpine-release   # -> 3.24.2
```

Bila output tidak sesuai → jangan lanjut, perbaiki dulu lingkungannya.

---

## 6. Checklist tersisa — 5 item pengerasan (dengan kriteria terima)

> Definisi **SELESAI**: semua item hijau + `README.md` tetap mereproduksi dari nol
> (`git clone` → `bootstrap.sh` → `fake-run cat /etc/alpine-release` → `3.24.2`) +
> semua perubahan di-commit & di-push ke `main`.

### 6.1 `openat2` (SYS 437) masuk aturan `svsp.c` — celah kecil ✅ SELESAI (kode); ⚠️ tak dapat diverifikasi on-device

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

**Status akhir (verifikasi on-device, 2026-10-07):** kode `openat2` sudah masuk
(`rules[]`, `path_argidx()`, handler baca `struct open_how` via `process_vm_readv`,
dengan fallback `__has_include(<linux/openat2.h>)` untuk deklarasi `struct open_how`
— kompilasi bersih di sysroot musl dan bionic) — **tetapi jalur NOTIF tidak bisa
diverifikasi di perangkat ini**: seccomp EKSTERNAL Android (app sandbox) mengeksekusi
`kill(SIGSYS)` pada `openat2` SEBELUM seccomp USER_NOTIF svsp melihatnya. Bukti:
`examples/o2test.c` (syscall `openat2` + `openat` + `statx` + print) — di dalam wadah,
proses ber-`openat2` langsung mati `st=31` (SIGSYS) **tanpa satu pun notif `nr=437`**
muncul di `SVSP_DEBUG=1`; sementara `openat`/`statx` berjalan normal (0 EFAULT/EPERM).
Artinya `openat2` mustahil dipakai program apa pun di wadah pada Android ini — rute
satu-satunya adalah jalur cepat LD_PRELOAD (shim mencegat `openat2` bila program musl
memang memanggilnya). Kode tetap dipertahankan (benar + murah), dilaporkan jujur sebagai
"tidak dapat diverifikasi on-device".

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

**Hasil akhir (build final, 2026-10-07):**
- Bare (host, tanpa svsp): **1.11–1.59 μs/op** (dulu 1.38–1.49).
- Di bawah `svsp`: terukur **51.7–470 μs/op** lintas run (fluktuasi dominan termal/governor
  CPU — bare tetap ~1.4 μs/op pada sesi yang sama, jadi bukan regresi kode). Overhead
  USER_NOTIF (round-trip kernel per notif) memang puluhan–ratusan μs; cache 6.4 membuat
  rewrite berulang pada path sama lebih murah daripada passthrough ulang pada run hangat.

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

**Koreksi premis + implementasi final:** "grandchild reaping" ternyata **bukan fitur** —
anak SATU-SATUNYA `svsp` adalah target itu sendiri (svsp tidak me-fork lagi). Draft awal
`while (waitpid(-1, &st, WNOHANG))` justru **men-reap target** dan kehilangan status
exit-nya → rc `svsp` selalu 1. Perbaikan final di poll-timeout: `waitpid(pid,&st,WNOHANG)
== pid` → simpan `tst=st` & berhenti; fallback `waitpid(pid,&st,0)`; keluarkan
`WEXITSTATUS` (atau `128 + WTERMSIG`). Terbukti: `exit 7` → rc=7, `exit 42` → rc=42
(via `fake-run --svsp`).

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

**Implementasi final:** cache FNV-1a 512 entri + **buffer path 4096** dan **JANGAN simpan
path ≥ 4095** (sebelumnya buffer 256B bisa memotong path panjang → cache menandai
berbeda/rusak). Juga: `read_string()` dibatasi per-halaman (tidak pernah melewati batas
4K) dan `struct open_how` dibaca page-bounded — menutup kemungkinan overread-EFAULT di
tepi mapping. Semua uji regresi tetap hijau.

### 6.5 Uji daya tahan nyata (Claude + `apk add` sungguhan) ✅ SELESAI (perangkat ini)

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

**Hasil akhir + temuan (2026-10-07, semua empiris di perangkat ini):**

(a) **`apk add` nyata — INSTALASI SUKSES, dua limitasi apk-tools/Android didokumentasikan:**
- `fake-run --svsp --base=$R $R/sbin/apk add --no-scripts --no-cache ncurses` → **3/3 paket**
  terpasang penuh (19 paket, 9447 KiB): file, hardlink, dan symlink
  (`libncursesw.so.6 → libncursesw.so.6.6`, `usr/bin/tput` 67 KB) semua benar.
- **Bug riwayat yang ditemukan oleh apk:** `path_argidx()` default 0 membuat
  `renameat`/`renameat2`/`linkat` membaca a[0] (dirfd, mis. 3) sebagai pointer path →
  `process_vm_readv` EFAULT → **semua operasi rename/link apk gagal** ("Bad address").
  Perbaikan: `path_argidx` = 1 untuk `renameat`/`renameat2`/`linkat`, 1 untuk `symlink`,
  2 untuk `symlinkat` (rename/link tetap 0). Ini **temuan terpenting seluruh review**
  (bug laten yang hampir tak kelihatan — hanya kena program yang banyak pakai renameat/linkat).
- **Limitasi 1 — db apk (bukan bug kita):** akhir commit db memakai
  `linkat(AT_EMPTY_PATH=0x400, /proc/self/fd/N, ...)` yang butuh `CAP_DAC_READ_SEARCH`
  → EPERM rootless → "failed to write database: Permission denied" (rc=2). **Terbukti
  inherent**: kontrol `apk --root $R ... add ncurses` vanila (semua syscall passthrough,
  tanpa rewrite) gagal PERSIS sama.
- **Limitasi 2 — skrip trigger:** `execve` skrip shebang (`#!/bin/sh`) dijalankan KERNEL;
  interpreter `/bin/sh` host = bionic `/system/bin/sh` yang **TIDAK BISA link di child
  kita** (CANNOT LINK libc.so — bionic butuh `/linkerconfig/ld.config.txt` namespace yang
  tak tersedia di proses non-zygote; terbukti juga bionic manapun di bawah svsp gagal,
  bahkan yang disalin ke dalam base). Workaround sah: `--no-scripts` (data instalasi utuh).
- **Binary baru hasil apk:** `PT_INTERP`-nya mentah (`/lib/ld-musl-aarch64.so.1` host tak
  ada) → jalankan via jalur cepat (loader dipanggil eksplisit, INTERP tak relevan) —
  sekarang berfungsi berkat `LD_LIBRARY_PATH` wadah (temuan di bawah). Setelah
  `patchelf --set-interpreter` (seperti bootstrap), bisa juga via `--svsp`.
- **Bug `fake-run` (2, ditemukan lewat 6.5):** (1) cabang LD_PRELOAD tidak men-set
  `LD_LIBRARY_PATH=$BASE/lib:$BASE/usr/lib` → binary multi-lib (tput/infocmp) gagal
  memuat `libncursesw.so.6`; (2) cabang svsp tidak men-set HOME/PATH/TMPDIR/PWD/USER
  (paritas env dengan jalur cepat hilang) → kini keduanya set identik.
- **Jalankan native:** `fake-run --base=$R $R/usr/bin/tput cols` → `80` rc=0;
  `infocmp xterm` → baca terminfo dari `/etc/terminfo/x/xterm` dalam wadah ✓.
- **Catatan mode:** binary DINAMIS yang kena syscall diblokir seccomp luar (Android)
  dapat SIGSYS di bawah `--svsp` (tanpa shim) → gunakan jalur cepat (default untuk
  dinamis); `--svsp` untuk statis. Not angka bug.

(b) **Claude Code — SESI NYATA di wadah:** `--version` → `2.1.291 (Claude Code)` rc=0;
`--help` → usage rc=0; **TUI terbuka** (via pty: `Welcome to Claude Code v2.1.291 ...
Checking connectivity...`); config **terbukti terisolasi**: `$R/root/.claude/{projects,
sessions}` dibuat oleh claude, `~/.claude` host tak tersentuh. Syarat: pratinstal
`/usr/bin/claude` hasil `patchelf` DICOPY KE DALAM base — claude melakukan self re-exec
dengan path absolutnya sendiri; path di luar base di-rewrite ke dalam wadah (semantik
isolasi yang benar) → ENOENT bila claude hidup di luar. Ini perilaku yang diharapkan,
bukan bug.

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
- **Bionic (host) TIDAK BISA berjalan di bawah `svsp`** maupun sebagai interpreter
  shebang (`#!/bin/sh` → `/system/bin/sh` → CANNOT LINK libc.so — namespace
  `/linkerconfig/ld.config.txt` tak tersedia di child non-zygote). Wadah ini **musl-only**;
  skrip trigger apk memakai shebang ini → gunakan `--no-scripts`.
- **Binary dinamis multi-lib** (tput, infocmp, dsb.) butuh `LD_LIBRARY_PATH=$BASE/lib:
  $BASE/usr/lib` (sudah diset oleh `fake-run`); binary hasil `apk add` punya PT_INTERP
  mentah → jalankan via jalur cepat, atau `patchelf --set-interpreter` dulu untuk `--svsp`.
- Build final `svsp` **bersih dari print diagnostik** (hanya `DBG` ter-guard `SVSP_DEBUG`).

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
cp $HOME/musl-test/package/claude $R/usr/local/bin/claude   # WAJIB di dalam wadah (self re-exec)
patchelf --set-interpreter $R/lib/ld-musl-patched.so.1 $R/usr/local/bin/claude
env -u LD_PRELOAD timeout 60 $FR --base=$R $R/usr/local/bin/claude --version  # 2.1.291
env -u LD_PRELOAD $FR --base=$R $R/usr/local/bin/claude --help                # usage rc=0
# isolasi config (bukti): setelah run, $R/root/.claude/{projects,sessions} ADA
# ncurses asli (hasil apk add --no-scripts; jalur cepat, INTERP tak relevan)
env -u LD_PRELOAD $FR --base=$R $R/usr/bin/tput cols        # 80
env -u LD_PRELOAD $FR --base=$R $R/usr/bin/infocmp xterm    # terminfo wadah
# apk add nyata (instalasi sukses; db-commit EPERM = limitasi apk-tools rootless, lihat §6.5)
env -u LD_PRELOAD timeout 300 $FR --svsp --base=$R $R/sbin/apk add --no-scripts --no-cache ncurses
# openat2 = SIGSYS eksternal Android (buktikan: st=31, TANPA notif nr=437 sama sekali)
cp examples/o2test.c /tmp/opencode/ 2>/dev/null; cp examples/o2test.c $R/usr/bin/ 2>/dev/null
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
- **Review + penyelesaian arena (2026-10-07, sesi `ses_eedd596b2ffeZjy2ArmIlOPnjW`)**:
  - **Cacat bawaan arena pada `svsp.c`:** tidak bisa build (`struct open_how` redefined →
    solusi `__has_include(<linux/openat2.h>)`); rc selalu 1 (`while(waitpid(-1,...))`
    me-reap target → status exit hilang) → fix `waitpid(pid,...)` utk target + `tst`;
    cache 256B → **4096 + skip store path ≥ 4095** (cegah pemotongan).
  - **Bug nyata ditemukan lewat uji 6.5(a) (`apk add`) — PALING PENTING:** `path_argidx()`
    default 0 → `renameat`/`renameat2`/`linkat` (dan path `symlink`/`symlinkat`) membaca
    dirfd sebagai pointer path → EFAULT → "Bad address" pada semua commit apk. Fix:
    `path_argidx` = 1 utk renameat/renameat2/linkat, 1 utk symlink, 2 utk symlinkat
    (rename/link tetap 0). Plus `read_string()` page-bounded (4K) & `open_how`
    page-bounded.
  - **`libfakeroot.c`:** hapus `#include <linux/stat.h>` (tidak ada di minirootfs musl-dev);
    verifikasi `_GNU_SOURCE` + `sys/stat.h` cukup untuk `statx`.
  - **`fake-run` (2 bug):** cabang LD_PRELOAD tanpa `LD_LIBRARY_PATH=$BASE/lib:$BASE/usr/lib`
    (binary multi-lib gagal) + cabang svsp tanpa HOME/PATH/TMPDIR/PWD/USER (paritas env).
  - **6.5(a) `apk add`:** instalasi sukses (3 paket + 19 pkg total, symlink benar); db-commit
    EPERM = limitasi **apk-tools rootless** (linkat `AT_EMPTY_PATH` butuh CAP_DAC_READ_SEARCH,
    terbukti via kontrol `apk --root` vanila); trigger shebang tak jalan (bionic `/bin/sh`
    tak bisa link di child — wadah musl-only) → `--no-scripts`.
  - **6.5(b) Claude:** `--version`/`--help` rc=0, **TUI terbuka** di wadah (pty), config
    terisolasi di `$R/root/.claude`; prasyarat binary di-dalam base (self re-exec).
  - **6.1 on-device:** `examples/o2test.c` membuktikan `openat2` di-SIGSYS seccomp eksternal
    Android sebelum NOTIF (st=31, 0 notif nr=437) → kode benar, **tak dapat diverifikasi
    on-device** (satu-satunya limitasi yang tak bisa dipaksa).
  - Merge `c7d6ccb0-brainstorming` via `--no-ff` (`2b135d3`) + commit akhir penyelesaian
    (belum di-push saat dokumen ditulis).
  - **Pintu masuk `alpine` + installer (arena `53f0c95`+`cfea6b9`, di-FF ke main):**
    `alpine` = masuk shell interaktif wadah (pengganti proot-distro login), `install.sh`
    satu-perintah, `uninstall.sh`. Review lalu dua perbaikan:
  - **Fix `alpine` (`-c`):** branch `-c "cmd"` semula di-pass ke `fake-run <prog>` ("program
    tak ada: -c") → kini `fake-run ... /bin/sh -c "..."`; program biasa tetap passthrough
    langsung (resolusi PATH wadah oleh fake-run). Teruji: `alpine -c "echo; cat /etc/alpine-release"`,
    interaktif via stdin, `alpine tree -L 1 /usr`, `alpine apk add --no-scripts --no-cache`.
  - **Fix shim `libfakeroot.c` (exec anak ber-interp mentah) — TERPENTING:** dari shell
    interaktif, binary hasil `apk add` segar (PT_INTERP mentah `/lib/ld-musl-aarch64.so.1`)
    tidak bisa di-exec (kernel cari interp di host → ENOENT); binary terpatchelf (apk, tput,
    claude) jalan. Fix: intercept `exec*` wrapper kini memeriksa ELF target — bila dinamis
    ber-INTERP mentah → re-exec via `$BASE/lib/ld-musl-patched.so.1 host args...` (pola
    persis jalur cepat fake-run). Terbukti: `bc` (`6*7`→42), `tree`, `figlet` jalan dari
    dalam shell; `claude --version`→2.1.291; statik (gobukti/svsp) tak berubah; ebench
    tak terpengaruh. Warning build lama (fopen-family) non-fatal, build `-nostdlib` eksak
    bootstrap (`35440` byte).
  - **Environment note (bukan bug):** DNS publik (1.1.1.1/8.8.8.8/dll.) sesekali di-drop
    carrier → `apk` "DNS: transient error" (resolver apk-tools 3.0 punya sendiri, abaikan
    `/etc/hosts`); `--no-cache` menjauhkan cache-write EPERM; `--no-scripts` wajib. Saat
    window DNS baik, `apk add` jalan normal (tree, figlet, bc, tput, ncurses terpasang
    hari ini). SIGSYS flaky sesekali (race startup vs seccomp Android, logcat `signal 31`)
    pada jalur cepat — 5/5 sukses pada sampling, pre-existing.
- **BUG AKTIF terbuka: flake boot "not found" (env LD_* hilang ~0-25%)** — investigasi
  lengkap, bukti empiris, locus tersisa, dan eksperimen berikutnya ada di **§11**. Status
  git terkini (`3274c0b` lokal, `origin/main` tertinggal) juga di §11.1.

---

## 11. BUG AKTIF — FLAKE BOOT "not found" (handoff lengkap ke arena.ai)

> **INI SUDAH SELESAI — root cause & fix ada di §12 (SOLVED).** §11 di bawah adalah jejak
> investigasi saat handoff (status git di 11.1 sudah kedaluwarsa: `main` kini sudah push).
>
> **Ini pekerjaan TERBUKA paling penting.** Fungsi inti semua hijau (figlet Q, alpine-release
> 3.24.2, claude 2.1.291, tput 80, bc 42, apk-tools 3.0.8-r0, `alpine -c echo halo-wadah`),
> tapi boot wadah **sesekali** (0–25%, tergantung kondisi device) kehilangan LD_PRELOAD →
> shim tak terload → binary eksternal "not found". Sudah 2 hari diselidiki; baca 11.3–11.7
> sebelum menyentuh apa pun — semua teori lama sudah dibunuh dengan bukti.

### 11.1 Status git SEKARANG (bereskan duluan)

```sh
cd ~/Brainstorming && git log --oneline -3 && git status --short && git log origin/main --oneline -1
```

- Local `main` = **`3274c0b`** ("fix: fk_exec_one argv[0] off-by-one — binary apk add punya
  argumen ekstra") — **fix arena TERVERIFIKASI benar (figlet render satu "Q" = off-by-one
  sembuh; tree abs+bare OK) dan sudah di-merge FF** di atas `f446942`.
- **`origin/main` MASIH `f446942` — `3274c0b` BELUM DI-PUSH.**
- Working tree: `fake-run` MODIFIED tak-ter-commit (54+/2−) = **percobaan fix yang GAGAL**
  (lihat 11.9). **JANGAN di-push sebelum diputuskan keep/revert.**
- Saran: push `3274c0b` sekarang (fix independen, terverifikasi), biarkan diff `fake-run`
  sebagai WIP sampai flake tuntas.

### 11.2 Gejala & laju

```
alpine -c "tree -L 1 /usr | head"      # kadang "tree: not found"
fake-run --base=$R /bin/sh -c 'tree'   # kadang "not found"
```
- Identik command → hasil berbeda antar run. Laju terukur: 11/50, 8/40, 7/30, 3/24, 4/30,
  2/30, 5/30, 6/40 — **bergeser antar blok (0–25%)**, kemungkinan termal/condition device.
- Semua run GAGAL identik: ash hidup, tapi `LD_PRELOAD` & `LD_LIBRARY_PATH` **HILANG dari
  env-nya** → shim (libfakeroot.so) tak pernah di-dlopen → tanpa rewrite path → binary di
  luar base "not found" (kernel ENOENT = kontrolnya, lihat 11.3#1).

### 11.3 Fakta mekanisme (TERBUKTI, jangan dibantah tanpa bukti baru)

1. **Kontrol**: binary ber-interp mentah di-exec polos di host → kernel ENOENT. **Setiap
   boot non-dispatch = "not found"** — apa pun penyebab LD_PRELOAD hilang.
2. **Dispatch** (shim `execve` wrapper → loader-as-main, env=`environ`) → **100% andal,
   TIDAK PERNAH flake**. Jalur runtime di dalam wadah aman. Flake hanya di **BOOT**
   (rantai `fake-run` → `env(1)` → loader → ash).
3. **DBG shim** (`FK_DBG`: constructor + semua wrapper exec + interpose posix_spawn):
   run GAGAL punya **NOL baris `[fk]` termasuk constructor** → shim memang tidak pernah
   terload → `LD_PRELOAD` benar-benar tidak ada saat loader membaca env.
4. **Ringkasan env run GAGAL** (dump `env | sort`, 24 run): hilang **PERSIS dua var**
   `LD_PRELOAD` + `LD_LIBRARY_PATH`; 10 baris lain utuh (FAKE_BASE, HOME, PATH, PWD, ...
   lengkap). Bukan truncation blok.
5. **Marker test (DISKRIMINATOR KUNCI, 30× lewat fake-run)**:
   `fake-run LD_PRELOAD_32=mark2 FAKE_MARK=mark1 --base=$R /bin/sh -c 'env | sort'`
   → run UNSET (8/30) kehilangan **LD_PRELOAD + LD_LIBRARY_PATH + LD_PRELOAD_32 — persis
   ketiga nama di daftar `-u` env(1)** — sementara **FAKE_MARK (var TERAKHIR, tidak di
   `-u`) SELALU ADA**. ⇒ strip menyasar **tepat set nama opsi `-u`** env(1), selalu
   bersamaan, bukan truncation.
6. **env(1) = GNU coreutils 9.11** (`/data/data/com.termux/files/usr/bin/env -> coreutils`,
   bukan busybox). Proses opsi: `-i` kosongkan, `-u NAME` unset, `NAME=value` set —
   berdasar uji, urutan normalnya deterministik-bersih (lihat 11.4).

### 11.4 Riwayat eksperimen (kronologis, semua on-device)

| # | Uji | Hasil | Makna |
|---|---|---|---|
| a | `env(1) → loader` direct-invocation (path benar) | 0/32 bersih | loader-as-main OK bila env benar |
| b | `env(1) → $R/bin/sh` manual (E1/E2) | 0/20, 0/20 | env(1)→ash manual bersih |
| c | `env(1) -i <daftar persis fake-run> → $PREFIX/bin/env` (bionic) | 24/24, 0/50 | **env(1) standalone deterministik-bersih** |
| d | `env(1)` → loader → (semua varian) | 100% bersih | dispatch/loader OK |
| e | rantai `fake-run` | **FLAKY** (11/50 … 5/30) | flake butuh konteks fake-run |
| f | Marker test lewat fake-run | 8/30 UNSET (trio `-u` hilang, FAKE_MARK ada) | strip = tepat daftar `-u`; lihat 11.3#5 |
| g | `env(1)` → ash langsung + marker (TANPA fake-run) | **0/40 bersih** | loader-as-interp BUKAN pelaku strip |
| h | `bash -c 'exec env -i <sama>'` → ash | **0/40 bersih** | "bash di tengah" bukan pelaku |
| i | `fake-run` bare (TANPA `timeout`) | **5/30 FLAKY** | `timeout` tak bersalah |
| j | `fake-run` TANPA prefix `env -u LD_PRELOAD` (preload termux-exec host tetap hadir di rantai) | **4/30 FLAKY** | teori "harness yang buang preload = penyebab" **MATI** |
| k | `/proc/self/environ` utk bedakan envp-asli vs `environ` | **DIBLOKIR Android** (proc=0 bahkan di run SET) | jangan dipakai; alternatif lihat 11.7#E6 |
| l | `command -v env` | GNU coreutils 9.11 | bukan busybox |

### 11.5 Yang sudah DIBUNUH (jangan ulangi)

- ❌ Bukan seccomp (logcat bersih, tanpa SIGSYS di jam flake; histori SIGSYS 01:54-02:03 &
  x-sh CANNOT-LINK 12:55 = lampau).
- ❌ Bukan truncation blok env (FAKE_MARK var terakhir selalu ada).
- ❌ Bukan env(1) standalone (0/50 dengan daftar flag persis).
- ❌ Bukan loader-as-interpreter per se (env→ash langsung 0/40).
- ❌ Bukan `timeout` (fake-run bare tetap flaky).
- ❌ Bukan strip `env -u LD_PRELOAD` di harness (fr tanpa prefix tetap flaky 4/30).
- ❌ Bukan bash-di-tengah (bash -c exec env 0/40).

### 11.6 Locus tersempit yang tersisa

Flake **MEMBUTUHKAN skrip fake-run** di antara env(1) dan ash. Yang fake-run tambahkan
vs uji g/h yang bersih:

1. `cd "$HOME_BASE"` sebelum exec (baris 121) → CWD env(1) = `$HOME_BASE` (uji g/h dari `$HOME`).
2. `PWD="$HOME_BASE"` di daftar env (uji g/h TIDAK men-set PWD).
3. Subproses sebelum exec: `is_dynamic` → `readelf -l` (fork+exec), `interp_of` → `od|dd|tr|head`.
4. `set -euo pipefail`; `mkdir -p root tmp`; ekspansi `TERM="${TERM:-...}"` +
   `${extra_env[@]+...}`.
5. `exec env` (env menggantikan proses bash) vs `fork env` di uji g. — sudah dianulir oleh h
   (bash -c 'exec env') tapi h tidak punya #1/#2/#3.

### 11.7 Eksperimen berikutnya (murah, urut prioritas — lakukan semuanya)

- **E1**: `cd $R && env -i <daftar penuh fake-run TERMASUK PWD=$HOME_BASE> $R/bin/sh -c 'sig'` 40×
  → isolasi CWD+PWD. (Pakai `$PREFIX/bin/env` + marker LD_PRELOAD_32/FAKE_MARK utk verifikasi.)
- **E2**: salinan fake-run dengan baris `cd "$HOME_BASE"` dihapus (atau `cd "$HOME"`) → 40×.
- **E3**: salinan fake-run dengan `PWD=` dihapus dari kedua daftar env → 40×.
- **E4**: fake-run + debug `echo` penuh baris `exec env -i` tiap run (pasti argv identik tiap run).
- **E5**: hapus **seluruh flag `-u`** dari daftar env (redundan: `env -i` sudah mulai kosong) → 40×.
  Kalau bersih: interaksi `-u`-list di coreutils env 9.11 adalah pemicunya → fix = buang `-u`.
- **E6** (bila E1–E5 tak ada yang bersih): cari strip di dalam musl ldso —
  `strings ~/alpine-rootfs/lib/ld-musl-patched.so.1 | grep -E '^LD_(PRELOAD|LIBRARY_PATH|PRELOAD_32)'`,
  lalu bandingkan dengan loader STOCK (`bbx-old-bak`? atau ekstrak ulang dari bootstrap).
  Musl setara glibc punya sanitasi env LD_* di sebagian jalur init; loader = stock yang
  di-byte-patch 7 situs sinyal ⇒ **bandingkan perilaku stock vs patched** (uji boot dengan
  `LD_PRELOAD` via `env` ke binary ber-interp STOCK bila ada salinannya).
  Cek juga `LD_DEBUG`-style: musl dukung `LD_DEBUG=all`? (musl tidak; jangan buang waktu).

### 11.8 Arah fix (saat locus ditemukan)

- **Paling murah & elegan bila E5 bersih**: hilangkan flag `-u` redundan dari kedua cabang
  `exec env -i` di fake-run. Tetap `-i` (mulai kosong) + `NAME=value` murni.
- **Paling kokoh (anti-semua-locus)**: buat boot TIDAK bergantung pada env sama sekali —
  patch loader agar **selalu dlopen shim** (byte-patch prolog fungsi tertentu utk memanggil
  dlopen path tetap, atau bangun loader dari sumber musl dengan shim di-hardcode).
  Jalur dispatch shim (11.3#2) sudah terbukti 100% andal — idealnya boot masuk ke jalur itu
  secepat mungkin.
- **Fallback pragmatis**: boot lewat perantara C statis kecil (bukan env coreutils) yang membangun
  env secara deterministik dengan `execve` polos — tapi fakta #g (0/40) menunjukkan env coreutils
  sendiri bukan masalahnya, jadi ini bukan prioritas.

### 11.9 Inventaris perubahan & artefak

**WIP `fake-run` (UNCOMMITTED — jangan push tanpa keputusan):**
1. `od -An -vu1` → `od -An -t u1` di `is_dynamic` + kajian `interp_of` (fix nyata: `-vu1`
   tidak valid di host ini; fallback lama kena juga). **LAYAK PERTAHANKAN.**
2. `interp_of()` baru + cabang direct-exec bila `IP == $HOME_BASE/lib/ld-musl-patched.so.1`
   (interp sudah host-resolvable → exec langsung, hindari loader-as-main saat boot).
   **TERBUKTI TIDAK MENYEMBUHKAN flake** (regresi 8/40, 7/30 setelah edit; uji i/j tetap
   flaky). **Keep atau revert?** — direct-exec sendiri benar & sejalan arah fix 11.8, tapi
   saat ini mubazir karena pelaku strip bukan loader-as-main. Keputusan di tangan arena.
3. `$PREFIX/bin/fake-run` sudah disinkron = salinan hasil edit ini.

**Shim produksi `~/libfakeroot.so` (35440 B, sha256 `083e69e3...`, build arena 3274c0b,
terpasang):** interpose `execv/execl/execve/execvp/execvpe` + loader re-exec utk interp
mentah + handler SIGSYS. **Belum** interpose `posix_spawn/posix_spawnp` (grep = 0) —
coreutils/python memakainya; DBG shim punya wrapper-nya tapi produksi belum (gap nyata,
belum terbukti terkait flake). `statx` sudah di-interpose.

**Artefak debug di `/data/data/com.termux/files/usr/tmp/opencode/` (bukan repo):**
`flake-test.sh` (mode fr/frn/al), `iso.sh` (A/B/C), `locus2.sh` (direct/bashmid),
`locus-test.sh` (proc — diblokir Android), `libfakeroot-dbg.{c,so}` (shim FK_DBG:
constructor + exec + posix_spawn), `env-1..24-{SET,UNSET}.txt` (dump env 24 run),
`interp_of.sh` (impl uji interp_of), `libfakeroot-new/final*.so` (build lama).

### 11.10 Cara repro cepat (copy-paste)

```sh
R=$HOME/alpine-rootfs
# sig probe: [x] = shim-load OK; [] = FLAKE (LD_PRELOAD hilang)
for i in $(seq 1 30); do
  out=$(fake-run --base="$R" /bin/sh -c 'printf "sig=[%s]\n" "${LD_PRELOAD+x}"' 2>&1)
  echo "$out" | grep -q 'sig=\[x\]' || echo "run$i: UNSET [$out]"
done
# marker: UNSET harus kehilangan tepat trio -u (LD_PRELOAD, LD_LIBRARY_PATH, LD_PRELOAD_32)
fake-run LD_PRELOAD_32=mark2 FAKE_MARK=mark1 --base="$R" /bin/sh -c 'env | sort' | grep -E '^(LD_|FAKE_)'
```
Syarat lingkungan: jalankan dari shell Termux biasa (bukan coba bereksperimen sambil
dipanaskan device — laju flake sensitif kondisi termal). Pakai `timeout 20` kalau takut gantung.

---

## 12. ✅ SOLVED — AKAR MASALAH DITEMUKAN & DIPERBAIKI

> **Flake boot "not found" = race SIGPIPE `readelf | grep -q` di `is_dynamic()` fake-run,
> di bawah `set -o pipefail`, yang sesekali salah klasifikasi binary DINAMIS → statis →
> MODE=svsp → boot lewat supervisor svsp → `svsp.c:722` men-strip trio LD_* → ash tanpa
> shim.** Bukan env(1), bukan `-u`, bukan loader, bukan truncation. Semua bukti §11.3–11.7
> yang dulu terlihat "strip tepat daftar `-u`" sebenarnya adalah `unsetenv` svsp.c — kebetulan
> nama var yang sama. Bagian di bawah adalah rantai penemuan on-device (7 Okt 2026).

### 12.1 Rantai penemuan (urutan eksperimen sesungguhnya)

1. **Balasan arena `d011680`** (branch `arena/c7d6ccb0-brainstorming`) = implementasi E5:
   hapus flag `-u` redundan dari kedua `exec env -i` + fix `od -vu1 → -t u1` + bersihkan
   `env -u LD_PRELOAD` di `alpine`. → **DIVERIFIKASI EMPIRIS: TIDAK menyembuhkan**
   (arena2 tanpa `-u`: 3/20; WIP dengan `-u`: 2/20 — selang-seling, kondisi sama).
2. **E2** (hapus `cd "$HOME_BASE"`): juga tidak menyembuhkan (2/20 vs 1/20) → `cd` bukan pemicu.
3. **E1** (`cd $R && env -i <daftar penuh + PWD> $R/bin/sh` TANPA fake-run): **0/40 bersih**
   → pelaku WAJIB ada di dalam skrip fake-run, bukan di daftar env/CWD/PWD.
4. **NDYN** (arena2 minus blok `if is_dynamic`): **0/24 lalu 0/60** vs arena2 asli 2/24 lalu
   **8/60** (selang-seling) → **panggilan `is_dynamic` (subproses `readelf`) = pemicunya**.
5. **TIMING** (ndyn + `sleep 0.05` di top / di posisi readelf): **0/30 dan 0/30** →
   BUKAN soal delay/timing — soal subproses readelf itu sendiri.
6. **FDIAG** (arena2 + log `MODE` + log kegagalan is_dynamic): 5/40 UNSET ↔ **5 DYNF ↔
   5 MODE=svsp — korelasi 1:1 SEMPURNA** → saat is_dynamic gagal, MODE jadi `svsp`.
7. **Pembacaan `svsp.c:722`**: `unsetenv("LD_PRELOAD"); unsetenv("LD_LIBRARY_PATH");
   unsetenv("LD_PRELOAD_32");` — svsp **sengaja** men-strip trio itu sebelum `execv`
   target (komentar: "Menutup celah LD_PRELOAD"). → persis pola env gagal sepanjang riset
   (trio LD_* hilang, FAKE_MARK ada, sisanya utuh).
8. **Senjata bukti pamungkas** (`set -o pipefail`):
   - `readelf -l f | grep -q 'Requesting'`: **14/60 gagal (23%)** — persis laju flake!
   - `readelf -l f | grep 'Requesting' >/dev/null`: **0/60** — deterministik.
   → `grep -q` keluar begitu cocok → readelf kena **SIGPIPE (rc 141)** → di bawah pipefail
   status pipeline = 141 → `is_dynamic` return false → MODE=svsp. Bila readelf menulis
   output lebih cepat dari penutupan pipe, lolos (rc 0) → MODE=ldpreload.

### 12.2 Mengapa semua gejala lama cocok

| Gejala lama (§11) | Penjelasan sebenarnya |
|---|---|
| "Hilang persis trio daftar `-u` env(1)" | `unsetenv` satu-satu di svsp.c — nama var kebetulan sama; akhirnya FAKE_MARK (bukan LD_*) lolos |
| "Flake butuh skrip fake-run" | hanya fake-run yang memanggil `readelf` (lewat `is_dynamic`) |
| "env direct / bashmid / locus2 / E1 selalu bersih" | tidak ada `readelf` di rantainya |
| "Laju bergeser 0–25% antar blok, sensitif termal" | race kecepatan readelf vs `grep -q`; beban/thermal menggeser peluang SIGPIPE |
| "Balasan arena (hapus `-u`) tak menyembuhkan" | `-u` tidak pernah jadi mekanisme — strip datang dari svsp.c |
| "Not found" | MODE=svsp → `svsp.c` strip LD_* → ash tanpa shim → tanpa rewrite path → binary luar base ENOENT |

### 12.3 Fix (di `fake-run`, 1 baris efektif)

```sh
# SEBELUM (BUKAN ini):
readelf -l "$f" 2>/dev/null | grep -q 'Requesting program interpreter'
# SESUDAH (aman): grep biasa — tidak keluar lebih awal, readelf selesai normal, tanpa SIGPIPE
readelf -l "$f" 2>/dev/null | grep 'Requesting program interpreter' >/dev/null
```

Catatan:
- `fake-run` sekarang = basis arena `d011680` (od `-t u1` + tanpa `-u`) + fix di atas.
- Cabang direct-exec/`interp_of` dari WIP lokal lama **TIDAK dipertahankan** — percobaan
  itu menyelesaikan hantu; loader-as-main bukan masalah. fake-run jadi lebih sederhana.
- `svsp.c` sengaja strip LD_* → **JANGAN ubah**, itu memang desain (proteksi celah preload
  di jalur supervisor). Yang salah adalah klasifikasi dinamis/statis, bukan svsp-nya.
- `alpine` tidak diubah di main ini (perubahan arena hanya kosmetik `env -u LD_PRELOAD`).

### 12.4 Verifikasi empiris (on-device, sesudah fix)

```
flake probe 100×                  : 0/100  UNSET   (sebelumnya 8–14/60 ≈ 13–23%)
marker test (4 var, 1×)           : 4/4    ADA     (trio LD_* kini hadir)
alpine-release                    : 3.24.2 ✓
tput cols                         : 80 ✓
bc (stdin '6*7')                  : 42 ✓
alpine -c echo halo-wadah         : halo-wadah ✓
figlet Q (via sh -c, 6 baris art) : ✓ (figlet = binary standalone, BUKAN applet busybox)
busybox figlet Q                  : "applet not found" = WAJAR (figlet tak ada di busybox)
```

Sampel besar selang-seling sesudah fix: probe 100× + regresi fungsi = semua hijau. Laju
flake sebelumnya terukur 13–23% pada sesi yang sama (kondisi termal sebanding).

### 12.5 Pelajaran / rekomendasi lanjutan

1. **Pola** untuk masa depan: `grep -q` di pipeline ber-`set -o pipefail` = SIGPIPE race.
   Ganti dengan `grep ... >/dev/null` atau `grep -q ... || true` bila status tak penting.
   Audit seluruh repo: `grep -rn "grep -q" --include=*.sh` (fake-run/alpine/installer).
2. Verifikasi balasan agent secara empiris sebelum menerima (aturan proyek; E5 terbukti mati).
3. Status git saat commit ini: `main` berisi §11 (handoff) + §12 (SOLVED); branch arena
   `d011680` punya versi fix E5 yang salah arah — bagian od-fix-nya sudah diadopsi, bagian
   hapus `-u` tidak berbahaya dan ikut teradopsi. Tidak perlu sinkronisasi khusus.
---

# 13. svsp statis musl: penolakan linker64 device lain (CANNOT LINK verneed[0])

## 13.1 Gejala

Di device LAIN (selain device utama), `bootstrap.sh` gagal di lengan V2 dengan
error yang sama persis seperti transient lama — tetapi **persisten 3/3**:

```
CANNOT LINK EXECUTABLE ".../usr/bin/svsp": cannot find "libc.so" from verneed[0] in DT_NEEDED list for ".../usr/bin/svsp"
```

Kesimpulan "sekali-kedip transient" ternyata salah untuk device umum: di sebagian
device, linker64 **menolak binary bionic dinamis** `svsp` (yang dibuild clang
device itu sendiri) secara deterministik. Beda versi clang/lld/linker64 antar
device → struktur verneed/DT_NEEDED yang dihasilkan ditolak.

## 13.2 Keputusan: svsp = binary STATIS musl

`svsp` cuma supervisor USER_NOTIF murni syscall (stdio/stdlib/unistd/fcntl/
poll/signal/socket/stat/linux headers — tak ada dl*, pthread, atau API bionic
eksklusif). Karena itu bisa dibuild **statis musl** (pola `boot-static` +
compiler-rt builtins), dan:

- kernel **exec langsung** — tidak ada linker host, tidak ada verneed/DT_NEEDED
  yang bisa ditolak linker64 mana pun;
- berjalan di SEMUA device Android (bukan cuma yang linker64-nya menerima);
- ukurannya ~50 KB setelah strip (vs 15 KB bionic — biaya tetap wajar).

## 13.3 Perubahan

- `svsp.c`: tambah `#include <stddef.h>` (musl lebih ketat: `offsetof`); komentar
  compile diubah ke resep statis.
- `bootstrap.sh musl_dev()`: selain musl-dev, kini menginstall paket
  `linux-headers` (UAPI `linux/audit.h`, `filter.h`, `seccomp.h`, `openat2.h`)
  ke wadah dengan pola cache+indeks yang sama; guard reuse diperluas.
- `bootstrap.sh build_binaries()`: build svsp:
  ```
  clang --target=aarch64-alpine-linux-musl --sysroot=$BASE -static -nostdlib -O2 \
    -o svsp $BASE/usr/lib/crt1.o $BASE/usr/lib/crti.o svsp.c \
    $BASE/usr/lib/libc.a $BASE/usr/lib/crtn.o \
    $(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-aarch64-android.a
  ```
  `libclang_rt.builtins` dibutuhkan vfprintf musl (soft-float `__addtf3` dll).
- `fake-run`: pesan error bila svsp tak ada diarahkan ke bootstrap.sh.

## 13.4 Verifikasi empiris (device utama, sesudah fix)

```
V1 LD_PRELOAD cat /etc/alpine-release          : 3.24.2 ✓
V2 --svsp cat (bionic dinamis) di bawah svsp statis : 3.24.2 ✓
V3 boot-static (anak statis)                   : 3.24.2 BOOT-STATIC-OK ✓
svsp langsung: --base + loader wadah + busybox : VERSION_ID=3.24.2 ✓
sh -c multi-perintah + subshell (id/cat/ls)    : ✓
tree -L 1 / + figlet                            : ✓
build: ELF 64-bit statically linked, ~50 KB     ✓
```

Instalasi ulang penuh `./install.sh` harus hijau; device yang tadinya menolak
svsp bionic kini tak bisa menolak svsp statis (tanpa linker = tanpa penolakan).

---

# 14. Beres-beres UX: warning shim hilang + prompt shell pendek

## 14.1 13 warning `-Wconditional-type-mismatch` libfakeroot.c → 0

Penyebab: makro `CALL(fn, ...)` punya fallback `(errno = ENOSYS, -1)` yang
bertipe `int`, dipakai oleh fungsi return-pointer (`fopen`, `opendir`,
`realpath`, `getcwd`, …). Conditional `next ? hasil(fn) : int` → tipe mismatched.

Fix (tipe-generik, tanpa ubah semantik):
```c
#define CALL(fn, ...) (next_##fn ? next_##fn(__VA_ARGS__) \
                      : (errno = ENOSYS, (__typeof__(next_##fn(__VA_ARGS__)))-1))
```
`__typeof__(callsite)` = tipe return fungsi → fallback ikut tipe itu: `-1` untuk
int/ssize_t, `NULL`-ish untuk pointer (jalur mati — hanya dipakai kalau dlsym
gagal). Build kini `0 warnings`.

## 14.2 Prompt shell interaktif `alpine` (dan fake-run) panjang

Sebelum: `localhost:/data/data/com.termux/files/home/alpine-rootfs$ `
— `\w` = path penuh karena cwd = `$BASE` dan `PWD` di-hardcode `$HOME_BASE`.

Akar: `fake-run` hardcode `cd "$HOME_BASE"` + `PWD="$HOME_BASE"` di `env -i`;
busybox ash mempercayai `PWD` env, jadi cd di wrapper tak mempan.

Fix:
- `fake-run`: dukung `FAKE_START` (opsional; dikosongkan = perilaku lama),
  `PWD="$(pwd)"` di kedua `env -i` agar env PWD selalu = cwd nyata.
- `alpine` (interaktif): `FAKE_START="$BASE/root"` → login shell mendarat di
  `/root` wadah → `\w` = `~`.
- Di luar /root, getcwd shim sudah rewrite `$BASE→/` → `cd /etc` tampil
  `localhost:/etc$` (pendek juga).

Hasil: `localhost:~$ ` di awal, `localhost:/etc$` di luar home. Verifikasi via
PTY (`script`) + baterai V1–V4 + marker — semua hijau.

## 15. ✅ SOLVED — DNS "transient error" (EAI_AGAIN 5s) + internet penuh di wadah

Keluhan user: "kok gbsa akses internet DNS transient error". Akar: **libc musl
wadah hanya punya 6 JUMP_SLOT (keluarga malloc) di ld-musl** — panggilan internal
libc (resolver buka `/etc/resolv.conf`, `/etc/hosts`) direct-bound → LD_PRELOAD
shim TIDAK bisa interpose → `open` host tak ada file wadahnya → fallback
127.0.0.1 → timeout 5s → EAI_AGAIN.

Fix (blok §3 resolver di libfakeroot.c):
- Shim mengekspor resolver sendiri: `getaddrinfo` / `gethostbyname` /
  `gethostbyname2` / `freeaddrinfo` — baca resolv.conf via base-prefix,
  UDP paralel gaya `__res_msend`, IPv4 dulu.
- `bootstrap.sh`: `setup_dns()` tulis `resolv.conf` wadah = 4 NS
  (1.1.1.1 / 1.0.0.1 / 8.8.8.8 / 8.8.4.4) + `options timeout:2 attempts:3`
  (sebelumnya masih `192.0.2.1`!). Pin `/etc/hosts` stale
  `140.248.130.132 dl-cdn.alpinelinux.org` DIHAPUS (DNS reliable, pin tak perlu).

Verifikasi: `dg` (statis) 0.01s; `dg-dyn2` (dinamis) example.com rc=0 0.03s
IPv4-first, NXDOMAIN 0.01s; `ping example.com` 0% loss; `wget` nama rc=0;
`apk update` → "OK: 28554 distinct packages available".

## 16. ✅ SOLVED — apk-tools 3.0.8: O_TMPFILE + trigger script + chown

- **O_TMPFILE**: apk tulis DB via `openat(...,O_RDWR|O_TMPFILE)` lalu publish
  `linkat("/proc/self/fd/N",...)` → GAGAL di Android (EACCES/ENOENT) → DB
  `installed` tak pernah ter-link. Shim mask `O_TMPFILE` (`FK_FLAGS` di
  open/open64/openat/openat64) → apk fallback nama-temp `installed.tmp` +
  `renameat` relatif (jalan dalam base). `apk add` → "OK".
- **musl mkstemp/mkdtemp/tmpfile direct-bound** → tambah interposer keluarga
  mkstemp/mkostemp/mkstemps/mkostemps/mkdtemp/creat + tmpfile (di $BASE/tmp).
- **Shebang script exec**: kernel/musl-loader resolve `#!/bin/sh` di HOST →
  "CANNOT LINK EXECUTABLE /bin/sh". Dua lapis fix:
  (a) `fake-run`: parse `#!` + arg shebang → exec INTERP dari dalam base
      (loader+interp+script), jalur svsp & ldpreload.
  (b) shim `fk_try_shebang_exec`: utk exec child (mis. `sh -c './x.sh'`).
- **Env-merge** (`fk_ensure_wadah_env`): apk scrub env script (hanya
  APK_SCRIPT/APK_PACKAGE) → anak trigger tanpa LD_PRELOAD/PATH → rusak. Bila
  envp tak membawa FAKE_BASE → gabung `environ` induk (kunci envp menang).
- **chown→0** (shim + svsp): `apk_fsdir_update_perms` fchownat → EPERM tanpa
  root → `num_dir_update_errors`. Fakeroot-style: pretend success (wadah
  single-user). Builtin `busybox --install -s` trigger kini tereksekusi.

## 17. ✅ SOLVED — ditutup di ronde 4 (device: selftest 0 FAIL, 2026-10-07)

> **UPDATE FINAL §23:** semua butir 1 & 2 terverifikasi hijau di device lewat
> `./selftest` (0 FAIL), termasuk kriteria penentu `--svsp apk add acl`
> transaksi nyata rc=0. Butir 3 (device-2) & 4 (bersih-bersih) lihat §23.3.
> Jejak root-cause & fix: §18 (env-merge + flag basi), §19 (shebang wrapper),
> §20 (/proc/self), §22 (script_absolutize).

1. **`apk add/del` selalu "1 error" (rc=1) SILENT** — tanpa baris ERROR, tanpa
   pesan "failed to write database". Muncul di SETIAP transaksi tulis DB
   (juga `--no-scripts`, `--no-chown`). `apk update` & baca = bersih. Dugaan
   kuat: `errors += r` per-change / `num_dir_update_errors` / `run_triggers`
   via jalur yang TIDAK mencetak apa pun. Replika manual trigger busybox
   persis anak apk (`env -i APK_SCRIPT=trigger APK_PACKAGE=busybox <trigger>
   /bin`) → `rc=127 /bin/busybox: not found` → env-merge diduga BELUM bekerja
   di jalur busybox env→script (debug: apakah execve interposer kena).
2. **svsp path: DB write EACCES** — `fake-run --svsp apk add` →
   "failed to write database: Permission denied". Mask O_TMPFILE hanya di
   SHIM; jalur svsp (tanpa shim) harus mask O_TMPFILE di level openat (svsp.c).
3. **Verifikasi device-2**: `git pull && ./install.sh`; pastikan resolv.conf
   wadah 4NS+options via setup_dns().
4. Bersihkan artifact uji: `$BASE/tmp/*.apk`, hello scripts, dg/dg-dyn2
   (dg/dg-dyn2 tetap di wadah utk alat uji DNS).

## 18. ✅ ROOT-CAUSE + FIX bug §17.1 & §17.2 (svsp O_TMPFILE, "1 error" silent, env-merge)

Dikerjakan di branch `arena/a172be3f-brainstorming`. Semua analisis memakai
sumber apk-tools v3.0.8 (`/home/user/scratch/apk38`, tag `v3.0.8`). Yang
diverifikasi di sandbox x86_64 hanya logika (seccomp USER_NOTIF + preload);
**belum** diverifikasi di device Android (sandbox ini tak punya toolchain
musl/aarch64 maupun device).

### 18.1 Bug §17.2 — svsp: "failed to write database: Permission denied"

**Rantai syscall lengkap** (mengapa hanya jalur svsp yang kena; jalur shim
LD_PRELOAD aman karena `FK_FLAGS` sudah mem-mask O_TMPFILE):

1. `apk_db_write_layers` (`database.c:2242-2250`):
   `ld->fd = openat(db->root_fd, layer, O_DIRECTORY|O_RDONLY|O_CLOEXEC)`
   lalu `apk_ostream_to_file(ld->fd, "installed"|"triggers"|"scripts.tar.gz"|"world", 0644)`.
2. `__apk_ostream_to_file` (`src/io.c`): karena `is_proc_fd_ok()` true
   (`$BASE/proc` symlink ke `/proc`, dibuat `bootstrap.sh:152-168`), dipakai
   `tmpfile=true` → `openat(ld->fd, ".", O_RDWR|O_TMPFILE|O_CLOEXEC, mode)`.
3. Path `"."` RELATIF → svsp lama `changed==0` → jawab CONTINUE → **child**
   membuka O_TMPFILE sungguhan (anon-inode).
4. Publish di `fdo_close`: `linkat(AT_FDCWD, "/proc/self/fd/N", ld->fd,
   "installed.tmp.<pid>", AT_SYMLINK_FOLLOW)` — juga relatif/passthrough →
   butuh **CAP_DAC_READ_SEARCH** → selalu EACCES di Android →
   `apk_ostream_cancel` → `apk_err("System state may be inconsistent: failed
   to write database: %s")` (`database.c:2341-2344`).

**Fix di `svsp.c`** (semua sudah di-commit):

- `SVSP_MASK_TMPFILE(f)` — mask HANYA bila bit anon benar-benar set
  (`(f & O_TMPFILE) == O_TMPFILE`, semantik kernel; O_TMPFILE mengandung bit
  O_DIRECTORY sehingga masking tanpa syarat merusak open direktori biasa).
- **Force supervisor** menangani `openat` ber-O_TMPFILE walau path tak
  berubah (relatif "."). Dirfd child dipin via
  `open("/proc/<pid>/fd/<dirfd>", O_PATH)` (`child_fd_ref`), lalu supervisor
  mengeksekusi `openat(dirfd_pin, ".", masked_flags, mode)`.
- Hasil masking = buka direktori dgn O_RDWR → **EISDIR** → apk jatuh ke
  fallback nama-temp: `installed.tmp.<id>` + `renameat` RELATIF (relatif →
  tetap CONTINUE → child kerjakan sendiri → jalan). Bila masking tak sengaja
  sukses, fd ditutup dan dipaksa `-EISDIR` juga (deterministik).
- `openat2`: `how.flags` ikut di-mask (jalur ini praktis mati di Android —
  openat2 kena SIGSYS eksternal, HANDOFF §6.1 — tapi ditutup tetap).
- **`proc_self_fix()`**: syscall yang DIEKSEKUSI SUPERVISOR melihat
  `/proc/self` sebagai dirinya, bukan child. Diterjemahkan ke
  `/proc/<pid-child>` utk C_OPEN/C_STAT/C_STATX/C_STATFS/C_READLINK/C_SIDE;
  C_EXEC/C_CHDIR dikecualikan (child yang menjalankan → `/proc/self` benar).
  Dipanggil SETELAH `cache_store` dan tidak pernah di-cache (kunci cache =
  path asli; pid beda per notifikasi). `rewrite2()` dipakai utk path kedua
  rename/link/linkat; target `symlink(at)` sengaja TIDAK (isi link bukan
  path yang di-resolve sekarang).
- **Bonus fix (bug laten fd-passing)**: buffer cmsg `sendmsg/recvmsg`
  listener diperlebar ke `CMSG_SPACE(sizeof(int))` via union ter-align;
  buffer lama `sizeof(struct cmsghdr)+int` memicu MSG_CTRUNC di glibc x86_64
  (fd listener hilang → "tak dapat listener"). Di aarch64/musl kebetulan
  pas, tapi bentuk lama tetap salah secara spesifikasi.

**Verifikasi sandbox (x86_64, seccomp USER_NOTIF sungguhan)** — arsitektur
filter di-swap ke x86 hanya utk test (repo tetap aarch64):

```
1. openat(dfd, ".", O_RDWR|O_TMPFILE|O_CLOEXEC, 0644) -> EISDIR   OK
2. fallback installed.tmp + renameat relatif (CONTINUE)           OK
3. open(O_RDONLY|O_DIRECTORY) murni tak ter-mask                  OK
4. O_TMPFILE path absolut juga ter-mask                           OK
5. readlink("/proc/self/exe") supervisor -> /proc/<pid>/exe       OK
```

### 18.2 Bug §17.1 — "1 error" silent + trigger rc=127

Ada DUA penyebab independen yang saling menumpuk:

**(a) Flag basi `f:`/`s:` yang dipersist di database** (sumber satu-satunya
yang sepenuhnya SILENT setelah semua jalur lain dieleminasi):

- `commit.c:484-485`: `r = change->old_pkg && (old_pkg->ipkg->broken_files
  || broken_script); ... errors += r;` — dihitung tanpa pesan bila
  `print_change()` (`commit.c:56-101`) return false (versi sama, bukan
  reinstall, tag repo sama).
- Flag itu field `f:` di DB: parse `database.c:1029-1030` (`f`=broken_files,
  `s`=broken_script, `x`=broken_xattr, `S`=sha256-160), tulis
  `database.c:1130-1141`, hanya reset saat reinstall nyata
  (`database.c:3234-3235`). Kegagalan ekstraksi/script di era sebelum fix
  §16 meninggalkan `f`/`s` → tiap transaksi berikutnya "1 error" selamanya.
- Jalur lain yang dianggap tapi TIDAK silent (coret): write-config,
  `num_dir_update_errors` (apk_warn), trigger/script failure (apk_err),
  log redirection (`print.c:261-282` menulis ke console DAN log).

**Obat**: script baru `./apk-doctor` (root repo):
- tanpa arg → lapor paket ber-flag `f`/`s` (beserta `P:` pemiliknya);
- `--clear-broken` → tulis ulang `installed`, field `f:` hanya menyimpan
  `x`/`S` (baris dibuang bila kosong), backup `.bak-doctor.<pid>`;
- alternatif resmi: `apk fix --reinstall <paket>`.

**(b) env-merge kalah melawan `clearenv()`** (penyebab rc=127 replika
trigger `env -i APK_SCRIPT=... <trigger> /bin`):

- `fk_ensure_wadah_env` lama menggabungkan **`environ` LIVE**. busybox
  `env -i` memanggil `clearenv()` → musl men-set `environ=NULL` → tak ada
  yang bisa digabung → child tanpa LD_PRELOAD/PATH → 127 "not found".
- Fix: **snapshot deep-copy `environ` di constructor** (`fk_snapshot_env`);
  merge memakai `environ` live bila masih ada, jatuh ke snapshot. envp
  tetap menang per-kunci; envp boleh NULL.
- Celah interposer juga ditutup: ditambahkan `posix_spawn`,
  `posix_spawnp` (musl menjalankan pointer `__execve` internal dari
  clone(CLONE_VM|CLONE_VFORK) — TIDAK lewat PLT, tak bisa di-intercept dari
  execve; loader dirangkai langsung di interposer), `fexecve`, `execveat`
  (termasuk AT_EMPTY_PATH via procfd), `execle`, `execlp`.

**Verifikasi sandbox (LD_PRELOAD x86_64)**:

```
1. open(O_RDONLY|O_DIRECTORY) tak ter-mask FK_FLAGS baru     OK
2. O_TMPFILE -> EISDIR                                       OK
3. clearenv() + execvp script shebang -> LD_PRELOAD &
   FAKE_BASE tersuntik ulang dari snapshot (exit 0)          OK
4. clearenv() + posix_spawn/p ("testprint printenv") ->
   child melihat LD_PRELOAD & FAKE_BASE, rc=0                OK
```

### 18.3 Catatan desain yang JANGAN diubah

- Mask O_TMPFILE harus kondisional `(f & O_TMPFILE) == O_TMPFILE` — masking
  tanpa syarat ikut mencabut O_DIRECTORY (O_TMPFILE = __O_TMPFILE|O_DIRECTORY).
- `proc_self_fix` tidak boleh untuk C_EXEC/C_CHDIR (child yang mengeksekusi;
  `/proc/self` justru benar) dan tidak boleh masuk rewrite_cache.
- Jangan mem-fake `unshare`/`mount`/`uid_map`: `context.c:74-82` otomatis
  men-set APK_NO_CHROOT saat `--root=/` (jalur shim), dan `--usermode`
  native apk (autodetect `st_uid != 0` di `database.c:2035-2044`) sudah
  menonaktifkan chown/xattr/devices.

### 18.4 Yang BELUM bisa diverifikasi di sandbox (jujur)

- Build musl/aarch64 (`libfakeroot.so`, `svsp` statis) — tak ada toolchain
  cross di sandbox; build tetap via perintah di HANDOFF §7/§13 di device.
- Perilaku nyata apk 3.0.8 di Termux (trigger busybox, tulis DB, rename
  atomik antar layer) — wajib uji device (checklist §18.5).
- `apk-doctor` baru dites thd DB tiruan; DB asli punya field lebih lengkap
  (parser hanya menyentuh baris `f:`/`P:` jadi aman, tapi konfirmasi di device).

### 18.5 Checklist verifikasi device (belum dilakukan)

```bash
cd ~/Brainstorming && git pull   # branch arena/a172be3f-brainstorming
./install.sh                     # rebuild shim+svsp sesuai resep §7/§13

# A. Bersihkan flag basi peninggalan era pra-fix (bug 17.1a):
./apk-doctor                     # lapor
./apk-doctor --clear-broken      # bersihkan (backup otomatis)

# B. Bebas "1 error" (bug 17.1):
fake-run apk add --no-cache hello && echo RC=$?     # harus rc=0, tanpa "error"
fake-run apk del hello && echo RC=$?                # rc=0
fake-run apk add busybox                            # ulang utk paket ber-trigger

# C. svsp DB write (bug 17.2):
fake-run --svsp apk add --no-cache hello            # TANPA "failed to write database"
SVSP_DEBUG=1 fake-run --svsp apk add --no-cache acl 2>&1 | grep -i tmpfile
#   (terlihat "O_TMPFILE di-mask ... path=[.]")

# D. Trigger script jalan dgn env minimal (17.1b):
env -i APK_SCRIPT=trigger APK_PACKAGE=busybox \
  fake-run $BASE/lib/apk/db/scripts.tar/busybox.post-install /bin
#   → rc=0 (dulu 127)

# E. Device-2 (sisa §17.3): git pull && ./install.sh; cek resolv.conf wadah
grep -c nameserver $BASE/etc/resolv.conf            # harap 4 NS + options

# F. Bersih-bersih (sisa §17.4): rm $BASE/tmp/*.apk, hello scripts, dg-dyn2
```

Jika A–D lulus tapi masih ada "1 error": jalankan
`fake-run apk add --simulate -v <pkg>` dan `strace`-equivalent via
`FK_DEBUG=1 fake-run apk add ...` lalu bandingkan field `f:` sebelum/sesudah
dengan `./apk-doctor`.

## 19. ✅ Hasil verifikasi device (feedback agent lokal 2026-10-07) + fix ronde 2

Feedback lengkap: `device-feedback/2026-10-07.md` (device-1, Infinix X6855,
Android 16, kernel 6.12, apk-tools 3.0.8, clang 21.1.8).

### 19.1 Yang TERKONFIRMASI di device

- **§18.2a terbukti rantai penuh**: DB awal bersih → satu trigger gagal di
  jalur svsp menulis `f:s` → transaksi berikutnya "1 error" SILENT walau
  `--no-scripts` → `apk-doctor --clear-broken` → transaksi bersih rc=0.
- **§17.2 beres**: log SVSP_DEBUG menampilkan `O_TMPFILE di-mask path=[.]`;
  TIDAK ada lagi "failed to write database" di seluruh sesi.
- **§17.1 jalur shim beres**: `apk add/del` (paket pengganti `acl`, soalnya
  `hello` tak ada di Alpine v3.24) rc=0 tanpa error; trigger busybox jalan.
- `install.sh` + resep build §7/§13 akurat; resolv.conf wadah 4 NS + options.
- svsp stabil utk paket besar (`ca-certificates openssl`, 2 detik, tanpa hang).

### 19.2 Bug baru yang ditemukan agent lokal + fix ronde 2

1. **Trigger svsp rc=127** (`* execve: No such file or directory`): kernel
   host me-resolve `#!/bin/busybox sh` terhadap root host. Fix:
   **shebang-in-container di svsp** (`shebang_wrap()` di svsp.c): saat C_EXEC
   mengenai script wadah ber-interpreter absolut wadah, supervisor menulis
   ulang file itu (child terblok saat notifikasi → bebas race) jadi wrapper
   `#!/system/bin/sh` yang meng-exec interpreter wadah lewat loader musl
   patched; isi asli dipindah ke `<file>.orig-svsp`; idempoten (marker);
   pasangan unlink dibersihkan di C_SIDE + oleh `apk-doctor --clear-broken`.
   Ini juga menutup kasus `ca-certificates` trigger (`CANNOT LINK /bin/sh`).
   Override: `SVSP_HOST_SH` (default `/system/bin/sh`), `SVSP_LOADER`
   (default `$BASE/lib/ld-musl-patched.so.1`; bila tak ada, exec langsung).
   Catatan: execve path RELATIF yang tak berubah kini tetap masuk handler
   (dulu CONTINUE langsung) tapi melewati write_mem — string `.rodata` aman.
2. **`passthrough()` svsp diperluas**: `/system /apex /vendor /product
   /linkerconfig /data` wajib passthrough — tanpa itu wrapper host-sh dan
   linker bionic (buka `/system/lib64/...`) ikut ter-rewrite dan rusak.
3. **Regresi `fake-run` SBARGS** (dari commit ce108b9):
   `interp="$(shebang_parse ...)"` → subshell membuang array SBARGS → arg
   shebang hilang ("applet not found"). Fix: hasil lewat `SHEBANG_INTERP`,
   tanpa `$(...)`.
4. **`fake-run` + `env -i`**: `set -u` mati karena `HOME`/`PREFIX` tak ada.
   Fix: `: "${PREFIX:=/data/data/com.termux/files/usr}"`, `: "${HOME:=...}"`.
   Plus pass-through `SVSP_DEBUG`/`FK_DEBUG` eksplisit menembus `env -i`
   (tadi harus lewat argumen `fake-run --svsp SVSP_DEBUG=1 apk ...`).
5. Koreksi resep §18.5 (B/C/D): paket contoh `hello` → **`acl`** (hello tak
   ada di v3.24); debug via argumen, bukan prefix env; path trigger §18.5 D
   keliru — file trigger hidup sementara di `$BASE/lib/apk/exec/<nama>`
   saat eksekusi, bukan `lib/apk/db/scripts.tar/...`.
6. Catatan kecil: lock apk bisa lengket (RC=99 EAGAIN, ulang saja); backup
   `installed.bak-doctor.*` aman dihapus kapan pun.

### 19.3 Verifikasi sandbox ronde 2 (x86_64; device = ronde berikutnya)

```
shebang wrap: execve relatif "lib/apk/exec/t.trigger" (#! /bin/busybox sh)
  -> wrapper -> busybox sh t.trigger.orig-svsp /bin      argv utuh, rc=0
wrapper idempoten (run ke-2 tanpa rewrite ulang)         OK
unlink trigger -> .orig-svsp ikut terhapus               OK
fake-run --svsp script shebang -> SBARGS utuh (argv[1]=sh) OK
env -i tanpa HOME/PREFIX -> fake-run jalan (dulu unbound)  OK
apk-doctor --clear-broken bersihkan *.orig-svsp yatim      OK
```

### 19.4 Checklist device RONDE 2 (agent lokal, lihat AGENT-BRIEF-DEVICE.md)

```bash
cd ~/Brainstorming && git pull            # branch arena/a172be3f-brainstorming
./install.sh

# 1. Trigger svsp kini harus rc=0 (inti fix §19.2.1):
fake-run --svsp apk del --no-scripts attr acl busybox 2>/dev/null  # bila ada
./apk-doctor --clear-broken              # pastikan mulai dari DB bersih
fake-run --svsp apk add --no-cache acl; echo RC=$?      # HARUS rc=0
fake-run --svsp apk add --no-cache ca-certificates openssl; echo RC=$?
ls $BASE/lib/apk/exec/                   # harap kosong (trigger dibersihkan apk+svsp)

# 2. "1 error" tidak kembali: transaksi lanjutan (dengan trigger!) rc=0:
fake-run apk add busybox; echo RC=$?
./apk-doctor                              # harap "bersih"

# 3. Regresi fake-run:
fake-run /bin/busybox sh -c 'echo SBARGS-ok'            # jalur biasa
env -i APK_SCRIPT=trigger APK_PACKAGE=busybox bash fake-run --ldpreload \
  $BASE/lib/apk/exec/<trigger> /bin 2>/dev/null || true  # hanya bila file ada

# 4. Laporkan ke device-feedback/<tanggal>-r2.md: RC tiap langkah, isi
#    lib/apk/exec, output ./apk-doctor, dan sisa pesan aneh apa pun.
```

## 20. Feedback ronde 2 → fix ronde 3 (`'bin/busybox' is not an absolute path`)

Feedback: `device-feedback/ronde2.md`. Kemajuan besar: wrapper svsp benar
(tangkapan wrapper di device persis desain), `ca-certificates` trigger bebas
`CANNOT LINK`, `lib/apk/exec/` bersih, SBARGS/env-i beres. Sisa: `--svsp apk
add acl` masih rc=1 — `busybox: 'bin/busybox' is not an absolute path`.

### 20.1 Root cause (dibuktikan dari sumber busybox + test sandbox)

1. busybox `--install` (`libbb/appletlib.c:880`): `xmalloc_readlink(
   "/proc/self/exe")` dulu; **hanya bila readlink GAGAL** jatuh ke `argv[0]`
   dan mati bila argv[0] tak absolut.
2. exec `/bin/busybox` dari script trigger: string path cuma 11 byte,
   rewrite `$BASE/bin/busybox` (45) tak muat → fallback C_EXEC menulis
   `"bin/busybox"` relatif (jalan, cwd=base) — tapi **buffer path = buffer
   argv[0]** (ash/sh memakai string sama) → argv[0] ikut jadi relatif.
3. Di device, readlink `/proc/self/exe` GAGAL sehingga fallback argv[0]
   dipakai. Kenapa gagal: sejak ronde 1 svsp MEMAKSA supervisor menangani
   `/proc/self/*` (`proc_self_fix` → supervisor membaca `/proc/<pid>/exe`
   antar-proses; akses lintas-proses ke `/proc/pid/exe` butuh izin ala
   ptrace yang dibatasi sebagian Android/SELinux).

### 20.2 Fix ronde 3

- **svsp: `/proc/self/*` TIDAK lagi dipaksa ke supervisor.** Path `/proc/...`
  passthrough (changed=0) → CONTINUE → **child membaca dirinya sendiri** —
  selalu diizinkan kernel dan selalu benar. `proc_self_fix` kini hanya
  diterapkan bila supervisor benar-benar mengeksekusi (changed=1). Dengan
  ini busybox mendapat path absolut dari readlink sendiri → argv[0] relatif
  tak lagi dipersoalkan. (Test sandbox: exec fallback menghasilkan argv[0]
  relatif + readlink-self absolut — persis kondisi device, dan busybox akan
  memilih hasil readlink.)
- **`SYS_unlink` di-guard `#ifdef`** di blok pembersihan `.orig-svsp`
  (arm64 tak punya SYS_unlink; patch lokal agent lokal ronde 2 kini resmi,
  silakan buang patch lokalnya saat `git pull`).
- **`fake-run`: default `HOME` = `/data/data/com.termux/files/home`**
  (sebelumnya `$PREFIX/home` — tak ada di device).
- **Shim (`libfakeroot.c`): passthrough prefix host** `/data /system /apex
  /vendor /product /linkerconfig /dev /proc /sys` di `fk_rewrite` — konsisten
  dgn svsp §19.2.2; artefak host (mis. file di `/data/.../tmp`) kini bisa
  diakses program wadah. `/dev /proc /sys` sebelumnya via symlink `$BASE/*`,
  hasil akhir sama.

### 20.3 Verifikasi sandbox ronde 3

```
exec fallback "/bin/fbb" (tak muat): argv[0]=relatif, readlink-self absolut OK
O_TMPFILE suite (EISDIR + fallback renameat + O_DIRECTORY murni)   OK
wrap chain shebang (argv[1]=sh utuh)                               OK
fake-run --svsp script shebang SBARGS                              OK
shim: O_DIRECTORY murni, O_TMPFILE→EISDIR, /data passthrough,
      env-merge clearenv                                           OK
```

### 20.4 Checklist device RONDE 3 (kriteria lulus = butir 1 rc=0 murni)

```bash
cd ~/Brainstorming && git pull && git status     # pastikan patch lokal SYS_unlink
                                                 # dibuang/ditimpa (sudah di hulu)
./install.sh

./apk-doctor --clear-broken
fake-run --svsp apk add --no-cache acl; echo RC=$?           # HARUS rc=0
fake-run --svsp apk add --no-cache ca-certificates openssl; echo RC=$?  # rc=0
fake-run apk add busybox; echo RC=$?                          # rc=0
./apk-doctor                                                  # "bersih"
fake-run --svsp apk add --no-cache attr; echo RC=$?           # transaksi lanjutan rc=0
ls $BASE/lib/apk/exec/                                        # kosong
# ekstra (shim /data passthrough):
fake-run /bin/busybox test -e /data/data/com.termux/files/home && echo PASSTHROUGH-OK
# isi device-feedback/ronde3.md (template di repo), commit + push.
```

## 21. Strategi pragmatis: shim = jalur utama apk (keputusan)

Sesudah 3 ronde device, pembagiannya tegas:

| Jalur | Status device | Pakai untuk |
|---|---|---|
| `fake-run apk ...` (shim LD_PRELOAD) | **HIJAU PENUH** — add/del/trigger/`fix --reinstall` rc=0 (ronde 1-2) | **semua pemakaian apk sehari-hari** |
| `fake-run --svsp ...` | DB write beres; shebang wrapper ada; verifikasi trigger menunggu | biner STATIS (musl statis tak bisa LD_PRELOAD) & eksperimen |

Konsekuensi:
- Kriteria "selesai" utk kebutuhan praktis = jalur shim hijau (sudah tercapai
  di device). Paritas svsp diperlakukan sebagai penyempurnaan, bukan blocker.
- Verifikasi device dipangkas jadi SATU perintah: **`./selftest`**
  (baris `[PASS]/[FAIL]`, exit code = jumlah gagal, cleanup otomatis).
  Mode `--diagnose` = read-only (artefak + DB + resolv.conf). Hasil utk
  feedback: `./selftest | tee device-feedback/selftest-$(date +%F).txt`.
- **Plan C** (bila trigger svsp tetap gagal di device): hybrid —
  `fake-run --svsp apk` otomatis `--no-scripts` (tulis DB via svsp, terbukti
  beres) lalu trigger dijalankan ulang via jalur shim. Belum diimplementasi;
  hanya bila diperlukan.

### 21.1 Keputusan: TETAP Alpine, jangan pindah distro

Analisis (pertanyaan user 2026-10-07): dari ~6 kelas bug yang ditemui, 4-5
bersumber dari batasan Android tanpa root (interpreter ELF, shebang script
paket, chown tanpa root, seccomp) dan akan muncul lagi di distro APA PUN.
Pindah ke Debian/glibc malah menambah beban: loader glibc jauh lebih sulit
di-patch daripada musl, plus permukaan baru (dpkg lock, maintainer scripts).
Yang pindah distro hanya menghapus bug khusus apk (O_TMPFILE publish, silent
flag) — keduanya sudah diatasi (`SVSP_MASK_TMPFILE` + `apk-doctor`). Untuk
kebutuhan tool harian murni, `pkg` bawaan Termux tetap paling realistis.

## 22. Ronde 3: koreksi root-cause + fix `script_absolutize` (argv[0] absolut)

Feedback: `device-feedback/ronde3.md` + 2× `selftest`. **Koreksi penting dari
agent lokal atas §20.1** (diterima, bukti kuat):

- busybox Alpine 1.37.0-r31 build ini membaca
  `readlink(bb_busybox_exec_path)` dengan `bb_busybox_exec_path =
  "/bin/busybox"` (bukan `/proc/self/exe` — `strings` & log SVSP_DEBUG
  `nr=78 [/bin/busybox]` membuktikan). `/bin/busybox` wadah = **file
  reguler** → readlink = EINVAL **selalu**, dengan/tanpa fix §20.2.
- Yang menentukan = **fallback argv[0]**; di rantai wrapper argv[0] =
  `bin/busybox` relatif (buffer 13 byte tak muat path host 60 byte) → mati.
  Top-level `fake-run --svsp /bin/busybox --install -s` rc=0 (argv[0]
  absolut) memperkuat bukti.
- Fix §20.2 (/proc/self CONTINUE) tetap benar & bekerja, tapi untuk jalur
  yang tak dipakai build ini.

### 22.1 Fix ronde 4: `script_absolutize()` di `shebang_wrap()`

Saat wrap, isi `.orig-svsp` di-rewrite: tiap token path-absolut wadah yang
menunjuk **file reguler + X_OK** di `$BASE` diganti jadi path base penuh
(`/bin/busybox` → `$BASE/bin/busybox`). Efeknya `sh` wadah mengalokasikan
buffer sesuai panjang sumber (60 byte) → rewrite C_EXEC pas → **argv[0]
absolut** → busybox `--install` (dan program lain yang peduli argv[0])
jalan. Generik utk semua script, bukan cuma busybox. Batas: file ≤1 MB,
≤64 token unik, terpanjang dulu; token prefix host & `$BASE/...` dilewati;
stat mengikuti symlink (`$BASE/bin/sh`→busybox ikut cocok). Catatan jujur:
string data dalam script yang kebetulan == path biner ikut terganti (langka).

Verifikasi sandbox (repro persis kegagalan device):
```
trigger "#!/bin/busybox sh" + baris "/bin/busybox --install -s"
  -> .orig-svsp berisi "$BASE/bin/busybox" (shebang & command)     OK
  -> rantai wrapper -> sh -> busybox: argv[0] absolut, --install OK OK
  -> idempoten (run ke-2)                                          OK
  -> O_TMPFILE suite + fallback renameat + O_DIRECTORY murni       OK
```

### 22.2 Fix bug selftest (temuan 2a/2b agent lokal)

- File sementara tak lagi hardcode `/tmp` (tak tertulis di Termux) →
  `mktemp "${TMPDIR:-$PREFIX/tmp}/..."` fallback `$BASE/tmp`.
- Kriteria svsp tak lagi vakum: `apk del --no-scripts $PKG` disisipkan
  antara step shim dan svsp sehingga `--svsp apk add $PKG` transaksi nyata
  (trigger jalan).

### 22.3 Keputusan V4 `/system` (temuan 4)

Passthrough `/system` (§20.2) **dipertahankan** (butuh utk host-sh wrapper
svsp & konsistensi). Konsekuensi: metadata `/system/*` terlihat (stat OK),
isi tetap terlindungi izin Android (cat EACCES utk file root). `install.sh`
quick-test kini menerima ENOENT **atau** EACCES sebagai "terisolasi".
Baris historis §8 V4 ("ENOENT") = keadaan pra-§20.2, tak perlu diubah.

### 22.4 Dicatat apa adanya (tanpa aksi)

- RC=139 (SIGSEGV) sekali di probe agent lokal, tak berulang di 4 percobaan;
  tak cukup bukti menyebut bug svsp. Dipantau di ronde berikut.
- busybox ash wadah tak mendukung `${PIPESTATUS[0]}` (bash-ism) — gunakan
  `sh -c 'cmd; echo $?'` di script wadah.

### 22.5 Checklist RONDE 4 = `./selftest` versi baru (sudah mencakup semua)

```bash
cd ~/Brainstorming && git pull && ./install.sh
./selftest | tee device-feedback/selftest-$(date +%F).txt
```
Kriteria lulus: **0 FAIL** — khususnya `svsp: apk add $PKG rc=0` kini
transaksi nyata. Bila masih ada FAIL, sertakan output penuh + `SVSP_DEBUG=1
fake-run --svsp apk add --no-cache acl 2>&1 | tail -40`.

## 23. ✅ PENUTUPAN RESMI — bug §17 selesai (ronde 4, device: 0 FAIL)

### 23.1 Hasil ronde 4 (device-feedback `a60513b`)

`./selftest` versi §22.5 dijalankan di device-1 (Android 16, kernel 6.12):

```
== RINGKASAN: 0 FAIL ==
```

Semua 17 check PASS, termasuk dua kriteria penentu yang gagal di ronde
sebelumnya:
- `svsp: apk add acl rc=0` — **transaksi NYATA** (paket di-purge dulu oleh
  selftest versi baru; trigger busybox tereksekusi via wrapper svsp).
- `apk-doctor: DB bersih` — rantai flag basi `f:s` tak lagi terbentuk karena
  trigger svsp kini sukses.

Verifikasi independen agent lokal di luar selftest (commit message):
`fake-run --svsp apk add --no-cache acl` transaksi nyata → **rc=0**
(`script_absolutize` bekerja: argv[0] absolut → `busybox --install` jalan).

### 23.2 Peta lengkap fix §17 (untuk arsip)

| Bug | Root cause akhir | Fix | Verifikasi |
|---|---|---|---|
| "1 error" silent (§17.1) | flag `f:`/`s:` basi dipersist DB (`commit.c:484`) | `apk-doctor --clear-broken` + trigger tak lagi gagal | device ronde 1-4 |
| trigger rc=127 (§17.1b) | env-merge pakai `environ` live (habis di-`clearenv`) + interposer bolong | snapshot env constructor; posix_spawn(p)/fexecve/execveat/execle/execlp | device ronde 1 |
| svsp "failed to write database" (§17.2) | O_TMPFILE path relatif lolos → publish linkat butuh CAP_DAC_READ_SEARCH | mask O_TMPFILE + pin dirfd child + EISDIR → fallback `.tmp` | device ronde 1 |
| svsp trigger ENOENT (§19) | kernel host resolve shebang ke root host | `shebang_wrap()` wrapper `#!/system/bin/sh` | device ronde 2 |
| `CANNOT LINK /bin/sh` (§19) | passthrough prefix host kurang | `/system /apex /vendor ...` | device ronde 2 |
| `'bin/busybox' not absolute` (§20-22) | busybox readlink(`/bin/busybox`) selalu EINVAL → fallback argv[0]; buffer 13 byte → fallback relatif | `script_absolutize()` di `.orig-svsp` | device ronde 4 |
| Regresi SBARGS fake-run | subshell `$(shebang_parse)` | `SHEBANG_INTERP` | device ronde 2 |
| `env -i` unbound HOME/PREFIX | `set -u` | default eksplisit | device ronde 2-3 |

### 23.3 Yang TETAP terbuka (di luar jangkauan agent)

1. **Verifikasi device-2** (§17 butir 3) — butuh device fisik kedua; agent
   lokal hanya punya satu device. Bila device-2 tersedia: `git pull &&
   ./install.sh && ./selftest`.
2. Backup `installed.bak-doctor.*` di `$BASE/lib/apk/db/` — aman dihapus
   kapan saja (hanya arsip perbaikan flag).
3. RC=139 tunggal di probe agent (ronde 3) — tak berulang; tak ada aksi.

### 23.4 Arah berikutnya (bila lanjut)

- ARENA-REPLY §7(a) rootless DB write via `linkat(AT_EMPTY_PATH)`: nilai
  tambahnya kecil menurut bukti device (O_TMPFILE mask sudah menutup gejala).
- Pembersihan lanjutan / paket tambahan wadah; fitur baru dari pemilik.
- Keputusan distro: TETAP Alpine (§21.1).

## 24. KASUS BARU — OpenCode (Bun) spawn child gagal di wadah

Laporan pemilik (2026-10-08), matriks gejala di device:

```
opencode --help          ✅
opencode serve (manual)  ✅
OpenCode spawn serve     ❌
opencode --standalone    ❌
```

Hipotesis pemilik: child yang dibuat Bun mewarisi "environment custom"
(loader/library Alpine + libfakeroot.so). **Analisis arena:** di jalur svsp
env justru sudah disanitasi (`svsp` me-`unsetenv` LD_* sebelum exec target;
fake-run svsp tak men-set LD_PRELOAD), jadi warisan env tak merusak binary
statis. Tersangka sebenarnya ada di translasi path syscall child.

### 24.1 Dua mode kegagalan yang DIBUKTIKAN di sandbox (svsp x86, repro C statis
meniru pola Zig/Bun: fork + execve syscall mentah)

1. **Memori read-only (.rodata)** — Zig/Bun mengoper path `execve` sebagai
   literal compile-time → `process_vm_writev` ditolak → svsp menjawab
   `-ENOENT` → spawn gagal padahal file ada. **FIX:** fallback `write_mem`
   via `pwrite` ke `/proc/<pid>/mem` (hak ptrace sama; menembus proteksi
   halaman — terbukti: uji `PWRITE-RODATA-OK`). Kasus repro C: dulu exit 127
   errno=2 → kini CHILD-OK.
2. **Asumsi cwd=base pada fallback path-relatif** — fallback `pbuf+1`
   (C_EXEC) hanya benar selama cwd child = base. Begitu app chdir (Bun ke
   direktori proyek; chdir passthrough `/data/...` pun memindahkan cwd),
   execve relatif salah resolve. Ditemukan juga: **C_CHDIR absolut tak
   pernah muat** (rewrite selalu +baselen) → selama ini silent no-op
   (cwd terpaku di base — "kebetulan" menyelamatkan fallback lama, tapi
   membohongi app). **FIX:** helper `rel_from_cwd()` — hitung path relatif
   dari cwd AKTUAL child (`readlink /proc/<pid>/cwd`) ke target; dipakai di
   C_EXEC dan C_CHDIR sebelum fallback lama. Uji D (chdir `/root/proj` kini
   NYATA + execve dari cwd baru) → CHILD-OK.

Regresi build baru: rantai wrap ronde-4 (`BUSYBOX-INSTALL-OK argv[0]`
absolut, idempoten) + execve relatif = tetap hijau. Cabang O_TMPFILE tak
disentuh diff ini (diverifikasi ulang via selftest `apk add` transaksi nyata
di device).

### 24.2 Yang BELUM pasti (butuh bukti device)

- Pesan error pasti dari spawn yang gagal (belum pernah dikirim).
- Jalur mana yang dipakai menjalankan opencode: bila lewat jalur
  LD_PRELOAD/loader patched (bukan svsp), binary STATIS Bun lolos dari
  interposer entirely (syscall mentah) → child execve sampai ke kernel
  dengan path wadah → ENOENT. Fix untuk varian ini = jalankan via svsp
  (`fake-run --svsp` / auto).
- Bun juga bisa memakai buffer terlalu kecil untuk `../` relatif — bila
  masih gagal, log SVSP_DEBUG akan menunjukkan "muat tak cukup".

### 24.3 Briefing device = RONDE 5 (AGENT-BRIEF-DEVICE.md)

Rebuild svsp (`./install.sh`), ulangi matriks opencode, tangkap
`SVSP_DEBUG=1` bila masih ada yang ❌.

## 25. RONDE 5 — dua akar masalah OpenCode DITEMUKAN & DIPERBAIKI

Feedback device `device-feedback/ronde5.md` (basis `a0f575a`) sangat menentukan:
fix §24 benar tapi **bukan** penyebab kegagalan device. Matriks device: `--help`
dan `serve` manual ✅ di ketiga jalur; `spawn serve` + `--standalone` ❌ di svsp
(HTTP 500) dan ❌ di shim (loader). Dua bug berbeda:

### 25.1 svsp — O_PATH tak bisa lewat ADDFD (→ EACCES palsu)

Device: `[svsp] ADDFD nr=56 -> fd=-1 errno=9`, dijawab `-EACCES` buta
(`svsp.c:885`). Log server OpenCode: `PermissionDenied: FileSystem.realPath (/)
... EACCES: permission denied, lstat '/'` → `/api/fs/list` & `/api/location`
HTTP 500 → TUI mati.

Sebab (direproduksi 1:1 **di sandbox x86**, jadi ini batasan kernel umum, bukan
khas Android): `SECCOMP_IOCTL_NOTIF_ADDFD` memakai `fget(srcfd)` di kernel, dan
`fget()` **menolak file `FMODE_PATH`**. Jadi setiap `open(..., O_PATH)` yang
path-nya di-rewrite (dikerjakan supervisor lalu dikirim via ADDFD) pasti gagal
`EBADF` → child dapat EACCES. Open O_PATH passthrough (`/data/...`) lolos karena
dijalankan child sendiri — persis pola terbalik yang dilaporkan device.

**FIX:** supervisor membuka tanpa `O_PATH` (`O_RDONLY`, flag lain dipertahankan)
→ fd biasa lolos ADDFD, dan tetap sah untuk `fstat`, `readlink /proc/self/fd/N`,
serta sebagai `dirfd` `openat()`. Plus: `ADDFD` gagal kini membalas **errno asli**
(bukan `-EACCES` buta yang menyamarkan diagnosis).

Bukti sandbox sebelum→sesudah: `O_PATH file/dir//` EACCES(13) → OK.

**Batas yang diakui (diuji, belum terpecahkan):** degradasi ini tak menolong
2 sudut — `O_PATH|O_NOFOLLOW` pada **simlink** (jadi ELOOP) dan file **tanpa izin
baca** bagi supervisor (jadi EACCES). Reopen via `/proc/self/fd` sudah dicoba dan
gagal (objeknya sendiri tak bisa dibuka O_RDONLY). Bila OpenCode menyentuh kasus
itu, akan muncul di ronde 6.

### 25.2 shim — `/proc/self/exe` menunjuk LOADER (→ "cannot load serve")

Device: `ld-musl-patched.so.1: cannot load serve: No such file or directory`.
Analisis: di jalur ldpreload kita menjalankan `loader /path/opencode args...`,
sehingga `/proc/self/exe` proses itu = **loader**. Bun memakai `execPath`
(= `/proc/self/exe`) untuk men-spawn dirinya sendiri → `posix_spawn(loader,
["ld-musl-patched.so.1","serve",...])` → loader menganggap `serve` sebagai nama
program → error persis di atas. Keluarga bug §20.1 (`/proc/self/*`), varian shim.

**FIX (libfakeroot.c):**
1. Saat merangkai exec/spawn lewat loader, wariskan `FAKEROOT_EXE=<hostpath>`.
2. `readlink("/proc/self/exe")` dan `/proc/<pid-sendiri>/exe` menjawab
   `FAKEROOT_EXE` bila ada (verifikasi sandbox: `exe=/opt/alpine/root/.opencode/
   bin/opencode`, dan tanpa env tetap apa adanya).
3. Jaring pengaman `fk_fix_loader_argv()`: bila ada yang meng-exec/spawn **loader
   langsung** dengan `argv[1]` yang bukan path absolut, sisipkan kembali program
   sebenarnya.

### 25.3 Temuan device lain yang saya terima

- **bootstrap.sh hard-abort** gara-gara 52 file `.codex` mode 0555 (read-only) →
  `pinterp` EACCES. Device menambal manual dgn `chmod u+w`. **Belum saya ubah di
  kode** — saran device (chmod u+w dalam loop pinterp, atau skip+warn) saya setujui
  dan masuk antrean ronde 6.
- **Path di brief saya salah**: binary ada di `$BASE/root/.opencode/bin/opencode`
  (dinamis musl, v2.0.24), bukan `usr/local/bin`. Hipotesis "statis" saya **gugur** —
  device benar, PT_INTERP ada.
- **"Hang" spawn = konflik port 49374** dengan service opencode HOST, bukan bug
  wadah. Natif + port bebas = spawn sukses penuh.

## 26. RONDE 6 — svsp TUNTAS; shim diperbaiki di 3 lapis

`device-feedback/ronde6.md` (basis `ca64cb4`): **10/12 ✅**. Fix O_PATH §25.1
**TERVERIFIKASI di perangkat**: matriks svsp 4/4 ✅ (kasus 3 & 4 yang dulu HTTP 500
kini TUI render + anak benar-benar spawn, port 49474 000→200), repro O_PATH
`open(O_PATH)('/')` EACCES → **OK fd=3**. Device juga menangkap konfound sendiri
(pass-1 shim/svsp menyambung ke daemon natif) dan mengulang dgn keadaan bersih —
kualitas data ronde ini tinggi.

**Kesimpulan praktis: OpenCode jalan penuh di dalam wadah lewat jalur svsp.**

### 26.1 Mengapa fix shim §25.2 belum menggigit (diagnosis device benar)

Device: `FAKEROOT_EXE` **kosong di env anak**, dan log bun menunjukkan
`command=<loader> args=["serve","--stdio","--port","0"]`. Sebabnya ada tiga dan
semuanya saya tambal sekarang:

1. **`fake-run` tak pernah men-set `FAKEROOT_EXE`.** Jalur ldpreload meng-exec
   `env -i ... "$LOADER" "$host" ...`, jadi proses induk sendiri tak punya env itu
   (§25.2 hanya mewariskannya untuk anak yang dirangkai libfakeroot). **FIX:**
   `FAKEROOT_EXE="$host"` ditambahkan ke env `fake-run`.
2. **Bun membaca `/proc/self/exe` lewat syscall mentah** (Zig `selfExePath`), bukan
   libc → interposer `readlink` memang tak pernah kena. Dugaan device **tepat**.
   Maka perbaikan tak boleh bergantung pada interposisi baca; andalannya adalah
   jaring pengaman di sisi spawn.
3. **`fk_self_exe()` tak punya sumber cadangan.** **FIX:** bila `FAKEROOT_EXE`
   kosong, baca `/proc/self/exe`; bila hasilnya = loader, ambil
   `/proc/self/cmdline[1]` (= program sebenarnya saat dijalankan `loader prog ...`).

Dgn (1)+(3), `fk_fix_loader_argv()` punya bahan untuk menyisipkan ulang program
saat Bun men-spawn `loader ["serve",...]`.

**Verifikasi sandbox (harness meniru device):** program yang membaca execPath via
**syscall mentah** lalu `posix_spawn` ke loader:
- tanpa shim → `ld-musl-patched.so.1: cannot load serve: No such file or directory`
  (error device tereproduksi persis);
- dgn shim → `LOADER OK -> program=/tmp/B/app/opencode argumen: serve --stdio`.
Fallback `cmdline[1]`: tanpa `FAKEROOT_EXE`, `readlink(/proc/self/exe)` dari dalam
shim mengembalikan program, bukan loader.

Catatan: ini **belum** verifikasi perangkat. Bun bisa saja memakai jalur spawn lain
(`fork`+`execve` mentah dari Zig) yang tak lewat PLT — bila ronde 7 masih gagal,
jawabannya adalah "pakai jalur svsp" (sudah 4/4 ✅), bukan menambah tambalan shim.

### 26.2 Sudut §25.1 — satu PECAH, satu batas arsitektural

- **File tanpa izin baca (mode 000): PECAH.** O_PATH asli tak memeriksa izin baca,
  degradasi O_RDONLY memeriksa. Karena di konteks fake-root supervisor adalah
  pemilik berkas: pinjam bit `S_IRUSR` sebentar → buka → **kembalikan mode semula**
  (hanya bila bukan simlink & `st_uid == geteuid()`). Uji sandbox: svsp
  `O_PATH mode 000` EACCES → **OK**, dan `stat` sesudahnya tetap `0`.
- **Simlink + `O_PATH|O_NOFOLLOW`: TETAP ELOOP — batas arsitektural.** Kernel
  `fget()` menolak fd `FMODE_PATH` di ADDFD, dan tak ada fd non-O_PATH yang mewakili
  simlink itu sendiri. Tidak ada tambalan jujur; didokumentasikan apa adanya.

### 26.3 Lain-lain

- DBG `ADDFD` tak lagi mencetak errno basi pada baris sukses (`errno=0` sebelum ioctl)
  — persis catatan kosmetik device.
- **bootstrap.sh 0555 masih belum diperbaiki** (ronde 5 §1). Tidak terpicu di ronde 6
  hanya karena `chmod u+w` ronde 5 persisten. Tetap di antrean.

## 27. RONDE 7 — shim tinggal 1 bug; realpath namespace + utang bootstrap dibayar

`device-feedback/ronde7.md` (basis `b9d1cdb`): **11/12 ✅**. Terverifikasi di
perangkat: `FAKEROOT_EXE` ter-set (env anak & env daemon), jaring pengaman spawn
bekerja (`command=` kini **program asli**, `cannot load serve` HILANG, kasus 4 shim
✅), `O_PATH` mode-000 OK, DBG `errno=0` (tak basi lagi), simlink O_NOFOLLOW tetap
ELOOP sesuai ekspektasi. svsp tetap 4/4 ✅.

### 27.1 Sisa: shim kasus 3 — `LocationNotFoundError: Location not found: /root`

Device menunjukkan server anak **sehat** (daemon cmdline = program asli, HOME &
`FAKEROOT_EXE` benar, port 200) tapi **klien TUI** mati. Dugaan device ("ada
terjemahan path yang menghasilkan `/root` telanjang") **tepat**; sumbernya
ketemu di `libfakeroot.c`:

`realpath()` dulu **selalu** melepas prefix `$BASE` dari hasil. Maka
`realpath("$BASE/root")` — dan `HOME` memang di-set `fake-run` ke path HOST —
mengembalikan `/root`. Klien Bun lalu memakai hasil itu lewat **syscall mentah**
(Zig tak lewat libc, jadi tak ter-interposisi) → kernel host tak kenal `/root`
→ `LocationNotFoundError`. Jalur natif tak terkena (tanpa shim, tanpa strip);
svsp tak terkena (path wadah memang sah di sana).

**FIX:** jawab di **namespace yang ditanyakan**. Bila pemanggil bertanya dengan
path HOST (`$BASE/...`), kembalikan path HOST; hanya bila ia bertanya dengan path
wadah, prefix dilepas seperti dulu. Ini mempertahankan ilusi fake-chroot untuk
apk dkk. (tak ada perubahan perilaku untuk input `/etc`, `/`) sekaligus berhenti
memberi path wadah kepada pemanggil yang jelas-jelas bekerja di namespace host.

Uji sandbox: `realpath($B/root)` → `$B/root` (dulu `/root`); `realpath(/etc)` →
`/etc`; `realpath(/)` → `/`. `getcwd()` **tidak** diubah (ilusi cwd tetap).

Catatan jujur: ini belum diverifikasi perangkat, dan Bun bisa saja menurunkan
lokasi dari sumber lain (`$HOME` mentah, `getpwuid`). Bila ronde 8 masih gagal,
rekomendasi tetap: **pakai jalur svsp** (4/4 ✅ sejak ronde 6).

### 27.2 Utang bootstrap 0555 DIBAYAR

`bootstrap.sh` `rewrite_interp()`: bila `pinterp` gagal I/O (rc=2) karena berkas
read-only (mis. 52 berkas `.codex` 0555 di ronde 5 yang dulu mematikan seluruh
bootstrap), kini **pinjam bit tulis sebentar → ulangi → pulihkan mode semula**,
dan hanya dianggap gagal bila tetap tak bisa. Device tak perlu lagi `chmod u+w`
manual.

## 28. RONDE 8 — getcwd silang-namespace (kandidat terakhir shim kasus 3)

`device-feedback/ronde8.md` (basis `56bfa20`): **11/12, stagnan**. Dua hasil:

1. **Utang bootstrap 0555 LUNAS & terverifikasi**: device sengaja men-`chmod a-w`
   dua ELF `.codex` → `install.sh` RC=0, `PT_INTERP 24 file, 0 gagal`, dan mode
   dikembalikan persis (555→555, 444→444). Ronde 5 (hard-abort) tak terulang.
2. **Kasus 3 shim masih `LocationNotFoundError: /root`** — error identik. Device
   membuktikan mekanisme §27 sendiri benar (`realpath $HOME`→host, `realpath
   /root`→`/root`, `realpath .`→host) sehingga `/root` **datang dari jalur lain**,
   dan menemukan kandidatnya:

```
fake-run /bin/sh -c 'cd /root && pwd -P && readlink /proc/self/cwd'
/root                          <- pwd -P (getcwd, di-interposisi) = NAMESPACE WADAH
/data/.../alpine-rootfs/root   <- kernel cwd                      = HOST
```

Jadi satu proses yang sama melihat dua namespace. §27 sengaja tidak menyentuh
`getcwd` — keputusan itu ternyata **salah** untuk pemakai syscall mentah: Bun
mengambil lokasi proyek dari getcwd (libc, ter-interposisi → `/root`) lalu
men-stat-nya lewat syscall mentah (tak ter-interposisi → kernel host tak kenal)
→ ENOENT → `LocationNotFoundError`.

**FIX §28:** `getcwd()` kini menjawab **kebenaran host**, konsisten dgn realpath
§27 (prinsip: shim tak boleh menyilangkan namespace di dalam satu proses, karena
kita tak bisa menjamin semua pembaca lewat libc). Escape hatch
**`FAKE_VIEW=container`** memulihkan tampilan lama bila ada yang bergantung padanya.

Uji sandbox: default → `getcwd=realpath(.)=kernel cwd=$B/root`;
`FAKE_VIEW=container` → `getcwd=/root` (perilaku lama) sementara kernel tetap host.

**Risiko yang saya akui:** ini mengubah tampilan cwd untuk SEMUA pemakai jalur
shim (apk, busybox, prompt shell). Karena itu brief ronde 9 meminta **`./selftest`
penuh (17 tes)**, bukan hanya matriks opencode. Bila ada regresi apk, jalan
mundurnya satu env: `FAKE_VIEW=container` di `fake-run`.

Catatan: bila kasus 3 shim TETAP gagal sesudah ini, saya berhenti menambal shim
untuk Bun — jalur **svsp sudah 4/4 sejak ronde 6** dan itu jawaban yang sah
untuk tugas "OpenCode jalan di dalam wadah".

## 29. RONDE 9 — ✅ TUGAS OPENCODE SELESAI (matriks 12/12, selftest 0 FAIL)

`device-feedback/ronde9.md` (basis `c5f9138`) + `device-feedback/selftest-ronde9.txt`:

| Sasaran | Hasil |
|---|---|
| Matriks opencode 4 kasus × 3 jalur | ✅ **12/12 — pertama kali full hijau** |
| `./selftest` penuh (17 tes) | ✅ **0 FAIL** — tak ada regresi apk/busybox dari §28 |
| Kasus 3 shim (`LocationNotFoundError`) | ✅ **TERATASI** — TUI render, 0 baris error, 000→200 |
| Regresi svsp | ✅ 4/4 (stabil sejak ronde 6) |

Hipotesis getcwd ronde 8 dari device **terbukti**: menyilangkan namespace di
dalam satu proses adalah akar bug terakhir. Risiko regresi yang saya khawatirkan
di §28 **tidak terjadi** (selftest 0 FAIL), jadi `FAKE_VIEW=container` tetap
sekadar escape hatch, bukan default.

### 29.1 Perjalanan bug OpenCode (ringkas, untuk arsip)

| Ronde | Temuan | Perbaikan |
|---|---|---|
| 5 | svsp: `ADDFD errno=9` dibalas `-EACCES` buta; shim: `cannot load serve` | — (diagnosis) |
| 6 | O_PATH tak bisa lewat ADDFD (kernel `fget()` tolak `FMODE_PATH`) | §25.1 buka tanpa O_PATH; errno asli |
| 7 | `FAKEROOT_EXE` tak pernah di-set `fake-run`; Bun baca `/proc/self/exe` via syscall mentah | §26 env + fallback `cmdline[1]` + jaring pengaman spawn |
| 8 | `realpath` selalu melepas prefix base | §27 jawab di namespace yang ditanyakan |
| 9 | `getcwd` silang-namespace (`pwd -P`=/root vs kernel=host) | §28 getcwd = kebenaran host |

Sudut yang **sengaja dibiarkan** (batas arsitektural, bukan kelalaian):
`O_PATH|O_NOFOLLOW` pada simlink → ELOOP di svsp. Tak ada fd non-`O_PATH` yang
mewakili simlink dan ADDFD menolak `FMODE_PATH`.

### 29.2 Sisa kecil yang diperbaiki ronde ini

- **`FAKE_VIEW` tak lolos `env -i`** (temuan device §4): hatch §28 senyap tak
  berefek lewat `fake-run`. Kini diteruskan eksplisit seperti `SVSP_DEBUG`/`FK_DEBUG`.
- **`pwd` logis masih `/root`** setelah `cd /root` eksplisit: itu `$PWD` yang
  di-set shell dari argumen yang diketik, **bukan** hasil `getcwd` — di luar
  jangkauan shim dan tanpa dampak (kasus 3 ✅). Dicatat, tidak ditambal.

### 29.3 Status proyek

Dua tugas besar selesai: **§17 bug wadah (ronde 4, selftest 0 FAIL)** dan
**OpenCode di dalam wadah (ronde 9, 12/12 + selftest 0 FAIL)**. Ketiga jalur
(natif, svsp, shim) kini menjalankan OpenCode penuh termasuk spawn anak dan
`--standalone`.

## 30. RONDE 11 — `FAKEROOT_EXE` basi pada pemakaian nested (bug laten §26)

Ronde 10 menutup brief dengan semua hijau (hatch `FAKE_VIEW` hidup, selftest
0 FAIL, standalone svsp+shim render). **Ronde 11 datang tanpa brief**: device
mereproduksi error dari sesi pemakaian NYATA (user masuk shell wadah lalu
mengetik `opencode`):

```
Starting background server...
Error: Server process exited with code 2
$B/bin/sh: can't open 'serve': No such file or directory
```

### 30.1 Akar masalah (device membuktikan dgn kontrol A/B)

`fake-run` men-set `FAKEROOT_EXE=$B/bin/sh` untuk **shell**-nya (benar untuk
proses itu). Shell lalu menjalankan `opencode`, yang **mewarisi env itu apa
adanya**. `fk_self_exe()` versi §26 mempercayai env lebih dulu → opencode
mengira dirinya `/bin/sh` → `process.execPath` salah → spawn
`command=sh args=["serve",...]` → `sh` mencari skrip bernama `serve` → exit 2.
Jaring pengaman §26 diam karena `command` bukan loader.

Kontrol A/B device: `FAKEROOT_EXE=` (dikosongkan) → TUI render ✅;
`FAKEROOT_EXE=$B/bin/sh` (bawaan) → gagal ❌. Satu variabel, kausal.

**Kenapa matriks 12/12 tak menangkapnya:** semua tes brief menjalankan
`fake-run $OC` **langsung** (env = opencode, kebetulan benar). Pola alami
"masuk shell → ketik opencode" tak pernah ada di brief mana pun. Ini **bug
laten sejak §26**, bukan regresi ronde 10 — dan murni kelemahan desain tes saya.

### 30.2 FIX §30 — kebenaran per-proses dulu, env cadangan terakhir

Kandidat #2 device saya ambil (paling bersih). Urutan `fk_self_exe()` sekarang:

1. `readlink /proc/self/exe`; bila **bukan** loader → itulah program ini (env
   diabaikan — inilah yang mematikan nilai basi);
2. bila exe == loader → `/proc/self/cmdline[1]` (pola "loader prog args");
3. barulah env `FAKEROOT_EXE` (hanya bila exe tak terbaca).

Hasilnya juga **ditulis balik** ke env (`setenv`), jadi keturunan mewarisi nilai
segar, bukan basi.

Uji sandbox (4 skenario, semua benar):

| Skenario | Hasil |
|---|---|
| env basi `bin/sh` + exec langsung (bug device) | self-exe = **opencode** ✅ (dulu `bin/sh`) |
| exe == loader, tanpa env | cmdline[1] = opencode ✅ (fallback §26 utuh) |
| exe == loader + env basi | opencode ✅ — kebenaran proses menang |
| exec langsung tanpa env | opencode ✅ |

Dan `FAKEROOT_EXE` yang diteruskan ke anak ikut terkoreksi di semua skenario.

### 30.3 Pelajaran untuk penulisan brief

Matriks "12/12" hanya menguji jalur peluncuran langsung. Brief ronde 12 karena
itu menambahkan **jalur pemakaian manusia**: masuk `alpine` interaktif →
ketik `opencode`, plus nested dua tingkat. Workaround device (`FAKEROOT_EXE=
opencode`) tak diperlukan lagi bila fix ini benar.
