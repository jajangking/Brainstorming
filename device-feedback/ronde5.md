# Device Feedback — Ronde 5 (OpenCode/Bun spawn, HANDOFF §24)

Tanggal: 2026-10-08 · Basis: `a0f575a` (§24 write_mem fallback / relatif cwd-aware)
Target brief: matriks 4 kasus opencode, 0 ❌.

---

## 1. Build (brief tugas 1)

- `git pull` → RC=0 (`a60513b..a0f575a`).
- `./install.sh` **pertama: RC=1** — bootstrap hard-abort di rewrite PT_INTERP:

```
[+] PT_INTERP di-set: 24 file, 52 gagal
[!] I/O gagal: $BASE/root/.codex/packages/standalone/releases/0.161.0-.../lib/gstreamer-1.0/*.so
[!] ... (52 file, semua di bawah root/.codex/)
[x] ada file ELF yang gagal di-rewrite
[✗] Bootstrap gagal!
```

- Penyebab: 52 file paket codex di wadah bermode **`-r-xr-xr-x` (0555, tanpa bit tulis)**
  → `pinterp` gagal tulis (EACCES). Disk 74G kosong, tidak ada proses codex berjalan.
- Tindakan (lingkungan, bukan kode): `find $BASE/root/.codex -type f ! -perm -u+w -exec chmod u+w {} +`
  → `./install.sh` ulang: **RC=0**, `PT_INTERP: 24 file, 0 gagal`, build shim+svsp OK,
  quick-test hijau (`Test dinamis: 3.24.2`, `Isolasi: EACCES (benar)`).
  Log: `~/files/usr/tmp/opencode/install-r5.log` (gagal), `install-r5b.log` (OK).
- **Catatan utk Arena:** `bootstrap.sh:214` menghitung file read-only sebagai *gagal fatal*.
  File 0555 wajar muncul di ekstraksi paket (codex dsb.) — saran: `chmod u+w` dulu di dalam
  loop pinterp atau skip+warn, jangan abort seluruh bootstrap.

## 2. Discrepancy path brief

- Brief: `SVSP_DEBUG=1 fake-run --svsp $HOME/alpine-rootfs/usr/local/bin/opencode ...`
  → **path `$BASE/usr/local/bin/opencode` TIDAK ADA.**
- Binary asli: **`$BASE/root/.opencode/bin/opencode`** (ELF aarch64, dinamis musl,
  INTERP = `$BASE/lib/ld-musl-patched.so.1`, **v2.0.24**, not stripped).
  `opencode2` = wrapper `exec "$(dirname "$0")/opencode" "$@"`.
  Semua perintah di bawah memakai `OC=$BASE/root/.opencode/bin/opencode`.

## 3. Matriks (tugas 2) — perintah persis & hasil

Variabel: `B=$HOME/alpine-rootfs`, `OC=$B/root/.opencode/bin/opencode`.
Jalur natif: `env -u LD_PRELOAD LD_LIBRARY_PATH=$B/lib:$B/usr/lib HOME=$B/root $OC ...`
Jalur svsp: `env -u LD_PRELOAD fake-run --svsp $OC ...` (HOME otomatis `$B/root`, PATH container)
Jalur shim: `env -u LD_PRELOAD fake-run $OC ...`

| # | Kasus | natif | svsp | shim |
|---|-------|-------|------|------|
| 1 | `--help` | ✅ RC=0 | ✅ RC=0 | ✅ RC=0 (`opencode v2.0.24`) |
| 2 | `serve` manual (timeout 10 + `curl /`) | ✅ 200 | ✅ 200 | ✅ 200 |
| 3 | spawn serve (binary polos, `</dev/null`) | ✅ **dgn port bebas** (lihat §5) | ❌ **RC=1, TUI 500** | ❌ **RC=1, loader** |
| 4 | `--standalone` | ✅ TUI render penuh, hidup s/d timeout | ❌ **RC=1, TUI 500** | ❌ **RC=1, exited before readiness** |

Detail:

- **Kasus 1 svsp:** keluar argumen lengkap `--standalone / --server / subcommands...`.
- **Kasus 2:** `server listening on http://127.0.0.1:4096` + `server password ...`; probe
  `GET /, /session, /config, /app` → semua HTTP 200 (SPA) di ketiga jalur.
- **Kasus 3 svsp:** output hanya `Starting background server...` → server anak **naik**
  (port aktif, HTTP 200) tapi klien TUI mati:
  `UnknownError: An error occurred in Effect.tryPromise ... ClientError: UnexpectedStatus: 500 ... [cause]: Error: {"status":500}`.
- **Kasus 4 svsp:** teks error persis (SUDAH DILAMPIRKAN per brief, log
  `~/files/usr/tmp/opencode/r5-standalone.log` & `r5-standalone-logs.log`):

```
UnknownError: An error occurred in Effect.tryPromise
    at ... Tui.run ...
  [cause]: ClientError: UnexpectedStatus: 500
    [cause]: Error: {"status":500}
```

  Log server (`--print-logs`, jalur svsp) menyebut penyebabnya:

