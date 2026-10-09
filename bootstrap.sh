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
#   8. runtime C/C++ (libstdc++ + libgcc) via apk wadah — minirootfs TIDAK
#      menyertakannya, padahal binary pihak-ketiga (mis. opencode) butuh
#      libstdc++.so.6 (temuan device pasca-install-fresh 2026-10-08).
#   9. paket dasar (curl, bash, git) via apk wadah — peralatan standar
#      yang diharapkan selalu ada di setiap wadah.
#  10. verifikasi: dinamis (LD_PRELOAD), supervisor (--svsp), STATIS,
#      isolasi ENOENT, runtime C++ (libstdc++).
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
    if [ -f "$BASE/usr/lib/libc.a" ] && [ -d "$BASE/usr/include" ] \
       && [ -f "$BASE/usr/include/linux/seccomp.h" ]; then
        log "toolchain musl-dev + linux-headers sudah ada — dipakai ulang"; return 0
    fi
    [ -n "${NO_DL:-}" ] && die "toolchain musl kurang & mode --no-download"
    local V pkg cached="$HOME/musl-dev-cache.apk"
    log "cari versi paket di indeks repositori Alpine..."
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
    tar -xzf "$cached" -C "$BASE" usr/ 2>/dev/null || die "ekstraksi musl-dev gagal"

    # Header UAPI linux (linux/audit.h dll) dibutuhkan untuk build svsp statis
    # (musl). Paket terpisah dari musl-dev; ikuti pola cache + indeks yang sama.
    if [ ! -f "$BASE/usr/include/linux/seccomp.h" ]; then
        local LH cached_lh="$HOME/linux-headers-cache.apk"
        LH="$(tar -xzOf "$TMPW/apkindex.tar.gz" 2>/dev/null \
            | grep -a -m1 -A2 '^P:linux-headers$' | grep -a '^V:' | cut -d: -f2 || true)"
        [ -n "$LH" ] || die "linux-headers tidak ditemukan di indeks"
        log "toolchain: linux-headers-$LH"
        if [ ! -f "$cached_lh" ]; then
            curl -fSL -o "$cached_lh" "$CDN/main/aarch64/linux-headers-$LH.apk" \
                || die "gagal unduh linux-headers-$LH.apk"
        fi
        tar -xzf "$cached_lh" -C "$BASE" usr/ 2>/dev/null \
            || die "ekstraksi linux-headers gagal"
    fi
    log "toolchain musl terpasang (crt*.o + libc.a + header + linux UAPI)"
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

# ------------------------------------------------------------- DNS wadah
# Resolver musl membaca resolv.conf WADAH (path /etc/resolv.conf di-rewrite).
# Di jaringan seluler, UDP/53 ke resolver publik kadang jatuh -> EAI_AGAIN
# transient ("Transient name resolution failure"/"bad address", kadang-kadang).
# Strategi tahan banting: 4 nameserver publik + opsi retry musl. Terbukti
# empiris: musl 1.2.5 menghormati `options` (timeout:1 attempts:1 -> 1.01s
# vs 5.01s default).
setup_dns() {
    printf 'nameserver 1.1.1.1\nnameserver 1.0.0.1\nnameserver 8.8.8.8\nnameserver 8.8.4.4\noptions timeout:2 attempts:3\n' \
        > "$BASE/etc/resolv.conf"
    log "resolv.conf wadah: 4 nameserver + retry musl (timeout:2 attempts:3)"
}

# ------------------------------------------------------- runtime libs C/C++
# minirootfs TIDAK menyertakan libstdc++/libgcc, padahal banyak binary
# pihak-ketiga (mis. opencode, bun) butuh libstdc++.so.6. Pasang dari awal
# supaya "fresh install" langsung bisa jalanin binary C++ (temuan device
# pasca-install-fresh 2026-10-08: opencode gagal start dengan
# `Error loading shared library libstdc++.so.6`).
# Syarat: dipanggil SETELAH install() — fake-run butuh shim yang sudah
# terpasang ($HOME/libfakeroot.so) untuk jalur LD_PRELOAD apk.
runtime_libs() {
    if [ -f "$BASE/usr/lib/libstdc++.so.6" ] && [ -f "$BASE/usr/lib/libgcc_s.so.1" ]; then
        log "runtime libstdc++/libgcc sudah ada — dipakai ulang"; return 0
    fi
    [ -x "$PREFIX/bin/fake-run" ] || die "fake-run belum terpasang (runtime_libs dipanggil sebelum install?)"
    log "pasang runtime C++ (libstdc++ + libgcc) via apk wadah..."
    env -u LD_PRELOAD "$PREFIX/bin/fake-run" --base="$BASE" apk add libstdc++ libgcc \
        || die "apk add libstdc++ libgcc gagal"
}

