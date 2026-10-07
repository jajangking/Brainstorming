# ARENA-REPLY — balasan untuk arena.ai

**Kepada:** arena.ai (batch `c7d6ccb0-brainstorming`)
**Dari:** jajangking (review + penyelesaian di perangkat nyata)
**Tanggal:** 2026-10-07 | Branch: `main` (`282e3ca`), kerja arena ada di `2b135d3` (merge `--no-ff`, authorship dipertahankan)

Terima kasih atas batch pengerasan 6.1–6.5. Semua sudah di-review, diperbaiki,
diverifikasi **empiris di perangkat Android arm64 ini** (bukan strace/teori), dan
di-push ke `main`. Verdict lengkap + log di `HANDOFF.md` (§6, §10).

---

## 1. Yang arena benar — diterima apa adanya

- `libfakeroot.c`: `execl()` argc selalu 64 (UB) → scan NULL terminator; `getwd()` int-truncation → `char *`; intercept `statx()` — semua betul, terbukti jalan.
- `fake-run` `${extra_env[@]+...}` under `set -u` — betul.
- `segcshim.c` debug conditional + `block-trap.c` `#include <errno.h>` — betul.
- `examples/ebench.c` — bagus, dipakai.

## 2. Cacat pada batch arena yang sudah saya perbaiki

1. **`svsp.c` tidak build:** `struct open_how` redefined (bionic & musl-dua-duanya punya header). Fix: `__has_include(<linux/openat2.h>)` + fallback manual.
2. **rc `svsp` selalu 1:** `while (waitpid(-1, WNOHANG))` **me-reap target** dan kehilangan status exit. Fix: poll-timeout `waitpid(pid,&st,WNOHANG)==pid → tst`; fallback `waitpid(pid,&st,0)`; `WEXITSTATUS`/`128+WTERMSIG`. Terbukti: `exit 7 → 7`, `exit 42 → 42`.
3. **Cache 256B memotong path:** → buffer 4096 + skip simpan path ≥ 4095.

## 3. Temuan TERPENTING dari uji daya tahan (6.5) — bug laten arena & saya

- **`path_argidx()` default 0 → EFAULT masif:** `renameat`/`renameat2`/`linkat` membaca **dirfd sebagai pointer path** → process_vm_readv "Bad address" → **SEMUA komit rename/link apk gagal**. Baru terlihat saat `apk add` nyata. Fix: `path_argidx` = 1 untuk renameat/renameat2/linkat, 1 untuk symlink, 2 untuk symlinkat (rename/link tetap 0). Ini temuan paling berharga dari seluruh review.
- Ditambah harden: `read_string()` + `open_how` dibaca **page-bounded 4K** (cegah overread-EFAULT di tepi mapping).

## 4. Hasil di perangkat (semua hijau)

| Item | Status |
|---|---|
| V1 dinamis / V2 `--svsp` / V3 boot-static / V4 isolasi `/system/build.prop` ENOENT | ✅ rc=0 |
| gobukti (Go statis) — rewrite + isolasi + walk `/etc` | ✅ rc=0 |
| rc target `exit 7`/`exit 42` | ✅ 7 / 42 |
| eptest (epoll) | ✅ `epoll_ctl = 0` |
| **6.5(a) `apk add --no-scripts ncurses`** | ✅ 3/3 paket (19 pkg, 9447 KiB, symlink benar); `tput cols` → `80` rc=0; `infocmp xterm` baca terminfo wadah |
| **6.5(b) Claude Code** | ✅ `--version`/`--help` rc=0; **TUI terbuka** di wadah; config terisolasi di `$R/root/.claude` (host `~/.claude` tak tersentuh) |
| ebench (jujur) | bare **1.1–1.6 μs/op**; under-svsp **51–470 μs/op** (dominan termal; cache membuat rewrite ulang lebih murah di run hangat) |

## 5. Yang TIDAK bisa diverifikasi (jujur, bukan ditutup-tutupi)

- **6.1 `openat2`:** kode benar & masuk, tetapi **tak dapat diverifikasi on-device** —
  seccomp EKSTERNAL Android membunuh `openat2` dengan SIGSYS **sebelum** USER_NOTIF
  svsp melihatnya. Bukti `examples/o2test.c`: proses ber-`openat2` mati `st=31`,
  **nol** notif `nr=437` di `SVSP_DEBUG=1`.

## 6. Limitasi yang TERBUKTI bukan bug kode ini (kontrol ilmiah)

1. **db apk EPERM** (linkat `AT_EMPTY_PATH` butuh `CAP_DAC_READ_SEARCH`) — kontrol
   `apk --root` vanila gagal identik → limitasi apk-tools rootless.
2. **Skrip trigger shebang `#!/bin/sh`** → kernel exec bionic `/system/bin/sh` yang tak
   bisa link di child non-zygote → wadah ini **musl-only**; pakai `--no-scripts`.

## 7. Untuk arena.ai kalau dipanggil lagi

Tidak ada item tersisa di 6.1–6.5. Kalau mau kontribusi lanjutan, prioritas yang jujur
bernilai: (a) membuat `apk db` berjalan rootless (mis. wrap `linkat AT_EMPTY_PATH` →
fallback `rename`), (b) interpreter shebang dalam-wadah (mis. binfmt-misc style wrapper
agar skrip `.trigger` apk jalan tanpa `--no-scripts`), (c) `LD_LIBRARY_PATH`/`PATH` sudah
dibereskan — jangan di-revert. Regresi wajib tetap: V1–V4 + gobukti + claude (lihat §8 HANDOFF).

— jajangking