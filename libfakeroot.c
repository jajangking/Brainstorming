/*
 * libfakeroot.so - isolasi path untuk binary musl (fakechroot tanpa root/chroot).
 *
 * Prinsip: semua path ABSOLUT dari program di-rewrite menjadi $FAKE_BASE/path,
 * persis seperti chroot tetapi tanpa chroot. Path relatif dibiarkan (wadah
 * harus chdir($FAKE_BASE) dulu di wrapper). Sembarang path di luar base
 * (host /dev /proc /sys /data dll) dipasang sbg symlink DI DALAM base oleh
 * pengguna (mis. base/dev/null -> /dev/null) sehingga resolusi kernel yang
 * menyelesaikannya, bukan kita.
 *
 * Sekalian memasang handler SIGSYS untuk menetralkan seccomp Android
 * (proses yang tidak memblokir SIGSYS; untuk yang memblokir, gunakan
 * loader musl yang di-patch).
 *
 * Kompilasi (musl):
 *   clang --target=aarch64-alpine-linux-musl --sysroot=$ROOTFS \
 *         -fPIC -shared -O2 -o libfakeroot.so libfakeroot.c
 *
 * Pemakaian (wrapper):
 *   cd $ROOTFS
 *   LD_PRELOAD=/path/libfakeroot.so FAKE_BASE=$ROOTFS \
 *     $ROOTFS/lib/ld-musl-aarch64.so.1 <binary> ...
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <dlfcn.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <alloca.h>
#include <utime.h>
#include <sys/statfs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

extern char **environ;

static const char *fk_base;

/* ------------------------------------------------------------------ */
/* 1. SIGSYS handler (netralisir seccomp)                             */
/* ------------------------------------------------------------------ */
static void fk_sigsys(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    (void)sig;
    uc->uc_mcontext.pc = (unsigned long)info->si_call_addr + 4;
    switch (info->si_syscall) {
    case SYS_setgid: case SYS_setuid: case SYS_setreuid:
    case SYS_setregid: case SYS_setresuid: case SYS_setresgid:
    case SYS_setgroups: case SYS_setfsuid: case SYS_setfsgid:
        uc->uc_mcontext.regs[0] = 0;                 /* sukses */
        break;
    default:
        uc->uc_mcontext.regs[0] = (unsigned long)-1; /* -EPERM */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 2. Path rewriting                                                  */
/* ------------------------------------------------------------------ */
/* Prefix path HOST Android yang TIDAK boleh di-rewrite ke $BASE.
 * Konsisten dgn svsp passthrough (HANDOFF §19.2.2). Tanpa ini, akses
 * artefak host (mis. file di /data/data/.../tmp) via wadah jadi ENOENT
 * karena di-rewrite ke $BASE/data/... (device-feedback ronde 2 butir 5). */
static int fk_host_prefix(const char *p) {
    return !strncmp(p, "/data/", 6) || !strcmp(p, "/data")
        || !strncmp(p, "/system/", 8) || !strcmp(p, "/system")
        || !strncmp(p, "/apex/", 6)
        || !strncmp(p, "/vendor/", 8)
        || !strncmp(p, "/product/", 9)
        || !strncmp(p, "/linkerconfig/", 14)
        || !strncmp(p, "/dev/", 5) || !strcmp(p, "/dev")
        || !strncmp(p, "/proc/", 6) || !strcmp(p, "/proc")
        || !strncmp(p, "/sys/", 5) || !strcmp(p, "/sys");
}

static void fk_rewrite(char *out, size_t n, const char *p) {
    if (fk_host_prefix(p)) {                          /* host: apa adanya */
        snprintf(out, n, "%s", p);
        return;
    }
    size_t bl = strlen(fk_base);
    if (strncmp(p, fk_base, bl) == 0 &&
        (p[bl] == '\0' || p[bl] == '/')) {
        snprintf(out, n, "%s", p);                   /* sudah di base */
        return;
    }
    snprintf(out, n, "%s%s", fk_base, p);
}

static int fk_is_abs(const char *p) { return p && p[0] == '/'; }

#define NEXT(fn) \
    static __typeof__(&fn) next_##fn; \
    do { if (!next_##fn) next_##fn = dlsym(RTLD_NEXT, #fn); } while (0);
#define CALL(fn, ...) (next_##fn ? next_##fn(__VA_ARGS__) : (errno = ENOSYS, (__typeof__(next_##fn(__VA_ARGS__)))-1))

/* Android: men-publish anon O_TMPFILE lewat linkat("/proc/self/fd/N") GAGAL
 * (EACCES/ENOENT) -> apk-tools v3 (dan lain-lain) yang menulis via O_TMPFILE
 * mati di langkah publish. Mask flag O_TMPFILE -> openat(...,".",O_RDWR)
 * -> EISDIR -> pemakai jatuh ke fallback nama-temp + renameat (relatif, jalan).
 *
 * PENTING: mask HANYA bila bit anon benar-benar set, uji persis semantik
 * kernel ((flags & O_TMPFILE) == O_TMPFILE). O_TMPFILE = __O_TMPFILE |
 * O_DIRECTORY; masking tanpa syarat menggerus bit O_DIRECTORY dari open
 * biasa (open(dir, O_RDONLY|O_DIRECTORY) lalu sukses membuka NON-dir). */
#ifdef O_TMPFILE
#define FK_HAS_TMPFILE(f) (((f) & O_TMPFILE) == O_TMPFILE)
#define FK_FLAGS(f) (FK_HAS_TMPFILE(f) ? ((f) & ~O_TMPFILE) : (f))
#else
#define FK_HAS_TMPFILE(f) 0
#define FK_FLAGS(f) (f)
#endif

/* ---- debug (FK_DEBUG=1 utk melacak open/rename yang gagal) ---- */
static int fk_dbg_on(void) {
    static int v = -1;
    if (v < 0) v = fk_base && getenv("FK_DEBUG") ? 1 : 0;
    return v;
}
static void fk_dbg(const char *fmt, ...) {
    if (!fk_dbg_on()) return;
    va_list ap; va_start(ap, fmt);
    char msg[512]; vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    write(2, "[fk] ", 5);
    write(2, msg, strlen(msg));
    write(2, "\n", 1);
}

/* ---- open family ---- */
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(open);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); int fd = CALL(open, b, FK_FLAGS(flags), mode); if (fd < 0) fk_dbg("open %s -> %s FAIL errno=%d", path, b, errno); return fd; }
    return CALL(open, path, FK_FLAGS(flags), mode);
}
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(open64);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); return CALL(open64, b, FK_FLAGS(flags), mode); }
    return CALL(open64, path, FK_FLAGS(flags), mode);
}
int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(openat);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); int fd = CALL(openat, dirfd, b, FK_FLAGS(flags), mode); if (fd < 0) fk_dbg("openat(%d) %s -> %s FAIL errno=%d", dirfd, path, b, errno); return fd; }
    return CALL(openat, dirfd, path, FK_FLAGS(flags), mode);
}
int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(openat64);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); return CALL(openat64, dirfd, b, FK_FLAGS(flags), mode); }
    return CALL(openat64, dirfd, path, FK_FLAGS(flags), mode);
}

/* ---- mkstemp family ---- 
 * musl mengimplementasikan mkstemp/mkdtemp/tmpfile dengan megil OPEN INTERNAL
 * (direct-bound, PLT hanya untuk malloc family) -> shim tak bisa rewrite
 * template -> file dibuat di path HOST (tak ada) -> ENOENT. Karena itu
 * interpose FUNGSINYA (yang dipanggil apk dkk lewat PLT), rewrite template,
 * teruskan ke real. */
