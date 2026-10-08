# Brainstorming — Linux Native di Android (Tanpa Root, Tanpa Proot)

Jalankan binary Linux (Claude Code, busybox, Alpine packages, program Go/C) secara **native** di Android lewat Termux — tanpa root, tanpa proot, **native speed**.

## ⚡ Install Satu Perintah

```bash
pkg install git && git clone https://github.com/jajangking/Brainstorming && cd Brainstorming && ./install.sh
```

Atau tanpa clone:
```bash
curl -sL https://raw.githubusercontent.com/jajangking/Brainstorming/main/install.sh | bash
```

**Hasil:** `fake-run <program>` — jalankan binary Linux apapun di wadah Alpine palsu.

## 📦 Isi Wadah (otomatis, saat install)

Fresh install sudah brings working toolchain — tak perlu `apk add` manual:

| Paket | Kenapa perlu |
|---|---|
| `libstdc++` + `libgcc` | binary C++ pihak-ketiga (opencode, bun) butuh `libstdc++.so.6`; minirootfs tidak menyertakan |
| `curl`, `bash`, `git` | peralatan standar (bawaan wadah cuma busybox `ash`) |
| Node.js / Python / ripgrep | bisa dipasang sendiri lewat `apk` atau tool manager (Hermes Agent, dsb) |

Kalau binary besar (>64 MB, mis. opencode ~195 MB) terunduh **setelah** bootstrap,
jalankan `./bootstrap.sh` sekali lagi (idempoten) agar PT_INTERP-nya ikut di-rewrite
— batas sekarang 512 MB. Untuk biner yang di-*exec* dari parent statis, supervisor
sudah menanganinya otomatis (§43).

## 🚀 Cara Pakai

```bash
alpine                                # masuk shell Alpine (kayak proot-distro login)
alpine cat /etc/alpine-release        # jalankan command
alpine apk add vim                    # install package
alpine -c "ls /etc"                   # jalankan command tanpa masuk shell

# Atau pakai fake-run langsung:
fake-run cat /etc/alpine-release      # → 3.24.2
fake-run sh                           # masuk shell
fake-run --svsp <program>             # paksa supervisor (binary statis)
```

> **Tips performa:** `fake-run` per-panggilan punya ongkos setup (~0,1 dtk). Untuk
> kerja beruntun, masuk sekali (`alpine`) lalu kerjakan di dalam — spawn berikutnya
> jauh lebih murah.

### Contoh: Jalankan Claude Code di Android

```bash
# Download Claude Code (arm64 musl)
mkdir -p ~/musl-test && cd ~/musl-test
curl -LO https://github.com/anthropics/claude-code/releases/latest/download/claude-code-linux-arm64-musl.tgz
tar xzf claude-code-linux-arm64-musl.tgz

# Jalankan di wadah
fake-run ~/musl-test/package/claude --version
# → 2.1.291 (Claude Code)

# Config otomatis terisolasi di ~/alpine-rootfs/root/.claude
```

### Contoh: Hermes Agent di dalam wadah

Installer Hermes (Python + Node + uv) pernah gagal 8× karena 4 bug distro
(§40/§42/§43/§44) — semuanya sudah diperbaiki. Sekarang berhasil penuh:

```bash
alpine                                                   # masuk wadah
curl -fsSL https://hermes-agent.nousresearch.com/install.sh -o /root/h.sh
bash /root/h.sh --non-interactive                        # (~10 menit, unduh toolchain)
export PATH="$PATH:/root/.hermes/hermes-agent/.hermes/bin" # sudah ada di .bashrc
hermes --version                                         # v0.21.6
hermes setup                                             # pairing/token (interaktif)
```

### Contoh: opencode di dalam wadah

```bash
alpine
# taruh binary opencode (musl) di /root/.opencode/bin/, lalu:
/root/.opencode/bin/opencode          # TUI, config terisolasi di root/.config/opencode
```

## 📁 Path Penting