# ------------------------------------------------------- paket dasar wadah
# Peralatan standar yang diharapkan ada di setiap wadah: curl (unduh),
# bash (shell skrip umum — bawaan wadah hanya busybox ash), git (klon repo
# & tooling). `ncurses-base`/`ncurses-term` menyediakan database terminfo —
# tanpa itu `tput` absen dan aplikasi TUI menebak-nebak escape sequence,
# sehingga warna/ketikan bisa kacau. Dipasang via apk wadah, idempoten.
# Syarat: dipanggil SETELAH install() (butuh fake-run + shim), sama
# seperti runtime_libs.
base_packages() {
    local missing=0 p
    for p in curl bash git nodejs npm; do
        [ -x "$BASE/usr/bin/$p" ] || [ -x "$BASE/bin/$p" ] || missing=1
    done
    [ -d "$BASE/usr/share/terminfo" ] || missing=1
    if [ "$missing" -eq 0 ]; then
        log "paket dasar (curl bash git nodejs npm + terminfo) sudah ada — dipakai ulang"; return 0
    fi
    [ -x "$PREFIX/bin/fake-run" ] || die "fake-run belum terpasang (base_packages dipanggil sebelum install?)"
    log "pasang paket dasar (curl bash git nodejs npm + terminfo) via apk wadah..."
    # nama paket terminfo di Alpine 3.24: ncurses-terminfo-base (umum) +
    # ncurses-terminfo (256-color dkk). 'ncurses-base'/'ncurses-term' tak ada.
    # nodejs+npm ikut dasar karena `npm install -g` adalah jalur paling sering
    # dipakai app di dalam wadah (§48) dan pasang ulang saat itu perlu ~1 menit
    # jaringan seluler — jauh lebih lambat daripada ~45 detik bersama toolchain.
    env -u LD_PRELOAD "$PREFIX/bin/fake-run" --base="$BASE" apk add curl bash git \
        ncurses-terminfo-base ncurses-terminfo nodejs npm \
        || die "apk add paket dasar gagal"
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
    local n=0 fail=0 f rc magic
    while IFS= read -r f; do
        # §38: PRA-SARING tanpa spawn. Device ronde 16: `find` menyapu SELURUH
        # $BASE termasuk klon git pengguna (root/.hermes = 16.168 berkas) dan
        # memanggil pinterp satu per satu -> install >7 menit (tampak hang).
        # Objek git/pack/teks tak pernah ELF, jadi baca 4 byte magic lewat
        # builtin `read` (tanpa proses baru) dan lewati yang bukan ELF.
        magic=""; LC_ALL=C read -rn4 magic < "$f" 2>/dev/null || true
        [ "$magic" = $'\x7fELF' ] || continue
        rc=0; "$TMPW/pinterp" "$f" "$BASE/lib/ld-musl-patched.so.1" || rc=$?
        # §27: berkas paket kerap ber-mode read-only (0555, mis. isi .codex) —
        # pinterp lalu gagal EACCES dan dulu MEMBATALKAN seluruh bootstrap
        # (device ronde 5). Pinjam bit tulis sebentar, ulangi, lalu PULIHKAN
        # mode semula.
        if [ "$rc" = 2 ] && [ -e "$f" ] && [ ! -w "$f" ]; then
            m=$(stat -c '%a' "$f" 2>/dev/null || echo "")
            if [ -n "$m" ] && chmod u+w "$f" 2>/dev/null; then
                rc=0; "$TMPW/pinterp" "$f" "$BASE/lib/ld-musl-patched.so.1" || rc=$?
                chmod "$m" "$f" 2>/dev/null || true
                [ "$rc" = 0 ] && log "  (mode $m dipinjam sementara: $f)"
            fi
        fi
        case $rc in
            0) n=$((n+1)) ;;
            1) : ;;                          # bukan ELF64 / tanpa INTERP — skip
            3) patchelf --set-interpreter "$BASE/lib/ld-musl-patched.so.1" "$f" \
                 || { fail=$((fail+1)); warn "patchelf gagal: $f"; }
               n=$((n+1)) ;;
            2) fail=$((fail+1)); warn "I/O gagal: $f" ;;
            4) warn "dilewati (sedang dieksekusi): $f" ;;   # ETXTBSY — bukan kegagalan
        esac
    done < <(find "$BASE" \
        \( -type d \( -name .git -o -name node_modules -o -name __pycache__ \
                     -o -name .venv -o -name .cache \) -prune \) -o \
        \( -type f -size +64c -size -512M -print \))
    # Batas atas 512M (bukan 64M): binary modern bisa jumbo (device
    # 2026-10-08: opencode 195MB lolos dari rewrite -> INTERP stock
    # /lib/... -> kernel host tak bisa resolve -> --svsp ENOENT, padahal
    # jalur shim jalan karena fake-run bypass INTERP). Pra-saring magic
    # ELF 4-byte membuat sapuan file besar tetap murah.
    log "PT_INTERP di-set: $n file${fail:+, $fail gagal}"
    [ "$fail" -eq 0 ] || die "ada file ELF yang gagal di-rewrite"
}