int mkstemp(char *t) {
    NEXT(mkstemp);
    if (fk_is_abs(t)) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, t);
        int fd = CALL(mkstemp, b);
        if (fd < 0) fk_dbg("mkstemp %s -> %s FAIL errno=%d", t, b, errno);
        return fd;
    }
    return CALL(mkstemp, t);
}
int mkostemp(char *t, int fl) {
    NEXT(mkostemp);
    if (fk_is_abs(t)) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, t);
        int fd = CALL(mkostemp, b, fl);
        if (fd < 0) fk_dbg("mkostemp %s -> %s FAIL errno=%d", t, b, errno);
        return fd;
    }
    return CALL(mkostemp, t, fl);
}
int mkstemps(char *t, int sl) {
    NEXT(mkstemps);
    if (fk_is_abs(t)) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, t);
        int fd = CALL(mkstemps, b, sl);
        if (fd < 0) fk_dbg("mkstemps %s -> %s FAIL errno=%d", t, b, errno);
        return fd;
    }
    return CALL(mkstemps, t, sl);
}
int mkostemps(char *t, int sl, int fl) {
    NEXT(mkostemps);
    if (fk_is_abs(t)) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, t);
        int fd = CALL(mkostemps, b, sl, fl);
        if (fd < 0) fk_dbg("mkostemps %s -> %s FAIL errno=%d", t, b, errno);
        return fd;
    }
    return CALL(mkostemps, t, sl, fl);
}
char *mkdtemp(char *t) {
    NEXT(mkdtemp);
    if (fk_is_abs(t)) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, t);
        char *r = CALL(mkdtemp, b);
        if (r) memcpy(t, r, strlen(r) + 1);   /* template harus diupdate! */
        else fk_dbg("mkdtemp %s -> %s FAIL errno=%d", t, b, errno);
        return r ? t : NULL;
    }
    return CALL(mkdtemp, t);
}
int creat(const char *p, mode_t m) {
    NEXT(creat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(creat, b, m); }
    return CALL(creat, p, m);
}
/* tmpfile musl = mkstemp("/tmp/tmpfile-XXXXXX") internal -> bypass. Buat sendiri
 * di $BASE/tmp (template base-prefix aman: fk_rewrite meneruskannya apa adanya). */
FILE *tmpfile(void) {
    char t[PATH_MAX];
    snprintf(t, sizeof t, "%s/tmp/tmpfile-XXXXXX", fk_base);
    int fd = mkstemp(t);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w+b");
    if (!f) { close(fd); unlink(t); }
    return f;
}

/* ---- stat family ---- */
int stat(const char *p, struct stat *s) {
    NEXT(stat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(stat, b, s); }
    return CALL(stat, p, s);
}
int lstat(const char *p, struct stat *s) {
    NEXT(lstat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(lstat, b, s); }
    return CALL(lstat, p, s);
}
int fstatat(int dirfd, const char *p, struct stat *s, int fl) {
    NEXT(fstatat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(fstatat, dirfd, b, s, fl); }
    return CALL(fstatat, dirfd, p, s, fl);
}
#ifdef SYS_statx
int statx(int dirfd, const char *p, int flags, unsigned int mask, struct statx *stx) {
    NEXT(statx);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(statx, dirfd, b, flags, mask, stx); }
    return CALL(statx, dirfd, p, flags, mask, stx);
}
#endif
#ifdef __GLIBC__
int stat64(const char *p, struct stat64 *s) {
    NEXT(stat64);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(stat64, b, s); }
    return CALL(stat64, p, s);
}
int lstat64(const char *p, struct stat64 *s) {
    NEXT(lstat64);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(lstat64, b, s); }
    return CALL(lstat64, p, s);
}
int fstatat64(int dirfd, const char *p, struct stat64 *s, int fl) {
    NEXT(fstatat64);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(fstatat64, dirfd, b, s, fl); }
    return CALL(fstatat64, dirfd, p, s, fl);
}
#endif
int access(const char *p, int m) {
    NEXT(access);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(access, b, m); }
    return CALL(access, p, m);
}
int faccessat(int dirfd, const char *p, int m, int fl) {
    NEXT(faccessat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(faccessat, dirfd, b, m, fl); }
    return CALL(faccessat, dirfd, p, m, fl);
}

/* ---- loader re-exec (binary dinamis ber-interp mentah) ---- */
static const char *fk_loader_path(void) {
    static char lp[PATH_MAX];
    if (!lp[0]) snprintf(lp, sizeof lp, "%s/lib/ld-musl-patched.so.1", fk_base);
    return lp;
}

/* 1 jika path = ELF dinamis dengan PT_INTERP mentah (mis. /lib/ld-musl-aarch64.so.1):
 * eksekusi langsung oleh kernel akan gagal (host /lib tidak ada) maka harus
 * dijalankan lewat loader patched. 0 untuk statik/script/non-ELF/interp patched
 * (path absolute /data/... yang host-resolvable, sudah aman dieksekusi polos). */
