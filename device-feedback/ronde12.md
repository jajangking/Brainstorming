# Device Feedback — Ronde 12 (verifikasi fix FAKEROOT_EXE basi / nested, HANDOFF §30)

Tanggal: 2026-10-08 · Basis: `a6a628b` (§30 `fk_self_exe`: kebenaran per-proses
dulu — `/proc/self/exe` bila bukan loader → `cmdline[1]` → env FAKEROOT_EXE
cadangan terakhir)

**Verdict: FIX TERVERIFIKASI — semua tugas hijau, tak ada satu pun kegagalan
(kontrol A/B ronde 11 tidak diperlukan). Jalur pemakaian manusia yang dulu
gagal kini jalan penuh.**

---

## 1. Build (tugas 1)

- `git pull` → RC=0 (`25f36ea..a6a628b`, §30).
- `./install.sh` → **RC=0**: `PT_INTERP: 24 file, 0 gagal`, quick-test hijau
  (`Test dinamis: 3.24.2`, `Isolasi: EACCES (benar)`).
  Log: `~/files/usr/tmp/opencode/install-r12.log`.

## 2. Reproduksi ronde 11 TANPA workaround (tugas 2)

`env -u LD_PRELOAD fake-run /bin/sh -c '$HOME/.opencode/bin/opencode </dev/null'`
(didahului port 49474 dibuktikan 000 — 49374 dipakai host service sesi ini):

```
RC=124 (timeout mematikan TUI sehat)
TUI render: 1
tanpa satu pun baris "can't open 'serve'"/Error
49474 sesudah: 200 (anak spawn sendiri)
```

→ **Fix §30 bekerja**: error `can't open 'serve'` exit 2 hilang total.
(Ronde 11: RC=1, TUI 0, error identik.) Log: `r12-tugas2.log`.

## 3. Jalur pemakaian manusia (tugas 3) — yang dulu tak pernah diuji

`alpine` = login shell (`exec fake-run /bin/sh -l` → baca `.profile`) —
beda dgn `alpine -c` (non-login: `command -v opencode` **tidak ada** di PATH;
interaktif menemukannya di `$HOME/.opencode/bin/opencode` — inilah yang
dipakai user di paste ronde 11).

| Emulasi (stdin pipe = "ketik di prompt") | Hasil |
|---|---|
| `alpine` → `command -v opencode` | ✅ `$HOME/.opencode/bin/opencode` |
| `alpine` → `opencode` (bare, port 000→200) | ✅ **TUI render**, RC=124, tanpa error |
| `alpine` → `opencode --standalone` | ✅ **TUI render**, RC=124 |

Log: `r12-alpine-bare.log`, `r12-alpine-standalone.log`.
(Catatan jujur: emulasi via pipe, bukan TTY interaktif sungguhan — TUI tetap
ter-render ke stdout, kriteria "render ya/tidak" terpenuhi.)

## 4. Nested dua tingkat + nilai (tugas 4)

**Nilai dalam shell** (untuk SHELL = jawaban benar):

```
fake-run /bin/sh -c 'echo $FAKEROOT_EXE; sh -c "echo $FAKEROOT_EXE; readlink /proc/self/exe"'
FAKEROOT_EXE = $B/bin/sh                      ← shell dirinya sendiri ✓
sh -c  → FAKEROOT_EXE = $B/bin/sh             ← sh2 ✓ (untuk sh2 sendiri)
         readlink /proc/self/exe = $B/bin/busybox  ← kebenaran per-proses ✓
```

**Nested dua tingkat** `shell → sh -c → opencode --standalone`:

```
RC=124, TUI render: 1, tanpa baris error
```

**Inspeksi /proc selagi opencode (pid 20097) hidup — kunci verifikasi §30:**

