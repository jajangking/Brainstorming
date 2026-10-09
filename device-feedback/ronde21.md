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

## Ronde 23 sisipan — 9router sampai dashboard hidup (§51/§52)

Keluhan awal: `9router` selalu `[Process completed (signal 9)]`. Empat bug
bertumpuk. Yang penting dicatat: **gejala sama, akar sama sekali berbeda.**

### §52 — `busybox lsof` mengabaikan `-t` dan `-i`

```
$ lsof -ti:49374                     # port TIDAK dipakai
18560	/data/.../bash	0	/dev/null    # ← baris penuh, bukan PID
```

App seperti 9router (`killProcessOnPort`) membaca output itu, ambil `parts[0]`
sebagai PID, lalu `kill -9 "18560\t..."` → argumen invalid → gagal diam-diam.
Karena `lsof -i` juga diabaikan, **selalu** dapat baris, jadi jalur ini selalu
mengeksekusi kode yang tak pernah berhasil.

### §51b — Node/libuv + NETLINK_ROUTE diblokir Android

```
SystemError [ERR_SYSTEM_ERROR]: uv_interface_addresses returned Unknown system error 13
    at getLanIp (…/9router/cli.js:114:41)
    at startServer (…/9router/cli.js:620:19)
```

Akar sebenarnya ketemu lewat probe C (`socket`/`bind`/`sendto`/`getifaddrs`):

```
socket(AF_NETLINK)  = 3   OK
bind(AF_NETLINK)    = -1  EACCES     ← juga di HOST Termux!
sendto(RTM_GETLINK) = -1  EACCES     ← juga di HOST Termux!
getifaddrs()        = -1  EACCES
```

Jadi **batas kernel Android**, bukan efek wadah — dan bukan bisa diperbaiki
shim/svsp karena Node memanggil syscall mentah. Fix: preload yang membungkus
`os.networkInterfaces()` dengan fallback loopback (konservatif, tak mengarang
alamat LAN).

### Jebakan investigasi

- `strace` tanpa `-f` **tidak melihat** apa yang dilakukan proses anak svsp;
  dengan `-f` baru kelihatan bahwa yang EACCES adalah `sendto` netlink.
- `ps aux` di dalam wadah juga bermasalah: `busybox ps` cuma 4 kolom
  (`PID USER TIME COMMAND`), sedangkan 9router mengasumsikan format GNU 11
  kolom. Ini sempat membuat saya menyimpulkan 9router "menembak PID sendiri".
  Kenyataannya: `/usr/bin/ls` memang tidak ada (bisanya di `/bin`).
  Diverifikasi ulang dengan audit 333 file: 0 mismatch.
- `lsof` GNU sempat hilang setelah `apk add` (`/usr/bin` jadi tak terbaca dari
  host selama satu waktu) — sudah dipulihkan.

### Verifikasi

```
alpine -c '9router --skip-update -p 20128'   → dashboard HTTP 200
./selftest                                     → 0 FAIL (matrix §40–§52)
```

## Jejak sampingan

- `9router` butuh satu kali `npm install -g` (~50–60 detik di jaringan seluler),
  lalu jalan normal. Postinstall-nya (SQLite/tray warm-up) ikut berhasil.
- `npm install -g` dipakai juga untuk `cowsay` sebagai paket uji cepat
  (11–41 paket, ~5 detik).

## Ronde 22 sisipan — `$PWD` (§50)

Prompt menunjukkan `alpine:/data/data/com.termux/files/home/alpine-rootfs❯`.
Akar masalahnya satu baris di `svsp.c`: `setenv("PWD", base, 1)` menyetel path
host. Fix-nya perlu **dua lapis** — hanya menyetel `$PWD` tak cukup, karena
shell membandingkan `$PWD` dengan device+inode cwd saat start dan jatuh ke
`getcwd()` kalau beda, sehingga path host tetap muncul:

```
$ env | grep ^PWD          → PWD=/root        (svsp benar)
$ sh -c 'echo $PWD'        → /data/.../base   (shell mengoreksi jadi getcwd)
```

Setelah `chdir()` supervisor ikut ke `$BASE/root`, keduanya konsisten:

```
PWD=/
PWD=/root   (dengan FAKE_START=$BASE/root)
$ cd /usr; test -x bin/node   → OK
```

`Node process.cwd()` tetap path host — disengaja, konsisten dengan §27/§28
(`getcwd` menjawab kebenaran host agar runtime syscall-mentah tak gagal).
`FAKE_VIEW=container` memulihkan tampilan wadah.

Verifikasi: `selftest` → **0 FAIL** (matrix §40–§50). Sabotage §50 (svsp +
fake-run dikembalikan ke HEAD) → **2 FAIL**, terbukti check-nya bekerja.