static int fk_needs_loader(const char *path) {
    if (strcmp(path, fk_loader_path()) == 0) return 0;          /* jangan rekursif */
    NEXT(open);
    int fd = CALL(open, path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char h[64];
    ssize_t n = read(fd, h, 64);
    if (n < 64 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2) {
        close(fd); return 0;                                    /* non-ELF / bukan ELF64 */
    }
    unsigned long phoff = 0;
    for (int i = 0; i < 8; i++) phoff |= (unsigned long)h[32 + i] << (8 * i);
    unsigned int phesz = h[54] | ((unsigned int)h[55] << 8);
    unsigned int phnum = h[56] | ((unsigned int)h[57] << 8);
    if (phesz < 56 || phnum == 0 || phnum > 64 || phesz > 128) { close(fd); return 0; }
    unsigned char *ph = alloca((size_t)phesz * phnum);
    n = pread(fd, ph, (size_t)phesz * phnum, (off_t)phoff);
    if (n < (ssize_t)(phesz * phnum)) { close(fd); return 0; }
    for (unsigned int i = 0; i < phnum; i++) {
        unsigned char *e = ph + i * phesz;
        unsigned long type = (unsigned long)e[0] | ((unsigned long)e[1] << 8) |
                             ((unsigned long)e[2] << 16) | ((unsigned long)e[3] << 24);
        if (type != 3) continue;                                /* PT_INTERP */
        unsigned long offp = 0, sz = 0;
        for (int k = 0; k < 8; k++) {
            offp |= (unsigned long)e[8 + k] << (8 * k);
            sz   |= (unsigned long)e[32 + k] << (8 * k);
        }
        if (sz == 0) break;
        if (sz > PATH_MAX - 1) sz = PATH_MAX - 1;
        char interp[PATH_MAX];
        n = pread(fd, interp, sz, (off_t)offp);
        close(fd);
        if (n < 1) return 0;
        interp[n] = '\0';
        size_t bl = strlen(fk_base);
        int patched = (strncmp(interp, fk_base, bl) == 0 &&
                       (interp[bl] == '/' || interp[bl] == '\0'));
        return patched ? 0 : 1;                                 /* interp resolvable? polos; selain itu loader */
    }
    close(fd);
    return 0;                                                   /* statik / tanpa PT_INTERP */
}

/* Script shebang: kernel mem-resolve interp (mis. "#!/bin/sh") di HOST ->
 * salah (Android /bin/sh ≠ ash wadah). Deteksi "#!" dan jalankan interp
 * dari dalam base secara eksplisit; interp di-rewrite + dilewatkan ke
 * fk_exec_one (loader bila perlu). Return: 0=exec berhasil (tak kembali),
 * -1=gagal (errno ter-set, JANGAN lanjut ke execve polos), -2=bukan script
 * shebang (biarkan kernel menangani). */
static int fk_exec_one(const char *hostpath, char *const argv[], char *const envp[]);
static int fk_try_shebang_exec(const char *hostpath, char *const argv[], char *const envp[]) {
    NEXT(open);
    int fd = CALL(open, hostpath, O_RDONLY);
    if (fd < 0) return -2;
    char hdr[160];
    ssize_t n = read(fd, hdr, sizeof hdr - 1);
    NEXT(close);
    CALL(close, fd);
    if (n < 3 || hdr[0] != '#' || hdr[1] != '!') return -2;
    hdr[n] = '\0';
    for (ssize_t i = 0; i < n; i++) if (hdr[i] == '\n') { hdr[i] = '\0'; break; }
    char *p = hdr + 2;
    while (*p == ' ') p++;
    char *interp = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    int ilen = (int)(p - interp);
    if (ilen <= 0 || ilen >= PATH_MAX) return -2;
    while (*p == ' ' || *p == '\t') p++;
    char *sarg = p;                        /* argumen shebang opsional */
    int slen = (int)strlen(sarg);
    if (interp[0] != '/') return -2;       /* interp relatif: biarkan kernel */
    char ib[PATH_MAX];
    memcpy(ib, interp, (size_t)ilen); ib[ilen] = '\0';
    if (strcmp(ib, hostpath) == 0) return -2;   /* interp == target: hindari loop */
    char ir[PATH_MAX];
    fk_rewrite(ir, sizeof ir, ib);
    int narg = 0;
    while (argv && argv[narg]) narg++;
    char **nav = malloc(((size_t)2 + (slen ? 1 : 0) + (size_t)narg) * sizeof(char *));
    if (!nav) { errno = ENOMEM; return -1; }
    int m = 0;
    nav[m++] = ir;
    if (slen) {
        nav[m] = malloc((size_t)slen + 1);
        if (!nav[m]) { free(nav); errno = ENOMEM; return -1; }
        memcpy(nav[m], sarg, (size_t)slen + 1);
        m++;
    }
    nav[m++] = (char *)hostpath;
    for (int k = 1; k < narg; k++) nav[m++] = argv[k];
    nav[m] = NULL;
    int r = fk_exec_one(ir, nav, envp);
    free(nav);
    return r;                              /* -1 kalau interp ikut gagal */
}

/* envp minimal (apk scrub script env: hanya APK_SCRIPT/APK_PACKAGE) -> script
 * wadah jalan TANPA shim/LD_PRELOAD/PATH (path host, hpanya rusak). Bila envp
 * tidak membawa FAKE_BASE wadah, gabungkan environ induk (kunci envp menang). */
static int fk_env_has_key(char *const envp[], const char *key, size_t klen) {
    for (int i = 0; envp[i]; i++)
        if (strncmp(envp[i], key, klen) == 0 && envp[i][klen] == '=') return 1;
    return 0;
}

/* Snapshot environ saat constructor. SUMBER MERGE HARUS INI, bukan environ
 * live: busybox `env -i` memanggil clearenv() yang mengosongkan environ live
 * (musl men-set environ=NULL). Dengan environ live, script trigger apk
 * (yang di-exec dengan envp = PATH/APK_SCRIPT/APK_PACKAGE saja) jalan TANPA
 * LD_PRELOAD/FAKE_BASE -> "not found" rc=127 (bug §17.1 HANDOFF). */
static char **fk_env0;   /* deep copy, NULL-terminated */
static void fk_snapshot_env(void) {
    if (fk_env0) return;
    int ne = 0; while (environ && environ[ne]) ne++;
    char **snap = malloc(((size_t)ne + 1) * sizeof(char *));
    if (!snap) return;
    int m = 0;
    for (int i = 0; i < ne; i++) {
        snap[m] = strdup(environ[i]);
        if (!snap[m]) break;
        m++;
    }
    snap[m] = NULL;
    fk_env0 = snap;
}

static char **fk_ensure_wadah_env(char *const envp[]) {
    if (envp && fk_env_has_key(envp, "FAKE_BASE", 9)) return (char **)envp;
    /* sumber merge: environ live bila masih hidup, jatuh ke snapshot.
     * entri envp menang atas sumber. */
    char *const *src = (environ && environ[0]) ? (char *const *)environ
                                               : (char *const *)fk_env0;
    int ne = 0; while (src && src[ne]) ne++;
    int np = 0; while (envp && envp[np]) np++;
    char **merged = malloc(((size_t)ne + np + 1) * sizeof(char *));
    if (!merged) return envp ? (char **)envp : (char **)src;
    int m = 0;
    for (int i = 0; i < np; i++) merged[m++] = envp[i];
    for (int j = 0; j < ne; j++) {
        const char *kv = src[j];
        const char *eq = strchr(kv, '=');
        size_t kl = eq ? (size_t)(eq - kv) : strlen(kv);
        if (!envp || !fk_env_has_key(envp, kv, kl)) merged[m++] = (char *)kv;
    }
    merged[m] = NULL;
    return merged;
}

/* §25 (/proc/self/exe saat jalan via loader):
 * Bila program dijalankan sebagai "ld-musl-patched.so.1 /path/prog args...",
 * maka /proc/self/exe milik proses = LOADER, bukan prog. Bun/OpenCode memakai
 * execPath (= /proc/self/exe) untuk men-spawn dirinya sendiri, sehingga
 * terbit "ld-musl-patched.so.1: cannot load serve: No such file or directory"
 * (loader menerima argv[1]="serve" sebagai nama program). Perbaikan:
 * wariskan path program lewat env FAKEROOT_EXE, lalu readlink("/proc/self/exe")
 * menjawab path itu — dan sediakan jaring pengaman bila loader tetap
 * dipanggil tanpa nama program. */
#define FK_EXE_KEY "FAKEROOT_EXE="

static const char *fk_self_exe(void) {
    const char *v = getenv("FAKEROOT_EXE");
    return (v && *v) ? v : NULL;
}

/* argv utk loader: [loader, hostpath, argv[1..]] + env FAKEROOT_EXE=hostpath */
static char **fk_loader_env(char *const envp[], const char *hostpath) {
    int ne = 0; while (envp && envp[ne]) ne++;
    char **out = malloc(((size_t)ne + 2) * sizeof(char *));
    if (!out) return (char **)envp;
    int m = 0;
    for (int i = 0; i < ne; i++) {
        if (strncmp(envp[i], FK_EXE_KEY, sizeof FK_EXE_KEY - 1) == 0) continue;
        out[m++] = envp[i];
    }
    size_t need = sizeof FK_EXE_KEY + strlen(hostpath);
    char *ent = malloc(need);
    if (ent) { snprintf(ent, need, FK_EXE_KEY "%s", hostpath); out[m++] = ent; }
    out[m] = NULL;
    return out;
}

/* Jaring pengaman: ada yang meng-exec LOADER langsung dgn argv[1] yang bukan
 * program (mis. "serve") karena ia membaca /proc/self/exe. Sisipkan kembali
 * program sebenarnya di depan. Return argv baru atau NULL bila tak perlu. */
static char **fk_fix_loader_argv(const char *hostpath, char *const argv[]) {
    const char *self = fk_self_exe();
    if (!self) return NULL;
    if (strcmp(hostpath, fk_loader_path()) != 0) return NULL;
    if (!argv || !argv[0]) return NULL;
    if (argv[1] && argv[1][0] == '/') return NULL;   /* sudah benar */
    int n = 0; while (argv[n]) n++;
    char **nav = malloc(((size_t)n + 2) * sizeof(char *));
    if (!nav) return NULL;
    nav[0] = argv[0];
    nav[1] = (char *)self;
    for (int k = 1; k < n; k++) nav[k + 1] = argv[k];
    nav[n + 1] = NULL;
    return nav;
}

/* exec satu path (sudah di-rewrite ke host) — lewat loader patched bila perlu,
 * persis jalur cepat fake-run: loader host argv... (argv target dipertahankan). */
static int fk_exec_one(const char *hostpath, char *const argv[], char *const envp[]) {
    char **menv = fk_ensure_wadah_env(envp);
    char *const *e2 = (char *const *)menv;
    if (fk_needs_loader(hostpath)) {
        const char *loader = fk_loader_path();
        int n = 0;
        while (argv && argv[n]) n++;
        /* loader hostpath argv[1] argv[2] ... NULL
         * (argv[0] = program name TIDAK di-copy karena hostpath sudah jadi argv[0]
         *  saat loader me-re-exec: ld-musl.so.1 hostpath → program argv[0] = hostpath) */
        char **nav = malloc(((size_t)n + 2) * sizeof(char *));
        if (!nav) { errno = ENOMEM; return -1; }
        nav[0] = (char *)loader;
        nav[1] = (char *)hostpath;
        int m = 2;
        for (int k = 1; k < n; k++) nav[m++] = argv[k];  /* skip argv[0] */
        nav[m] = NULL;
        NEXT(execve);
        char **lenv = fk_loader_env(e2, hostpath);
        int r = CALL(execve, loader, nav, lenv);
        free(nav);
        return r;
    }
    {   /* exec langsung ke loader dgn argv[1] bukan program (Bun execPath) */
        char **fx = fk_fix_loader_argv(hostpath, argv);
        if (fx) {
            NEXT(execve);
            int r = CALL(execve, hostpath, fx, e2);
            free(fx);
            return r;
        }
    }
    int r = fk_try_shebang_exec(hostpath, argv, e2);
    if (r != -2) return r;                   /* script shebang: hasil sudah final */
    NEXT(execve);
    return CALL(execve, hostpath, argv, e2);
}

/* ---- exec family ---- */
int execve(const char *p, char *const argv[], char *const envp[]) {
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return fk_exec_one(b, argv, envp); }
    return fk_exec_one(p, argv, envp);
}
int execl(const char *p, const char *a0, ...) {
    NEXT(execl);
    char *b;
    if (fk_is_abs(p)) { b = malloc(PATH_MAX); fk_rewrite(b, PATH_MAX, p); }
    else b = (char *)p;
    /* hitung argc: scan varargs sampai NULL (jangan hardcode 64) */
    va_list ap, ap2;
    va_start(ap, a0);
    va_copy(ap2, ap);
    int argc = 0;
    while (va_arg(ap2, const char *) != NULL) argc++;
    va_end(ap2);
    /* susun argv */
    char **argv = alloca((size_t)(argc + 2) * sizeof(char *));
    argv[0] = (char *)a0;
    for (int i = 1; i <= argc; i++) argv[i] = va_arg(ap, char *);
    va_end(ap);
    argv[argc + 1] = NULL;
    long r = fk_exec_one(b, argv, environ);
    if (b != p) free(b);
    return (int)r;
}

