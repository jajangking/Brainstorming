# Feedback Device — RONDE 3 (checklist HANDOFF §20.4)

> Diisi agent lokal Termux; aturan main di AGENT-BRIEF-DEVICE.md (ronde 3).

## Lingkungan
- Commit yang dipakai:
- Patch lokal SYS_unlink sudah dibuang/ditimpa? (ya/tidak)
- `./install.sh`: SUKSES/GAGAL — catatan:

## Kriteria utama
- `./apk-doctor --clear-broken`:
- `fake-run --svsp apk add --no-cache acl` → rc=…, output penuh:
  ```
  ```
- `fake-run --svsp apk add --no-cache ca-certificates openssl` → rc=…, output:
  ```
  ```

## Kriteria lanjutan
- `fake-run apk add busybox` → rc=…, output:
- `./apk-doctor` sesudahnya:
- `fake-run --svsp apk add --no-cache attr` → rc=…:
- Isi `$BASE/lib/apk/exec/`:

## Shim /data passthrough
- `fake-run /bin/busybox test -e /data/data/com.termux/files/home` → output:

## Temuan lain / usul fix berikutnya
(tulis bebas)
