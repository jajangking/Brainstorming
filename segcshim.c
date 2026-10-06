/*
 * libsegcshim.so - LD_PRELOAD netralisir seccomp Android.
 *
 * Android (untrusted_app) memblokir banyak syscall dgn SECCOMP_RET_TRAP
 * (SIGSYS). Sinyal ini BISA ditangkap. Shim ini memasang handler yang:
 *   - melewati instruksi svc #0 (syscall tak pernah dieksekusi)
 *   - menentukan return value sendiri:
 *       set*id family -> 0 (sukses; uid tak berubah, tak apa sbg shim CLI)
 *       sisanya       -> -EPERM (jujur: chroot/mount memang tak bisa)
 *
 * Dibuat -nostdlib / tanpa dependensi libc tertentu -> bisa dimuat oleh
 * loader musl, glibc, maupun bionic (symbol `syscall` / `sigaction`
 * diselesaikan oleh libc proses target).
 *
 * Pemakaian:  LD_PRELOAD=/path/libsegcshim.so <binary>
 * (env LD_PRELOAD Termux HARUS dilepas dulu)
 */
#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SA_SIGINFO
#define SA_SIGINFO 4
#endif

/* struct sigaction milik bionic TIDAK sama dgn layout kernel (ada
 * pengkonversian di fungsi sigaction() libc). Karena kita memanggil
 * rt_sigaction secara mentah, susun manual dgn layout kernel aarch64:
 *   handler@0, flags@8, restorer@16, mask@24 (sigset 8 byte) */
struct ksigaction {
    void    *sa_handler;
    unsigned long sa_flags;
    void    *sa_restorer;
    unsigned long sa_mask;
};

static void on_sigsys(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    (void)sig;
    syscall(SYS_write, 2, "HIT\n", 4);   /* penanda debug */
    /* si_call_addr = alamat instruksi svc; +4 = instruksi berikutnya */
    uc->uc_mcontext.pc = (unsigned long)info->si_call_addr + 4;

    switch (info->si_syscall) {
    case SYS_setgid:     case SYS_setuid:    case SYS_setreuid:
    case SYS_setregid:   case SYS_setresuid: case SYS_setresgid:
    case SYS_setgroups:  case SYS_setfsuid:  case SYS_setfsgid:
        uc->uc_mcontext.regs[0] = 0;                 /* sukses */
        break;
    default:
        uc->uc_mcontext.regs[0] = (unsigned long)-1; /* -EPERM */
        break;
    }
}

__attribute__((constructor))
static void segcshim_init(void) {
    struct ksigaction ka;
    ka.sa_handler   = (void *)on_sigsys;
    ka.sa_flags     = SA_SIGINFO;
    ka.sa_restorer  = 0;
    ka.sa_mask      = 0;
    syscall(SYS_rt_sigaction, SIGSYS, &ka, 0, 8);
    syscall(SYS_write, 2, "INIT\n", 5);   /* penanda debug */
}