static int fk_execvp_search(const char *file, char *const argv[], char *const envp[], int use_path) {
    if (!use_path || strchr(file, '/')) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, file); return fk_exec_one(b, argv, envp);
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const char *p = path;
    for (;;) {
        const char *end = strchr(p, ':');
        size_t dl = end ? (size_t)(end - p) : strlen(p);
        if (dl == 0) dl = 1, p = ".";
        char *cand = malloc(dl + 1 + strlen(file) + 1);
        memcpy(cand, p, dl); cand[dl] = (dl == 1 && p[0] == '.') ? 0 : '/';
        if (!(dl == 1 && p[0] == '.')) strcpy(cand + dl + 1, file); else strcpy(cand + dl, file);
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, cand);
        free(cand);
        long r = fk_exec_one(b, argv, envp);
        if (errno != ENOENT) return (int)r;
        if (!end) break;
        p = end + 1;
    }
    errno = ENOENT;
    return -1;
}
int execvp(const char *file, char *const argv[]) {
    return fk_execvp_search(file, argv, environ, 1);
}
int execvpe(const char *file, char *const argv[], char *const envp[]) {
    return fk_execvp_search(file, argv, envp, 1);
}
int execv(const char *p, char *const argv[]) {
    char b[PATH_MAX]; fk_rewrite(b, sizeof b, p);
    return fk_exec_one(b, argv, environ);
}

/* ---- spawn/exec-via-fd (bug §17.1 HANDOFF) ----
 * posix_spawn musl memanggil pointer __execve internal langsung dari
 * clone(CLONE_VM|CLONE_VFORK) — TIDAK lewat PLT, jadi interposer execve
 * kita tak pernah melihatnya. Maka posix_spawn(p) di-interpose langsung:
 * path di-rewrite + loader dirangkai di sini. fexecve/execveat menutup
 * varian exec lewat fd; execle/execlp menutup varian varargs yang masih
 * bolong. */
static void fk_host_path(char *out, size_t n, const char *p) {
    if (fk_is_abs(p)) fk_rewrite(out, n, p);
    else snprintf(out, n, "%s", p);
}

static int fk_spawn_common(pid_t *res, const char *hostpath,
                           const posix_spawn_file_actions_t *fa,
                           const posix_spawnattr_t *attr,
                           char *const argv[], char *const envp[]) {
    char **menv = fk_ensure_wadah_env(envp);
    NEXT(posix_spawn);
    if (fk_needs_loader(hostpath)) {
        /* ELF dinamis wadah: rangkai "loader hostpath argv..." (sama dgn
         * jalur cepat fake-run / fk_exec_one). */
        int n = 0; while (argv && argv[n]) n++;
        char **nav = malloc(((size_t)n + 2) * sizeof(char *));
        if (!nav) return ENOMEM;
        const char *loader = fk_loader_path();
        nav[0] = (char *)loader;
        nav[1] = (char *)hostpath;
        for (int k = 1; k < n; k++) nav[k + 1] = argv[k];
        nav[n + 1] = NULL;
        char **lenv = fk_loader_env(menv, hostpath);
        int r = CALL(posix_spawn, res, loader, fa, attr, nav, lenv);
        int e = errno; free(nav); errno = e;
        return r;
    }
    {   /* spawn langsung ke loader dgn argv[1] bukan program (Bun execPath) */
        char **fx = fk_fix_loader_argv(hostpath, argv);
        if (fx) {
            int r = CALL(posix_spawn, res, hostpath, fa, attr, fx, menv);
            int e = errno; free(fx); errno = e;
            return r;
        }
    }
    int r = CALL(posix_spawn, res, hostpath, fa, attr, argv, menv);
    if (r == ENOENT) {
        /* shebang dgn interpreter wadah (mis. #!/bin/sh): kernel host tak
         * kenal pathnya -> ENOENT. Fallback: child menangani via
         * fk_exec_one (paham shebang). attr/file_actions tidak diterapkan
         * ulang di jalur fallback — memadai utk skenario wadah. */
        pid_t pid = fork();
        if (pid == 0) { fk_exec_one(hostpath, argv, envp); _exit(127); }
        if (pid > 0) { if (res) *res = pid; return 0; }
        return errno;
    }
    return r;
}

int posix_spawn(pid_t *res, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    char b[PATH_MAX]; fk_host_path(b, sizeof b, path);
    return fk_spawn_common(res, b, fa, attr, argv, envp);
}

int posix_spawnp(pid_t *res, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    char b[PATH_MAX];
    if (strchr(file, '/')) { fk_host_path(b, sizeof b, file);
                             return fk_spawn_common(res, b, fa, attr, argv, envp); }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const char *p = path;
    for (;;) {
        const char *end = strchr(p, ':');
        size_t dl = end ? (size_t)(end - p) : strlen(p);
        char cand[PATH_MAX];
        if (dl == 0) snprintf(cand, sizeof cand, "%s", file);
        else snprintf(cand, sizeof cand, "%.*s/%s", (int)dl, p, file);
        fk_rewrite(b, sizeof b, cand);
        if (access(b, X_OK) == 0)
            return fk_spawn_common(res, b, fa, attr, argv, envp);
        if (!end) break;
        p = end + 1;
    }
    return ENOENT;
}

int fexecve(int fd, char *const argv[], char *const envp[]) {
    /* fd hasil open() yang sudah di-shim = fd file HOST; exec via procfd
     * mempertahankannya. "/proc/self/fd/N" di-rewrite jadi $BASE/proc/self/...
     * tetapi $BASE/proc symlink ke /proc -> resolusi sama persis. */
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    return execve(p, argv, envp);
}

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags) {
    if ((flags & AT_EMPTY_PATH) && (!path || !path[0])) {
        char p[64];
        snprintf(p, sizeof p, "/proc/self/fd/%d", dirfd);
        return execve(p, argv, envp);
    }
    char host[PATH_MAX];
    if (path && path[0] == '/') {
        fk_rewrite(host, sizeof host, path);
    } else if (dirfd == AT_FDCWD) {
        snprintf(host, sizeof host, "%s", path ? path : "");
    } else {
        char pl[64], d[PATH_MAX];
        snprintf(pl, sizeof pl, "/proc/self/fd/%d", dirfd);
        ssize_t rl = readlink(pl, d, sizeof d - 1);
        if (rl < 0) return -1;
        d[rl] = '\0';
        if (rl > 0 && d[rl - 1] == '/') d[rl - 1] = '\0';
        snprintf(host, sizeof host, "%s/%s", d, path ? path : "");
    }
    return fk_exec_one(host, argv, envp);
}

