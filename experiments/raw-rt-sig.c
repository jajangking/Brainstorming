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

/* layout kernel aarch64 (bukan struct sigaction bionic) */
struct ksigaction {
    void *sa_handler;
    unsigned long sa_flags;
    void *sa_restorer;
    unsigned long sa_mask;
};

static volatile int caught = 0;

static void on_sigsys(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    (void)sig;
    caught = 1;
    uc->uc_mcontext.pc = (unsigned long)info->si_call_addr + 4;
    uc->uc_mcontext.regs[0] = 0;
}

int main(void) {
    /* jalur 1: rt_sigaction mentah dgn ksigaction */
    struct ksigaction ka;
    memset(&ka, 0, sizeof ka);
    ka.sa_handler = (void *)on_sigsys;
    ka.sa_flags = SA_SIGINFO;
    long r = syscall(SYS_rt_sigaction, SIGSYS, &ka, 0, 8);
    printf("rt_sigaction mentah -> %ld\n", r);

    r = syscall(SYS_setgid, getgid());
    printf("setgid -> ret=%ld caught=%d %s\n", r, caught,
           (r == 0 && caught) ? "SUKSES" : "GAGAL");
    return caught ? 0 : 1;
}