| Apa | Path |
|---|---|
| **Masuk shell** | `alpine` (atau `fake-run sh`) |
| Wadah Alpine | `~/alpine-rootfs/` |
| Runner universal | `$PREFIX/bin/fake-run` |
| Supervisor | `$PREFIX/bin/svsp` |
| Shim LD_PRELOAD | `~/libfakeroot.so` |
| Loader patched | `~/alpine-rootfs/lib/ld-musl-patched.so.1` |
| Wrapper loader otomatis (§43) | `~/alpine-rootfs/.svsp-elfwrap/` |
| Config Claude | `~/alpine-rootfs/root/.claude` |
| Config opencode | `~/alpine-rootfs/root/.config/opencode/service.json` (port 49474) |

## ❓ Apa Bedanya dengan proot-distro / Termux?

| | proot-distro | Termux | Brainstorming |
|---|---|---|---|
| **Masuk shell** | `proot-distro login alpine` | shell Termux | `alpine` |
| **Mekanisme** | ptrace (setiap syscall ditranslasi parent) | bionic asli | loader dipatch + shim in-process + supervisor seccomp |
| **Kecepatan syscall** | 🐢 5–20× lebih lambat | 🚀 native | 🚀 1–470 μs/op |
| **Root?** | Tidak | Tidak | Tidak |
| **Binary Linux musl** | Jalan (lambat) | ❌ bukan ELF musl | ✅ Shim (dinamis) / supervisor (statis) |
| **glibc (apt, Debian)** | ✅ rootfs penuh | ✅ repo `termux-glibc` | ❌ musl only |
| **Isolasi host FS** | Semu (ptrace) | Tidak ada (host = userland) | Ya (path rewrite; `/proc`,`/dev`,`/sys` passthrough) |
| **Kematangan** | bertahun-tahun | matang | muda — bug baru masih mungkin (§40–§44 born device 2026-10-08) |

Pilihan cepat: butuh glibc/apt berat → proot; mau tool Termux asli → Termux;
mau userland Linux + binary Node/Python/Claude/opencode tanpa root → Brainstorming.

## 🔧 Uninstall

```bash
cd ~/Brainstorming && ./uninstall.sh
```

## 📚 Dokumentasi Lengkap

| File | Isi |
|---|---|
| `HANDOFF.md` | Status mutakhir, checklist, gotchas kritis, resep build |
| `BRAINSTORM.md` | Peta kemungkinan/mustahil + argumen teknis |
| `ARENA-REPLY.md` | Verdict review + hasil uji device |
| `device-feedback/` | Log uji device per ronde (ronde 19 = Hermes Agent sukses) |

## 🏗️ Arsitektur

