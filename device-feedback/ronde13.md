# Device Feedback — Ronde 13 (default port wadah di bootstrap, HANDOFF §32)

Tanggal: 2026-10-08 · Basis: `bee7471` (§32 bootstrap tulis `{ "port": 49474 }`
hanya bila config belum ada)

**Verdict: 3/3 tugas hijau. Saran default port (insiden ronde 12 §8) terpasang
persis seperti didesain — config user tak pernah ditimpa.**

---

## 1. Tugas 1 — install + config lama TIDAK berubah

- `git pull` → RC=0 (`dae651a..bee7471`).
- `./install.sh` → **RC=0**, `PT_INTERP: 23 file, 0 gagal`, quick-test hijau.
- Config saya (`port: 49474` + password, md5 `a9df1a3580dd3159183f636f18846b16`)
  **identik sebelum/sesudah** — log persis:

  ```
  [+] opencode: config service sudah ada, dibiarkan (/data/.../root/.config/opencode/service.json)
  ```

## 2. Tugas 2 — jalur instalasi BARU (config dipindah sementara)

- Backup `service.json` (md5 tercatat) → hapus → `./install.sh` → **RC=0** →
  file dibuat bootstrap berisi persis:

  ```
  { "port": 49474 }
  ```

  dengan log `[+] opencode: default port wadah di-set 49474 (hindari bentrok
  49374 milik host)` ✓ — bukan 49374.
- Config asli **dikembalikan persis** (md5 `a9df1a...` sama, mode 600).

## 3. Tugas 3 — asap singkat

| Tes | Hasil |
|---|---|
| `alpine` → `opencode` | ✅ TUI render (RC=124; daemon 49474 sudah hidup → connect, tanpa error) |
| `./selftest` | ✅ **0 FAIL** (`akhir: DB tetap bersih`) |

## 4. Catatan kejujuran (2 pengamatan kecil)

1. **Count `PT_INTERP 24 → 23 file`:** bukan regresi — daemon opencode yang
   saya tinggal hidup utk user (insiden ronde 12 §8) membuat binary-nya
   **ETXTBSY** saat install: log `root/.opencode/bin/opencode: busy (sedang
   dieksekusi) — dilewati` (rc=4, memang didesain "bukan kegagalan").
   INTERP binary itu sudah terpasang dari install sebelumnya → fungsi nol.
   Edge case sah: install saat service berjalan melewatkan binary itu (ok bila
   sudah pernah di-patch; pada install SANGAT awal binary tak mungkin sedang
   jalan). Cukup dicatat.
2. Tugas 2 memindahkan config **sementara** sambil daemon tetap jalan — tidak
   berdampak (daemon sudah membuka port-nya); file asli utuh kembali.

## 5. Kejujuran & lingkungan

- Lingkungan tak berubah dari awal ronde: config **49474** (asli saya,
  dipulihkan utuh), daemon user tetap hidup (HTTP 200), host service 49374
  utuh (401), backup temporary sudah dipindahkan kembali (tak ada sisa).
- Log: `install-r13.log`, `install-r13b.log`, `r13-alpine-opencode.log`
  (di `~/files/usr/tmp/opencode/`).

## 6. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| RC=0 + config lama tak berubah ("config service sudah ada, dibiarkan") | ✅ terverifikasi (md5 identik) |
| Jalur baru: config dipindah → install → file `{ "port": 49474 }` → balikin asli | ✅ terverifikasi (log `default port wadah di-set 49474`, asli utuh) |
| Asap: alpine→opencode TUI + selftest 0 FAIL | ✅ keduanya |