```
cmdline : $B/root/.opencode/bin/opencode --standalone
exe     : $B/root/.opencode/bin/opencode       ← KERNEL TRUTH = program asli ✓
env     : FAKEROOT_EXE=$B/bin/sh               ← MASIH BASI (warisan shell)
env     : LD_PRELOAD=$HOME/libfakeroot.so      ← shim aktif
```

→ Env `FAKEROOT_EXE` memang tidak bisa di-refresh dari parent (tetap `bin/sh`),
**tapi tidak masalah lagi**: `fk_self_exe()` kini memakai kebenaran per-proses
(exe = opencode) lebih dulu — persis desain §30. Spawn anak kini `command=`
program yang benar.

## 5. Regresi (tugas 5)

- `./selftest` → **0 FAIL** (RC=0, `akhir: DB tetap bersih`).
- Matriks singkat (protokol keadaan bersih: port dibuktikan 000 sebelum run):

| Kasus | svsp | shim |
|---|---|---|
| 3 bare spawn | ✅ TUI, RC=124 | ✅ TUI, RC=124, 0 baris error |
| 4 `--standalone` | ✅ TUI, RC=124 | ✅ TUI, RC=124, 0 baris error |

- Kontrol A/B ronde 11: **tidak dijalankan — tidak ada yang gagal** (sesuai
  instruksi brief bila ada kegagalan).

## 6. Kejujuran & lingkungan

- Port config dipindah 49374 → 49474 selama uji spawn (konflik dgn host
  service sesi ini, pola ronde 5–9), **dikembalikan ke 49374** (RC=0);
  49474 = 000; host service utuh (401); semua daemon uji di-kill (pid 18968,
  20532, 22098 + 1 daemon task 3) setelah port dibuktikan 200.
- Tidak ada file wadah yang diubah round ini.
- Emulasi "interaktif" = pipe stdin (bukan PTY asli) — dicatat di §3.
- Log mentah: `install-r12.log`, `r12-tugas2.log`, `r12-alpine-{bare,standalone}.log`,
  `r12-nested2.log`, `r12-bare-{svsp,shim}.log`, `r12-standalone-{svsp,shim}.log`.

## 7. Verdict vs target brief

| Sasaran | Hasil |
|---|---|
| Reproduksi ronde 11 tanpa workaround | ✅ TUI render, error hilang |
| Jalur manusia (`alpine` → opencode, lalu --standalone) | ✅ keduanya TUI render |
| Nested dua tingkat + nilai konsisten | ✅ TUI render; exe=program asli walau env basi (bukti prioritas §30) |
| Regresi selftest | ✅ 0 FAIL |
| Regresi matriks kasus 3 & 4 svsp+shim | ✅ 4/4 |

## 8. Insiden pasca-ronde 12 (pemakaian user, dilaporkan utk kronologi)

User menempel error baru: `Managed service port 49374 ... already in use`.
Ini **bukan bug shim** — justru bukti fix §30 bekerja: anak spawn kini hidup
sampai tahap bind port (dulu mati di `can't open 'serve'`). Penyebabnya konflik
lingkungan yang terdokumentasi sejak ronde 5: **49374 = service host opencode
(pid 6486, sesi agent ini), sementara config wadah selalu saya kembalikan ke
49374** setelah tiap uji → pemakaian default user pasti bentrok.

Tindakan lingkungan (bukan kode):

- `opencode service set port 49474` untuk wadah → **baseline permanen baru**:
  jalur user (`alpine` → `opencode`) diverifikasi TUI render, daemon hidup di
  49474 (HTTP 200), host 49374 utuh (401).
- **Protokol ronde berikutnya:** uji spawn tetap port 49474 (kini baseline,
  langkah "restore 49374" DIBATALKAN — mengembalikannya justru mengulang insiden).
- Catatan utk Arena (opsional): wadah & host opencode sama-sama default 49374 —
  di perangkat tempat host service berjalan, default wadah pasti bentrok;
  pertimbangkan default port berbeda saat bootstrap.
