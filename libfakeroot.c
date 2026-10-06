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
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <alloca.h>
#include <utime.h>
#include <sys/statfs.h>

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
static void fk_rewrite(char *out, size_t n, const char *p) {
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
#define CALL(fn, ...) (next_##fn ? next_##fn(__VA_ARGS__) : (errno = ENOSYS, -1))

/* ---- open family ---- */
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(open);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); return CALL(open, b, flags, mode); }
    return CALL(open, path, flags, mode);
}
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(open64);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); return CALL(open64, b, flags, mode); }
    return CALL(open64, path, flags, mode);
}
int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(openat);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); return CALL(openat, dirfd, b, flags, mode); }
    return CALL(openat, dirfd, path, flags, mode);
}
int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    NEXT(openat64);
    if (fk_is_abs(path)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, path); return CALL(openat64, dirfd, b, flags, mode); }
    return CALL(openat64, dirfd, path, flags, mode);
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

/* ---- exec family ---- */
int execve(const char *p, char *const argv[], char *const envp[]) {
    NEXT(execve);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(execve, b, argv, envp); }
    return CALL(execve, p, argv, envp);
}
int execl(const char *p, const char *a0, ...) {
    NEXT(execl);
    char *b;
    if (fk_is_abs(p)) { b = malloc(PATH_MAX); fk_rewrite(b, PATH_MAX, p); }
    else b = (char *)p;
    va_list ap; va_start(ap, a0);
    int argc = 0; for (va_arg(ap, const char *); argc < 64; argc++) va_arg(ap, const char *);
    va_end(ap);
    /* susun argv */
    va_start(ap, a0);
    char **argv = alloca((size_t)(argc + 2) * sizeof(char *));
    argv[0] = (char *)a0;
    for (int i = 1; i <= argc; i++) argv[i] = va_arg(ap, char *);
    va_end(ap);
    argv[argc + 1] = NULL;
    NEXT(execve);
    long r = CALL(execve, b, argv, environ);
    if (b != p) free(b);
    return (int)r;
}

static int fk_execvp_search(const char *file, char *const argv[], char *const envp[], int use_path) {
    NEXT(execve);
    if (!use_path || strchr(file, '/')) {
        char b[PATH_MAX]; fk_rewrite(b, sizeof b, file); return CALL(execve, b, argv, envp);
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
        long r = CALL(execve, b, argv, envp);
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
    NEXT(execve);
    return CALL(execve, b, argv, environ);
}

/* ---- stdio ---- */
FILE *fopen(const char *p, const char *m) {
    NEXT(fopen);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(fopen, b, m); }
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
    NEXT(chown);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(chown, b, u, g); }
    return CALL(chown, p, u, g);
}
int lchown(const char *p, uid_t u, gid_t g) {
    NEXT(lchown);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(lchown, b, u, g); }
    return CALL(lchown, p, u, g);
}
int fchownat(int dirfd, const char *p, uid_t u, gid_t g, int fl) {
    NEXT(fchownat);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(fchownat, dirfd, b, u, g, fl); }
    return CALL(fchownat, dirfd, p, u, g, fl);
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
        return CALL(rename, x, y);
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
ssize_t readlink(const char *p, char *buf, size_t n) {
    NEXT(readlink);
    if (fk_is_abs(p)) { char b[PATH_MAX]; fk_rewrite(b, sizeof b, p); return CALL(readlink, b, buf, n); }
    return CALL(readlink, p, buf, n);
}
ssize_t readlinkat(int d, const char *p, char *buf, size_t n) {
    NEXT(readlinkat);
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
int getwd(char *buf) { return getcwd(buf, PATH_MAX) != NULL ? (int)(long)buf : -1; }

/* ------------------------------------------------------------------ */
static void fk_init(void) __attribute__((constructor));
static void fk_init(void) {
    fk_base = getenv("FAKE_BASE");
    if (!fk_base || !*fk_base) fk_base = "/data/data/com.termux/files/home/alpine-rootfs";

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fk_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);
}