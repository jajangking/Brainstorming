/*
 * svsp.c — supervisor seccomp USER_NOTIF (fake-chroot level kernel)
 *
 * Menutup celah LD_PRELOAD: binary statis & syscall mentah.
 *
 * Arsitektur:
 *   - child: prctl(NO_NEW_PRIVS) + filter seccomp (NEW_LISTENER) utk syscall
 *     path -> RET_USER_NOTIF; kirim listener fd ke parent via socketpair
 *     SCM_RIGHTS; lalu exec target (filter diwariskan ke binary apa pun,
 *     statis sekalipun).
 *   - parent (tanpa filter): melayani notifikasi; path absolut di-rewrite ke
 *     FAKE_BASE; hasilnya dieksekusi sendiri (ekivalen: namespace sama) atau
 *     via ADDFD (open), atau rewrite-memori+CONTINUE (execve/chdir).
 *
 * Aturan rewrite:
 *   - /dev|/proc|/sys*  -> passthrough (host)
 *   - sudah di bawah base -> passthrough
 *   - absolut lainnya   -> base + path
 *   - relatif           -> passthrough (cwd child = base)
 *   Jika rewrite menghasilkan string sama -> CONTINUE (jalankan native di
 *   child, hemat round-trip; /proc/self benar utk child).
 *
 * Susunan BPF arch-check yang benar (bug klasik jt/jf):
 *   LD arch; JEQ(AUDIT_ARCH_AARCH64, jt=1, jf=0); RET KILL
 *   -> match: lompati KILL (lanjut); mismatch: jatuh ke KILL.
 *
 * Compile (statis musl — lintas device, tanpa linker/bionic):
 *   clang --target=aarch64-alpine-linux-musl --sysroot=$ROOTFS -static -nostdlib -O2 \
 *     -o svsp $ROOTFS/usr/lib/crt1.o $ROOTFS/usr/lib/crti.o svsp.c \
 *     $ROOTFS/usr/lib/libc.a $ROOTFS/usr/lib/crtn.o \
 *     $(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-aarch64-android.a
 * Binary statis => kernel exec langsung, tak ada verneed/DT_NEEDED yang bisa
 * ditolak linker64 device ("CANNOT LINK ... libc.so from verneed[0]").
 * Pakai:
 *   svsp --base=$ROOTFS $ROOTFS/lib/ld-musl-patched.so.1 $ROOTFS/bin/busybox cat /etc/os-release
 *   svsp --base=$ROOTFS /path/ke/binary-STATIS arg...
 * Env: SVSP_DEBUG=1 utk log layanan.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/types.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>   /* utimes() — tanpa ini: implicit declaration */
#include <limits.h>     /* PATH_MAX — untuk snapshot /proc/net (§51) */
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

/* CATATAN KRITIS: TIDAK ada syscall rmdir(2) di arm64 (tabel asm-generic);
 * bionic/musl mengemulasi rmdir lewat unlinkat. JANGAN pernah fallback
 * SYS_rmdir ke nomor lain — di arm64 nomor 21 = epoll_ctl. Fallback lama
 * (SYS_rmdir=21) membuat filter men-*match* epoll_ctl sebagai "rmdir",
 * membaca fd sebagai pointer path, gagal, dan membalas EFAULT — meruntuhkan
 * runtime Go statis (netpoll: "epollctl failed with 14").
 */
#ifndef SYS_readlink
#define SYS_readlink 89
#endif

/* openat2: arm64 SYS=437 (asm-generic); pastikan angka TIDAK pernah
 * ditebak-tebak — 437 terverifikasi (lih. HANDOFF.md). struct open_how
 * resmi dari <linux/openat2.h> (bionic/glibc punya); fallback manual
 * hanya bila header tak tersedia — JANGAN define ulang bila sudah ada. */
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef HAVE_STRUCT_OPEN_HOW
#if defined(__has_include) && __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define HAVE_STRUCT_OPEN_HOW 1
#else
struct open_how {
    __u64 flags;
    __u64 mode;
    __u64 resolve;
};
#endif
#endif

#define DBG(...) do { if (svsp_debug) { fprintf(stderr, "[svsp] " __VA_ARGS__); } } while (0)

static int svsp_debug = 0;
static char *base;              /* FAKE_BASE */
static size_t baselen;

/* ------------------------------------------------------------------ */
/* memori remote (child)                                               */
/* ------------------------------------------------------------------ */
static ssize_t read_mem(pid_t pid, void *addr, void *buf, size_t len) {
    struct iovec lo = { buf, len }, ro = { addr, len };
    return process_vm_readv(pid, &lo, 1, &ro, 1, 0);
}
static ssize_t write_mem(pid_t pid, void *addr, const void *buf, size_t len) {
    struct iovec lo = { (void *)buf, len }, ro = { addr, len };
    ssize_t r = process_vm_writev(pid, &lo, 1, &ro, 1, 0);
    if (r == (ssize_t)len) return r;
    /* Fallback halaman read-only (.rodata): Zig/Bun mengoper path execve
     * sebagai literal compile-time -> process_vm_writev ditolak. Tulis via
     * /proc/<pid>/mem: hak ptrace sama, tembus proteksi halaman (terbukti
     * di uji sandbox PWRITE-RODATA-OK; kasus OpenCode/Bun §24 HANDOFF). */
    int sav = errno;
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/mem", (int)pid);
    int fd = open(mp, O_RDWR);
    if (fd >= 0) {
        ssize_t w = pwrite(fd, buf, len, (off_t)(uintptr_t)addr);
        if (w == (ssize_t)len) { close(fd); return w; }
        sav = errno;
        close(fd);
    }
    errno = sav;
    return r;
}

/* Path relatif dari cwd AKTUAL child ke path host `to` (tulis ke relbuf).
 * Return 0 sukses. Fallback rewrite execve/chdir dulu mengasumsikan
 * cwd=base — salah begitu app chdir (Bun/OpenCode ke direktori proyek,
 * atau chdir passthrough). Baca cwd via /proc/<pid>/cwd lalu susun
 * segmen "../" + sisa. */
static int rel_from_cwd(pid_t pid, const char *to, char *relbuf, size_t cap) {
    char cwd[4096], lpath[64];
    snprintf(lpath, sizeof lpath, "/proc/%d/cwd", (int)pid);
    ssize_t cl = readlink(lpath, cwd, sizeof cwd - 1);
    if (cl <= 0) return -1;
    cwd[cl] = 0;
    const char *c = cwd, *t = to;
    for (;;) {                                   /* lewati prefix bersama */
        while (*c == '/') c++;
        while (*t == '/') t++;
        const char *ce = strchr(c, '/'); if (!ce) ce = c + strlen(c);
        const char *te = strchr(t, '/'); if (!te) te = t + strlen(t);
        size_t csl = (size_t)(ce - c), tsl = (size_t)(te - t);
        if (csl != tsl || (csl && memcmp(c, t, csl) != 0)) break;
        c = ce; t = te;
        if (!*c && !*t) {                        /* direktori sama */
            if (cap < 2) return -1;
            relbuf[0] = '.'; relbuf[1] = 0; return 0;
        }
    }
    size_t off = 0;
    while (*c) {                                 /* tiap sisa cwd = "../" */
        while (*c == '/') c++;
        if (!*c) break;
        if (off + 3 >= cap) return -1;
        memcpy(relbuf + off, "../", 3); off += 3;
        const char *ce = strchr(c, '/');
        c = ce ? ce : c + strlen(c);
    }
    while (*t == '/') t++;
    size_t tl = strlen(t);
    if (tl == 0) {
        if (off < 3 || off + 1 > cap) return -1;
        relbuf[off - 1] = 0; return 0;           /* "../.." tanpa slash ujung */
    }
    if (off + tl + 1 > cap) return -1;
    memcpy(relbuf + off, t, tl + 1);
    return 0;
}

/* baca string hingga max, kembalikan panjang (tiada NUL -> -1).
 * PENTING: baca PER-HALAMAN (batas 4K). process_vm_readv yang meminta
 * byte melewati akhiran mapping child (mis. string di ujung heap/stack,
 * persis seperti path kedua renameat/linkat milik apk) gagal EFAULT
 * "Bad address" walau stringnya utuh — baca berhenti di batas halaman,
 * lanjut ke halaman berikutnya hanya bila NUL belum ditemukan. */
