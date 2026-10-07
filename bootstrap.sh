#!/data/data/com.termux/files/usr/bin/bash
# bootstrap.sh — bangun "wadah palsu" dari nol + pasang runner universal.
#
#   ./bootstrap.sh [--base=DIR] [--prefix=DIR] [--no-download] [--help]
#
# Yang dilakukan (idempoten — aman dijalankan ulang):
#   1. rootfs Alpine mini (v3.24.2, aarch64): pakai cache $HOME/alpine-minirootfs.tar.gz
#      bila ada, kalau tidak unduh dari CDN resmi.
#   2. toolchain musl-dev (header + libc.a/crt*.o) — dibutuhkan utk build shim,
#      sekaligus memberi kemampuan membangun binary STATIS musl.
#   3. patch loader musl -> ld-musl-patched.so.1 (SIGSYS tak pernah diblokir)
#      — patch byte di offset tetap, DIVERIFIKASI hash sebelum & sesudah
#      (bila hash stock tidak cocok: berhenti keras, jangan korup diam-diam).
#   4. rewrite PT_INTERP semua binary dinamis di wadah -> loader patched
#      (agar dieksekusi langsung kernel host — jalur supervisor — tetap jalan).
#   5. passthrough /dev /proc /sys (symlink host, set yang terbukti).
#   6. build libfakeroot.so (shim SIGSYS+rewrite path) + svsp (supervisor USER_NOTIF).
#   7. pasang fake-run + svsp ke $prefix/bin, shim ke ~/libfakeroot.so.
#   8. verifikasi: dinamis (LD_PRELOAD), supervisor (--svsp), STATIS, isolasi ENOENT.
#
# Hasil akhir: `fake-run <program>` universal — dinamis cepat, statis otomatis
# lewat supervisor; tanpa root, tanpa proot, native speed.

set -euo pipefail

: "${PREFIX:=/data/data/com.termux/files/usr}"
BASE="${FAKE_BASE:-$HOME/alpine-rootfs}"
TMPW="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/bootstrap.XXXXXX")"
trap 'rm -rf "$TMPW"' EXIT

# ---------- versi & konstanta yang DI-PIN ----------
ALPINE_VER=3.24.2
CDN="https://dl-cdn.alpinelinux.org/alpine/v3.24"
MINIROOTFS_URL="$CDN/releases/aarch64/alpine-minirootfs-$ALPINE_VER-aarch64.tar.gz"
# hash loader musl stock (minirootfs 3.24.2, APRIL 2026 build) dan hasil patch:
STOCK_SHA="32377e6d71725bb019e9ff6d5e9f16b4d5156d6f2c36504191c2d6a7c4d4a44d"
PATCHED_SHA="0131918ffbbdc1d50f842448e0ba8eecbd5e61babbd04e10984f1876e0818f47"
# situs patch: prolog fungsi 8-byte diganti `mov w0,#0; ret` (SIGSYS tak diblokir)
PATCH_SITES="0x24BB8 0x6993C 0x69980 0x69994 0x699A8 0x699BC 0x699EC"

log()  { printf '\033[1;32m[+] %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[!] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[x] %s\033[0m\n' "$*" >&2; exit 1; }

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
[ "${1:-}" = "--help" ] && usage 0
for a in "$@"; do
    case "$a" in
        --base=*)  BASE="${a#--base=}" ;;
        --prefix=*) PREFIX="${a#--prefix=}" ;;
        --no-download) NO_DL=1 ;;
        *) die "opsi tak dikenal: $a (lihat --help)" ;;
    esac
done

for c in clang curl tar gzip grep od sha256sum strip readelf patchelf; do
    command -v "$c" >/dev/null 2>&1 || die "butuh: $c (pkg install clang binutils patchelf ...)"
done

SRC="$(cd "$(dirname "$0")" && pwd)"
SRCFAKE="$SRC/fake-run"; SRCSVSP="$SRC/svsp.c"; SRCSHIM="$SRC/libfakeroot.c"; SRCPINT="$SRC/pinterp.c"
[ -f "$SRCSVSP" ] || die "svsp.c tidak ada di $SRC"
[ -f "$SRCSHIM" ] || die "libfakeroot.c tidak ada di $SRC"

