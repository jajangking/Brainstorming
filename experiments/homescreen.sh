#!/data/data/com.termux/files/usr/bin/bash
# homescreen.sh — DEPRECATED shim (eksperimen).
# Implementasi tunggal sekarang di homescreen.mjs (Node, ala opencode).
# Shim ini hanya meneruskan argumen agar perintah lama tetap jalan.
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/homescreen.mjs" "$@"
