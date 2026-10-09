# Hermes Agent di wadah Alpine

Panduan pengguna. Untuk detail teknis (akar masalah, nomor §, strace),
lihat `HANDOFF.md §41` dan `device-feedback/ronde21.md`.

## Pasang baru (sekali saja)

```bash
# 1. Wadah + tool (di Termux). Sudah punya? lewati.
cd ~/Brainstorming && ./install.sh

# 2. Installer resmi Hermes (di dalam shell wadah).
alpine
curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash -- --non-interactive
# ^ AKAN error di akhir ("Building the hermes command and apps failed").
#   Itu NORMAL di Android — lanjut langkah 3, jangan install ulang.

# 3. Finishing (di Termux, ~10-20 menit).
hermes-setup
# Tunggu sampai tulis SELESAI.
```

## Pakai harian

```bash
alpine
hermes            # TUI. Perintah lain: hermes --version, hermes update, hermes model
```

`hermes` langsung bisa dipanggil karena `alpine` menambahkan
`~/.hermes/hermes-agent/.hermes/bin` ke PATH tiap shell baru.

## Kalau error

| Gejala | Arti | Aksi |
|---|---|---|
| `Building the hermes command and apps failed` / `app products or command publication failed` (akhir installer) | Ekspektasi di Android (biner Go `lefthook` + build web mati) | Jalankan `hermes-setup`, tunggu SELESAI |
| `hermes: command not found` di shell | Shell lama (sebelum fix PATH) | Keluar shell, masuk `alpine` lagi |
| `hermes update` tulis `did not finish` | Sisa tail tertunda | Jalankan `hermes-setup` |
| `hermes update` tulis `already running` | Lock basi dari run yang mati | `hermes-setup` membersihkannya bila aman; kalau membandel, laporkan |
| `hermes-setup` sendiri gagal >3 ronde | Kasus baru | Copy-kan blok `[1-5]` yang merah + 10 baris akhir |

## Pantangan

- **Jangan hapus `~/alpine-rootfs/root/.hermes`** untuk "install bersih".
  Itu menghancurkan venv + tools (ratusan MB) dan memaksa semuanya dari nol.
  Semua perbaikan di bawah ini inkremental.
- **Jangan patch file di `~/.hermes/hermes-agent`** (repo git mereka).
  Updater mengecek tree bersih; file ubahan bikin update gagal permanen.
- **Jangan `npm ci` manual di repo Hermes.** Selalu lewat `hermes-reinstall`
  (ia menyembunyikan `.git` supaya `prepare` skip `lefthook`, lalu menulis
  receipt agar installer bisa reuse).

## Skrip bantu (semua di `$PREFIX/bin`, ikut `bootstrap.sh`/`uninstall.sh`)

| Skrip | Guna |
|---|---|
| `hermes-setup` | Orkestrator: update → deps → web → TUI → tail + retry. Satu perintah finishing. |
| `hermes-reinstall` | Tulis ulang Node deps via `node-deps.mjs --reuse` (aman lefthook). |
| `hermes-web-build` | Build `web_dist` 2 fase (typecheck, lalu vite) + publish + record. |
| `hermes-nodeps-driver.py`, `web-split.mjs` | Driver untuk dua skrip di atas. |

## Batas yang diketahui (bukan bug, jangan dilaporkan sebagai bug distro)

- `networkInterfaces()` hanya lapor `lo`. NETLINK_ROUTE diblokir kernel
  Android; fallback loopback disengaja. Dashboard tulis `localhost`, bukan
  IP LAN — akses dari device lain pakai IP WiFi manual.
- Setiap `hermes update` yang menyentuh Node deps butuh `hermes-setup`
  sekali sesudahnya (receipt harus ditulis ulang untuk tree baru).
- Biner Go statis yang memanggil `exec.LookPath` (atau `eaccess`) mati
  SIGSYS di Android. Bukan efek wadah — terbukti juga di host Termux.