```
┌─────────────────────────────────────────────────────┐
│                    Android (aarch64)                 │
│                                                     │
│  ┌───────────────┐    ┌──────────────────────────┐  │
│  │  Binary        │    │  Binary Statis           │  │
│  │  Dinamis       │    │  (Go, C tanpa loader)    │  │
│  │  (musl/glibc)  │    │                          │  │
│  └───────┬───────┘    └────────────┬─────────────┘  │
│          │                        │                 │
│          ▼                        ▼                 │
│  ┌───────────────┐    ┌──────────────────────────┐  │
│  │  LD_PRELOAD    │    │  svsp (seccomp           │  │
│  │  libfakeroot   │    │  USER_NOTIF supervisor)  │  │
│  │  .so           │    │                          │  │
│  └───────┬───────┘    └────────────┬─────────────┘  │
│          │                        │                 │
│          ▼                        ▼                 │
│  ┌──────────────────────────────────────────────┐   │
│  │  Path Rewrite: /foo → ~/alpine-rootfs/foo    │   │
│  └──────────────────────────────────────────────┘   │
│          │                                          │
│          ▼                                          │
│  ┌──────────────────────────────────────────────┐   │
│  │  ~/alpine-rootfs/ (Alpine 3.24.2 aarch64)    │   │
│  │  /bin  /usr  /etc  /lib  /proc  /dev  /sys   │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

## 📊 Benchmark

Overhead per syscall (µs/op):

| Skenario | μs/op |
|---|---|
| Bare (tanpa supervisor) | **1.1–1.6** |
| Under svsp (passthrough CONTINUE) | **~50** |
| Under svsp (rewrite ADDFD) | **~470** |

Terukur di device (Termux aarch64, spawn 50× `true`):

| Skenario | Total | Per-spawn |
|---|---|---|
| Host Termux (`/usr/bin/true`) | 0,91 dtk | ~18 ms |
| `fake-run busybox true` (jalur shim) | 6,05 dtk | ~121 ms |
| `fake-run --svsp busybox true` | 13,32 dtk | ~266 ms |

Ongkos itu **per pemanggilan `fake-run`** (deteksi ELF + siapkan env + exec).
Kalau sudah di dalam shell wadah, spawn berikutnya tidak lewat biaya ini —
karena itu `alpine` (shell) lebih murah untuk kerja beruntun.

## ✅ Yang Sudah Terbukti

- [x] Claude Code (`--version`, `--help`, TUI) — config terisolasi
- [x] `apk add ncurses` — 3 paket terinstall, `tput` jalan
- [x] Program Go statis (`gobukti`) — rewrite + isolasi + walk directory
- [x] Binary C statis (`boot-static`) — baca `/etc/alpine-release`
- [x] Isolasi: `/system/build.prop` → ENOENT
- [x] Exit code propagation (`exit 7` → 7, `exit 42` → 42)
- [x] `openat2` support (kode masuk, tak bisa verif on-device)
- [x] Node.js 26 musl (`-e`, event-loop I/O, `npm install --offline`) di wadah
- [x] opencode dua jalur (shim + `--svsp`; binary jumbo butuh rewrite INTERP)
- [x] **Hermes Agent v0.21.6** installer penuh (`rc=0`): uv, node, npm, python,
      venv, deps, config, build produk — lihat `device-feedback/ronde19.md`
- [x] **Fresh-install dari nol** (wadah terpisah via `bootstrap.sh --base=`):
      V1–V5 hijau + §40 (bash supervisor), §42 (node), §43 (ELF stock-INTERP
      dari parent statis), §44 (rename abs→rel) semua lolos

## ⚠️ Limitasi (Bukan Bug)

- `chroot`/`mount`/namespace sungguhan → **permanen mustahil** (seccomp AND-min)
- glibc (apt/Debian) → repo ini musl-only; butuh `proot-distro`
- `openat2` → seccomp eksternal Android SIGSYS-kill sebelum supervisor
- `faccessat2`/`io_uring_setup` → trap Android SEBELUM notif svsp; ditangani
  handler SIGSYS in-process milik shim (§34/§35, §42: io_uring dijawab fd
  /dev/null agar libuv mundur anggun ke epoll — Node.js 26 jalan)
- Binary baru ber-INTERP stock: sejak §43 supervisor otomatis me-redirect lewat
  wrapper loader (`~/.svsp-elfwrap/`), jadi tak perlu patch manual. Yang perlu
  `bootstrap.sh` ulang hanya file **>512 MB** (scan PT_INTERP).
- **Hardlink** (`ln` tanpa `-s`) → EACCES: restriksi SELinux Android untuk
  untrusted_app (terbukti juga di host Termux, bukan isu wadah)
- `apk db` commit → EPERM (`linkat AT_EMPTY_PATH` butuh CAP root)
- Skrip trigger apk → shebang `/bin/sh` tak bisa link di child (musl-only)

## 🤝 Kontribusi

```bash
git clone https://github.com/jajangking/Brainstorming
cd Brainstorming
# Baca HANDOFF.md untuk konteks lengkap
```

---

**License:** MIT | **Platform:** Termux (Android aarch64) | **Alpine:** 3.24.2