int execle(const char *p, const char *a0, ...) {
    va_list ap, ap2;
    va_start(ap, a0);
    va_copy(ap2, ap);
    int argc = 0;
    while (va_arg(ap2, const char *) != NULL) argc++;
    va_end(ap2);
    char **argv = alloca((size_t)(argc + 2) * sizeof(char *));
    argv[0] = (char *)a0;
    for (int i = 1; i <= argc; i++) argv[i] = va_arg(ap, char *);
    char *const *envp = va_arg(ap, char *const *);
    va_end(ap);
    char *b;
    if (fk_is_abs(p)) { b = malloc(PATH_MAX); fk_rewrite(b, PATH_MAX, p); }
    else b = (char *)p;
    long r = fk_exec_one(b, argv, envp);
    if (b != p) free(b);
    return (int)r;
}

int execlp(const char *p, const char *a0, ...) {
    va_list ap, ap2;
    va_start(ap, a0);
    va_copy(ap2, ap);
    int argc = 0;
    while (va_arg(ap2, const char *) != NULL) argc++;
    va_end(ap2);
    char **argv = alloca((size_t)(argc + 2) * sizeof(char *));
    argv[0] = (char *)a0;
    for (int i = 1; i <= argc; i++) argv[i] = va_arg(ap, char *);
    va_end(ap);
    argv[argc + 1] = NULL;
    return fk_execvp_search(p, argv, environ, 1);
}

/* ---- stdio ---- */
FILE *fopen(const char *p, const char *m) {
    NEXT(fopen);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); FILE *f = CALL(fopen, b, m); if (!f) fk_dbg("fopen %s -> %s FAIL errno=%d", p, b, errno); return f; }
    return CALL(fopen, p, m);
}
FILE *fopen64(const char *p, const char *m) {
    NEXT(fopen64);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(fopen64, b, m); }
    return CALL(fopen64, p, m);
}
FILE *freopen(const char *p, const char *m, FILE *f) {
    NEXT(freopen);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(freopen, b, m, f); }
    return CALL(freopen, p, m, f);
}
FILE *freopen64(const char *p, const char *m, FILE *f) {
    NEXT(freopen64);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(freopen64, b, m, f); }
    return CALL(freopen64, p, m, f);
}
DIR *opendir(const char *p) {
    NEXT(opendir);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(opendir, b); }
    return CALL(opendir, p);
}

/* ---- dir / link / file ops ---- */
int chdir(const char *p) {
    NEXT(chdir);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(chdir, b); }
    return CALL(chdir, p);
}
int chmod(const char *p, mode_t m) {
    NEXT(chmod);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(chmod, b, m); }
    return CALL(chmod, p, m);
}
int fchmodat(int dirfd, const char *p, mode_t m, int fl) {
    NEXT(fchmodat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(fchmodat, dirfd, b, m, fl); }
    return CALL(fchmodat, dirfd, p, m, fl);
}
int chown(const char *p, uid_t u, gid_t g) {
    /* Fakeroot-style: chown selalu EPERM tanpa root — berpura-pura sukses.
     * (file tetap milik user; wadah single-user, uid/gid 0 tak terpakai.) */
    (void)p; (void)u; (void)g;
    errno = 0;
    return 0;
}
int lchown(const char *p, uid_t u, gid_t g) {
    (void)p; (void)u; (void)g;
    errno = 0;
    return 0;
}
int fchown(int fd, uid_t u, gid_t g) {
    (void)fd; (void)u; (void)g;
    errno = 0;
    return 0;
}
int fchownat(int dirfd, const char *p, uid_t u, gid_t g, int fl) {
    (void)dirfd; (void)p; (void)u; (void)g; (void)fl;
    errno = 0;
    return 0;
}
int mkdir(const char *p, mode_t m) {
    NEXT(mkdir);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(mkdir, b, m); }
    return CALL(mkdir, p, m);
}
int mkdirat(int dirfd, const char *p, mode_t m) {
    NEXT(mkdirat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(mkdirat, dirfd, b, m); }
    return CALL(mkdirat, dirfd, p, m);
}
int mknod(const char *p, mode_t m, dev_t d) {
    NEXT(mknod);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(mknod, b, m, d); }
    return CALL(mknod, p, m, d);
}
int mknodat(int dirfd, const char *p, mode_t m, dev_t d) {
    NEXT(mknodat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(mknodat, dirfd, b, m, d); }
    return CALL(mknodat, dirfd, p, m, d);
}
int rmdir(const char *p) {
    NEXT(rmdir);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(rmdir, b); }
    return CALL(rmdir, p);
}
int unlink(const char *p) {
    NEXT(unlink);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(unlink, b); }
    return CALL(unlink, p);
}
int unlinkat(int dirfd, const char *p, int fl) {
    NEXT(unlinkat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(unlinkat, dirfd, b, fl); }
    return CALL(unlinkat, dirfd, p, fl);
}
int rename(const char *a, const char *b2) {
    NEXT(rename);
    if (fk_is_abs(a) || fk_is_abs(b2)) {
        char x[PATH_MAX], y[PATH_MAX];
        fk_rewrite(x, sizeof x, a); fk_rewrite(y, sizeof y, b2);
        int r = CALL(rename, x, y);
        if (r < 0) fk_dbg("rename %s->%s (%s->%s) FAIL errno=%d", a, b2, x, y, errno);
        return r;
    }
    return CALL(rename, a, b2);
}
int renameat(int d1, const char *a, int d2, const char *b2) {
    NEXT(renameat);
    if (fk_is_abs(a) || fk_is_abs(b2)) {
        char x[PATH_MAX], y[PATH_MAX];
        fk_rewrite(x, sizeof x, a); fk_rewrite(y, sizeof y, b2);
        return CALL(renameat, d1, x, d2, y);
    }
    return CALL(renameat, d1, a, d2, b2);
}
int renameat2(int d1, const char *a, int d2, const char *b2, unsigned f) {
    NEXT(renameat2);
    if (fk_is_abs(a) || fk_is_abs(b2)) {
        char x[PATH_MAX], y[PATH_MAX];
        fk_rewrite(x, sizeof x, a); fk_rewrite(y, sizeof y, b2);
        return CALL(renameat2, d1, x, d2, y, f);
    }
    return CALL(renameat2, d1, a, d2, b2, f);
}
int link(const char *a, const char *b2) {
    NEXT(link);
    if (fk_is_abs(a) || fk_is_abs(b2)) {
        char x[PATH_MAX], y[PATH_MAX];
        fk_rewrite(x, sizeof x, a); fk_rewrite(y, sizeof y, b2);
        return CALL(link, x, y);
    }
    return CALL(link, a, b2);
}
int linkat(int d1, const char *a, int d2, const char *b2, int fl) {
    NEXT(linkat);
    if (fk_is_abs(a) || fk_is_abs(b2)) {
        char x[PATH_MAX], y[PATH_MAX];
        fk_rewrite(x, sizeof x, a); fk_rewrite(y, sizeof y, b2);
        return CALL(linkat, d1, x, d2, y, fl);
    }
    return CALL(linkat, d1, a, d2, b2, fl);
}
int symlink(const char *a, const char *b2) {
    NEXT(symlink);
    if (fk_is_abs(b2)) {
        char y[PATH_MAX];
        fk_rewrite(y, sizeof y, b2);
        return CALL(symlink, a, y);
    }
    return CALL(symlink, a, b2);
}
int symlinkat(const char *a, int d, const char *b2) {
    NEXT(symlinkat);
    if (fk_is_abs(b2)) {
        char y[PATH_MAX];
        fk_rewrite(y, sizeof y, b2);
        return CALL(symlinkat, a, d, y);
    }
    return CALL(symlinkat, a, d, b2);
}
/* true bila p menunjuk exe milik proses ini (/proc/self/exe atau /proc/<pid>/exe) */
static int fk_is_self_exe(const char *p) {
    if (!p) return 0;
    if (strcmp(p, "/proc/self/exe") == 0) return 1;
    char mine[64];
    snprintf(mine, sizeof mine, "/proc/%d/exe", (int)getpid());
    return strcmp(p, mine) == 0;
}

