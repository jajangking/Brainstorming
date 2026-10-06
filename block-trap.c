#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef SA_SIGINFO
#define SA_SIGINFO 4
#endif
struct ksigaction { void *sa_handler; unsigned long sa_flags; void *sa_restorer; unsigned long sa_mask; };

static volatile int caught = 0;
static void on_sigsys(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    (void)sig;
    caught++;
    uc->uc_mcontext.pc = (unsigned long)info->si_call_addr + 4;
    uc->uc_mcontext.regs[0] = 0;
}

int main(void) {
    struct ksigaction ka;
    memset(&ka, 0, sizeof ka);
    ka.sa_handler = (void *)on_sigsys;
    ka.sa_flags = SA_SIGINFO;
    syscall(SYS_rt_sigaction, SIGSYS, &ka, 0, 8);

    /* tiru busybox: blok SEMUA sinyal */
    unsigned long full = (unsigned long)-1; /* set semua bit */
    syscall(SYS_rt_sigprocmask, SIG_BLOCK, &full, 0, 8);

    errno = 0;
    long r = syscall(SYS_setgid, getgid());
    printf("setgid SELAGI DIBLOKIR -> ret=%ld errno=%d caught=%d  %s\n", r, errno, caught,
           caught ? "OK ditangkap" : "TIDAK ditangkap!");

    /* lepas blok -> sinyal pending (kalau ada) baru dikirim */
    unsigned long zero = 0;
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &zero, 0, 8);
    printf("setelah UNBLOCK -> caught=%d\n", caught);
    printf(caught ? "SELAMAT (handler jalan)\n" : "MATI karena...\n");
    return caught ? 0 : 1;
}