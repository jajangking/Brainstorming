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
            bn=$(basename "$f")
            case "$bn" in "$want") ;; *) continue ;; esac
            # symlink: ikuti ke target (kecuali applet busybox)
            if [ -L "$f" ]; then
                t=$(readlink -f "$f" 2>/dev/null)
                case "$t" in */busybox|*/bin/busybox) continue ;; esac
                [ -n "$t" ] && [ -x "$t" ] || continue
                f="$t"
            fi
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
seen_ino=""
rows=""
count=0

# ------------------------------------------------------------------ versi
# Menjalankan `--version` itu mahal: tiap probe = satu proses (lewat supervisor
# kalau di dalam wadah). Karena itu hasilnya dicache di
# $HOME_DIR/.cache/app-versions, key = "path size mtime". `app` kedua kali
# berjalan hampir instan, dan probing hanya terjadi untuk file yang berubah.
CACHE="$HOME_DIR/.cache/app-versions"
mkdir -p "$HOME_DIR/.cache" 2>/dev/null || true

ver_of() { # <bin> -> baris versi pertama (kosong bila gagal/tak diketahui)
    f="$1"
    st=$(stat -c '%s:%Y' "$f" 2>/dev/null) || return 1
    key="$f|$st"
    if [ -f "$CACHE" ]; then
        # Tanpa -x: baris cache adalah "<path>|<size>:<mtime>|<versi>", jadi
        # pencocokan harus awalan (prefix), bukan seluruh baris.
        hit=$(grep -F -m1 -- "$key|" "$CACHE" 2>/dev/null | head -1)
        if [ -n "$hit" ]; then
            # buang key "<path>|<size>:<mtime>|" -> sisakan versinya
            printf '%s' "${hit#"$key|"}"
            return 0
        fi
    fi
    # Cache miss. Menjalankan biner mahal (lewat supervisor = proses ekstra),
    # jadi probing hanya atas permintaan: APP_PROBE=1 app. Tanpa itu versi
    # kosong dibiarkan — tabel tetap akurat nama/ukuran/lokasi, dan hanya
    # kolom VERSI yang belum terisi.
    [ "${APP_PROBE:-0}" = 1 ] || return 0
    out=$(timeout 8 "$f" --version </dev/null 2>/dev/null | head -1 | tr -d '\r')
    printf '%s|%s\n' "$key" "$out" >> "$CACHE" 2>/dev/null || true
    printf '%s' "$out"
    return 0
}

for d in $APPDIRS; do
    [ -d "$d" ] || continue
    for f in "$d"/*; do
        [ -f "$f" ] || continue
        [ -x "$f" ] || continue
        bn=$(basename "$f")
        # --- symlink ---
        #  * symlink ke busybox (applet)     -> buang
        #  * symlink ke file >= ambang        -> ikut (ukuran = target)
        #  * symlink ke file kecil (launcher) -> ikut bila target-nya
        #   _skrip yang memanggil app besar_;| follow 1-2 tingkat
        #    lalu cari executable >= ambang di path yang dirujuk skrip.
        # PENTING: tgt WAJIB di-set untuk file biasa juga. Kalau hanya di
        # dalam cabang symlink, iterasi berikutnya mewarisi tgt lama sehingga
        # stat/size/inode dibaca dari file yang SALAH (device 2026-10-09:
        # opencode & node hilang dari daftar).
        if [ -L "$f" ]; then
            tgt=$(readlink -f "$f" 2>/dev/null)
            case "$tgt" in
                */busybox|*/bin/busybox) continue ;;
                *) [ -n "$tgt" ] && [ -x "$tgt" ] || continue ;;
            esac
            tsz=$(stat -c %s "$tgt" 2>/dev/null || echo 0)
            [ "$tsz" -ge "$min_bytes" ] || continue     # symlink kecil: bukan app
        else
            tgt="$f"
        fi
        # dedupe by inode: python/python3/python3.14 = file yang sama
        ino=$(stat -c %i "$tgt" 2>/dev/null || echo "$f")
        case " $seen_ino " in *" $ino "*) continue ;; esac
        seen_ino="$seen_ino $ino"
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
            # helper turunan, bukan aplikasi mandiri
            *-acp|*-mcp|*-harness|*-agent|*-gen-src) continue ;;
            # alias berefiks angka (opencode2, node2) bukan app baru
            *[0-9]) continue ;;
            # helper interpreter & alat kecil lainnya
            pip|pip[0-9]*|idle|idle[0-9]*|*-config|pyvenv.cfg|python3-dbg) continue ;;
        esac
        sz=$(stat -c %s "$tgt" 2>/dev/null || echo 0)
        # Ukuran >= ambang -> pasti aplikasi (mis. opencode 195 MB).
        if [ "$sz" -lt "$min_bytes" ]; then
            # Yang kecil (launcher/venv ~100 B) hanya diterima kalau ada di
            # direktori milik user (~/.local/bin, ~/.opencode/bin, ...): di
            # sana isinya memang aplikasi, sedangkan /usr/local/bin & /opt
            # bisa berisi utilitas kecil yang tak terkait.
            case "$f" in
                "$HOME_DIR"/*) : ;;
                *) continue ;;
            esac
        fi
        seen="$seen $bn"
        ver=$(ver_of "$tgt")
        # rapikan: buang kontrol char, potong sesuai " · " / " / "
        ver=$(printf '%s' "$ver" | sed 's/[[:cntrl:]]//g; s/[[:space:]]*$//')
        case "$ver" in
            *" · "*) ver=$(printf '%s' "$ver" | sed 's/ *·.*$//') ;;
            *' / '*)  ver=$(printf '%s' "$ver" | sed 's| *\/.*$||') ;;
        esac
        [ ${#ver} -gt 34 ] && ver=$(printf '%s…' "$(printf '%s' "$ver" | cut -c1-33)")
        # Versi kosong = belum pernah di-probe. APP_PROBE=1 akan mengisinya.
        [ -n "$ver" ] || ver="—"
        short=$(printf '%s' "$f" | sed "s|^$HOME_DIR||")
        # dedupe lintas nama: versi identik + basename mirip = app yang sama
        # (mis. opencode & opencode2, hermes & hermes-acp)
        if printf '%s' "$rows" | cut -d'|' -f2 | grep -qxF "$ver"; then
            base_a=$(printf '%s' "$bn" | sed 's/[0-9]*$//')
            dup=0
            for r in $(printf '%s' "$rows" | cut -d'|' -f1); do
                [ "$(printf '%s' "$r" | sed 's/[0-9]*$//')" = "$base_a" ] && dup=1 && break
            done
            [ "$dup" = 1 ] && continue
        fi
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