# ---------------------------------------------------------------- rootfs
provision_rootfs() {
    if [ -f "$BASE/lib/ld-musl-patched.so.1" ]; then
        if [ "$(sha256sum "$BASE/lib/ld-musl-patched.so.1" | cut -d' ' -f1)" = "$PATCHED_SHA" ]; then
            log "rootfs sudah siap & loader ter-patch ($BASE) — dipakai ulang"; return 0
        fi
        warn "loader patched ada tapi hash beda — dibangun ulang"
    fi

    if [ ! -f "$BASE/lib/ld-musl-aarch64.so.1" ]; then
        # perlu rootfs segar
        SRC_TAR="$HOME/alpine-minirootfs.tar.gz"
        if [ ! -f "$SRC_TAR" ]; then
            [ -n "${NO_DL:-}" ] && die "rootfs kosong & mode --no-download"
            log "unduh minirootfs Alpine $ALPINE_VER (aarch64)..."
            curl -fSL -o "$SRC_TAR" "$MINIROOTFS_URL" || die "gagal unduh minirootfs"
        fi
        # verifikasi loader di tarball == stock build yang kita pin
        local ldr
        ldr="$TMPW/stock-ld"
        mkdir -p "$ldr"
        tar -xzf "$SRC_TAR" -C "$ldr" "./lib/ld-musl-aarch64.so.1" || die "arsip minirootfs rusak/mismatch"
        [ "$(sha256sum "$ldr/lib/ld-musl-aarch64.so.1" | cut -d' ' -f1)" = "$STOCK_SHA" ] \
            || die "loader di tarball ≠ build 3.24.2 yang kita pin — versi beda, patch basi. Hapus $SRC_TAR utk unduh ulang."
        log "ekstrak rootfs segar ke $BASE"
        mkdir -p "$BASE"
        tar -xzf "$SRC_TAR" -C "$BASE"
    else
        [ "$(sha256sum "$BASE/lib/ld-musl-aarch64.so.1" | cut -d' ' -f1)" = "$STOCK_SHA" ] \
            || die "loader stock di $BASE ≠ build yang kita pin (patch offset basi). Gunakan wadah 3.24.2 resmi."
    fi

    # patch loader
    log "patch loader -> ld-musl-patched.so.1"
    cp "$BASE/lib/ld-musl-aarch64.so.1" "$BASE/lib/ld-musl-patched.so.1"
    for off in $PATCH_SITES; do
        printf '\000\000\200\122\300\003\137\326' \
            | dd of="$BASE/lib/ld-musl-patched.so.1" bs=1 seek=$(($off)) conv=notrunc status=none
    done
    [ "$(sha256sum "$BASE/lib/ld-musl-patched.so.1" | cut -d' ' -f1)" = "$PATCHED_SHA" ] \
        || die "hasil patch loader ≠ hash yang diharapkan — BUKAN masalah kecil, stop."
    log "loader patched OK ($PATCHED_SHA)"
}

# ----------------------------------------------------------- toolchain musl-dev
musl_dev() {
    if [ -f "$BASE/usr/lib/libc.a" ] && [ -d "$BASE/usr/include" ]; then
        log "toolchain musl-dev sudah ada — dipakai ulang"; return 0
    fi
    [ -n "${NO_DL:-}" ] && die "musl-dev kurang & mode --no-download"
    local V pkg cached="$HOME/musl-dev-cache.apk"
    log "cari versi musl-dev di indeks repositori Alpine..."
    curl -fsSL "$CDN/main/aarch64/APKINDEX.tar.gz" -o "$TMPW/apkindex.tar.gz" \
        || die "gagal unduh indeks paket (APKINDEX.tar.gz)"
    V="$(tar -xzOf "$TMPW/apkindex.tar.gz" 2>/dev/null \
        | grep -a -m1 -A2 '^P:musl-dev$' | grep -a '^V:' | cut -d: -f2 || true)"
    [ -n "$V" ] || die "musl-dev tidak ditemukan di indeks"
    pkg="musl-dev-$V.apk"
    log "toolchain: $pkg"
    if [ ! -f "$cached" ]; then
        curl -fSL -o "$cached" "$CDN/main/aarch64/$pkg" || die "gagal unduh $pkg"
    fi
    tar -xzf "$cached" -C "$BASE" usr/ || die "ekstraksi musl-dev gagal"
    log "musl-dev terpasang (usr/include + usr/lib/*.a + crt*.o)"
}

# --------------------------------------------------------------- passthrough
setup_passthrough() {
    link() {
        local t="$1" d="$2"
        # sudah ada (file/symlink, termasuk dangling) -> biarkan
        if [ -e "$BASE$t" ] || [ -L "$BASE$t" ]; then return 0; fi
        # direktori kosong dari tarball -> buang dulu baru symlink
        if [ -d "$BASE$t" ] && [ -z "$(ls -A "$BASE$t" 2>/dev/null)" ]; then rmdir "$BASE$t"; fi
        ln -s "$d" "$BASE$t"
    }
    link "/proc" "/proc"
    link "/sys" "/sys"
    link "/dev/__properties__" "/dev/__properties__"
    link "/dev/full" "/dev/full"
    link "/dev/null" "/dev/null"
    link "/dev/random" "/dev/random"
    link "/dev/tty" "/dev/tty"
    link "/dev/urandom" "/dev/urandom"
    link "/dev/zero" "/dev/zero"
    mkdir -p "$BASE/root" "$BASE/tmp"
    log "passthrough /dev /proc /sys + root/tmp siap"
}