# ------------------------------------------------------ build & install
build_binaries() {
    log "build libfakeroot.so (shim)"
    clang --target=aarch64-alpine-linux-musl --sysroot="$BASE" \
        -fPIC -shared -O2 -nostdlib -o "$HOME/libfakeroot.so" "$SRCSHIM" \
        || die "build shim gagal (musl-dev sudah terpasang?)"

    # svsp = biner STATIS musl (bukan bionic dinamis): kernel exec langsung,
    # tanpa linker host, tanpa verneed/DT_NEEDED -> kebal terhadap penolakan
    # linker64 versi tertentu ("CANNOT LINK ... verneed[0] ..."). Pola sama
    # dengan boot-static + compiler-rt builtins utk soft-float printf.
    log "build svsp (supervisor USER_NOTIF, statis musl)"
    local CRT="$(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-aarch64-android.a"
    [ -f "$CRT" ] || die "compiler-rt builtins tidak ditemukan: $CRT"
    clang --target=aarch64-alpine-linux-musl --sysroot="$BASE" -static -nostdlib -O2 \
        -o "$TMPW/svsp" \
        "$BASE/usr/lib/crt1.o" "$BASE/usr/lib/crti.o" "$SRCSVSP" \
        "$BASE/usr/lib/libc.a" "$BASE/usr/lib/crtn.o" "$CRT" \
        || die "build svsp gagal"
    strip "$TMPW/svsp"
}

install() {
    mkdir -p "$PREFIX/bin"
    # Pasang secara ATOMIK: tulis ke file sementara lalu `mv` (rename).
    # `cp` langsung ke target GAGAL "Text file busy" (ETXTBSY) bila binary
    # sedang dieksekusi — mis. ada app wadah yang masih jalan saat update
    # (device 2026-10-09: svsp dikunci proses, bootstrap gagal). rename()
    # mengganti inode, jadi proses yang sedang jalan tak terganggu.
    install_atomic() { # install_atomic <src> <dst> [mode]
        local src=$1 dst=$2 mode=${3:-0755} tmp
        mkdir -p "$(dirname "$dst")"
        tmp="$dst.new.$$"
        cp "$src" "$tmp" && chmod "$mode" "$tmp" && mv -f "$tmp" "$dst"
    }
    install_atomic "$TMPW/svsp" "$PREFIX/bin/svsp"
    install_atomic "$SRCFAKE" "$PREFIX/bin/fake-run"
    [ -f "$SRC/alpine" ] && install_atomic "$SRC/alpine" "$PREFIX/bin/alpine"
    # `app` — daftar aplikasi terpasang (dipakai dari dalam shell wadah)
    if [ -f "$SRC/app" ]; then
        install_atomic "$SRC/app" "$PREFIX/bin/app"
        install_atomic "$SRC/app" "$BASE/usr/local/bin/app"
    fi
    # stub `alpine` di dalam wadah: `alpine` asli milik host (bionic) tak bisa
    # dieksekusi dari dalam wadah, jadi beri petunjuk alih-alih "not found".
    [ -f "$SRC/alpine-in-container" ] && \
        install_atomic "$SRC/alpine-in-container" "$BASE/usr/local/bin/alpine"
    log "terpasang: $PREFIX/bin/fake-run, $PREFIX/bin/svsp, $PREFIX/bin/alpine, ~/libfakeroot.so"
    [ -f "$PREFIX/bin/app" ] && log "daftar aplikasi: app (juga di dalam wadah: /usr/local/bin/app)"
}

