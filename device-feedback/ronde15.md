# Device Feedback — Ronde 15 (handler SIGSYS tahan ditimpa, HANDOFF §35)

Tanggal: 2026-10-08 · Basis: `2ae7356` (§35 `sigaction()`/`signal()` SIGSYS di-interpose)

**Verdict: §35 BEKERJA — akar temuan ronde 14 tuntas. Repro minimal saya kini
hijau, dan `Bad system call` di installer Hermes HILANG.** Tanpa regresi.

---

## 1. Tugas 1 — install

- `git pull` → RC=0 (`38e5ecc..2ae7356`, §35).
- `./install.sh` → **RC=0** (`PT_INTERP: 24 file, 0 gagal`); `~/libfakeroot.so`
  di-rebuild (`12:23`), config `port 49474` dibiarkan. Log: `install-r15.log`.

## 2. Tugas 2 — regresi (hijau)

| Tes | Hasil |
|---|---|
| `./selftest` | ✅ **0 FAIL** |
| `alpine` → `opencode` | ✅ **TUI render** (daemon 49474 connect, RC=124) |
| `apk add tree` → pakai → `apk del tree` | ✅ add RC=0 (GNU tree 2.3.2), del RC=0, `apk info -e tree` **rc=1** (bersih; `/usr/bin/tree` kembali jadi symlink busybox bawaan) |

## 3. Tugas 3 — repro minimal saya (dua-duanya FIXED ✅)

```
alpine -c 'set -u; trap ":" EXIT; echo "[$(uname -s)]"'   ->  [Linux]
alpine -c 'trap ":" EXIT; x=$(env); echo ${#x}'           ->  601   (> 0)
```

Ronde 14 keduanya kosong; kini terisi. **Akar yang saya laporkan resmi beres.**

## 4. Tugas 4 — Hermes DENGAN shim: `Bad system call` HILANG ✅

```
alpine -c 'bash /tmp/hermes-install.sh'
┌─────────────────────────────────────────────────────────┐
│             ☤ Hermes Agent Installer                    │
│         An open source AI agent by Nous Research.       │
└─────────────────────────────────────────────────────────┘

✗ git is required. Install it with your system package manager.
```

- **Tidak ada lagi** `unsupported platform: ` maupun `Bad system call` (RC 159).
- Script kini lolos `check_platform` dan sampai **prasyarat**: berhenti wajar di
  `git is required` — murni **lingkungan** (git belum dipasang di wadah), bukan bug.
- Deteksi Termux **tidak** muncul di jalur `alpine`/`fake-run` — karena env
  whitelist `env -i` fake-run tidak meneruskan `TERMUX_VERSION`/`PREFIX` host ke
  wadah; jadi installer tidak "melihat" Termux. (Di ronde 14 ia muncul hanya saat
  saya bypass manual dari host lalu meng-override `PREFIX`; itu memang esensi
  uji "tanpa shim".)
- Uji lanjut sampai git di-`apk add` → clone/uv/Python **belum** dilakukan
  (di luar cakupan brief; dicatat sebagai langkah opsional berikutnya).

## 5. Tugas 5 — jawaban: **apakah saya memakai svsp untuk program dinamis ber-`trap`?**

**Jujur: TIDAK.** Di perangkat ini:
- Program **dinamis** (bash, opencode, apk, dll.) selalu lewat jalur **shim
  (libfakeroot/LD_PRELOAD)** — itulah model default `alpine`/`fake-run`.
- `svsp` saya pakai untuk **binary statis** (dan `fake-run --svsp` sesekali
  sebagai eksperimen). Uji `--svsp` pada kasus `trap` di ronde 14 murni
  penasaran, bukan pemakaian nyata.

→ Karena itu saya **tidak** menyarankan menambah komponen "sigsys-guard" di jalur
svsp demi kasus ini. Manfaatnya minim untuk pemakaian riil; lebih baik energi
diarahkan ke kasus dinamis lain yang benar-benar muncul.

## 6. Tugas 6 — perilaku yang berubah karena interposisi sigaction SIGSYS

Tidak ada regresi teramati sejauh ini: `selftest` 0 FAIL, `alpine`→`opencode`
TUI render, `apk add/del` normal, dan justru Hermes kini maju. Satu-satunya
konsekuensi teoretis (aplikasi yang membaca balik handler SIGSYS akan melihat
"miliknya tercatat" walau kernel sebenarnya milik shim) belum memicu masalah di
program yang saya uji. Akan saya laporkan bila muncul.

## 7. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| Regresi selftest + opencode TUI + apk add/del | ✅ 3/3 |
| `trap EXIT; $(uname -s)` → `[Linux]` | ✅ |
| `trap EXIT; $(env)` → panjang > 0 | ✅ (601) |
| Hermes: `Bad system call` hilang | ✅ (berhenti wajar di `git is required`) |
| Jawab pemakaian svsp | ✅ (tidak dipakai utk dinamis ber-trap) |
| Catat perubahan perilaku | ✅ (nihil) |

## 8. Lingkungan

Port tetap **49474** (baseline), host 49374 utuh (401). `/usr/bin/tree` kembali ke
symlink busybox bawaan (apk del bersih). Hermes di `/tmp` wadah (sementara).
Log: `install-r15.log`, `r15-alpine-opencode.log`, `hermes-r15.log`. `strace`
(sisa r14) masih terpasang di wadah — akan di-`apk del` bila Anda mau wadah bersih.