# RONDE 18 — svsp MENUTUP celah deteksi libc biner statis (uv)

## Jawaban atas pertanyaan Anda: **YA.**

Deteksi libc **lolos tanpa `UV_LIBC`** di jalur svsp. Hipotesis Anda benar: svsp
menerjemahkan path di tingkat syscall, sehingga uv (biner statis yang kebal
LD_PRELOAD) membaca **`/bin/sh` wadah** — bukan `/bin/sh` host Android — lalu
menjalankan `ld-musl-patched.so.1` yang banner versinya sudah benar (§38.1).

## Bukti

### 1. `fake-run --svsp uv python list` → jalan, deteksi libc lolos
```
$ ./fake-run --svsp "$UV" python list
cpython-3.14.7-linux-aarch64-musl              <download available>
cpython-3.14.7+freethreaded-linux-aarch64-musl <download available>
... (9 interpreter terdaftar)
```
Tidak ada lagi `Failed to determine the libc`. Bandingkan ronde 17 (jalur shim,
tanpa `UV_LIBC`): **gagal**. Selisihnya murni jalur eksekusi.

### 2. `env -u UV_LIBC fake-run --svsp uv python install 3.14 -v` → deteksi LOLOS, unduh jalan, lalu GAGAL DI EKSTRAKSI
```
DEBUG uv 0.12.3 (aarch64-unknown-linux-musl)
DEBUG Found download `cpython-3.14.7-linux-aarch64-musl` ...
Downloading cpython-3.14.7-linux-aarch64-musl (download) (27.9MiB)
error: Failed to install cpython-3.14.7-linux-aarch64-musl
  Caused by: Failed to extract archive: cpython-...tar.gz
  Caused by: I/O operation failed during extraction
  Caused by: failed to canonicalize path
    `/data/.../alpine-rootfs/root/.local/share/uv/python/.temp/.tmpR1tCcV`:
    No such file or directory (os error 2)
```
- **Deteksi libc: LOLOS** ✅ (tidak ada lagi error libc).
- **Unduh: LOLOS** — kebetulan DNS sehat ronde ini (27.9 MiB terunduh; di ronde
  16–17 DNS `EAI_AGAIN`).
- **Gagal baru, bukan soal libc:** saat ekstraksi, `fs::canonicalize()` Rust atas
  path **host** (`$BASE/root/.local/share/uv/python/.temp/.tmpXXXX`) → ENOENT.

## Temuan sampingan (reproduksi mini `realpath`)

Saya bangun binary musl-statis kecil yang memanggil `realpath()` lalu menjalankannya
lewat svsp:
```
svsp: realpath("/tmp")                = /tmp                 ✅
svsp: realpath("<path host absolut>") = NULL errno=2 (ENOENT)
shim: realpath("/tmp")                = /tmp                 ✅
```
Jadi `realpath`/canonicalize di svsp **hanya menerima path "wadah"**, bukan path
host absolut. uv (Rust) membangun path absolut host lalu meminta `canonicalize`
atasnya → gagal. Konsisten dengan gejala ekstraksi di atas.

Catatan kehati-hatian: yang saya buktikan adalah `realpath` atas path host absolut
gagal di svsp. Gejala uv di atas **cocok** dengan itu, tapi saya belum memastikan
itu sebab tunggalnya (uv bisa saja menunda `canonicalize` sampai sesudah mkdir,
dsb.). Saya laporkan sebagai indikasi kuat, bukan akar pasti.

## Rekomendasi
- **Jalur produksi tetap: shim + `UV_LIBC=musl`** (sudah permanen di §39.2).
  Tidak berubah dari ronde 17.
- **Jangan** dialihkan ke svsp untuk uv: jalur shim sudah cukup, sedangkan svsp
  punya gap `canonicalize` untuk program statis berbasis Rust. Tidak saya
  sarankan svsp jadi jalur default untuk tool statis non-musl.

## Topik ditutup
Tidak ada tugas lain. Hermes yang tersangkut DNS tidak dikejar; ronde ini justru
membuktikan svsp bisa lolos unduhan saat DNS kebetulan sehat — konsisten dengan
sifat jaringan, bukan bug wadah.

---

## Ringkasan
| Item | Hasil |
|---|---|
| svsp menjalankan uv (static)? | ✅ ya |
| Deteksi libc **tanpa `UV_LIBC`** di svsp? | ✅ **lolos** (jawaban: ya) |
| Unduh Python via svsp? | ✅ 27.9 MiB (DNS sehat ronde ini) |
| Ekstraksi via svsp? | ❌ `canonicalize(path host)` ENOENT |
| Rekomendasi jalur produksi? | Tetap shim + `UV_LIBC=musl` |
| Loader diubah? | Tidak |

Artefak: `$PREFIX/tmp/opencode/r18-canon.c`, `r18-canon` (binary uji),
`r18-svsp-list.log`, `r18-svsp-install.log`.