# ------------------------------------------------------------- verifikasi
# Device ini punya transient loader/runtime sesekali (CANNOT-LINK, SIGSYS) di bawah
# beban build — retry tiap lengan sebelum dianggap gagal, supaya sekali kedip tidak
# menggagalkan seluruh bootstrap.
try_ok() {  # try_ok <label> <max> -- <cmd...>  ; cetak output bila sukses, die bila terus gagal
    local label=$1 max=$2; shift 2; [ "$1" = "--" ] && shift
    local i rc=1 out
    for i in $(seq 1 "$max"); do
        out=$("$@") && rc=0 && break
        echo "  ⚠ $label percobaan $i/$max gagal (rc=$?) — coba lagi" >&2
        sleep 1
    done
    [ "$rc" -eq 0 ] && { printf '%s\n' "$out"; return 0; }
    die "$label gagal"
}

verify_run() {
    local base="$BASE"
    local fr
    if [ -x "$PREFIX/bin/fake-run" ]; then fr="$PREFIX/bin/fake-run"; else fr="$SRCFAKE"; fi

    echo "----- V1 dinamis (jalur cepat LD_PRELOAD) -----"
    try_ok "V1 dinamis" 3 -- env -u LD_PRELOAD "$fr" --base="$base" cat /etc/alpine-release

    echo "----- V2 supervisor dipaksa (--svsp, binary dinamis) -----"
    try_ok "V2 supervisor" 3 -- env -u LD_PRELOAD "$fr" --base="$base" --svsp busybox cat /etc/alpine-release

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
    try_ok "V3 statis" 3 -- env -u LD_PRELOAD "$fr" --base="$base" "$base/usr/bin/boot-static"

    echo "----- V4 isolasi (harus ENOENT) -----"
    env -u LD_PRELOAD "$fr" --base="$base" cat /system/build.prop || true

    echo "----- V5 runtime C++ (libstdc++) -----"
    [ -f "$base/usr/lib/libstdc++.so.6" ] \
        || die "libstdc++.so.6 tak ada di wadah — runtime_libs gagal?"
    echo "libstdc++.so.6 OK"

    # V6 npm global (§47/§48). Dua-duanya perlu: mkdir prefix global BUTUH
    # supervisor (Node memanggil syscall mentah), dan lstat("/data") TAK BOLEH
    # di-rewrite jadi $BASE/data. Tanpa V6, `npm install -g <paket>` gagal
    # ENOENT padahal semuanya benar — persis laporan device 2026-10-09.
    if [ -x "$base/usr/bin/node" ] && [ -x "$base/usr/bin/npm" ]; then
        echo "----- V6 npm -g (prefix global via supervisor) -----"
        try_ok "V6 npm -g" 3 -- env -u LD_PRELOAD "$fr" --base="$base" --svsp \
            sh -c 'cd "$HOME" && npm install -g is-number --no-audit --no-fund >/dev/null 2>&1 && npm ls -g --depth=0 >/dev/null'
        echo "npm install -g OK"
    else
        echo "----- V6 npm -g dilewati: node/npm belum ada di wadah -----"
    fi
}

# ------------------------------------------------- default port opencode
# §32 (insiden device pasca-ronde 12): opencode wadah DAN opencode host
# sama-sama default ke port service 49374. Di perangkat yang juga menjalankan
# opencode di host, pemakaian wadah apa adanya pasti kena
# "Managed service port 49374 ... already in use". Beri wadah default sendiri.
# Konservatif: hanya DIBUAT bila belum ada config (tak pernah menimpa pilihan
# user), dan hanya bila opencode memang terpasang di wadah.
setup_opencode_port() {
    local oc="$BASE/root/.opencode/bin/opencode"
    local cfgdir="$BASE/root/.config/opencode"
    local cfg="$cfgdir/service.json"
    [ -x "$oc" ] || return 0
    if [ -e "$cfg" ]; then
        log "opencode: config service sudah ada, dibiarkan ($cfg)"
        return 0
    fi
    mkdir -p "$cfgdir" || return 0
    printf '{ "port": 49474 }\n' > "$cfg" || return 0
    log "opencode: default port wadah di-set 49474 (hindari bentrok 49374 milik host)"
}

# =================================================================== main
provision_rootfs
musl_dev
setup_passthrough
setup_dns
fix_applet_links
rewrite_interp
build_binaries
install
runtime_libs
base_packages
setup_opencode_port
verify_run

echo
log "SELESAI. Pakai:  fake-run <program>   (dinamis cepat, statis auto-supervisor)"
echo "      base: $BASE | shim: $HOME/libfakeroot.so | alat: $PREFIX/bin/fake-run"