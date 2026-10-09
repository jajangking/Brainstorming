#!/data/data/com.termux/files/usr/bin/bash
#
# uninstall.sh — Hapus Brainstorming dari Termux.
#
# Pakai: ./uninstall.sh [--keep-rootfs] [--yes] [--dry-run]
#
# Opsi:
#   --keep-rootfs   Simpan wadah Alpine (utk app/config di dalamnya), hapus alat saja
#   --yes           Jangan tanya konfirmasi (untuk non-interaktif/script)
#   --dry-run       Tampilkan yang AKAN dihapus, tanpa menghapus apa pun

set -euo pipefail

RED=$'\033[1;31m'
GREEN=$'\033[1;32m'
YELLOW=$'\033[1;33m'
CYAN=$'\033[1;36m'
NC=$'\033[0m'

log()  { printf "${GREEN}[✓] %s${NC}\n" "$*"; }
warn() { printf "${YELLOW}[!] %s${NC}\n" "$*"; }
info() { printf "${RED}[i] %s${NC}\n" "$*"; }
dim()  { printf "${CYAN}    %s${NC}\n" "$*"; }

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
HOME="${HOME:-/data/data/com.termux/files/home}"
BASE="${FAKE_BASE:-$HOME/alpine-rootfs}"
KEEP_ROOTFS=0
ASSUME_YES=0
DRY_RUN=0

for a in "$@"; do
    case "$a" in
        --keep-rootfs) KEEP_ROOTFS=1 ;;
        -y|--yes)      ASSUME_YES=1 ;;
        -n|--dry-run)  DRY_RUN=1 ;;
        --help|-h)
            cat <<EOF
Pakai: ./uninstall.sh [--keep-rootfs] [--yes] [--dry-run]

  --keep-rootfs   Simpan wadah Alpine (app & config di dalamnya tetap ada),
                  hapus hanya alat di \$PREFIX/bin + shim + cache.
  --yes, -y       Jangan tanya konfirmasi.
  --dry-run, -n   Tampilkan yang akan dihapus, tanpa menghapus.

PERINGATAN: tanpa --keep-rootfs, SELURUH ~/alpine-rootfs dihapus — termasuk
app yang terpasang di dalamnya (Hermes, opencode, Claude Code, config).
Ukur dulu: du -sh ~/alpine-rootfs
EOF
            exit 0 ;;
    esac
done

# --- kumpulkan target lebih dulu, supaya bisa diringkas & dikonfirmasi ------
# Alat di host
TOOLS=("$PREFIX/bin/fake-run" "$PREFIX/bin/svsp" "$PREFIX/bin/alpine"
       "$PREFIX/bin/app" "$PREFIX/bin/hermes-reinstall" "$PREFIX/bin/hermes-web-build"
       "$PREFIX/bin/hermes-nodeps-driver.py" "$PREFIX/bin/web-split.mjs" "$HOME/libfakeroot.so")
# Cache unduhan
CACHES=("$HOME/alpine-minirootfs.tar.gz" "$HOME/musl-dev-cache.apk"
        "$HOME/linux-headers-cache.apk")
# Skrip kita di DALAM wadah (bukan konten user — tetap dibersihkan meski
# --keep-rootfs, karena tanpa fake-run keduanya tak berguna).
INSIDE=("$BASE/usr/local/bin/app" "$BASE/usr/local/bin/alpine"
        "$BASE/usr/local/share/fakeroot/node-netlink-safe.js"
        "$BASE/usr/local/share/fakeroot/procnet/if_inet6")
# .git.off = .git yang disembunyikan hermes-reinstall (§54). Kalau process-nya
# dibunuh di tengah jalan, file ini tertinggal dan `hermes update` ikut gagal —
# jadi harus dikembalikan (bukan dihapus) saat uninstall.
STASHED_GIT="$BASE/root/.hermes/hermes-agent/.git.off"
# Direktori yang kita buat sendiri (sisa registry launcher dll).
DIRS=("$PREFIX/share/brainstorming"
      "$BASE/usr/local/share/fakeroot")

echo
info "Hapus Brainstorming dari Termux..."
echo

# kumpulkan yang ada + hitung total ukuran dulu
present() { [ -e "$1" ] || [ -L "$1" ]; }

total_bytes=0
for f in "${TOOLS[@]}" "${CACHES[@]}" "${INSIDE[@]}"; do
    present "$f" || continue
    if [ -f "$f" ] && [ ! -L "$f" ]; then
        sz=$(du -k "$f" 2>/dev/null | cut -f1); total_bytes=$(( total_bytes + ${sz:-0} ))
    fi