# ------------------------------------------------- symlink applet busybox
# minirootfs Alpine: bin/cat -> /bin/busybox (ABSOLUT). Dari sisi HOST ini
# patah (host /bin tak ada busybox) dan fake-run tak bisa resolve. Wajib
# dijadikan relatif: bin/cat -> busybox.
fix_applet_links() {
    local n=0 f dir rel
    while IFS= read -r f; do
        dir="${f%/*}"
        rel="$(realpath --relative-to="$dir" "$BASE/bin/busybox" 2>/dev/null)"
        [ -n "$rel" ] || rel=busybox
        if [ "$(readlink "$f")" != "$rel" ]; then
            ln -sfn "$rel" "$f"; n=$((n+1))
        fi
    done < <(find "$BASE" -type l -lname '/bin/busybox')
    log "applet busybox di-relink relatif: $n symlink"
}

# ---------------------------------------------------------- rewrite INTERP
rewrite_interp() {
    log "build pinterp (rewrite PT_INTERP in-place)"
    clang -O2 -o "$TMPW/pinterp" "$SRCPINT" || die "build pinterp gagal"
    local n=0 fail=0 f rc
    while IFS= read -r f; do
        rc=0; "$TMPW/pinterp" "$f" "$BASE/lib/ld-musl-patched.so.1" || rc=$?
        case $rc in
            0) n=$((n+1)) ;;
            1) : ;;                          # bukan ELF64 / tanpa INTERP — skip
            3) patchelf --set-interpreter "$BASE/lib/ld-musl-patched.so.1" "$f" \
                 || { fail=$((fail+1)); warn "patchelf gagal: $f"; }
               n=$((n+1)) ;;
            2) fail=$((fail+1)); warn "I/O gagal: $f" ;;
        esac
    done < <(find "$BASE" -type f -size +64c)
    log "PT_INTERP di-set: $n file${fail:+, $fail gagal}"
    [ "$fail" -eq 0 ] || die "ada file ELF yang gagal di-rewrite"
}

# ------------------------------------------------------ build & install
build_binaries() {
    log "build libfakeroot.so (shim)"
    clang --target=aarch64-alpine-linux-musl --sysroot="$BASE" \
        -fPIC -shared -O2 -nostdlib -o "$HOME/libfakeroot.so" "$SRCSHIM" \
        || die "build shim gagal (musl-dev sudah terpasang?)"

    log "build svsp (supervisor USER_NOTIF)"
    clang -O2 -o "$TMPW/svsp" "$SRCSVSP" || die "build svsp gagal"
    strip "$TMPW/svsp"
}

install() {
    mkdir -p "$PREFIX/bin"
    cp "$TMPW/svsp" "$PREFIX/bin/svsp"
    chmod 0755 "$PREFIX/bin/svsp"
    cp "$SRCFAKE" "$PREFIX/bin/fake-run"
    chmod 0755 "$PREFIX/bin/fake-run"
    if [ -f "$SRC/alpine" ]; then
        cp "$SRC/alpine" "$PREFIX/bin/alpine"
        chmod 0755 "$PREFIX/bin/alpine"
    fi
    chmod 0755 "$HOME/libfakeroot.so"
    log "terpasang: $PREFIX/bin/fake-run, $PREFIX/bin/svsp, $PREFIX/bin/alpine, ~/libfakeroot.so"
}

# ------------------------------------------------------------- verifikasi
verify_run() {
    local base="$BASE"
    local fr
    if [ -x "$PREFIX/bin/fake-run" ]; then fr="$PREFIX/bin/fake-run"; else fr="$SRCFAKE"; fi

    echo "----- V1 dinamis (jalur cepat LD_PRELOAD) -----"
    env -u LD_PRELOAD "$fr" --base="$base" cat /etc/alpine-release

    echo "----- V2 supervisor dipaksa (--svsp, binary dinamis) -----"
    env -u LD_PRELOAD "$fr" --base="$base" --svsp busybox cat /etc/alpine-release

    echo "----- V3 STATIS (auto -> supervisor) -----"
    cat > "$TMPW/boot-static.c" <<'EOF'
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
int main(void){
    char b[128]; int fd=open("/etc/alpine-release",0);
    if(fd<0) return 1;
    ssize_t n=read(fd,b,127); if(n<0) return 2;
    b[n]=0; write(1,b,n); write(1," BOOT-STATIC-OK\n",16); return 0;
}
EOF
    clang --target=aarch64-alpine-linux-musl --sysroot="$base" -static -nostdlib \
        -o "$base/usr/bin/boot-static" \
        "$base/usr/lib/crt1.o" "$base/usr/lib/crti.o" "$TMPW/boot-static.c" \
        "$base/usr/lib/libc.a" "$base/usr/lib/crtn.o" \
        || die "build boot-static gagal"
    env -u LD_PRELOAD "$fr" --base="$base" "$base/usr/bin/boot-static"

    echo "----- V4 isolasi (harus ENOENT) -----"
    env -u LD_PRELOAD "$fr" --base="$base" cat /system/build.prop || true
}

# =================================================================== main
provision_rootfs
musl_dev
setup_passthrough
fix_applet_links
rewrite_interp
build_binaries
install
verify_run

echo
log "SELESAI. Pakai:  fake-run <program>   (dinamis cepat, statis auto-supervisor)"
echo "      base: $BASE | shim: $HOME/libfakeroot.so | alat: $PREFIX/bin/fake-run"