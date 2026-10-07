# Feedback Device — RONDE 2 (ceklist HANDOFF §19.4)

> Diisi agent lokal Termux; aturan main di AGENT-BRIEF-DEVICE.md (ronde 2).

## Lingkungan
- `git pull` commit terakhir yang dipakai:
- `./install.sh`: SUKSES/GAGAL — catatan:

## 1. Trigger svsp rc=0?
- `./apk-doctor --clear-broken` (pra-kondisi):
- `fake-run --svsp apk add --no-cache acl` → rc=…, output:
  ```
  ```
- `fake-run --svsp apk add --no-cache ca-certificates openssl` → rc=…, output:
  ```
  ```
- Isi `$BASE/lib/apk/exec/` sesudahnya:

## 2. "1 error" tidak kembali
- `fake-run apk add busybox` → rc=…, output:
- `./apk-doctor` sesudahnya:

## 3. Regresi fake-run
- `fake-run /bin/busybox sh -c 'echo SBARGS-ok'` → output:
- Uji trigger manual via fake-run (bila sempat): 

## Temuan lain / pesan aneh
(tulis bebas)

## Usul fix berikutnya
(tulis bebas)