```
level=INFO cause="PlatformError: PermissionDenied: FileSystem.realPath (/)
  ... at ConfigDiscovery.discover ... at ServerProcess.start ...
  [cause]: Error: EACCES: permission denied, lstat '/'"
  http.method=GET http.url=/api/fs/list  http.status=500
  http.method=GET http.url=/api/location http.status=500
```

## 4. Akar masalah (terbukti di device)

### 4.1 svsp: `open(O_PATH)` + path rewrite → ADDFD errno=9 → dijawab -EACCES

Repro C (di bawah) — pola **terbalik** natif vs svsp:

| panggilan | natif | svsp |
|---|---|---|
| `open("/etc/hosts", O_PATH)` (file, kena rewrite) | OK | **EACCES (13)** |
| `open("/etc/hosts", O_RDONLY)` (file, kena rewrite) | OK | OK |
| `open("/etc", O_PATH\|O_DIRECTORY)` (dir, kena rewrite) | OK | **EACCES (13)** |
| `open("/", O_PATH\|O_NOFOLLOW)` | OK | **EACCES (13)** |
| `open("/", O_RDONLY\|O_DIRECTORY)` | **EACCES** (kebijakan Android) | OK (di-rewrite ke `$BASE/`) |
| `lstat("/")`, `stat("/")` | OK | OK (rewritten → mode 40700 = `$BASE`) |
| `open(file /data/..., O_PATH)` (passthrough, **tanpa** rewrite) | OK | OK |

Baris `SVSP_DEBUG=1` yang menyebutnya (juga masuk log svsp standalone):

```
[svsp] nr=56 [/etc/hosts] -> [/data/data/com.termux/files/home/alpine-rootfs/etc/hosts]
[svsp] ADDFD nr=56 -> fd=-1 errno=9
[svsp] nr=56 [/tmp] -> [/data/data/com.termux/files/home/alpine-rootfs/tmp]
[svsp] ADDFD nr=56 -> fd=-1 errno=9
```

Kode (baca-saja, tidak saya ubah) — `svsp.c` sekitar baris 877–885:

```c
int nfd = ioctl(listener, SECCOMP_IOCTL_NOTIF_ADDFD, &add);
DBG("ADDFD nr=%ld -> fd=%d errno=%d\n", nr, nfd, errno);
if (nfd < 0)
    send_resp(listener, req->id, 0, -EACCES, 0);   /* errno asli (9/EBADF) dibuang */
```

Jadi: open yang **di-rewrite** dikerjakan supervisor → fd dikirim ke child via ADDFD →
ioctl gagal `errno=9 (EBADF)` → child menerima **EACCES**. Open O_PATH yang
**tidak** di-rewrite (passthrough /data) lolos (syscall child jalan sendiri).
Bun memakai O_PATH saat walk `realPath('/')` → ConfigDiscovery gagal → dua endpoint API
500 → TUI mati. Inilah kasus 3 & 4 jalur svsp.

Baris `[svsp]` execve (tugas 3) — translasi spawn anak **berhasil**, tidak ada
`muat tak cukup` / fallback `write_mem` yang gagal:

```
[svsp] >> nr=221 cls=5 pid=27126 ...
[svsp] nr=221 [/data/.../alpine-rootfs/root/.opencode/bin/opencode] -> [sama, absolut]
[svsp] >> nr=221 cls=5 pid=27137 ...   (anak --standalone ikut ter-rewrite)
```

Sumber repro (komplit, bisa dipakai Arena):

```c
/* repro-svsp.c */
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
int main(void) {
    struct stat st;
    if (lstat("/", &st)) printf("lstat('/') GAGAL: %s (errno=%d)\n", strerror(errno), errno);
    else printf("lstat('/') OK mode=%o dev=%lx\n", st.st_mode, (unsigned long)st.st_dev);
    int fd = open("/", O_PATH|O_NOFOLLOW|O_CLOEXEC);
    if (fd < 0) printf("open(O_PATH)('/') GAGAL: %s (errno=%d)\n", strerror(errno), errno);
    else { printf("open(O_PATH)('/') OK fd=%d\n", fd); close(fd); }
    fd = open("/", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) printf("open(O_RDONLY|O_DIRECTORY)('/') GAGAL: %s (errno=%d)\n", strerror(errno), errno);
    else { printf("open(O_RDONLY|O_DIRECTORY)('/') OK fd=%d\n", fd); close(fd); }
    if (stat("/", &st)) printf("stat('/') GAGAL: %s\n", strerror(errno));
    else printf("stat('/') OK mode=%o\n", st.st_mode);
    return 0;
}
```

Variasi file `/etc/hosts` + `/etc` (repro-opath2.c) memakai pola sama:
`clang -O0 -o repro repro-svsp.c && fake-run --svsp ./repro`.

### 4.2 shim: anak re-exec gagal di loader patched

Teks error persis (jalur `fake-run`, kasus 3):

```
Starting background server...
Error: Server process exited with code 1
/data/data/com.termux/files/home/alpine-rootfs/lib/ld-musl-patched.so.1: cannot load serve: No such file or directory
    at map (native:1:11)
    at reap ...
    at service.ensure ...
```

