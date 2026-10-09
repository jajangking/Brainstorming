# experiments — eksperimen sekali pakai (bukan bagian runtime)

Berkas di sini adalah alat uji/eksplorasi yang tidak dipakai alur utama
(`bootstrap.sh` / `fake-run` / `alpine` / `app`) dan tidak dirujuk dokumen
utama. Disimpan sebagai catatan cara eksplorasi, bukan kode produksi.

| Berkas | Isi |
|---|---|
| `raw-rt-sig.c` | uji `rt_sigaction` langsung (memetakan handler tanpa libc) |
| `unshare-matrix.c` | matriks uji `unshare()` + namespace di Android |

Kalau suatu saat eksperimen ini dipakai serius, pindahkan ke root dan sebut
di `HANDOFF.md`.

Kalau suatu saat eksperimen ini dipakai serius, pindahkan ke root dan sebut
di `HANDOFF.md`.