ssize_t readlink(const char *p, char *buf, size_t n) {
    NEXT(readlink);
    /* §25: jalan via loader -> /proc/self/exe = loader. Jawab program asli
     * supaya execPath Bun/OpenCode benar saat men-spawn dirinya sendiri. */
    if (fk_is_self_exe(p)) {
        const char *self = fk_self_exe();
        if (self) {
            size_t l = strlen(self);
            if (l > n) l = n;
            memcpy(buf, self, l);
            return (ssize_t)l;
        }
    }
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(readlink, b, buf, n); }
    return CALL(readlink, p, buf, n);
}
ssize_t readlinkat(int d, const char *p, char *buf, size_t n) {
    NEXT(readlinkat);
    if (fk_is_self_exe(p) && (d == AT_FDCWD || p[0] == '/')) {
        const char *self = fk_self_exe();
        if (self) {
            size_t l = strlen(self);
            if (l > n) l = n;
            memcpy(buf, self, l);
            return (ssize_t)l;
        }
    }
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(readlinkat, d, b, buf, n); }
    return CALL(readlinkat, d, p, buf, n);
}
char *realpath(const char *p, char *out) {
    NEXT(realpath);
    if (fk_is_abs(p)) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, p);
        char *r = CALL(realpath, b, out);
        if (r) {
            size_t bl = strlen(fk_base);
            if (strncmp(r, fk_base, bl) == 0 && (r[bl] == '/' || r[bl] == '\0'))
                memmove(r, r + bl, strlen(r + bl) + 1);   /* tampilkan /etc, bukan /data/... */
        }
        return r;
    }
    return CALL(realpath, p, out);
}

/* ---- misc ---- */
int truncate(const char *p, off_t l) {
    NEXT(truncate);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(truncate, b, l); }
    return CALL(truncate, p, l);
}
int truncate64(const char *p, off_t l) {
    NEXT(truncate64);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(truncate64, b, l); }
    return CALL(truncate64, p, l);
}
int utime(const char *p, const struct utimbuf *t) {
    NEXT(utime);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(utime, b, t); }
    return CALL(utime, p, t);
}
int utimes(const char *p, const struct timeval tv[2]) {
    NEXT(utimes);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(utimes, b, tv); }
    return CALL(utimes, p, tv);
}
int utimensat(int d, const char *p, const struct timespec ts[2], int fl) {
    NEXT(utimensat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(utimensat, d, b, ts, fl); }
    return CALL(utimensat, d, p, ts, fl);
}
int statvfs(const char *p, struct statvfs *s) {
    NEXT(statvfs);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(statvfs, b, s); }
    return CALL(statvfs, p, s);
}
int statfs(const char *p, struct statfs *s) {
    NEXT(statfs);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(statfs, b, s); }
    return CALL(statfs, p, s);
}
char *getcwd(char *buf, size_t n) {
    NEXT(getcwd);
    char *r = CALL(getcwd, buf, n);
    if (r) {
        size_t bl = strlen(fk_base);
        if (strncmp(r, fk_base, bl) == 0 && (r[bl] == '/' || r[bl] == '\0'))
            memmove(r, r + bl, strlen(r + bl) + 1);    /* /data/.../base -> "" ->"/" */
        if (r[0] == '\0') { r[0] = '/'; r[1] = '\0'; }
        else if (r[0] != '/') { memmove(r + 1, r, strlen(r) + 1); r[0] = '/'; }
    }
    return r;
}
char *getwd(char *buf) { return getcwd(buf, PATH_MAX); }

/* ------------------------------------------------------------------ */
/* 3. Resolver DNS sendiri (getaddrinfo / gethostbyname)              */
/*    LATAR: di libc musl (dibangun direct-call, PLT hanya utk malloc  */
/*    family), baca /etc/resolv.conf + /etc/hosts DI DALAM resolver    */
/*    TIDAK lewat PLT -> LD_PRELOAD shim tak bisa me-rewrite kedua file */
/*    itu. Akibatnya di jalur cepat nama tak pernah ter-resolve: musl   */
/*    baca resolv.conf HOST (tak ada di Android) -> fallback 127.0.0.1  */
/*    -> timeout 5s -> EAI_AGAIN = "DNS: transient error" (apk) /      */
/*    "bad address" (wget/ping). Jalur svsp aman (rewrite di level      */
/*    syscall), jalur shim tidak.                                      */
/*    SOLUSI: shim meng-ekspor getaddrinfo/gethostbyname sendiri yang   */
/*    membaca config DI WADAH (base-prefix, lewat open REAL) lalu query */
/*    UDP mentah ke tiap nameserver secara paralel dgn retry — persis   */
/*    gaya __res_msend musl. Numerik/hosts tetap instan.                */
/* ------------------------------------------------------------------ */
#define FK_MAXA 8
struct fk_dns_ans { int n4; unsigned char v4[FK_MAXA][4]; int n6; unsigned char v6[FK_MAXA][16]; };

/* baca file (path SUDAH base-prefix) lewat open REAL (dlsym) — bebas
 * interpose sehingga tidak terjadi double-rewrite. */
static int fk_read_into(const char *path, char *buf, size_t bufsz) {
    NEXT(open);
    int fd = CALL(open, path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    int e = errno;
    close(fd);
    errno = e;
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

static void fk_free_chain(struct addrinfo *res) {
    while (res) {
        struct addrinfo *n = res->ai_next;
        free(res->ai_canonname);
        free(res->ai_addr);
        free(res);
        res = n;
    }
}

static struct addrinfo *fk_mkaddr(int family, const void *src, int port, int socktype,
                                  int protocol, char *canon) {
    size_t slen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    struct addrinfo *ai = calloc(1, sizeof *ai);
    if (!ai) { free(canon); return NULL; }
    ai->ai_addr = calloc(1, slen);
    if (!ai->ai_addr) { free(canon); free(ai); return NULL; }
    ai->ai_family  = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen  = slen;
    ai->ai_canonname = canon;               /* kepemilikan pindah; bisa NULL */
    if (family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)ai->ai_addr;
        s4->sin_family = AF_INET;
        s4->sin_port   = htons((unsigned short)port);
        memcpy(&s4->sin_addr, src, 4);
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ai->ai_addr;
        s6->sin6_family = AF_INET6;
        s6->sin6_port   = htons((unsigned short)port);
        memcpy(&s6->sin6_addr, src, 16);
    }
    return ai;
}

static int fk_port_of(const char *service) {
    if (!service || !*service) return 0;
    if (*service >= '0' && *service <= '9') {
        long v = strtol(service, NULL, 10);
        return (v > 0 && v < 65536) ? (int)v : 0;
    }
    static const struct { const char *n; int p; } svc[] = {
        {"http",80},{"https",443},{"ftp",21},{"ssh",22},{"telnet",23},{"smtp",25},
        {"domain",53},{"dns",53},{"ntp",123},{"imap",143},{"imaps",993},{"pop3",110},
        {"pop3s",995},{"tftp",69},{"nntp",119},{"ldap",389},{"ldaps",636},{"sip",5060},
    };
    for (size_t i = 0; i < sizeof svc / sizeof *svc; i++)
        if (!strcmp(svc[i].n, service)) return svc[i].p;
    return 0;
}

/* encode nama -> label DNS; -1 bila terlalu panjang */
static int fk_dn_enc(char *out, const char *name) {
    int o = 0;
    const char *p = name;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l) {
            if (l > 63 || o + (int)l + 1 > 255) return -1;
            out[o++] = (char)l;
            memcpy(out + o, p, l);
            o += (int)l;
        }
        if (!dot) break;
        p = dot + 1;
        if (!*p) break;                     /* titik di ujung = sudah root */
    }
    out[o++] = 0;
    return o;
}

