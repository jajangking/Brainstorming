#!/data/data/com.termux/files/usr/bin/bash
#
# install.sh — Satu perintah install Brainstorming di Termux baru
#
# Pakai:
#   curl -sL https://raw.githubusercontent.com/jajangking/Brainstorming/main/install.sh | bash
#   ATAU
#   git clone https://github.com/jajangking/Brainstorming && cd Brainstorming && ./install.sh
#
# Apa yang dilakukan:
#   1. Cek environment (Termux, architecture)
#   2. Install dependencies (clang, binutils, patchelf, curl)
#   3. Clone repo (atau pakai yang sudah ada)
#   4. Jalankan bootstrap.sh (download Alpine, build, install)
#   5. Verifikasi semua jalan
#
# Hasil: `fake-run <program>` — jalankan binary Linux di Android, tanpa root, native speed

set -euo pipefail

# ========== Warna ==========
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
CYAN='\033[1;36m'
NC='\033[0m'

log()  { printf "${GREEN}[✓] %s${NC}\n" "$*"; }
warn() { printf "${YELLOW}[!] %s${NC}\n" "$*"; }
err()  { printf "${RED}[✗] %s${NC}\n" "$*" >&2; }
info() { printf "${BLUE}[i] %s${NC}\n" "$*"; }
step() { printf "\n${CYAN}━━━ %s ━━━${NC}\n" "$*"; }

die() { err "$*"; exit 1; }

# ========== Banner ==========
cat << 'BANNER'

    ╔═══════════════════════════════════════════════════════╗
    ║                                                       ║
    ║   ██████╗ ██████╗  █████╗ ██╗███╗   ██╗              ║
    ║   ██╔══██╗██╔══██╗██╔══██╗██║████╗  ██║              ║
    ║   ██████╔╝██████╔╝███████║██║██╔██╗ ██║              ║
    ║   ██╔══██╗██╔══██╗██╔══██║██║██║╚██╗██║              ║
    ║   ██████╔╝██║  ██║██║  ██║██║██║ ╚████║              ║
    ║   ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═╝  ╚═══╝              ║
    ║                                                       ║
    ║   Brainstorming — Linux Native di Android             ║
    ║   Tanpa Root • Tanpa Proot • Native Speed             ║
    ║                                                       ║
    ╚═══════════════════════════════════════════════════════╝

BANNER

# ========== Cek Environment ==========
step "Cek Environment"

# Cek Termux
if [ -z "${TERMUX_VERSION:-}" ] && [ ! -d "/data/data/com.termux" ]; then
    die "Ini bukan Termux! Install Termux dulu dari F-Droid: https://f-droid.org/packages/com.termux/"
fi
log "Termux terdeteksi"

# Cek architecture
ARCH=$(uname -m)
if [ "$ARCH" != "aarch64" ] && [ "$ARCH" != "arm64" ]; then
    die "Architecture $ARCH tidak didukung. Butuh aarch64/arm64"
fi
log "Architecture: $ARCH"

# Cek Android
if [ -f "/system/build.prop" ]; then
    log "Android terdeteksi"
else
    warn "Bukan Android? Mungkin di proot/chroot, hasil bisa beda"
fi

# ========== Install Dependencies ==========
step "Install Dependencies"

info "Update package list..."
pkg update -y 2>/dev/null || true

DEPS="clang binutils patchelf curl"
for dep in $DEPS; do
    if command -v "$dep" >/dev/null 2>&1; then
        log "$dep sudah ada"
    else
        info "Install $dep..."
        pkg install -y "$dep" || die "Gagal install $dep"
        log "$dep terinstall"
    fi
done

# ========== Clone/Pilih Repo ==========
step "Setup Repository"

REPO_URL="https://github.com/jajangking/Brainstorming.git"
INSTALL_DIR="$HOME/Brainstorming"

