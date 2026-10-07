/*
 * ebench.c — benchmark overhead supervisor svsp per round-trip.
 *
 * Mengukur loop open()+close() pada:
 *   (a) path passthrough (CONTINUE) — /root/...
 *   (b) path rewrite — /etc/alpine-release
 *
 * Build (statis, di Termux):
 *   clang --target=aarch64-alpine-linux-musl --sysroot=$R -static -nostdlib \
 *     -o ebench ebench.c $R/usr/lib/crt1.o $R/usr/lib/crti.o \
 *     $R/usr/lib/libc.a $R/usr/lib/crtn.o
 *
 * Jalankan:
 *   bare:  $R/usr/bin/ebench
 *   svsp:  svsp --base=$R $R/usr/bin/ebench
 *
 * Output: μs/op per kategori.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define N 1000

static long elapsed_ns(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000000000L + (b->tv_nsec - a->tv_nsec);
}

int main(void) {
    struct timespec t0, t1;
    int fd, i;

    /* buat file dummy di /root (passthrough) */
    const char *passthrough_path = "/root/ebench-dummy.txt";
    fd = open(passthrough_path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) { write(fd, "x", 1); close(fd); }

    const char *rewrite_path = "/etc/alpine-release";

    printf("[ebench] benchmark %d iterasi\n", N);

    /* 1) passthrough (CONTINUE) */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < N; i++) {
        fd = open(passthrough_path, O_RDONLY);
        if (fd >= 0) close(fd);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us_pt = elapsed_ns(&t0, &t1) / 1000;
    printf("  passthrough (CONTINUE): %ld μs total, %ld.%03ld μs/op\n",
           us_pt, us_pt / N, (us_pt * 1000 / N) % 1000);

    /* 2) rewrite (ADDFD) */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < N; i++) {
        fd = open(rewrite_path, O_RDONLY);
        if (fd >= 0) close(fd);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us_rw = elapsed_ns(&t0, &t1) / 1000;
    printf("  rewrite    (ADDFD):     %ld μs total, %ld.%03ld μs/op\n",
           us_rw, us_rw / N, (us_rw * 1000 / N) % 1000);

    /* bersihkan */
    unlink(passthrough_path);

    printf("[ebench] selesai\n");
    return 0;
}