#!/bin/sh
# app — daftar aplikasi yang terpasang di dalam wadah Alpine.
#
#   app            daftar aplikasi (nama, versi, ukuran, lokasi)
#   app <nama>     jalankan aplikasi itu, sisanya jadi argumen
#
# Tidak perlu daftar aplikasi/konfigurasi: semua lokasi app umum dipindai
# dan yang berukuran "besar" (>= APP_MIN_MB, default 1 MB) dianggap app.
# Binary sistem (busybox, coreutils, apk, ...) dikecualikan agar tabelnya
# tetap berisi aplikasi, bukan alat kecil.

APP_MIN_MB="${APP_MIN_MB:-1}"



G=$'\033[1;38;2;0;255;0m'    # hijau bright (terpasang)
O=$'\033[38;2;255;135;0m'    # orange (path/label)
D=$'\033[2;38;2;0;170;0m'    # hijau redup (rangkuman)
N=$'\033[0m'

# Dalam wadah, HOME sudah menunjuk root wadah ($BASE/root). Hanya kalau
# HOME kosong (mis. proses artificial) kita fallback ke root base.
HOME_DIR="$HOME"
[ -n "$HOME_DIR" ] || HOME_DIR="${FAKE_BASE:-$HOME}/root"

# ------------------------------------------------------------------ MODE JALAN
run_app() {
    want=$(printf '%s' "$1" | tr 'A-Z' 'a-z'); shift
    for d in $APPDIRS; do
        [ -d "$d" ] || continue
        for f in "$d"/*; do
            [ -f "$f" ] && [ -x "$f" ] || continue
            [ -L "$f" ] && continue                        # symlink applet
            bn=$(basename "$f")
            case "$bn" in "$want") ;; *) continue ;; esac
            printf '\033[?25h' 2>/dev/null || true
            exec "$f" "$@"
        done
    done
    echo "app: tidak ditemukan: $1" >&2
    return 1
}

# ------------------------------------------------------------------ KUMPULKAN
# Daftar direktori yang dicurigai berisi aplikasi milik user.
APPDIRS=""
for d in "$HOME_DIR/.local/bin" \
         "$HOME_DIR/.opencode/bin" \
         "$HOME_DIR/.claude/local" \
         "$HOME_DIR/.hermes/hermes-agent/.hermes/bin" \
         "$HOME_DIR/.hermes/tools" \
         /usr/local/bin /opt; do
    APPDIRS="$APPDIRS $d"
done
# .hermes/tools/*/bin (node, python, uv, ripgrep) dan /opt/*/bin
for d in "$HOME_DIR"/.hermes/tools/*/bin /opt/*/bin; do
    [ -d "$d" ] && APPDIRS="$APPDIRS $d"
done

# Mode jalankan: `app <nama> [args...]` (harus setelah APPDIRS terisi).
if [ $# -gt 0 ]; then
    run_app "$@"
    exit $?
fi

min_bytes=$(( APP_MIN_MB * 1024 * 1024 ))
seen=""
rows=""
count=0

for d in $APPDIRS; do
    [ -d "$d" ] || continue
    for f in "$d"/*; do
        [ -f "$f" ] || continue
        [ -x "$f" ] || continue
        bn=$(basename "$f")
        # buang symlink (applet busybox) & duplikat nama
        [ -L "$f" ] && continue
        case " $seen " in *" $bn "*) continue ;; esac
        # buang alat sistem supaya tabel berisi aplikasi, bukan utilitas
        case "$bn" in
            sh|ash|bash|busybox|apk|cat|ls|cp|mv|rm|ln|chmod|chown|mkdir|\
            grep|sed|awk|sort|head|tail|wc|tr|find|xargs|tar|gzip|gunzip|\
            ps|kill|sleep|date|env|which|id|whoami|echo|printf|test|expr|\
            touch|ln|readlink|realpath|basename|dirname|mktemp|nl|tee|od|dd|\
            true|false|yes|sync|seq|strings|file|stat|df|du|free|uptime|hostname|\
            clear|reset|stty|script|uname|arch|who|id) continue ;;
            *.so|*.so.*|*.a|*.o|*.py|*.pl|*.sh) continue ;;
        esac
        # hanya yang "besar"
        sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$sz" -ge "$min_bytes" ] || continue
        seen="$seen $bn"
        # versi: jalankan --version dengan timeout pendek, ambil baris pertama
        ver=$(timeout 8 "$f" --version 2>/dev/null | head -1 | tr -d '\r')
        [ ${#ver} -gt 48 ] && ver=$(printf '%s…' "$(printf '%s' "$ver" | cut -c1-47)")
        [ -n "$ver" ] || ver="-"
        case "$ver" in
            *[A-Za-z]*[0-9]*|*[0-9]*) : ;;
            *) ver="-" ;;
        esac
        short=$(printf '%s' "$f" | sed "s|^$HOME_DIR||")
        rows="$rows$bn|$ver|$(( sz / 1024 / 1024 )) MB|$short
"
        count=$(( count + 1 ))
    done
done

# ------------------------------------------------------------------ TAMPILKAN
printf '\n%s  Aplikasi di wadah%s  %s(dipindai otomatis, ambang %s MB)%s\n\n' \
    "$G" "$N" "$D" "$APP_MIN_MB" "$N"

if [ "$count" -eq 0 ]; then
    printf '  %s(tidak ada aplikasi besar terdeteksi)%s\n' "$D" "$N"
    printf '  %spasang di dalam wadah, mis. taruh biner di /usr/local/bin%s\n\n' "$D" "$N"
else
    printf '  %s●%s  %-14s %-26s %8s  %s%s%s\n' "$G" "$N" "NAMA" "VERSI" "UKURAN" "$O" "LOKASI" "$N"
    printf '%s' "$rows" | while IFS='|' read -r bn ver sz path; do
        [ -n "$bn" ] || continue
        printf '  %s●%s  %s%-14s%s %-26s %8s  %s%s%s\n' \
            "$G" "$N" "$G" "$bn" "$N" "$ver" "$sz" "$O" "$path" "$N"
    done
    printf '\n  %s%d aplikasi terpasang%s\n' "$D" "$count" "$N"
fi

# ringkasan wadah
apk_n=$(apk info 2>/dev/null | wc -l)
alpine=$(cat /etc/alpine-release 2>/dev/null)
if [ -n "$alpine" ]; then
    printf '  %sAlpine %s · %s paket apk%s\n\n' "$D" "$alpine" "$apk_n" "$N"
else
    printf '\n'
fi