if [ -d "$INSTALL_DIR/.git" ]; then
    log "Repo sudah ada di $INSTALL_DIR"
    cd "$INSTALL_DIR"
    info "Pull update terbaru..."
    git pull --rebase 2>/dev/null || warn "Pull gagal, pakai yang ada"
else
    info "Clone repo..."
    git clone "$REPO_URL" "$INSTALL_DIR" || die "Gagal clone repo"
    cd "$INSTALL_DIR"
    log "Repo terclone ke $INSTALL_DIR"
fi

# ========== Jalankan Bootstrap ==========
step "Install Brainstorming (bootstrap.sh)"

info "Ini butuh beberapa menit (download Alpine + build)..."
info "Duduk manis, biarkan berjalan ☕"
echo

./bootstrap.sh || die "Bootstrap gagal! Cek error di atas"

# ========== Verifikasi ==========
step "Verifikasi Instalasi"

BASE="$HOME/alpine-rootfs"
FR="$PREFIX/bin/fake-run"

# Cek binary terinstall
if [ ! -x "$FR" ]; then
    die "fake-run tidak terinstall di $FR"
fi
log "fake-run terinstall"

if [ ! -x "$PREFIX/bin/svsp" ]; then
    die "svsp tidak terinstall"
fi
log "svsp terinstall"

if [ ! -f "$HOME/libfakeroot.so" ]; then
    die "libfakeroot.so tidak ada"
fi
log "libfakeroot.so terinstall"

if [ ! -f "$BASE/lib/ld-musl-patched.so.1" ]; then
    die "Loader patched tidak ada"
fi
log "Loader patched OK"

# Quick test
info "Quick test..."
RESULT=$(env -u LD_PRELOAD "$FR" --base="$BASE" cat /etc/alpine-release 2>/dev/null || echo "GAGAL")
if echo "$RESULT" | grep "3.24.2" >/dev/null; then
    log "Test dinamis: $RESULT"
else
    die "Test gagal! Hasil: $RESULT"
fi

# Isolation test
ISO_RESULT=$(env -u LD_PRELOAD "$FR" --base="$BASE" cat /system/build.prop 2>&1 || true)
if echo "$ISO_RESULT" | grep -i "enoent\|no such" >/dev/null; then
    log "Isolasi: /system/build.prop → ENOENT (benar!)"
else
    warn "Isolasi: hasil tidak terduga: $ISO_RESULT"
fi

# ========== Selesai ==========
step "Instalasi Selesai! 🎉"

cat << EOF

${GREEN}Brainstorming sudah terinstall dan siap dipakai!${NC}

${CYAN}Cara pakai:${NC}
  fake-run cat /etc/alpine-release      # Jalankan command Alpine
  fake-run sh                           # Masuk shell Alpine
  fake-run apk add vim                  # Install package
  fake-run --svsp <program>             # Paksa pakai supervisor

${CYAN}Path penting:${NC}
  Wadah:     ~/alpine-rootfs/
  Runner:    $PREFIX/bin/fake-run
  Supervisor: $PREFIX/bin/svsp
  Shim:      ~/libfakeroot.so

${CYAN}Contoh install Claude Code:${NC}
  # Download Claude Code (arm64 musl)
  mkdir -p ~/musl-test && cd ~/musl-test
  curl -LO https://github.com/anthropics/claude-code/releases/latest/download/claude-code-linux-arm64-musl.tgz
  tar xzf claude-code-linux-arm64-musl.tgz
  # Jalankan di wadah
  fake-run ~/musl-test/package/claude --version

${CYAN}Tips:${NC}
  • Semua binary musl/glibc bisa jalan di wadah
  • Binary statis (Go, C) otomatis pakai supervisor
  • Config Claude mendarat di ~/alpine-rootfs/root/.claude
  • Install lagi kapan saja: ./install.sh (idempoten)

${YELLOW}Baca README.md untuk dokumentasi lengkap${NC}

EOF