Kasus 4 jalur shim:

```
Error: Standalone server exited before reporting readiness
    at cli.standalone.endpoint ...
    at cli.server-connection.resolve ...
```

Pola sama dengan keluarga bug §20.1 (argv/loader pada re-exec anak) — tetapi di jalur
**shim/LD_PRELOAD**, bukan svsp: svsp menerjemahkan execve anak dengan benar (§4.1).

### 4.3 "Hang" spawn = konflik port 49374, bukan bug wadah

- Saat test awal, spawn `opencode serve --service` (port default **49374 = 0xC0DE**)
  gagal berulang — log daemon (`$B/root/.local/share/opencode/log/opencode.log`):

```
level=ERROR message="cli process failed" cause="... Managed service port 49374 on 127.0.0.1
is already in use by another process. Configure another port with `opencode service set port <port>` ..."
```

- Pemegang port: **service opencode HOST sesi agent ini sendiri**
  (`$HOME/.local/state/opencode/service.json` → `http://127.0.0.1:49374`, pid 6486,
  v2.0.12; bukti: `curl 49374` → HTTP 401). Catatan: sandbox ini menyembunyikan
  `ps` (kosong) dan `/proc/net/tcp` (Permission denied) — identifikasi lewat service.json.
- `opencode service set port 49474` (config `$B/root/.config/opencode/service.json`) lalu:
  - **nativ, port bebas → spawn SUKSES**: TUI render penuh (`Ask anything…`, 2.0.24),
    server 49474 HTTP 200, watcher aktif. (RC=124 = timeout membunuh TUI yang sehat.)
  - **svsp, port bebas → server ANAK naik (49474 → 200) tapi TUI tetap 500** —
    identik kasus 4, `PlatformError ... realPath (/)` di log daemon → bug §4.1, bukan port.
  - **shim → tetap gagal** `cannot load serve` — bug §4.2, bukan port.
- Konklusi: kasus 3 ❌ natif-saat-port-sibuk itu artefak lingkungan (service lain jalan);
  ❌ svsp & shim adalah bug nyata §4.1/§4.2.

## 5. Hipotesis brief (tugas 4)

- `readelf -l $OC | grep -i interp` → **ADA**: `[Requesting program interpreter:
  .../alpine-rootfs/lib/ld-musl-patched.so.1]` → binary **dinamis musl**, **bukan statis**.
  Kalimat brief "kosong = statis = wajib jalur svsp" **tidak berlaku** untuk binary ini.
- Konsekuensi: LD_PRELOAD **berlaku** utk binary ini (jalur shim memang dipakai & terbukti
  rusak di re-exec anak, §4.2 — bukan karena "lolos interposer").
- `fake-run` mode auto: `is_dynamic → MODE=ldpreload` (loader musl patched) — inilah sebab
  binary **bionic** host ($PREFIX/bin/opencode) lewat shim → RC=127 `Error relocating ...
  __libc_init: symbol not found` (uji ronde-4); binar bionic hanya lewat `--svsp`.

## 6. Perubahan lingkungan saat test (jujur, apa adanya)

1. `chmod u+w` 52 file `$B/root/.codex/**` (agar bootstrap lolos) — izin tulis ditambahkan.
2. Port service wadah sempat diganti 49474 untuk uji spawn → **sudah dikembalikan ke 49374**.
   File `$B/root/.config/opencode/service.json` kini `{ "port": 49374, "password": ... }`
   (password ditulis oleh perintah `service set`; file kemungkinan dibuat saat uji).
3. Daemon uji dimatikan; port 49474 → HTTP 000 (mati). Service host pid 6486 (sesi agent)
   **tidak disentuh**. Satu `kill` terakhir memakai pid dari state service.json (1172) —
   log menunjukkan daemon sudah self-shutdown duluan; bila pid sudah di-recycle, kill bisa
   kena proses lain — tidak ada gejala kerusakan (shell/tes tetap normal), dilaporkan apa adanya.
4. `ps` di sandbox ini kosong (terbatas) — kebersihan proses hanya bisa diverifikasi via
   port/service.json.

## 7. Verdict ronde 5

- Target brief **TIDAK tercapai**: 2 dari 4 kasus ❌ di jalur svsp
  (`spawn serve` & `--standalone` → HTTP 500) + ❌ di jalur shim (loader `cannot load serve`).
- Sudah dilampirkan sesuai brief: perintah persis, `SVSP_DEBUG=1 fake-run --svsp ... --standalone
  | tail`, baris `[svsp]` (execve/ADDFD/write_mem), teks error persis OpenCode/Bun, plus repro C
  dan hasil natif-vs-svsp.
- Fix ada di dua titik (kewenangan Arena, saya tidak menyentuh kode):
  **(a)** svsp: `ADDFD` gagal utk fd O_PATH harus memakai errno asli / jalur lain untuk
  open O_PATH (svsp.c:885 saat ini membalas -EACCES buta);
  **(b)** shim: re-exec anak `serve --service` salah argumen ke loader patched.
