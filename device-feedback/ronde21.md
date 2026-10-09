# Ronde 21 — `npm install -g` di dalam wadah (2026-10-09)

## Keluhan

Dari shell Alpine:

```
npm install -g 9router
npm error code ENOENT
npm error syscall mkdir
npm error path /usr/local/lib
```

## Diagnosis

`/usr/local/lib` **ada** di wadah — jadi ini bukan direktori hilang. Baca stack
di `~/.npm/_logs/*.log`:

```
12 verbose stack Error: ENOENT: no such file or directory, mkdir '/usr/local/lib'
12 verbose stack     at async mkdir (node:internal/fs/promises:859:10)
12 verbose stack     at async Arborist.reify (.../@npmcli/arborist/lib/arborist/reify.js:106:7)
```

Dua lapis bug distro menumpuk:

### §48 — syscall mentah Node tak bisa di-interpose shim

`strace` saat `npm install -g`:

```
mkdirat(AT_FDCWD, "/data", 0777)          = -1 EEXIST
mkdirat(AT_FDCWD, "data", 0777)           = -1 EEXIST
mkdirat(AT_FDCWD, "com.termux", 0777)     = -1 EEXIST
...
```

Node/Bun memanggil syscall lewat **libuv**, bukan libc — jadi shim
`LD_PRELOAD` (`libfakeroot.so`) tidak pernah dipanggil (0 referensi
`libfakeroot` di trace). Path absolut pun tidak di-rewrite, sehingga
`mkdir("/usr/local/lib")` jatuh ke filesystem **host**, tempat `/usr/local`
tidak ada → ENOENT.

Node memang biner dinamis, jadi `fake-run` memilih mode `ldpreload`. Mode itu
benar untuk program libc (busybox, apk, bash), salah untuk runtime raw-syscall.

### §47 — `passthrough()` kehilangan padanan tanpa garis miring

Setelah §48, error bergeser ke `lstat '/data'`. Penyebabnya di `svsp.c`:

```c
|| !strncmp(p, "/data/", 6)     /* ada */
|| (baselen && strncmp(p, base, baselen) == 0)
```

Path persis `/data` (tanpa garis miring) **tidak** kena passthrough → di-rewrite
jadi `$BASE/data` → ENOENT. Node memanggil `lstat("/data")` sebagai komponen
pertama `fs.realpathSync` (`node:internal/modules/helpers:63`), jadi satu
karakter yang hilang ini menggagalkan seluruh `npm install -g`.

## Fix

| Berkas | Perubahan |
|---|---|
| `fake-run` | daftar `node*`/`bun`/`deno`/`uv`/`uvx`/`nodejs` → `MODE=svsp` |
| `alpine` | shell interaktif, `-c`, dan passthrough memakai `fake-run --svsp` |
| `svsp.c` | `passthrough()` += padanan tanpa garis miring: `/data`, `/apex`, `/vendor`, `/product`, `/linkerconfig` |
| `selftest` §8 | 4 check baru (§47 ×2, §48 ×2) |

Supervisor bukan sekadar lebih benar di sini, tapi juga **lebih cepat** di
device: rata-rata 285 ms (svsp) vs 372 ms (shim) per boot `sh -c`, 3×
berturut-turut.

## Verifikasi

```
$ alpine -c 'cd $HOME; npm install -g 9router'
added 11 packages in 1m
$ alpine -c '9router --version'
0.5.99
$ ./selftest
== RINGKASAN: 0 FAIL ==
```

Sabotage check: menghapus `|| !strcmp(p, "/data")` dari `svsp.c` → selftest
melaporkan **3 FAIL** (§47 dua check, §48 satu check fungsional). Setelah
dipulihkan → 0 FAIL.

## Jebakan yang sempat menyesatkan

- **`mkdir`/`mkdirat` sudah ada** di `libfakeroot.c` sejak awal. Menambahkannya
  lagi hanya menghasilkan `redefinition`. Gejalanya bukan "shim tak punya
  mkdir", tapi "shim tak pernah menyentuh syscall Node".
- **Wrapper `/usr/bin/npm` kelihatan memperbaiki** cowsay, tapi hanya menutupi
  §47. Setelah §47 diperbaiki, symlink asli `npm` juga jalan — wrapper
  dibuang agar tak perlu pemeliharaan.
- **Heredoc `<<EOF` tanpa kutip** Expansion `$VAR` merusak berkas tujuan.
  Sempat menimpa `9router/cli.js` (30 KB) jadi 518 B; dipulihkan dengan
  reinstall bersih. Selalu `'EOF'` bila isi tak boleh di-expand.
- Path `/data/.../alpine-rootfs/...` muncul di stack trace Node karena
  Node me-realpath-kan modul utama. Itu konsekuensi fake-chroot (bukan chroot
  sungguhan), bukan kebocoran yang perlu dikoreksi.

## Jejak sampingan

- `9router` butuh satu kali `npm install -g` (~50–60 detik di jaringan seluler),
  lalu jalan normal. Postinstall-nya (SQLite/tray warm-up) ikut berhasil.
- `npm install -g` dipakai juga untuk `cowsay` sebagai paket uji cepat
  (11–41 paket, ~5 detik).