/* lewati satu nama (mendukung pointer kompresi); posisi setelah nama */
static int fk_dn_skip(const unsigned char *msg, size_t msglen, size_t *off) {
    size_t p = *off;
    if (p >= msglen) return -1;
    for (;;) {
        if (p >= msglen) return -1;
        unsigned char l = msg[p];
        if (l == 0) { p++; break; }
        if ((l & 0xc0) == 0xc0) { p += 2; break; }          /* pointer 14-bit */
        if ((l & 0xc0) != 0) return -1;
        p += 1 + l;
    }
    *off = p;
    return 0;
}

static int fk_build_query(unsigned char *msg, size_t cap, const char *name, int qtype,
                          unsigned short id) {
    if (cap < 512) return -1;
    int o = 0;
    msg[o++] = (unsigned char)(id >> 8); msg[o++] = (unsigned char)id;
    msg[o++] = 0x01; msg[o++] = 0x00;                       /* RD */
    msg[o++] = 0x00; msg[o++] = 0x01;                       /* QDCOUNT=1 */
    msg[o++] = 0x00; msg[o++] = 0x00;
    msg[o++] = 0x00; msg[o++] = 0x00;
    msg[o++] = 0x00; msg[o++] = 0x00;
    char enc[256];
    int el = fk_dn_enc(enc, name);
    if (el < 0) return -1;
    memcpy(msg + o, enc, (size_t)el); o += el;
    msg[o++] = (unsigned char)(qtype >> 8); msg[o++] = (unsigned char)qtype;
    msg[o++] = 0x00; msg[o++] = 0x01;                       /* class IN */
    return o;
}

/* query satu tipe ke SEMUA nameserver paralel; hasil ditambahkan ke ans.
 * return: 0 = dapat; 1 = NXDOMAIN/tanpa record; -1 = timeout/syscall error */
static int fk_dns_query(const char *name, int qtype, struct sockaddr_in *ns, int nns,
                        int timeout_ms, int attempts, struct fk_dns_ans *ans) {
    unsigned short id = (unsigned short)((getpid() ^ (unsigned)time(NULL)) & 0xffff);
    unsigned char msg[512];
    int ml = fk_build_query(msg, sizeof msg, name, qtype, id);
    if (ml < 0) return -1;
    int fds[8], nf = 0;
    for (int i = 0; i < nns && nf < 8; i++) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        if (sendto(fd, msg, (size_t)ml, 0, (struct sockaddr *)&ns[i], sizeof ns[i]) < 0) {
            close(fd); continue;
        }
        fds[nf++] = fd;
    }
    if (nf == 0) return -1;
    int rc = -1;
    for (int a = 0; a < attempts; a++) {
        struct pollfd pf[8];
        for (int i = 0; i < nf; i++) { pf[i].fd = fds[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
        int pr = poll(pf, (nfds_t)nf, timeout_ms);
        if (pr > 0) {
            for (int i = 0; i < nf; i++) {
                if (!(pf[i].revents & POLLIN)) continue;
                unsigned char buf[4096];
                ssize_t n = recv(fds[i], buf, sizeof buf, 0);
                if (n < 12) continue;
                if (buf[0] != (unsigned char)(id >> 8) || buf[1] != (unsigned char)id) continue;
                if (!(buf[2] & 0x80)) continue;             /* QR=jawaban */
                int rcode = buf[3] & 0x0f;
                if (rcode == 3) { rc = 1; break; }          /* NXDOMAIN */
                if (rcode != 0) continue;                   /* SERVFAIL dll → server lain */
                int qdc = (buf[4] << 8) | buf[5];
                int anc = (buf[6] << 8) | buf[7];
                size_t off = 12;
                int okq = 1;
                for (int q = 0; q < qdc && okq; q++) { if (fk_dn_skip(buf, (size_t)n, &off)) okq = 0; else off += 4; }
                if (!okq) continue;
                int got = 0;
                for (int r = 0; r < anc; r++) {
                    if (fk_dn_skip(buf, (size_t)n, &off)) break;
                    if (off + 10 > (size_t)n) break;
                    int type = (buf[off] << 8) | buf[off + 1];
                    int rdlen = (buf[off + 8] << 8) | buf[off + 9];
                    unsigned char *rd = buf + off + 10;
                    if (off + 10 + (size_t)rdlen > (size_t)n) break;
                    if (type == 1 && rdlen == 4 && ans->n4 < FK_MAXA) {
                        memcpy(ans->v4[ans->n4++], rd, 4); got++;
                    } else if (type == 28 && rdlen == 16 && ans->n6 < FK_MAXA) {
                        memcpy(ans->v6[ans->n6++], rd, 16); got++;
                    }
                    off += 10 + (size_t)rdlen;
                }
                rc = got ? 0 : 1;
                break;
            }
            if (rc == 0 || rc == 1) break;
            rc = -1;
        }
        if (a + 1 < attempts)
            for (int i = 0; i < nf; i++)
                sendto(fds[i], msg, (size_t)ml, 0, (struct sockaddr *)&ns[i], sizeof ns[i]);
    }
    for (int i = 0; i < nf; i++) close(fds[i]);
    return rc;
}

/* parse nameserver (paralel) + options timeout/attempts dari resolv.conf wadah */
static int fk_load_resolv(struct sockaddr_in *ns, int max, int *timeout_ms, int *attempts) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/etc/resolv.conf", fk_base);
    char buf[4096];
    if (fk_read_into(path, buf, sizeof buf) < 0) return 0;
    char *s = strdup(buf);
    if (!s) return 0;
    int cnt = 0;
    char *save = NULL;
    for (char *line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "nameserver", 10) && (line[10] == ' ' || line[10] == '\t')) {
            char *ip = line + 10;
            while (*ip == ' ' || *ip == '\t') ip++;
            char ipb[64]; int k = 0;
            while (*ip && *ip != ' ' && *ip != '\t' && *ip != '\n' && k < 63) ipb[k++] = *ip++;
            ipb[k] = 0;
            if (cnt < max && inet_pton(AF_INET, ipb, &ns[cnt].sin_addr) == 1) {
                ns[cnt].sin_family = AF_INET;
                ns[cnt].sin_port   = htons(53);
                cnt++;
            }
        } else if (!strncmp(line, "options", 7) && (line[7] == ' ' || line[7] == '\t')) {
            char *o = line + 7;
            while (*o) {
                while (*o == ' ' || *o == '\t') o++;
                if (!strncmp(o, "timeout:", 8)) { long v = strtol(o + 8, NULL, 10); if (v >= 1 && v <= 30) *timeout_ms = (int)v * 1000; }
                else if (!strncmp(o, "attempts:", 9)) { long v = strtol(o + 9, NULL, 10); if (v >= 1 && v <= 10) *attempts = (int)v; }
                while (*o && *o != ' ' && *o != '\t') o++;
            }
        }
    }
    free(s);
    return cnt;
}

/* lookup di /etc/hosts wadah; nama dicocokkan case-insensitive */
static void fk_hosts_lookup(const char *name, struct fk_dns_ans *ans) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/etc/hosts", fk_base);
    char buf[8192];
    if (fk_read_into(path, buf, sizeof buf) < 0) return;
    char *s = strdup(buf);
    if (!s) return;
    char *save = NULL;
    for (char *line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *c = strchr(line, '#');
        if (c) *c = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        char ip[64]; int k = 0;
        while (*p && *p != ' ' && *p != '\t' && k < 63) ip[k++] = *p++;
        ip[k] = 0;
        struct in_addr a4; struct in6_addr a6;
        int is6 = inet_pton(AF_INET, ip, &a4) != 1;
        if (is6 && inet_pton(AF_INET6, ip, &a6) != 1) continue;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            char n2[256]; int k2 = 0;
            while (*p && *p != ' ' && *p != '\t' && k2 < 255) n2[k2++] = *p++;
            n2[k2] = 0;
            if (!k2) break;
            if (!strcasecmp(n2, name)) {
                if (!is6 && ans->n4 < FK_MAXA) memcpy(ans->v4[ans->n4++], &a4, 4);
                else if (is6 && ans->n6 < FK_MAXA) memcpy(ans->v6[ans->n6++], &a6, 16);
                break;
            }
        }
    }
    free(s);
}

