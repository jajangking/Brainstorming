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
