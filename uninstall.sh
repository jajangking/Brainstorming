#!/data/data/com.termux/files/usr/bin/bash
#
# uninstall.sh — Hapus Brainstorming dari Termux
#
# Pakai: ./uninstall.sh [--keep-rootfs]
#
# Opsi:
#   --keep-rootfs   Simpan wadah Alpine, hapus tools aja

set -euo pipefail

# ESC asli ($'...'), bukan teks literal \033 — konsisten dgn install.sh.
RED=$'\033[1;31m'
GREEN=$'\033[1;32m'
YELLOW=$'\033[1;33m'
NC=$'\033[0m'

log()  { printf "${GREEN}[✓] %s${NC}\n" "$*"; }
warn() { printf "${YELLOW}[!] %s${NC}\n" "$*"; }
info() { printf "${RED}[i] %s${NC}\n" "$*"; }

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
KEEP_ROOTFS=0

for a in "$@"; do
    case "$a" in
        --keep-rootfs) KEEP_ROOTFS=1 ;;
        --help)
            echo "Pakai: ./uninstall.sh [--keep-rootfs]"
            echo "  --keep-rootfs   Simpan wadah Alpine, hapus tools aja"
            exit 0
            ;;
    esac
done

echo
info "Hapus Brainstorming dari Termux..."
echo

# Hapus tools
for f in "$PREFIX/bin/fake-run" "$PREFIX/bin/svsp" "$PREFIX/bin/alpine" "$HOME/libfakeroot.so"; do
    if [ -f "$f" ] || [ -L "$f" ]; then
        rm -f "$f"
        log "Hapus: $f"
    fi
done

# Hapus cache
for f in "$HOME/alpine-minirootfs.tar.gz" "$HOME/musl-dev-cache.apk"; do
    if [ -f "$f" ]; then
        rm -f "$f"
        log "Hapus cache: $f"
    fi
done

# Hapus rootfs (opsional)
if [ "$KEEP_ROOTFS" -eq 0 ]; then
    if [ -d "$HOME/alpine-rootfs" ]; then
        rm -rf "$HOME/alpine-rootfs"
        log "Hapus wadah: ~/alpine-rootfs/"
    fi
else
    warn "Wadah ~/alpine-rootfs/ disimpan (--keep-rootfs)"
fi

echo
log "Brainstorming sudah dihapus!"
if [ "$KEEP_ROOTFS" -eq 1 ]; then
    echo "  Wadah Alpine masih ada di ~/alpine-rootfs/"
    echo "  Hapus manual: rm -rf ~/alpine-rootfs"
fi
echo