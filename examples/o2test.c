/*
 * o2test.c — bukti openat2 (SYS 437) direwrite oleh supervisor.
 *
 * Buka path di luar base (freeze) + path base (mustahil tanpa rewrite)
 * lewat syscall openat2 mentah; baca isinya; bandingkan.
 *
 * Build statis (Termux):
 *   clang --target=aarch64-alpine-linux-musl --sysroot=$R -static -nostdlib \
 *     -o o2test o2test.c $R/usr/lib/crt1.o $R/usr/lib/crti.o \
 *     $R/usr/lib/libc.a $R/usr/lib/crtn.o
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_openat2
#define SYS_openat2 437
#endif

/* ABI kernel: flags, mode, resolve — tiga u64 */
struct open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

static long o2open(const char *path, int flags) {
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags = flags;
    return syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof how);
}

static int try_read(int fd, const char *label, char *buf, int cap) {
    if (fd < 0) { printf("%-18s fd=%d errno=%d (%s)\n", label, fd, errno, strerror(errno)); return 0; }
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) { printf("%-18s read errno=%d\n", label, errno); return 0; }
    buf[n] = 0;
    printf("%-18s fd ok, isi=\"%s\"\n", label, buf);
    return 1;
}

int main(void) {
    char buf[128];
    long fd;
    setbuf(stdout, NULL);   /* output langsung muncul walau dieksekusi SIGSYS */

    /* 1) passthrough: /proc ok (di luar base) */
    fd = o2open("/nonexistent-o2test", O_RDONLY);
    printf("%-18s fd=%ld errno=%d (harap ENOENT)\n", "passthrough-miss", fd, errno);

    /* 2) rewrite: /etc/alpine-release harus menjadi $FAKE_BASE/etc/... */
    fd = o2open("/etc/alpine-release", O_RDONLY);
    try_read((int)fd, "rewrite(alpine)", buf, sizeof buf);

    /* 3) base absolut di luar jamahan — harus ENOENT (isolasi) */
    fd = o2open("/data", O_RDONLY | O_DIRECTORY);
    printf("%-18s fd=%ld errno=%d (harap ENOENT/ACCES)\n", "base-abs-isolated", fd, errno);

    /* 4) O_PATH via openat2 */
    fd = o2open("/etc/hostname", O_PATH);
    printf("O_PATH           fd=%ld errno=%d\n", fd, errno);

    /* 5) rewrite /dev/null -> base dev (bila ada) */
    fd = o2open("/dev/null", O_RDONLY);
    try_read((int)fd, "dev-null", buf, sizeof buf);

    return 0;
}