int getaddrinfo(const char *name, const char *service, const struct addrinfo *hints,
                struct addrinfo **res) {
    if (res) *res = NULL;
    struct addrinfo h;
    memset(&h, 0, sizeof h);
    if (hints) h = *hints;
    int fam = h.ai_family;                                  /* 0=UNSPEC */
    int socktype = h.ai_socktype ? h.ai_socktype : SOCK_STREAM;
    int proto = socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    int port = fk_port_of(service);

    /* NULL host: AI_PASSIVE → any; selainnya → loopback */
    if (!name) {
        int f = fam == AF_INET6 ? AF_INET6 : AF_INET;
        struct addrinfo *ai;
        if (f == AF_INET6) {
            struct in6_addr any = IN6ADDR_ANY_INIT, lo = IN6ADDR_LOOPBACK_INIT;
            ai = fk_mkaddr(AF_INET6, (h.ai_flags & AI_PASSIVE) ? &any : &lo, port, socktype, proto, NULL);
        } else {
            struct in_addr any = {0}, lo = { htonl(INADDR_LOOPBACK) };
            ai = fk_mkaddr(AF_INET, (h.ai_flags & AI_PASSIVE) ? &any : &lo, port, socktype, proto, NULL);
        }
        if (!ai) return -EAI_MEMORY;
        *res = ai;
        return 0;
    }

    /* IP literal → langsung (tidak perlu DNS; inet_pton murni) */
    struct in_addr na4; struct in6_addr na6;
    int is_v4 = inet_pton(AF_INET, name, &na4) == 1;
    int is_v6 = !is_v4 && inet_pton(AF_INET6, name, &na6) == 1;
    if (is_v4 || is_v6) {
        if (fam == AF_INET && is_v6) return -EAI_NONAME;   /* tanpa V4MAPPED */
        if (fam == AF_INET6 && is_v4) return -EAI_NONAME;
        int f = is_v4 ? AF_INET : AF_INET6;
        struct addrinfo *ai = fk_mkaddr(f, is_v4 ? (void *)&na4 : (void *)&na6, port, socktype, proto, NULL);
        if (!ai) return -EAI_MEMORY;
        *res = ai;
        return 0;
    }

    /* nama: /etc/hosts wadah dulu, lalu DNS (v4 dulu, v6 menyusul) */
    struct fk_dns_ans ans = {0};
    fk_hosts_lookup(name, &ans);

    int need4 = (fam == AF_UNSPEC || fam == AF_INET) && ans.n4 == 0;
    int need6 = (fam == AF_UNSPEC || fam == AF_INET6) && ans.n6 == 0;
    if (need4 || need6) {
        struct sockaddr_in ns[8];
        int tmo = 2000, att = 3;
        int nns = fk_load_resolv(ns, 8, &tmo, &att);
        if (nns == 0) {                                     /* fallback publik */
            ns[0].sin_family = AF_INET; ns[0].sin_port = htons(53);
            inet_pton(AF_INET, "1.1.1.1", &ns[0].sin_addr);
            ns[1] = ns[0]; inet_pton(AF_INET, "8.8.8.8", &ns[1].sin_addr);
            nns = 2;
        }
        if (need4) {
            int rc = fk_dns_query(name, 1, ns, nns, tmo, att, &ans);
            if (rc < 0 && ans.n4 + ans.n6 == 0) return -EAI_AGAIN;
        }
        if (need6) {
            int rc = fk_dns_query(name, 28, ns, nns, tmo, att, &ans);
            if (rc < 0 && ans.n4 + ans.n6 == 0) return -EAI_AGAIN;
        }
    }

    int n4 = (fam == AF_INET6) ? 0 : ans.n4;
    int n6 = (fam == AF_INET) ? 0 : ans.n6;
    if (n4 + n6 == 0) return -EAI_NONAME;

    char *canon = NULL;
    if (h.ai_flags & AI_CANONNAME) {
        canon = strdup(name);
        if (!canon) return -EAI_MEMORY;
    }
    struct addrinfo *head = NULL, **tail = &head;
    int first = 1;
    for (int i = 0; i < n4; i++) {
        struct addrinfo *ai = fk_mkaddr(AF_INET, ans.v4[i], port, socktype, proto, first ? canon : NULL);
        if (!ai) { freeaddrinfo(head); return -EAI_MEMORY; }
        first = 0;
        *tail = ai; tail = &ai->ai_next;
    }
    for (int i = 0; i < n6; i++) {
        struct addrinfo *ai = fk_mkaddr(AF_INET6, ans.v6[i], port, socktype, proto, first ? canon : NULL);
        if (!ai) { freeaddrinfo(head); return -EAI_MEMORY; }
        first = 0;
        *tail = ai; tail = &ai->ai_next;
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    fk_free_chain(res);
}

/* hostent klasik (A untuk gethostbyname, A/AAAA sesuai family utk _2);
 * memakai storage statis seperti libc (hasil tak perlu di-free). */
static struct hostent *fk_hostent_of(const char *name, const struct fk_dns_ans *ans, int family) {
    int alen = family == AF_INET ? 4 : 16;
    int n = family == AF_INET ? ans->n4 : ans->n6;
    if (n == 0) { h_errno = HOST_NOT_FOUND; return NULL; }
    size_t need = sizeof(struct hostent) + strlen(name) + 1 + sizeof(char *)   /* aliases[1] */
                + (size_t)(n + 1) * sizeof(char *) + (size_t)n * (size_t)alen;
    static char *b = NULL;
    static size_t bcap = 0;
    if (need > bcap) {
        char *nb = realloc(b, need);
        if (!nb) { h_errno = NO_RECOVERY; return NULL; }
        b = nb; bcap = need;
    }
    char *p = b;
    struct hostent *he = (struct hostent *)p; p += sizeof *he;
    he->h_name = p; memcpy(p, name, strlen(name) + 1); p += strlen(name) + 1;
    he->h_aliases = (char **)p;
    *(char **)p = NULL; p += sizeof(char *);
    he->h_addr_list = (char **)p; p += (size_t)(n + 1) * sizeof(char *);
    for (int i = 0; i < n; i++) {
        he->h_addr_list[i] = p;
        memcpy(p, family == AF_INET ? (void *)ans->v4[i] : (void *)ans->v6[i], (size_t)alen);
        p += alen;
    }
    he->h_addr_list[n] = NULL;
    he->h_addrtype = family;
    he->h_length = alen;
    return he;
}

struct hostent *gethostbyname2(const char *name, int family) {
    if (!name || (family != AF_INET && family != AF_INET6)) { h_errno = NO_RECOVERY; return NULL; }
    struct fk_dns_ans ans = {0};
    fk_hosts_lookup(name, &ans);
    int want_v4 = family == AF_INET && ans.n4 == 0;
    int want_v6 = family == AF_INET6 && ans.n6 == 0;
    if (want_v4 || want_v6) {
        struct sockaddr_in ns[8];
        int tmo = 2000, att = 3;
        int nns = fk_load_resolv(ns, 8, &tmo, &att);
        if (nns == 0) {
            ns[0].sin_family = AF_INET; ns[0].sin_port = htons(53);
            inet_pton(AF_INET, "1.1.1.1", &ns[0].sin_addr);
            ns[1] = ns[0]; inet_pton(AF_INET, "8.8.8.8", &ns[1].sin_addr);
            nns = 2;
        }
        int qtype = family == AF_INET ? 1 : 28;
        fk_dns_query(name, qtype, ns, nns, tmo, att, &ans);
    }
    return fk_hostent_of(name, &ans, family);
}

struct hostent *gethostbyname(const char *name) {
    return gethostbyname2(name, AF_INET);
}

/* ------------------------------------------------------------------ */
static void fk_init(void) __attribute__((constructor));
static void fk_init(void) {
    fk_base = getenv("FAKE_BASE");
    if (!fk_base || !*fk_base) fk_base = "/data/data/com.termux/files/home/alpine-rootfs";

    /* snapshot environ SEBELUM ada yang clearenv() (busybox `env -i`);
     * dipakai fk_ensure_wadah_env saat environ live sudah kosong. */
    fk_snapshot_env();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fk_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);
}