static ssize_t read_string(pid_t pid, void *addr, char *buf, size_t max) {
    size_t off = 0;
    while (off < max) {
        size_t page_rem = 4096 - ((uintptr_t)addr + off) % 4096;
        size_t chunk = max - off;
        if (chunk > page_rem) chunk = page_rem;
        ssize_t n = read_mem(pid, (void *)((uintptr_t)addr + off), buf + off, chunk);
        if (n < 0) {
            char *nul = memchr(buf, 0, off); /* yg sudah terbaca masih sah */
            return nul ? (ssize_t)(nul - buf) : -1;
        }
        off += (size_t)n;
        char *nul = memchr(buf + off - (size_t)n, 0, (size_t)n);
        if (nul) return (ssize_t)(nul - buf);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* rewrite path                                                         */
/* ------------------------------------------------------------------ */
static int passthrough(const char *p) {
    /* /system,/apex,/vendor,/product,/linkerconfig,/data: path HOST Android.
     * Wajib passthrough agar wrapper shebang (#!/system/bin/sh) dan linker
     * dinamis host (bionic membuka /system/lib64, /apex/...) tidak ikut
     * ter-rewrite ke $BASE (device-feedback 2026-10-07). */
    /* §47: padanan TANPA garis miring di akhir juga wajib ikut. Dulu "/data/" ada
     * tapi "/data" tidak, sehingga lstat("/data") (komponen pertama yang
     * dipanggil fs.realpathSync Node) ter-rewrite jadi $BASE/data -> ENOENT.
     * Gejala: `npm install -g <paket>` dari dalam wadah gagal "lstat
     * '/data'" (device 2026-10-09). */
    return p[0] != '/'                    /* relatif: resolve di child  */
        || !strncmp(p, "/dev/", 5) || !strcmp(p, "/dev")
        || !strncmp(p, "/proc/", 6) || !strcmp(p, "/proc")
        || !strncmp(p, "/sys/", 5) || !strcmp(p, "/sys")
        || !strncmp(p, "/system/", 8) || !strcmp(p, "/system")
        || !strncmp(p, "/apex/", 6) || !strcmp(p, "/apex")
        || !strncmp(p, "/vendor/", 8) || !strcmp(p, "/vendor")
        || !strncmp(p, "/product/", 9) || !strcmp(p, "/product")
        || !strncmp(p, "/linkerconfig/", 14) || !strcmp(p, "/linkerconfig")
        || !strncmp(p, "/data/", 6) || !strcmp(p, "/data")
        /* §58: penyimpanan internal HP. /sdcard & /storage hanya berguna
         * bila Termux sudah diberi izin (termux-setup-storage); tanpa itu
         * kernel tetap EACCES. Dipassthrough apa adanya (bukan di-rewrite)
         * supaya app wadah membaca/menulis langsung ke sana. */
        || !strncmp(p, "/sdcard/", 8) || !strcmp(p, "/sdcard")
        || !strncmp(p, "/storage/", 9) || !strcmp(p, "/storage")
        /* §60: mount Android lain. /system_ext WAJIB: linker bionic me-resolve
         * lib vendor (mis. libtranlog.so untuk app_process) lewat sana;
         * tanpa ini `am` (termux-api) mati CANNOT LINK di wadah. */
        || !strncmp(p, "/system_ext/", 12) || !strcmp(p, "/system_ext")
        || !strncmp(p, "/odm/", 5) || !strcmp(p, "/odm")
        || !strncmp(p, "/oem/", 5) || !strcmp(p, "/oem")
        || (baselen && strncmp(p, base, baselen) == 0);
}

/* tulis hasil rewrite ke out; return 1 jika berubah, 0 jika sama */
static int rewrite(const char *in, char *out, size_t outsz) {
    if (passthrough(in) || in[0] != '/') {
        snprintf(out, outsz, "%s", in);
        return 0;
    }
    snprintf(out, outsz, "%s%s", base, in);
    return 1;
}

/* ---- O_TMPFILE (bug §17.2 HANDOFF: "failed to write database: EACCES") ----
 * apk-tools v3 menulis DB lewat __apk_ostream_to_file() (src/io.c:1159-1170):
 *   openat(atfd, ".", O_RDWR|O_TMPFILE|O_CLOEXEC, mode)
 * lalu mem-publish anon-inode di fdo_close() (src/io.c:1080-1092):
 *   linkat(AT_FDCWD, "/proc/self/fd/N", atfd, "installed.tmp.<pid>", AT_SYMLINK_FOLLOW)
 * Publish itu butuh CAP_DAC_READ_SEARCH -> di Android selalu EACCES/EPERM ->
 * apk_ostream_cancel() -> "failed to write database: Permission denied".
 *
 * Shim LD_PRELOAD sudah men-mask O_TMPFILE (FK_FLAGS) sehingga apk jatuh ke
 * jalur nama-temp + renameat. svsp TIDAK, karena path-nya RELATIF (".") ->
 * rewrite() mengembalikan changed=0 -> CONTINUE -> child yang membuka dengan
 * flag O_TMPFILE utuh. Maka: (1) mask flag di sini, (2) paksa supervisor
 * menanganinya walau path tidak berubah. openat(".", O_RDWR) -> EISDIR ->
 * apk memakai "installed.tmp" + renameat (relatif -> CONTINUE -> jalan). */
#ifdef O_TMPFILE
#define SVSP_HAS_TMPFILE(f) (((f) & (__u64)O_TMPFILE) == (__u64)O_TMPFILE)
#define SVSP_MASK_TMPFILE(f) ((f) & ~(__u64)O_TMPFILE)
#else
#define SVSP_HAS_TMPFILE(f) 0
#define SVSP_MASK_TMPFILE(f) (f)
#endif

/* fd supervisor yang menunjuk fd milik CHILD (via /proc/<pid>/fd/N, O_PATH).
 * Dibutuhkan saat supervisor harus mengeksekusi syscall ber-path RELATIF
 * terhadap dirfd child (kasus O_TMPFILE di atas: dirfd = fd hasil openat
 * apk, bukan AT_FDCWD). O_PATH sah dipakai sebagai dirfd openat(2). */
static int child_fd_ref(pid_t pid, int fd) {
    char p[64];
    int n = snprintf(p, sizeof p, "/proc/%d/fd/%d", (int)pid, fd);
    if (n < 0 || (size_t)n >= sizeof p) { errno = ENAMETOOLONG; return -1; }
    return open(p, O_PATH | O_CLOEXEC);
}

/* Syscall yang DIEKSEKUSI SUPERVISOR sendiri melihat "/proc/self" sebagai
 * dirinya sendiri, bukan child — salah untuk fstatat("/proc/self/exe"),
 * open("/proc/self/fd/N"), linkat("/proc/self/fd/N"), dst. Ganti ke
 * /proc/<pid-child>. Return 1 bila path diubah.
 * PENTING: hasil ini JANGAN masuk rewrite_cache (kunci = path asli, pid
 * berbeda antar notifikasi). Karena itu dipanggil SETELAH cache_store(). */
static int proc_self_fix(char *p, size_t n, pid_t pid) {
    if (strncmp(p, "/proc/self", 10) != 0) return 0;
    if (p[10] != '\0' && p[10] != '/') return 0;
    char tmp[4096];
    int r = snprintf(tmp, sizeof tmp, "/proc/%d%s", (int)pid, p + 10);
    if (r < 0 || (size_t)r >= sizeof tmp || (size_t)r >= n) return 0;
    memcpy(p, tmp, (size_t)r + 1);
    return 1;
}

/* rewrite path kedua (rename/link/symlink) + koreksi /proc/self. */
static int rewrite2(const char *in, char *out, size_t outsz, pid_t pid) {
    int changed = rewrite(in, out, outsz);
    if (proc_self_fix(out, outsz, pid)) changed = 1;
    return changed;
}

/* ---- shebang-in-container untuk C_EXEC (device-feedback 2026-10-07) ----
 * Kernel host me-resolve interpreter `#!` terhadap ROOT HOST. Script wadah
 * (mis. trigger apk `#!/bin/busybox sh`) -> /bin/busybox tak ada di Android
 * -> execve ENOENT (rc=127), padahal jalur shim sukses. Di svsp child tak
 * punya shim, jadi supervisor yang membereskan — TANPA bedah memori argv:
 * script-nya sendiri (di $BASE, supervisor bisa tulis) DITULIS ULANG jadi
 * wrapper `#!/system/bin/sh` yang meng-exec interpreter wadah lewat loader
 * musl patched; isi asli dipindah ke <file>.orig-svsp. Child diblok selama
 * notifikasi -> bebas race. Idempoten (marker "svsp-shebang-wrapper"). */
static int host_pass_prefix(const char *p) {
    static const char *pre[] = { "/system/", "/data/", "/dev/", "/proc/",
                                 "/sys/", "/apex/", "/vendor/", "/product/",
                                 "/sdcard/", "/storage/",
                                 "/system_ext/", "/odm/", "/oem/",
                                 NULL };
    for (int i = 0; pre[i]; i++)
        if (strncmp(p, pre[i], strlen(pre[i])) == 0) return 1;
    return 0;
}

/* Rewrite token path-absolut wadah yg menunjuk FILE REGULER eksekutable di
 * base menjadi path base penuh (device-feedback ronde 3, KOREKSI §20.1):
 * busybox Alpine me-readlink("/bin/busybox") -> selalu gagal (file reguler)
 * -> fallback argv[0] -> argv[0] HARUS absolut. Buffer argv[0] dari exec
 * "/bin/busybox" cuma 13 byte (rewrite host 60 byte tak muat -> fallback
 * relatif -> busybox mati). Dengan menulis "$BASE/bin/busybox" langsung di
 * ISI script, sh mengalokasikan buffer 60 byte -> rewrite pas -> argv[0]
 * absolut. Syarat token: mulai '/', bukan prefix host, $BASE+token ada,
 * S_ISREG, dan X_OK. (Token data dalam string berkutip ikut terganti bila
 * kebetulan sama persis dgn path biner — langka; didokumentasikan.) */
#define ABS_MAXTOK 64
static char *script_absolutize(int sfd, size_t *outlen) {
    size_t cap = 1u << 20, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    for (;;) {
        ssize_t r = read(sfd, buf + len, cap - len);
        if (r < 0) { free(buf); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
        if (len == cap) break;
    }
    buf[len] = '\0';

    /* kumpulkan kandidat token */
    static char toks[ABS_MAXTOK][1024];
    size_t tlens[ABS_MAXTOK];
    int nt = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '/') continue;
        if (i > 0) {
            char c = buf[i - 1];
            /* hanya di awal kata: pemisah umum shell/script */
            if (c != ' ' && c != '\t' && c != '\n' && c != ';' && c != '|' &&
                c != '&' && c != '(' && c != '`' && c != '"' && c != '\'' &&
                c != '<' && c != '>' && c != '$' && c != '=')
                continue;
        }
        size_t j = i;
        while (j < len && buf[j] != ' ' && buf[j] != '\t' && buf[j] != '\n' &&
               buf[j] != ';' && buf[j] != '|' && buf[j] != '&' &&
               buf[j] != ')' && buf[j] != '"' && buf[j] != '\'' &&
               buf[j] != '`' && buf[j] != '<' && buf[j] != '>' &&
               buf[j] != ':')
            j++;
        size_t tl = j - i;
        if (tl == 0 || tl >= sizeof toks[0]) continue;
        char cand[1024];
        memcpy(cand, buf + i, tl); cand[tl] = '\0';
        if (host_pass_prefix(cand)) continue;
        if (strncmp(cand, base, baselen) == 0) continue;
        char hp[4608];
        snprintf(hp, sizeof hp, "%s%s", base, cand);
        struct stat stt;
        if (stat(hp, &stt) < 0 || !S_ISREG(stt.st_mode)) continue;
        if (access(hp, X_OK) != 0) continue;
        int dup = 0;
        for (int k = 0; k < nt; k++)
            if (tlens[k] == tl && memcmp(toks[k], cand, tl) == 0) { dup = 1; break; }
        if (dup) continue;
        if (nt >= ABS_MAXTOK) continue;
        memcpy(toks[nt], cand, tl + 1);
        tlens[nt] = tl;
        nt++;
    }
    if (nt == 0) { *outlen = len; return buf; }

    /* urutkan terpanjang dulu (hindari tabrakan prefiks saat ganti) */
    for (int a = 0; a < nt; a++)
        for (int b = a + 1; b < nt; b++)
            if (tlens[b] > tlens[a]) {
                char tc[1024]; memcpy(tc, toks[a], tlens[a] + 1);
                memcpy(toks[a], toks[b], tlens[b] + 1);
                memcpy(toks[b], tc, tlens[a] + 1);
                size_t tt = tlens[a]; tlens[a] = tlens[b]; tlens[b] = tt;
            }

    /* hitung ukuran hasil */
    size_t newlen = 0, i = 0;
    while (i < len) {
        int hit = -1;
        for (int k = 0; k < nt; k++)
            if (i + tlens[k] <= len && memcmp(buf + i, toks[k], tlens[k]) == 0) {
                hit = k; break;
            }
        if (hit >= 0) { newlen += baselen + tlens[hit]; i += tlens[hit]; }
        else { newlen++; i++; }
    }
    char *out = malloc(newlen + 1);
    if (!out) { free(buf); return NULL; }
    size_t o = 0; i = 0;
    while (i < len) {
        int hit = -1;
        for (int k = 0; k < nt; k++)
            if (i + tlens[k] <= len && memcmp(buf + i, toks[k], tlens[k]) == 0) {
                hit = k; break;
            }
        if (hit >= 0) {
            memcpy(out + o, base, baselen); o += baselen;
            memcpy(out + o, toks[hit], tlens[hit]); o += tlens[hit];
            i += tlens[hit];
        } else {
            out[o++] = buf[i++];
        }
    }
    out[o] = '\0';
    free(buf);
    DBG("script_absolutize: %d token di-rewrite\n", nt);
    *outlen = o;
    return out;
}

static int shebang_wrap(const char *H) {
    int fd = open(H, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    char hdr[256];
    ssize_t n = pread(fd, hdr, sizeof hdr - 1, 0);
    close(fd);
    if (n < 3 || hdr[0] != '#' || hdr[1] != '!') return 0;   /* bukan script */
    hdr[n] = '\0';
    if (strstr(hdr, "svsp-shebang-wrapper")) return 1;        /* sudah dibungkus */
    char *nl = strchr(hdr, '\n'); if (nl) *nl = '\0';         /* baris pertama */
    char *p = hdr + 2;
    while (*p == ' ' || *p == '\t') p++;
    char *interp = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p++ = '\0'; while (*p == ' ' || *p == '\t') p++; }
    char *sarg = p;                                            /* boleh kosong */
    if (interp[0] != '/') return 0;             /* interp relatif: urus kernel */
    if (host_pass_prefix(interp)) return 0;     /* memang path host */
    char ih[4352];
    snprintf(ih, sizeof ih, "%s%s", base, interp);
    if (access(ih, X_OK) != 0) return 0;        /* interp memang tak ada */

    const char *host_sh = getenv("SVSP_HOST_SH");
    char bbsh[4352];
    if (!host_sh || !*host_sh) {
        /* Default: busybox wadah (BUKAN /system/bin/sh!). Sejak §40 env
         * supervised membawa LD_PRELOAD=shim (musl) yg membuat sh host
         * (bionic) gagal load (CANNOT LINK). busybox musl + shim = cocok.
         * Argumen "sh" wajib (multicall butuh pemilih applet); override
         * SVSP_HOST_SH dipakai mentah seperti dulu. */
        snprintf(bbsh, sizeof bbsh, "%s/bin/busybox sh", base);
        host_sh = bbsh;
    }
    char loader[4352];
    const char *ld = getenv("SVSP_LOADER");
    if (ld && *ld) snprintf(loader, sizeof loader, "%s", ld);
    else snprintf(loader, sizeof loader, "%s/lib/ld-musl-patched.so.1", base);
    int use_loader = (access(loader, X_OK) == 0);

    /* isi asli -> <H>.orig-svsp (skali saja), dgn path biner wadah
     * di-absolutkan ke base (lihat script_absolutize) */
    char origp[4608];
    snprintf(origp, sizeof origp, "%s.orig-svsp", H);
    if (access(origp, F_OK) != 0) {
        int s = open(H, O_RDONLY | O_CLOEXEC);
        if (s < 0) return -1;
        size_t clen = 0;
        char *content = script_absolutize(s, &clen);
        close(s);
        if (!content) return -1;
        int d2 = open(origp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      st.st_mode & 0777);
        if (d2 < 0) { free(content); return -1; }
        int bad = 0;
        ssize_t w = 0;
        while (w < (ssize_t)clen) {
            ssize_t k = write(d2, content + w, clen - (size_t)w);
            if (k < 0) { bad = 1; break; }
            w += k;
        }
        if (close(d2) < 0) bad = 1;
        free(content);
        if (bad) { unlink(origp); return -1; }
    }

    /* wrapper ditulis ke tmp lalu rename (atomik) */
    char tmpp[4608];
    snprintf(tmpp, sizeof tmpp, "%s.wrap-tmp", H);
    int d = open(tmpp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                 st.st_mode & 0777);
    if (d < 0) return -1;
    char sargbuf[300];
    sargbuf[0] = '\0';
    if (sarg[0]) snprintf(sargbuf, sizeof sargbuf, " \"%s\"", sarg);
    char wbuf[10000];
    int len;
    if (use_loader)
        len = snprintf(wbuf, sizeof wbuf,
            "#!%s\n"
            "# svsp-shebang-wrapper v1 (dibuat otomatis oleh svsp; jangan diedit)\n"
            "exec \"%s\" \"%s\"%s \"%s\" \"$@\"\n",
            host_sh, loader, ih, sargbuf, origp);
    else
        len = snprintf(wbuf, sizeof wbuf,
            "#!%s\n"
            "# svsp-shebang-wrapper v1 (dibuat otomatis oleh svsp; jangan diedit)\n"
            "exec \"%s\"%s \"%s\" \"$@\"\n",
            host_sh, ih, sargbuf, origp);
    int bad = 0;
    if (len < 0 || (size_t)len >= sizeof wbuf) bad = 1;
    else {
        ssize_t w = 0;
        while (w < len) {
            ssize_t k = write(d, wbuf + w, (size_t)(len - w));
            if (k < 0) { bad = 1; break; }
            w += k;
        }
    }
    if (close(d) < 0) bad = 1;
    if (bad) { unlink(tmpp); return -1; }
    if (rename(tmpp, H) < 0) { unlink(tmpp); return -1; }
    DBG("shebang wrap: %s -> interp=%s%s%s\n", H, ih,
        sarg[0] ? " sarg=" : "", sarg[0] ? sarg : "");
    return 1;
}

/* ---- redirect loader utk ELF ber-INTERP stock (§43) ----
 * Biner dinamis yg PT_INTERP-nya BUKAN loader patched (mis. hasil apk add /
 * unduhan pasca-bootstrap) tak bisa dieksekusi langsung kernel host (host
 * tak punya /lib/...): execve dari parent STATIS (re-exec shim tak ada)
 * mati ENOENT — device 2026-10-08: python pm di-spawn uv statis.
 * Solusi transparan TANPA bedah memori argv (tak muat) dan TANPA menyentuh
 * isi biner (hash pm tetap valid): wrapper skrip sekali-tulis per path
 * biner yg mengeksekusi LOADER dgn biner sbg argumen. Wrapper stabil
 * (nama dari hash path + verifikasi isi) sehingga upgrade apk (ganti isi
 * di path sama) tak basi. Rantai: child -> wrapper -> sh -> loader ->
 * biner (2 exec ekstra, hanya utk biner stock). */
static int elf_stock_interp(const char *H) {
    /* 1 = ELF64 + punya PT_INTERP + interp tak berprefix base.
     * 0 = selain itu (statik / tanpa INTERP / sudah patched / tak terbaca). */
    int fd = open(H, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned char h[64];
    ssize_t n = read(fd, h, sizeof h);
    if (n < 64 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2) {
        close(fd); return 0;
    }
    unsigned long phoff = 0;
    for (int i = 0; i < 8; i++) phoff |= (unsigned long)h[32 + i] << (8 * i);
    unsigned phesz = h[54] | ((unsigned)h[55] << 8);
    unsigned phnum = h[56] | ((unsigned)h[57] << 8);
    if (phesz < 56 || phnum == 0 || phnum > 64 || phesz > 128) { close(fd); return 0; }
    int need = 0;
    for (unsigned i = 0; i < phnum; i++) {
        unsigned char e[64];
        ssize_t m = pread(fd, e, phesz < sizeof e ? phesz : sizeof e,
                          (off_t)(phoff + (unsigned long)i * phesz));
        if (m < 56) break;
        unsigned type = (unsigned)e[0] | ((unsigned)e[1] << 8) |
                        ((unsigned)e[2] << 16) | ((unsigned)e[3] << 24);
        if (type != 3) continue;                       /* PT_INTERP */
        unsigned long offp = 0, sz = 0;
        for (int k = 0; k < 8; k++) {
            offp |= (unsigned long)e[8 + k] << (8 * k);
            sz   |= (unsigned long)e[32 + k] << (8 * k);
        }
        if (sz == 0 || sz > 4096) break;
        char interp[4096];
        m = pread(fd, interp, sz - 1, (off_t)offp);
        close(fd);
        if (m < 1) return 0;
        interp[m] = '\0';
        if (strncmp(interp, base, baselen) == 0 &&
            (interp[baselen] == '/' || interp[baselen] == '\0'))
            return 0;                                  /* sudah patched */
        return 1;                                      /* stock -> butuh loader */
    }
    close(fd);
    return need;
}

static unsigned long elf_wrap_hash(const char *s) {
    unsigned long h = 1469598103934665603UL;           /* FNV-1a 64 */
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211UL; }
    return h;
}

/* Buat (bila belum ada & cocok) wrapper utk REALBIN (host-absolut).
 * Tulis path wrapper ke wbuf. Return 0 sukses. */
static int elf_wrap(const char *realbin, char *wbuf, size_t wcap) {
    const char *host_sh = getenv("SVSP_HOST_SH");
    char bbsh[4352], shbin[4352];
    if (!host_sh || !*host_sh) {
        /* Sama spt shebang_wrap: busybox wadah + " sh", bukan sh host
         * (bionic + LD_PRELOAD shim = CANNOT LINK sejak §40). */
        snprintf(shbin, sizeof shbin, "%s/bin/busybox", base);
        snprintf(bbsh, sizeof bbsh, "%s sh", shbin);
        host_sh = bbsh;
    } else {
        snprintf(shbin, sizeof shbin, "%s", host_sh);
    }
    char loader[4352];
    const char *ld = getenv("SVSP_LOADER");
    if (ld && *ld) snprintf(loader, sizeof loader, "%s", ld);
    else snprintf(loader, sizeof loader, "%s/lib/ld-musl-patched.so.1", base);
    if (access(loader, X_OK) != 0) return -1;
    if (access(shbin, X_OK) != 0) return -1;

    char dir[4608];
    snprintf(dir, sizeof dir, "%s/.svsp-elfwrap", base);
    mkdir(dir, 0755);                                  /* ada -> abaikan */

    unsigned long hh = elf_wrap_hash(realbin);
    char wp[4608], tmp[4608];
    int i;
    for (i = 0; i < 8; i++) {
        if (i == 0) snprintf(wp, sizeof wp, "%s/%016lx", dir, hh);
        else snprintf(wp, sizeof wp, "%s/%016lx.%d", dir, hh, i);
        int fd = open(wp, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char c[1024];
            ssize_t m = read(fd, c, sizeof c - 1);
            close(fd);
            if (m > 0) {
                c[m] = '\0';
                char *rl = strstr(c, "\n# real: ");
                if (rl) {
                    rl += 9;
                    char *e = strchr(rl, '\n');
                    if (e) *e = '\0';
                    if (strcmp(rl, realbin) == 0) break;  /* cocok, pakai */
                }
            }
            continue;                                  /* tabrakan, sufiks */
        }
        snprintf(tmp, sizeof tmp, "%s.tmp.%d", wp, (int)getpid());
        int d = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
        if (d < 0) return -1;
        char wb[9000];
        int len = snprintf(wb, sizeof wb,
            "#!%s\n"
            "# svsp-elf-wrapper v1 (otomatis oleh svsp utk biner INTERP-stock; jangan diedit)\n"
            "# real: %s\n"
            "exec \"%s\" \"%s\" \"$@\"\n",
            host_sh, realbin, loader, realbin);
        int bad = (len < 0 || (size_t)len >= sizeof wb);
        ssize_t w = 0;
        while (!bad && w < len) {
            ssize_t k = write(d, wb + w, (size_t)(len - w));
            if (k < 0) { bad = 1; break; }
            w += k;
        }
        if (close(d) < 0) bad = 1;
        if (bad) { unlink(tmp); return -1; }
        if (rename(tmp, wp) < 0) { unlink(tmp); return -1; }
        break;
    }
    if (i >= 8) return -1;
    if (strlen(wp) + 1 > wcap) return -1;
    strcpy(wbuf, wp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* kelas syscall                                                        */
/* ------------------------------------------------------------------ */
enum { C_OPEN = 1, C_STAT, C_STATX, C_STATFS, C_EXEC, C_READLINK,
       C_SIDE, C_CHDIR };

struct rule { int cls; long nr; };
static struct rule rules[] = {
    /* paling sering dipakai (openat, stat, statx) — di depan utk BPF scan pendek */
    { C_OPEN,      SYS_openat      },
    /* §51: open(2) polos (bukan openat) — musl getifaddrs memakainya untuk
     * /proc/net/if_inet6. Tanpa rule ini syscall-nya tak pernah sampai ke
     * supervisor, jadi fallback snapshot tak mungkin dipakai. */
#ifdef SYS_open
    { C_OPEN,      SYS_open        },
#endif
#ifdef SYS_openat2
    { C_OPEN,      SYS_openat2     },
#endif
    { C_STAT,      SYS_newfstatat  },
#ifdef SYS_statx
    { C_STATX,     SYS_statx       },
#endif
    { C_EXEC,      SYS_execve      },
#ifdef SYS_execveat
    { C_EXEC,      SYS_execveat    },
#endif
#ifdef SYS_statfs
    { C_STATFS,    SYS_statfs      },
#endif
    { C_CHDIR,     SYS_chdir       },
    { C_READLINK,  SYS_readlinkat  },
    { C_READLINK,  SYS_readlink    },
    { C_SIDE,      SYS_mkdirat     },
    { C_SIDE,      SYS_unlinkat    },
    { C_SIDE,      SYS_faccessat   },
    { C_SIDE,      SYS_fchmodat    },
    { C_SIDE,      SYS_fchownat    },
    { C_SIDE,      SYS_utimensat   },
    { C_SIDE,      SYS_mknodat     },
#ifdef SYS_mkdir
    { C_SIDE,      SYS_mkdir       },
#endif
#ifdef SYS_rmdir
    { C_SIDE,      SYS_rmdir       },
#endif
#ifdef SYS_unlink
    { C_SIDE,      SYS_unlink      },
#endif
#ifdef SYS_rename
    { C_SIDE,      SYS_rename      },
#endif
#ifdef SYS_renameat
    { C_SIDE,      SYS_renameat    },
#endif
#ifdef SYS_renameat2
    { C_SIDE,      SYS_renameat2   },
#endif
#ifdef SYS_link
    { C_SIDE,      SYS_link        },
#endif
#ifdef SYS_linkat
    { C_SIDE,      SYS_linkat      },
#endif
#ifdef SYS_symlink
    { C_SIDE,      SYS_symlink     },
#endif
#ifdef SYS_symlinkat
    { C_SIDE,      SYS_symlinkat   },
#endif
#ifdef SYS_access
    { C_SIDE,      SYS_access      },
#endif
#ifdef SYS_faccessat2
    { C_SIDE,      SYS_faccessat2  },
#endif
#ifdef SYS_chmod
    { C_SIDE,      SYS_chmod       },
#endif
#ifdef SYS_fchmodat2
    { C_SIDE,      SYS_fchmodat2   },
#endif
#ifdef SYS_chown
    { C_SIDE,      SYS_chown       },
#endif
#ifdef SYS_lchown
    { C_SIDE,      SYS_lchown      },
#endif
#ifdef SYS_truncate
    { C_SIDE,      SYS_truncate    },
#endif
#ifdef SYS_utimes
    { C_SIDE,      SYS_utimes      },
#endif
#ifdef SYS_mknod
    { C_SIDE,      SYS_mknod       },
#endif
};

static int cls_of(long nr) {
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++)
        if (rules[i].nr == nr) return rules[i].cls;
    return 0;
}

/* ------------------------------------------------------------------ */
/* BPF                                                                */
/* ------------------------------------------------------------------ */
static int build_filter(struct sock_filter **out, __u16 *out_len) {
    size_t nrules = sizeof rules / sizeof rules[0];
    size_t n = 4 + 2 * nrules + 1;
    struct sock_filter *f = calloc(n, sizeof *f);
    size_t i = 0;
    /* arch != aarch64 -> KILL (jt=1 utk match lanjut; jf=0 jatuh ke KILL) */
    f[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          offsetof(struct seccomp_data, arch));
    f[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                          AUDIT_ARCH_AARCH64, 1, 0);
    f[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL);
    f[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          offsetof(struct seccomp_data, nr));
    for (size_t r = 0; r < nrules; r++) {
        f[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                              (unsigned)rules[r].nr, 0, 1);
        f[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                              SECCOMP_RET_USER_NOTIF);
    }
    f[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    *out = f; *out_len = (__u16)i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* cache rewrite (hemat kerja per-notif)                               */
/* ------------------------------------------------------------------ */
#define CACHE_SIZE 512
#define CACHE_MASK (CACHE_SIZE - 1)

struct cache_entry {
    __u64 hash;             /* FNV-1a hash of input path */
    int   changed;          /* 1 if rewritten, 0 if passthrough */
    char  out[4096];        /* cached output path (samakan dgn pbuf) */
    char  in[4096];         /* cached input path (for collision check) */
    int   valid;
};
static struct cache_entry rewrite_cache[CACHE_SIZE];

static __u64 fnv1a(const char *s) {
    __u64 h = 0xcbf29ce484222325ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    return h;
}

static int cache_lookup(const char *in, char *out, size_t outsz, int *changed) {
    __u64 h = fnv1a(in);
    struct cache_entry *e = &rewrite_cache[h & CACHE_MASK];
    if (e->valid && e->hash == h && strcmp(e->in, in) == 0) {
        snprintf(out, outsz, "%s", e->out);
        *changed = e->changed;
        return 1;  /* hit */
    }
    return 0;  /* miss */
}

static void cache_store(const char *in, const char *out, int changed) {
    __u64 h = fnv1a(in);
    struct cache_entry *e = &rewrite_cache[h & CACHE_MASK];
    /* cache harus setia pada path asli: bila snprintf bakal memotong
     * (path > ~4095 byte), jangan simpan — rewrite ulang lebih aman
     * daripada cache yg diam-diam mengubah arti path. */
    if (strlen(in) >= sizeof e->in || strlen(out) >= sizeof e->out)
        return;
    e->hash = h;
    e->changed = changed;
    snprintf(e->in, sizeof e->in, "%s", in);
    snprintf(e->out, sizeof e->out, "%s", out);
    e->valid = 1;
}

/* ------------------------------------------------------------------ */
/* tangani satu notifikasi                                             */
/* ------------------------------------------------------------------ */
static void send_resp(int listener, __u64 id, __s64 val, __s32 error,
                      __u32 flags) {
    struct seccomp_notif_resp resp = { 0 };
    resp.id = id; resp.val = val; resp.error = error; resp.flags = flags;
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) < 0)
        DBG("SEND err %d\n", errno);
}

static int path_argidx(long nr) {
    switch (nr) {
    case SYS_openat:  case SYS_mkdirat:  case SYS_unlinkat:
#ifdef SYS_openat2
    case SYS_openat2:
#endif
    case SYS_newfstatat: case SYS_readlinkat: case SYS_faccessat:
    case SYS_fchmodat: case SYS_fchownat: case SYS_mknodat:
    case SYS_utimensat:
#ifdef SYS_faccessat2
    case SYS_faccessat2:
#endif
#ifdef SYS_fchmodat2
    case SYS_fchmodat2:
#endif
#ifdef SYS_statx
    case SYS_statx:
#endif
        return 1;
    /* rename/link-*: (dirfd, oldpath, dirfd, newpath[, flags]) —
     * path pertama di arg[1]; JANGAN default-0 (a[0] = dirfd!) */
#ifdef SYS_renameat
    case SYS_renameat:
#endif
#ifdef SYS_renameat2
    case SYS_renameat2:
#endif
#ifdef SYS_linkat
    case SYS_linkat:
        return 1;
#endif
    /* symlink: (target, linkpath) — yang di-rewrite = linkpath (a[1]) */
#ifdef SYS_symlink
    case SYS_symlink:
        return 1;
#endif
    /* symlinkat: (target, newdirfd, linkpath) — linkpath di arg[2] */
#ifdef SYS_symlinkat
    case SYS_symlinkat:
        return 2;
#endif
    /* rename/link lama: (oldpath, newpath) — oldpath di arg[0] = default */
    default:
        return 0;
    }
}

static void handle(int listener, const struct seccomp_notif *req) {
    const __u64 *a = req->data.args;
    long nr = (long)req->data.nr;
    pid_t pid = (pid_t)req->pid;
    int cls = cls_of(nr);
    char pbuf[4096], pout[4096], pbuf2[4096], pout2[4096];
    char *pathp; size_t path_aidx;

    DBG(">> nr=%ld cls=%d pid=%d id=%llu\n", nr, cls, (int)req->pid, req->id);
    if (!cls) { send_resp(listener, req->id, 0, -EPERM, 0); return; }

    path_aidx = (cls == C_EXEC && nr != SYS_execve) ? 1 : path_argidx(nr);
    pathp = (void *)(uintptr_t)a[path_aidx];

    ssize_t oldlen = read_string(pid, pathp, pbuf, sizeof pbuf - 1);
    if (oldlen < 0) { send_resp(listener, req->id, 0, -EFAULT, 0); return; }
    pbuf[oldlen] = 0;

    /* cek cache dulu */
    int changed;
    if (!cache_lookup(pbuf, pout, sizeof pout, &changed)) {
        changed = rewrite(pbuf, pout, sizeof pout);
        cache_store(pbuf, pout, changed);
    }

    /* Kapan supervisor TETAP menangani syscall yang path-nya tidak berubah
     * (selain itu CONTINUE = child menjalankan sendiri, paling murah):
     *   (a) open O_TMPFILE — flag hanya bisa di-mask di supervisor; path apk
     *       relatif (".") sehingga changed=0 (lihat SVSP_HAS_TMPFILE).
     *   (b) exec — script shebang wadah butuh wrapping (shebang_wrap).
     * CATATAN /proc/self (device-feedback ronde 2): JANGAN dipaksa ke
     * supervisor. Path /proc/... passthrough (changed=0) -> child jalankan
     * sendiri -> /proc/self memang benar di child. proc_self_fix diterapkan
     * HANYA bila supervisor benar-benar mengeksekusi (changed=1), lihat
     * bawah. Memaksa supervisor membaca /proc/<pid>/exe antar-proses bisa
     * gagal di Android (ptrace/SELinux) -> busybox --install gagal readlink
     * -> jatuh ke argv[0] -> "'bin/busybox' is not an absolute path". */
    int forced = 0;
    if (cls == C_OPEN && nr == SYS_openat && SVSP_HAS_TMPFILE(a[2]))
        forced = 1;
    /* §51: /proc/net/ adalah PASSTHROUGH (bagian /proc/ di atas), jadi
     * changed=0 -> CONTINUE -> child membuka sendiri dan kena EACCES dari
     * kernel Android. Padahal kita punya snapshot replacement. Paksa
     * supervisor menanganinya supaya fallback-nyaw could've dipakai. */
    if (!forced && cls == C_OPEN && pbuf[0] == '/' &&
        !strncmp(pbuf, "/proc/net/", 10))
        forced = 1;
    /* also for plain open(2), not just openat */
    if (!forced && cls == C_OPEN && nr != SYS_openat && pbuf[0] == '/' &&
        !strncmp(pbuf, "/proc/net/", 10))
        forced = 1;
    if (cls == C_EXEC)
        forced = 1;
    if (changed && cls != C_EXEC && cls != C_CHDIR)
        proc_self_fix(pout, sizeof pout, pid);

    /* pembersihan shebang-wrap: script wadah di-unlink (apk menghapus
     * trigger di lib/apk/exec pasca-run) -> buang juga .orig-svsp-nya.
     * Best effort; cwd supervisor == base (chdir di main). */
#ifdef SYS_unlink
    if (cls == C_SIDE && (nr == SYS_unlinkat || nr == SYS_unlink)) {
#else
    if (cls == C_SIDE && nr == SYS_unlinkat) {   /* arm64: SYS_unlink tak ada */
#endif
        char og[4400];
        if (pbuf[0] == '/' && !passthrough(pbuf))
            snprintf(og, sizeof og, "%s%s.orig-svsp", base, pbuf);
        else
            snprintf(og, sizeof og, "%s.orig-svsp", pbuf);
        unlink(og);
    }

    /* tidak berubah -> biarkan kernel menjalankannya di child */
    if (!changed && !forced) {
        send_resp(listener, req->id, 0, 0, SECCOMP_USER_NOTIF_FLAG_CONTINUE);
        return;
    }
    DBG("nr=%ld [%s] -> [%s]\n", nr, pbuf, pout);

    switch (cls) {
    case C_OPEN: {
        int fd;
        int atfd = AT_FDCWD, atfd_ref = -1;
        __u64 oflags = (nr == SYS_openat) ? a[2] : 0;
        int tmpfile_req = SVSP_HAS_TMPFILE(oflags);
        oflags = SVSP_MASK_TMPFILE(oflags);

        /* §25: O_PATH TIDAK BISA dikirim lewat SECCOMP_IOCTL_NOTIF_ADDFD.
         * Kernel memakai fget(srcfd) yang menolak file FMODE_PATH -> ioctl
         * gagal EBADF dan dulu dijawab -EACCES buta. Akibatnya Bun/OpenCode
         * (realPath('/') pakai O_PATH) dapat EACCES -> ConfigDiscovery gagal
         * -> HTTP 500. Terbukti di device (ronde 5) DAN di sandbox x86.
         * Solusi: buka tanpa O_PATH (O_RDONLY) — fd biasa bisa lewat ADDFD
         * dan tetap sah utk fstat/readlink /proc/self/fd/N/openat(dirfd). */
        int path_req = 0;
#ifdef O_PATH
        if (oflags & O_PATH) {
            path_req = 1;
            oflags = (oflags & ~(__u64)O_PATH & ~(__u64)O_ACCMODE) | O_RDONLY;
        }
#endif

        /* Path RELATIF yang harus dieksekusi supervisor (kasus O_TMPFILE):
         * dirfd milik child tak berarti di sini — pin lewat /proc/<pid>/fd/N. */
        if (tmpfile_req && nr == SYS_openat && pout[0] != '/' &&
            (__s64)a[0] != AT_FDCWD) {
            atfd_ref = child_fd_ref(pid, (int)a[0]);
            if (atfd_ref < 0) {
                DBG("O_TMPFILE: dirfd child %d tak bisa di-pin: %s\n",
                    (int)a[0], strerror(errno));
                send_resp(listener, req->id, 0, -EBADF, 0); break;
            }
            atfd = atfd_ref;
        }
        if (tmpfile_req)
            DBG("O_TMPFILE di-mask nr=%ld path=[%s] flags=0x%llx\n",
                nr, pout, (unsigned long long)a[2]);

#ifdef SYS_openat2
        if (nr == SYS_openat2) {
            struct open_how how;
            /* baca 24 byte dgn batas halaman (hindari EFAULT overread
             * bila struct di ujung mapping) */
            size_t oo = 0, need = sizeof how;
            char *dst = (char *)&how;
            int rbad = 0;
            while (oo < need) {
                size_t page_rem = 4096 - ((uintptr_t)a[2] + oo) % 4096;
                size_t chunk = need - oo;
                if (chunk > page_rem) chunk = page_rem;
                if (read_mem(pid, (void *)((uintptr_t)a[2] + oo), dst + oo, chunk) < 0) { rbad = 1; break; }
                oo += chunk;
            }
            if (rbad) { send_resp(listener, req->id, 0, -EFAULT, 0); break; }
            /* openat2: flag ada di how.flags (bukan a[2]). Mask di sini juga;
             * catatan: openat2 di Android kena SIGSYS eksternal sebelum notif
             * (HANDOFF §6.1), jadi jalur ini praktis tak terpakai. */
            tmpfile_req = SVSP_HAS_TMPFILE(how.flags);
            how.flags = SVSP_MASK_TMPFILE(how.flags);
            fd = syscall(SYS_openat2, AT_FDCWD, pout, &how, (size_t)a[3]);
        } else
#endif
            fd = openat(atfd, pout, (int)oflags, (int)a[3]);

#ifdef O_PATH
        /* Sudut-1 §25.1 (dikonfirmasi device ronde 6): file tanpa izin baca.
         * O_PATH asli TIDAK memeriksa izin baca, degradasi O_RDONLY memeriksa
         * -> EACCES padahal natif OK. Di konteks fake-root kita pemilik berkas,
         * jadi: pinjam bit baca sebentar, buka, lalu KEMBALIKAN mode semula. */
        if (fd < 0 && path_req && errno == EACCES) {
            int sav = errno;
            struct stat ms;
            if (fstatat(atfd, pout, &ms, AT_SYMLINK_NOFOLLOW) == 0 &&
                !S_ISLNK(ms.st_mode) && ms.st_uid == geteuid() &&
                fchmodat(atfd, pout, (ms.st_mode | S_IRUSR) & 07777, 0) == 0) {
                fd = openat(atfd, pout, (int)oflags, (int)a[3]);
                int e2 = errno;
                fchmodat(atfd, pout, ms.st_mode & 07777, 0);   /* pulihkan */
                errno = (fd < 0) ? e2 : 0;
            }
            if (fd < 0) errno = sav;
        }
#endif
        /* §51: SELURUH /proc/net/ EACCES di Termux (batas kernel Android —
         * terbukti juga di host: /proc/net/{tcp,route,dev,if_inet6}). Akibatnya
         * libuv uv_interface_addresses() gagal, Node melempar ERR_SYSTEM_ERROR
         * errno 13 di os.networkInterfaces(), dan app yang memanggilnya saat
         * start langsung crash (9router cli.js getLanIp, device 2026-10-09:
         * dashboard tak pernah tampil padahal server sehat).
         * Fallback: arahkan ke snapshot statis $BASE/proc/net (loopback saja).
         * Hanya bila host benar-benar menolak — kalau /proc/net bisa dibaca,
         * pakai yang host (lebih akurat). */
        if (fd < 0 && pout[0] == '/' &&
            !strncmp(pbuf, "/proc/net/", 10) &&
            (errno == EACCES || errno == EPERM || errno == ENOENT)) {
            char snap[PATH_MAX];
            const char *sd = getenv("FAKE_PROCNET");
            if (!sd || !*sd) sd = "/usr/local/share/fakeroot/procnet";
            /* Snapshot tinggal di dalam base: path = base + <suffix>.
             * suffix sd (mis. /usr/local/share/fakeroot/procnet) sudah path
             * absolut, jadi potong saja agar tak jadi <base><absolut>. */
            char sfx[PATH_MAX];
            snprintf(sfx, sizeof sfx, "%s/%s", sd, pbuf + 10);
            snprintf(snap, sizeof snap, "%s%s", base, sfx);
            int fd2 = openat(AT_FDCWD, snap, (int)oflags, 0);
            DBG("open /proc/net%s gagal errno=%d -> snapshot %s = fd %d\n",
                pbuf + 10, fd < 0 ? -errno : 0, snap, fd2);
            if (fd2 >= 0) fd = fd2;
        }
        /* Sudut-2 §25.1 BELUM terpecahkan: simlink dgn O_PATH|O_NOFOLLOW tetap
         * ELOOP — fd O_PATH ke simlink mustahil dikirim lewat ADDFD (kernel
         * fget() menolak FMODE_PATH) dan tak ada fd non-O_PATH yang mewakili
         * simlink itu sendiri. Batas arsitektural, didokumentasikan apa adanya. */

        if (atfd_ref >= 0) { close(atfd_ref); atfd_ref = -1; }

        if (fd >= 0 && tmpfile_req) {
            /* Masking mengubah "buat anon-file di dir X" menjadi "buka X".
             * Bila kernel tetap memberi fd, JANGAN dikirim ke child: semantiknya
             * beda (file bernama, bukan anon) dan publish linkat-nya tetap akan
             * gagal di Android. Paksa EISDIR — persis hasil masking di shim —
             * supaya pemakai (apk) jatuh ke jalur nama-temp + renameat. */
            close(fd);
            fd = -1; errno = EISDIR;
        }
        if (fd < 0) { send_resp(listener, req->id, 0, -errno, 0); break; }
        struct seccomp_notif_addfd add = { 0 };
        add.id = req->id;
        add.flags = SECCOMP_ADDFD_FLAG_SEND;
        add.srcfd = fd;
        add.newfd = 0;          /* kernel pilih fd terendah di child */
        errno = 0;      /* jangan laporkan errno basi pada baris sukses */
        int nfd = ioctl(listener, SECCOMP_IOCTL_NOTIF_ADDFD, &add);
        DBG("ADDFD nr=%ld -> fd=%d errno=%d%s\n", nr, nfd, errno,
            path_req ? " (O_PATH didegradasi ke O_RDONLY)" : "");
        if (nfd < 0) {
            /* errno ASLI, jangan -EACCES buta (ronde 5: menyamarkan EBADF
             * dan menyesatkan diagnosis berhari-hari). */
            int e = errno > 0 ? errno : EACCES;
            send_resp(listener, req->id, 0, -e, 0);
        }
        close(fd);
        break;
    }
    case C_STAT: {
        struct stat st;
        if (fstatat(AT_FDCWD, pout, &st, (int)a[3]) < 0) {
            send_resp(listener, req->id, 0, -errno, 0); break;
        }
        if (write_mem(pid, (void *)(uintptr_t)a[2], &st, sizeof st) < 0) {
            send_resp(listener, req->id, 0, -EFAULT, 0); break;
        }
        send_resp(listener, req->id, 0, 0, 0);
        break;
    }
    case C_STATX: {
#ifdef SYS_statx
        struct statx stx;
        if (syscall(SYS_statx, AT_FDCWD, pout, (int)a[2], (int)a[3], &stx) < 0) {
            send_resp(listener, req->id, 0, -errno, 0); break;
        }
        if (write_mem(pid, (void *)(uintptr_t)a[4], &stx, sizeof stx) < 0) {
            send_resp(listener, req->id, 0, -EFAULT, 0); break;
        }
        send_resp(listener, req->id, 0, 0, 0);
#endif
        break;
    }
    case C_STATFS: {
        struct statfs st;
        if (statfs(pout, &st) < 0) {
            send_resp(listener, req->id, 0, -errno, 0); break;
        }
        if (write_mem(pid, (void *)(uintptr_t)a[1], &st, sizeof st) < 0) {
            send_resp(listener, req->id, 0, -EFAULT, 0); break;
        }
        send_resp(listener, req->id, 0, 0, 0);
        break;
    }
    case C_READLINK: {
        size_t b_idx = (nr == SYS_readlink) ? 1 : 2;
        size_t s_idx = (nr == SYS_readlink) ? 2 : 3;
        char tmp[4096]; size_t cap = (size_t)a[s_idx];
        if (cap > sizeof tmp) cap = sizeof tmp;
        ssize_t r = readlink(pout, tmp, cap);
        if (r < 0) { send_resp(listener, req->id, 0, -errno, 0); break; }
        if (write_mem(pid, (void *)(uintptr_t)a[b_idx], tmp, (size_t)r) < 0) {
            send_resp(listener, req->id, 0, -EFAULT, 0); break;
        }
        send_resp(listener, req->id, 0, r, 0);
        break;
    }
    case C_EXEC: {
        /* shebang-in-container: bila target script wadah dgn interpreter
         * absolut wadah, tulis ulang jadi wrapper host (best effort; gagal
         * wrap = kernel kasih ENOENT seperti sebelumnya). */
        {
            char hc[4400];
            const char *hp;
            if (pbuf[0] == '/') hp = pout;
            else { snprintf(hc, sizeof hc, "%s/%s", base, pbuf); hp = hc; }
            shebang_wrap(hp);
        }
        /* §43: ELF ber-INTERP stock -> redirect via wrapper loader.
         * Berlaku umum (baik changed maupun tidak: `fake-run --svsp`
         * langsung pun mewarisi path host-real yg passthrough). Syarat
         * ketat agar biner host/bionic JANGAN dibungkus: lewati bila pbuf
         * absolut berprefix host TAPI di luar base (catatan: base sendiri
         * di bawah /data/, jadi "$BASE/..." harus tetap lolos!). Target
         * bungkus (pout) wajib di dalam base. Gagal wrap/redirect -> jatuh
         * ke logika lama di bawah (tanpa regresi). */
        {
            char wrap[4608];
            int is_host_abs = (pbuf[0] == '/' && host_pass_prefix(pbuf) &&
                               strncmp(pbuf, base, baselen) != 0);
            if (!is_host_abs &&
                strncmp(pout, base, baselen) == 0 &&
                elf_stock_interp(pout) &&
                elf_wrap(pout, wrap, sizeof wrap) == 0) {
                size_t wlen = strlen(wrap);
                const char *w = NULL;
                char wrel[4096];
                if (wlen <= (size_t)oldlen) {
                    w = wrap;
                } else if (rel_from_cwd(pid, wrap, wrel, sizeof wrel) == 0 &&
                           strlen(wrel) <= (size_t)oldlen) {
                    w = wrel;
                }
                if (w) {
                    DBG("execve elf-wrap: [%s] -> [%s]\n", pbuf, w);
                    errno = 0;
                    if (write_mem(pid, pathp, w, strlen(w) + 1) < 0) {
                        DBG("execve elf-wrap write_mem gagal (errno=%d)\n", errno);
                    } else {
                        send_resp(listener, req->id, 0, 0,
                                  SECCOMP_USER_NOTIF_FLAG_CONTINUE);
                        break;
                    }
                } else {
                    DBG("execve elf-wrap muat tak cukup: %s\n", wrap);
                }
            }
        }
        /* path RELATIF tak berubah: tak ada yang perlu di-rewrite di memori
         * child (lagi pula string bisa di .rodata) -> langsung CONTINUE. */
        if (!changed) {
            send_resp(listener, req->id, 0, 0,
                      SECCOMP_USER_NOTIF_FLAG_CONTINUE);
            break;
        }
        /* rewrite path di memori child; absolut dulu, lalu relatif thd cwd
         * AKTUAL child (§24: asumsi cwd=base salah begitu app chdir). */
        size_t newlen = strlen(pout);
        const char *w;
        char relbuf[4096];
        if (newlen <= (size_t)oldlen) {
            w = pout;
        } else if (rel_from_cwd(pid, pout, relbuf, sizeof relbuf) == 0 &&
                   strlen(relbuf) <= (size_t)oldlen) {
            w = relbuf;
        } else if (pbuf[0] == '/' && strlen(pbuf + 1) <= (size_t)oldlen) {
            w = pbuf + 1;            /* upaya terakhir: asumsi cwd=base */
        } else {
            /* tak muat di buffer path asli -> tak bisa rewrite */
            DBG("execve muat tak cukup: %s\n", pout);
            send_resp(listener, req->id, 0, -ENOENT, 0);
            break;
        }
        if (write_mem(pid, pathp, w, strlen(w) + 1) < 0) {
            /* path di memori read-only (.rodata): rewrite tak mungkin.
             * Alternatif (ENOENT) jujur: path itu tak ada di wadah.
             * Catatan: shell/dalang umumnya membangun string path di
             * stack/heap (writable) sehingga tetap berfungsi. */
            DBG("execve write_mem gagal (RO memori) nr=%ld oldlen=%zd\n", nr, oldlen);
            send_resp(listener, req->id, 0, -ENOENT, 0); break;
        }
        send_resp(listener, req->id, 0, 0, SECCOMP_USER_NOTIF_FLAG_CONTINUE);
        break;
    }
    case C_CHDIR: {
        size_t newlen = strlen(pout);
        char relbuf[4096];
        const char *w = NULL;
        if (newlen <= (size_t)oldlen) {
            w = pout;
        } else if (rel_from_cwd(pid, pout, relbuf, sizeof relbuf) == 0 &&
                   strlen(relbuf) <= (size_t)oldlen) {
            w = relbuf;
        } else if (pbuf[0] == '/' && strlen(pbuf + 1) <= (size_t)oldlen) {
            w = pbuf + 1;
        }
        if (w) {
            if (write_mem(pid, pathp, w, strlen(w) + 1) == 0)
                send_resp(listener, req->id, 0, 0,
                          SECCOMP_USER_NOTIF_FLAG_CONTINUE);
            else
                send_resp(listener, req->id, 0, -EFAULT, 0);
        } else {
            /* tetap tak muat: no-op (status quo §22; berubah jadi error
             * berisiko memecah script `cd` yang sebelumnya "jalan"). */
            DBG("chdir muat tak cukup: %s\n", pout);
            send_resp(listener, req->id, 0, 0, 0);
        }
        break;
    }
    case C_SIDE: {
        int rc;
        switch (nr) {
        case SYS_mkdirat:  rc = mkdirat(AT_FDCWD, pout, (mode_t)a[2]); break;
#ifdef SYS_mkdir
        case SYS_mkdir:    rc = mkdir(pout, (mode_t)a[1]); break;
#endif
#ifdef SYS_rmdir
        case SYS_rmdir:    rc = rmdir(pout); break;
#endif
#ifdef SYS_unlink
        case SYS_unlink:   rc = unlink(pout); break;
#endif
        case SYS_unlinkat: rc = unlinkat(AT_FDCWD, pout, (int)a[2]); break;
#ifdef SYS_rename
        case SYS_rename: {
            char *o2 = (void *)(uintptr_t)a[1];
            if (read_string(pid, o2, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite2(pbuf2, pout2, sizeof pout2, pid);
            rc = rename(pout, pout2); break;
        }
#endif
#ifdef SYS_renameat
        case SYS_renameat: {
            char *o2 = (void *)(uintptr_t)a[3];
            if (read_string(pid, o2, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite2(pbuf2, pout2, sizeof pout2, pid);
            rc = renameat(AT_FDCWD, pout, AT_FDCWD, pout2); break;
        }
#endif
#ifdef SYS_renameat2
        case SYS_renameat2: {
            char *o2 = (void *)(uintptr_t)a[3];
            if (read_string(pid, o2, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite2(pbuf2, pout2, sizeof pout2, pid);
            rc = syscall(SYS_renameat2, AT_FDCWD, pout, AT_FDCWD, pout2,
                         (int)a[4]); break;
        }
#endif
#ifdef SYS_link
        case SYS_link: {
            char *o2 = (void *)(uintptr_t)a[1];
            if (read_string(pid, o2, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite2(pbuf2, pout2, sizeof pout2, pid);
            rc = link(pout, pout2); break;
        }
#endif
#ifdef SYS_linkat
        case SYS_linkat: {
            char *o2 = (void *)(uintptr_t)a[3];
            if (read_string(pid, o2, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite2(pbuf2, pout2, sizeof pout2, pid);
            rc = linkat(AT_FDCWD, pout, AT_FDCWD, pout2, (int)a[4]); break;
        }
#endif
#ifdef SYS_symlink
        /* CATATAN: pbuf2 di symlink/symlinkat adalah ISI TARGET link (string
         * yang disimpan apa adanya), bukan path yang di-resolve sekarang —
         * karena itu sengaja TIDAK lewat rewrite2()/proc_self_fix(): menulis
         * "/proc/<pid>/..." ke dalam symlink akan basi begitu child mati. */
        case SYS_symlink: {
            char *t = (void *)(uintptr_t)a[0];
            if (read_string(pid, t, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite(pbuf2, pout2, sizeof pout2);
            rc = symlink(pout2, pout); break;
        }
#endif
#ifdef SYS_symlinkat
        case SYS_symlinkat: {
            char *t = (void *)(uintptr_t)a[0];
            if (read_string(pid, t, pbuf2, sizeof pbuf2 - 1) < 0) {
                send_resp(listener, req->id, 0, -EFAULT, 0); return;
            }
            rewrite(pbuf2, pout2, sizeof pout2);
            rc = symlinkat(pout2, AT_FDCWD, pout); break;
        }
#endif
#ifdef SYS_access
        case SYS_access:  rc = access(pout, (int)a[1]); break;
#endif
        case SYS_faccessat: rc = syscall(SYS_faccessat, AT_FDCWD, pout,
                                         (int)a[2]); break;
#ifdef SYS_faccessat2
        case SYS_faccessat2: rc = syscall(SYS_faccessat2, AT_FDCWD, pout,
                                          (int)a[2], (int)a[3]); break;
#endif
#ifdef SYS_chmod
        case SYS_chmod:   rc = chmod(pout, (mode_t)a[1]); break;
#endif
        case SYS_fchmodat:
            rc = fchmodat(AT_FDCWD, pout, (mode_t)a[2], (int)a[3]); break;
#ifdef SYS_fchmodat2
        case SYS_fchmodat2:
            rc = syscall(SYS_fchmodat2, AT_FDCWD, pout, (mode_t)a[2],
                         (int)a[3]); break;
#endif
#ifdef SYS_chown
        case SYS_chown:   rc = 0; break;   /* fakeroot: chown selalu EPERM tanpa root
                                              -> berpura-pura sukses (wadah single-user) */
#endif
#ifdef SYS_lchown
        case SYS_lchown:  rc = 0; break;
#endif
        case SYS_fchownat:
            rc = 0; break;
#ifdef SYS_truncate
        case SYS_truncate: rc = truncate(pout, (off_t)a[1]); break;
#endif
        case SYS_utimensat: {
            struct timespec ts[2];
            if (a[2]) {
                if (read_mem(pid, (void *)(uintptr_t)a[2], ts, sizeof ts) < 0) {
                    send_resp(listener, req->id, 0, -EFAULT, 0); return;
                }
                rc = utimensat(AT_FDCWD, pout, ts, (int)a[3]);
            } else rc = utimensat(AT_FDCWD, pout, NULL, (int)a[3]);
            break;
        }
#ifdef SYS_utimes
        case SYS_utimes: {
            struct timeval tv[2];
            if (a[1]) {
                if (read_mem(pid, (void *)(uintptr_t)a[1], tv, sizeof tv) < 0) {
                    send_resp(listener, req->id, 0, -EFAULT, 0); return;
                }
                rc = utimes(pout, tv);
            } else rc = utimes(pout, NULL);
            break;
        }
#endif
#ifdef SYS_mknod
        case SYS_mknod:   rc = mknod(pout, (mode_t)a[1], (dev_t)a[2]); break;
#endif
        case SYS_mknodat:
            rc = mknodat(AT_FDCWD, pout, (mode_t)a[2], (dev_t)a[3]); break;
        default:           rc = -1; errno = ENOSYS;
        }
        send_resp(listener, req->id, 0, rc ? -errno : 0, 0);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    svsp_debug = getenv("SVSP_DEBUG") != NULL;

    int i = 1;
    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (!strncmp(argv[i], "--base=", 7)) base = argv[i] + 7;
        i++;
    }
    if (!base) base = getenv("FAKE_BASE");
    if (!base || !*base) { fprintf(stderr, "svsp: butuh --base=DIR\n"); return 2; }
    baselen = strlen(base);

    if (i >= argc) { fprintf(stderr, "svsp: butuh program\n"); return 2; }
    char *prog = argv[i];

    /* §50: cwd supervisor haruskONSISTEN dengan $PWD yang kita setel di bawah.
     * Shell membandingkan $PWD dengan device+inode cwd saat start; kalau
     * kernel cwd = $BASE tapi $PWD = "/root", ash/bash menolaknya lalu jatuh
     * ke getcwd() — hasilnya path host panjang yang tidak kita mau.
     * FAKE_PWD datang dari fake-run dalam bentuk "/root"; chdir ke
     * $BASE/root supaya keduanya konsisten. Tanpa FAKE_PWD tetap $BASE
     * (perilaku lama) dan $PWD="/". */
    const char *want_pwd = getenv("FAKE_PWD");
    char startdir[4096];
    snprintf(startdir, sizeof startdir, "%s", base);
    if (want_pwd && want_pwd[0] == '/' && strcmp(want_pwd, "/") != 0) {
        snprintf(startdir, sizeof startdir, "%s%s", base, want_pwd);
    }
    if (chdir(startdir) < 0) { fprintf(stderr, "svsp: chdir %s: %s\n", base,
                                   strerror(errno)); return 2; }

    /* env ala wadah */
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    {
        char h[4096]; snprintf(h, sizeof h, "%s/root", base);
        setenv("HOME", h, 1);
        snprintf(h, sizeof h, "%s/tmp", base);
        setenv("TMPDIR", h, 1);
    }
    /* §50: PWD dalam bentuk WADAH, bukan path host. Shell tak bisa chroot()
     * (butuh root) sehingga cwd kernel tetap $BASE/...; tapi $PWD yang tampil
     * di prompt dan dibaca app (npm "verbose cwd", Node process.cwd()) harus
     * path yang natural: "/root", bukan "/data/.../alpine-rootfs/root".
     * Caller (fake-run) menyetel FAKE_PWD; tanpa itu $PWD = "/". */
    {
        const char *want = getenv("FAKE_PWD");
        if (want && want[0] == '/') {
            setenv("PWD", want, 1);
        } else {
            setenv("PWD", "/", 1);
        }
    }
    setenv("USER", "root", 1); setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("FAKE_BASE", base, 1);
    unsetenv("LD_PRELOAD_32");
    /* §45: LD_LIBRARY_PATH dari pemanggil DIPERTAHANKAN. Fake-run
     * menyetelnya ke <base>/lib:<base>/usr/lib — itu yang membuat biner
     * glibc (mis. loader+libc dari Debian di <base>/opt/glibc) bisa
     * mencari libc.so.6 yang benar; tanpa ini glibc jatuh ke /lib/ musl/
     * gcompat -> "no version information available". */
    /* §40: LD_PRELOAD dari pemanggil DIPERTAHANKAN (dulu di-unset di sini).
     * Supervised dynamic butuh shim: handler SIGSYS in-process (fk_sigsys)
     * untuk trap seccomp Android yg tiba sebelum notif svsp (faccessat2,
     * openat2 — HANDOFF §6.1). fake-run menyetelnya ke shim; bila tak ada
     * (pemanggilan langsung), perilaku lama tak berubah. Biner statis
     * mengabaikan LD_PRELOAD. */

    struct sock_filter *f; __u16 flen;
    build_filter(&f, &flen);
    struct sock_fprog prog_f = { .len = flen, .filter = f };

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) < 0) {
        perror("socketpair"); return 2;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }

    if (pid == 0) {
        /* child: pasang filter, kirim listener, exec target */
        close(sp[0]);
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) _exit(126);
        long lfd = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                           SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog_f);
        if (lfd < 0) { fprintf(stderr, "svsp(child): seccomp: %s\n",
                               strerror(errno)); _exit(126); }
        int listener = (int)lfd;

        /* buffer cmsg HARUS selebar CMSG_SPACE (bukan cmsghdr+int polos —
         * kurang padding alignment; di glibc fd hilang karena MSG_CTRUNC). */
        union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } ctl = { 0 };
        struct msghdr mh = { 0 };
        struct iovec iov; char byte = 'L';
        iov.iov_base = &byte; iov.iov_len = 1;
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = ctl.buf; mh.msg_controllen = sizeof ctl.buf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &listener, sizeof(int));
        mh.msg_controllen = cm->cmsg_len;
        if (sendmsg(sp[1], &mh, 0) < 0) _exit(126);
        close(sp[1]);

        execv(prog, &argv[i]);
        fprintf(stderr, "svsp(child): exec %s: %s\n", prog, strerror(errno));
        _exit(127);
    }

    /* parent: terima listener lalu layani */
    close(sp[1]);
    int listener = -1;
    char byte; struct iovec iov = { &byte, 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } ctl = { 0 };
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = ctl.buf; mh.msg_controllen = sizeof ctl.buf;
    if (recvmsg(sp[0], &mh, 0) < 0) { perror("recvmsg"); return 2; }
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    if (cm && cm->cmsg_type == SCM_RIGHTS)
        memcpy(&listener, CMSG_DATA(cm), sizeof(int));
    close(sp[0]);
    if (listener < 0) { fprintf(stderr, "svsp: tak dapat listener\n"); return 2; }
    DBG("listener=%d target_pid=%d base=%s\n", listener, (int)pid, base);

    /*
     * Cucu target BUKAN anak svsp — hanya TARGET yg bisa di-wait oleh svsp.
     * Zombie cucu = tanggung jawab proses target (normal, sama seperti
     * tanpa supervisor). SVSP hanya boleh merawat target + exit code-nya.
     * Jangan pernah waitpid(-1): satu-satunya anak svsp adalah target,
     * mereapnya duluan = status exit hilang (regresi rc selalu 1).
     */
    int tst = -1;   /* status target yg direap di loop (utk exit code) */
    for (;;) {
        /* poll dgn batas waktu: kernel Android ini TIDAK membangunkan RECV
         * yang terblokir saat target mati -> deteksi via waitpid(WNOHANG) */
        struct pollfd pf = { .fd = listener, .events = POLLIN };
        int pr = poll(&pf, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            DBG("poll err %d\n", errno); break;
        }
        if (pr == 0) {
            int st;
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) {
                tst = st;                  /* simpan utk exit code */
                DBG("target %d hilang (st=%d)\n", (int)pid, st);
                break;                     /* child sudah keluar */
            }
            if (r < 0 && errno == ECHILD) break;   /* sudah keluar? aman berhenti */
            continue;                      /* masih hidup, tunggu notif */
        }
        struct seccomp_notif req; memset(&req, 0, sizeof req);
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req) < 0) {
            if (errno == ENOENT) break;   /* target hilang */
            if (errno == EINTR) continue;
            DBG("RECV err %d\n", errno); break;
        }
        handle(listener, &req);
    }
    /* propagate child exit code: preferensi status yg direap di loop;
     * fallback tunggu target bila loop keluar lewat RECV-ENOENT */
    {
        int st = tst;
        if (st < 0) {
            pid_t r = waitpid(pid, &st, 0);
            if (r < 0) st = tst;           /* ECHILD — status tak tersedia */
        }
        if (st >= 0) {
            if (WIFEXITED(st)) { free(f); return WEXITSTATUS(st); }
            if (WIFSIGNALED(st)) { free(f); return 128 + WTERMSIG(st); }
        }
    }
    free(f);
    return 1;
}