done
if [ "$KEEP_ROOTFS" -eq 0 ] && [ -d "$BASE" ]; then
    sz=$(du -sk "$BASE" 2>/dev/null | cut -f1); total_bytes=$(( total_bytes + ${sz:-0} ))
fi
printf '  total: %s\n\n' "$(printf '%d' "$total_bytes" | awk '{if($1>1048576) printf "%.1f GB", $1/1048576; else printf "%.0f MB", $1/1024}')"

if [ "$KEEP_ROOTFS" -eq 0 ]; then
    warn "Akan menghapus SELURUH wadah: $BASE"
    dim "berisi app & config (Hermes, opencode, Claude Code, .config, ...)"
    warn "Untuk menyimpan isinya: ./uninstall.sh --keep-rootfs"
    echo
fi

# Konfirmasi (kecuali --yes / --dry-run)
if [ "$ASSUME_YES" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    if [ ! -t 0 ]; then
        info "stdin bukan terminal → gunakan --yes untuk konfirmasi otomatis"
        exit 1
    fi
    printf '  Lanjutkan penghapusan? [y/N] '
    read -r ans
    case "$ans" in
        y|Y|yes|YES) ;;
        *) info "Dibatalkan."; exit 0 ;;
    esac
    echo
fi

do_rm() { # do_rm <file> <label>
    local f=$1 label=$2
    present "$f" || return 0
    if [ "$DRY_RUN" -eq 1 ]; then
        dim "akan hapus [$label]: $f"
    else
        rm -f "$f" 2>/dev/null || { warn "gagal hapus: $f"; return 0; }
        log "Hapus [$label]: $f"
    fi
}

# 1) alat di host
for f in "${TOOLS[@]}"; do do_rm "$f" "alat"; done

# 2) skrip di dalam wadah (selalu, meski --keep-rootfs)
for f in "${INSIDE[@]}"; do do_rm "$f" "dalam wadah"; done
# §54: kembalikan .git yang sempat disembunyikan, jangan dihapus.
if [ -d "$STASHED_GIT" ] && [ ! -e "$BASE/root/.hermes/hermes-agent/.git" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "    akan dipulihkan: $STASHED_GIT -> $BASE/root/.hermes/hermes-agent/.git"
    else
        mv "$STASHED_GIT" "$BASE/root/.hermes/hermes-agent/.git" 2>/dev/null \
            && echo "    dipulihkan: .git (tersembunyi dari hermes-reinstall)"
    fi
fi

# 3) cache unduhan
for f in "${CACHES[@]}"; do do_rm "$f" "cache"; done

# 3b) direktori milik kita (hanya bila kosong — aman, tak menyentuh user)
for d in "${DIRS[@]}"; do
    [ -d "$d" ] || continue
    if [ -z "$(ls -A "$d" 2>/dev/null)" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            dim "akan hapus [dir kosong]: $d"
        else
            rmdir "$d" 2>/dev/null && log "Hapus [dir kosong]: $d"
        fi
    elif [ "$DRY_RUN" -eq 0 ]; then
        warn "tak kosong, dibiarkan: $d ($(ls -A "$d" | wc -l) entri)"
    fi
done

# 4) wadah
if [ "$KEEP_ROOTFS" -eq 0 ]; then
    if [ -d "$BASE" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            dim "akan hapus [wadah]: $BASE/"
        else
            rm -rf "$BASE"
            log "Hapus [wadah]: $BASE/"
        fi
    fi
else
    warn "Wadah $BASE/ disimpan (--keep-rootfs)"
fi

# 5) ringkasan sisa
echo
if [ "$DRY_RUN" -eq 1 ]; then
    log "Dry-run selesai — tidak ada yang dihapus."
    exit 0
fi

left=0
for f in "${TOOLS[@]}" "${CACHES[@]}"; do present "$f" && left=1; done
[ -d "$BASE" ] && left=1

if [ "$left" -eq 0 ]; then
    log "Brainstorming sudah dihapus!"
else
    log "Selesai. Yang tersisa:"
    for f in "${TOOLS[@]}" "${CACHES[@]}"; do present "$f" && dim "$f"; done
    [ -d "$BASE" ] && dim "$BASE/ (wadah,kept)"
fi
echo
echo "  ${CYAN}Repo${NC} masih ada di $HOME/Brainstorming — hapus manual bila selesai:"
echo "    rm -rf $HOME/Brainstorming"
echo