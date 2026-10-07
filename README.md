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

## 📁 Path Penting

| Apa | Path |
|---|---|
| **Masuk shell** | `alpine` (atau `fake-run sh`) |
| Wadah Alpine | `~/alpine-rootfs/` |
| Runner universal | `$PREFIX/bin/fake-run` |
| Supervisor | `$PREFIX/bin/svsp` |
| Shim LD_PRELOAD | `~/libfakeroot.so` |
| Config Claude | `~/alpine-rootfs/root/.claude` |

## ❓ Apa Bedanya dengan proot-distro?

| | proot-distro | Brainstorming |
|---|---|---|
| **Masuk shell** | `proot-distro login alpine` | `alpine` |
| **Kecepatan** | 🐢 Lambat (ptrace) | 🚀 **Native** (1-470 μs/op) |
| **Root?** | Tidak | Tidak |
| **Cara kerja** | Emulator syscall | Fake chroot + seccomp supervisor |
| **Binary statis Go?** | Jalan | Jalan (otomatis pakai supervisor) |
| **Install size** | Besar | Kecil |

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

| Skenario | μs/op |
|---|---|
| Bare (tanpa supervisor) | **1.1–1.6** |
| Under svsp (passthrough CONTINUE) | **~50** |
| Under svsp (rewrite ADDFD) | **~470** |

Overhead wajar untuk isolasi path tanpa root/proot.

## ✅ Yang Sudah Terbukti

- [x] Claude Code (`--version`, `--help`, TUI) — config terisolasi
- [x] `apk add ncurses` — 3 paket terinstall, `tput` jalan
- [x] Program Go statis (`gobukti`) — rewrite + isolasi + walk directory
- [x] Binary C statis (`boot-static`) — baca `/etc/alpine-release`
- [x] Isolasi: `/system/build.prop` → ENOENT
- [x] Exit code propagation (`exit 7` → 7, `exit 42` → 42)
- [x] `openat2` support (kode masuk, tak bisa verif on-device)

## ⚠️ Limitasi (Bukan Bug)

- `chroot`/`mount`/namespace sungguhan → **permanen mustahil** (seccomp AND-min)
- `openat2` → seccomp eksternal Android SIGSYS-kill sebelum supervisor
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