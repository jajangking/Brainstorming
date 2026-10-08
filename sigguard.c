/* sigguard.c — handler SIGSYS untuk biner GLIBC di dalam wadah.
 *
 * Kenapa butuh (device 2026-10-08): shim libfakeroot.so dibangun target
 * musl, jadi tak bisa di-preload ke proses glibc (CANNOT LINK). Padahal
 * biner glibc di容器 masih kena trap seccomp Android (faccessat2,
 * openat2, io_uring) yang tiba SEBELUM notif svsp → proses mati SIGSYS
 * (rc=159) tanpa handler in-process. shim_EXPORT bisa tetap dilation
 * (path rewrite) lewat supervisor seccomp; yang hilang cuma handler-nya.
 *
 * Sifat:
 *   - Freestanding: tanpa libc sama sekali (dibangun -nostdlib) → aman
 *     dimuat baik glibc maupun musl.
 *   - Pakai offset ABI aarch64 langsung, bukan struct libc:
 *       ucontext+40  = mcontext; regs[i] di +48+8i
 *       regs[8]      = nomor syscall saat svc terEksekusi
 *       regs[0]      = nilai balik
 *       +296         = __pc (alamat instruksi svc)
 *     → tak perlu menebak offset siginfo (berbeda glibc vs kernel).
 *   - Jawab: trap tak dikenal → ENOSYS (fallback libc lama),
 *     set*id → 0 (fake-root), io_uring_setup → fd /dev/null asli
 *     (agar libuv mundur ke epoll; lihat libfakeroot.c §42).
 *
 * Build:
 *   clang -O2 -fPIC -shared -nostdlib -o sigguard.so sigguard.c
 * Pakai (hanya untuk target glibc):
 *   fake-run --svsp FAKE_SHIM=/nonexistent \
 *     LD_PRELOAD=$BASE/opt/glibc/sigguard.so \
 *     LD_LIBRARY_PATH=$BASE/opt/glibc/lib/aarch64-linux-gnu <bin>
 */
#define SYS_rt_sigaction 134
#define SYS_openat       56
#define SYS_io_uring_setup 425

#define SIGSYS 31
#define SA_SIGINFO  0x00000004UL
#define SA_RESTORER 0x04000000UL

#define AT_FDCWD (-100)
#define O_CLOEXEC 0x80000UL
#define ENOSYS 38

/* offset dalam ucontext_t (aarch64, ABI) */
#define UC_MCONTEXT   40
#define UC_REGS(i)    (UC_MCONTEXT + 8 * (i))
#define UC_REG_X0     (UC_MCONTEXT)
#define UC_REG_X8     (UC_MCONTEXT + 8 * 8)
#define UC_PC         (UC_MCONTEXT + 8 * 32)

#define NR_setuid   146
#define NR_setgid   144
#define NR_setreuid 145
#define NR_setregid 143
#define NR_setresuid 147
#define NR_setresgid 149
#define NR_setgroups 146 /* arm64: setgroups=146? (lihat catatan di bawah) */

static long raw(long n, long a0, long a1, long a2, long a3) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

/* restorer wajib di aarch64 (kernel butuh SA_RESTORER) */
__attribute__((naked)) static void sigrestorer(void) {
    __asm__ volatile("mov x8, #139\n\tsvc #0");
}

/* buka /dev/null tanpa libc; hindari fd 0..2 (fd itu milik stdio) */
static int raw_devnull(void) {
    int fd = -1, i;
    for (i = 0; i < 4; i++) {
        long r = raw(SYS_openat, AT_FDCWD, (long)"/dev/null", O_CLOEXEC, 0);
        if ((unsigned long)r >= (unsigned long)-4096) return -1;
        fd = (int)r;
        if (fd > 2) return fd;
    }
    return -1;
}

static void on_sigsys(int sig, void *info, void *uctx) {
    unsigned char *uc = (unsigned char *)uctx;
    long nr = *(long *)(uc + UC_REG_X8);
    long *pc = (long *)(uc + UC_PC);
    long ret;

    switch (nr) {
    case 144: case 145: case 146: case 147: case 148: case 149:
    case 150: case 152: case 153: case 155: case 156:
        ret = 0;                       /* set*id → sukses (fake-root) */
        break;
    case SYS_io_uring_setup: {
        int fd = raw_devnull();
        ret = fd > 2 ? fd : -ENOSYS;   /* §42: fd asli, libuv mundur epoll */
        break;
    }
    default:
        ret = -ENOSYS;                 /* tak dikenal → fallback libc */
        break;
    }
    *(long *)(uc + UC_REG_X0) = ret;
    *pc = *pc + 4;                     /* lewati svc */
}

struct k_sigaction {
    void *handler;
    unsigned long flags;
    void *restorer;
    unsigned long mask;
};

__attribute__((constructor)) static void sigguard_init(void) {
    static const char m0[] = "sigguard: loaded\n";
    raw(64, 2, (long)m0, sizeof m0 - 1, 0);
    struct k_sigaction sa;
    sa.handler = (void *)on_sigsys;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = (void *)sigrestorer;
    sa.mask = 0;
    long r = raw(SYS_rt_sigaction, SIGSYS, (long)&sa, 0, 8 /* sizeof sigset_t */);
    if (r < 0) {                          /* diagnosa di >&2 */
        static const char m1[] = "sigguard: rt_sigaction GAGAL\n";
        raw(64, 2, (long)m1, sizeof m1 - 1, 0);
    }
}