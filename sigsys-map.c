#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static volatile sig_atomic_t caught = 0;
static int last_sc = -1;

static void on_sigsys(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    last_sc = info->si_syscall;
    caught = 1;
    uc->uc_mcontext.pc = (unsigned long)info->si_call_addr + 4;
    switch (info->si_syscall) {
    case SYS_setgid: case SYS_setuid: case SYS_setreuid: case SYS_setregid:
    case SYS_setresuid: case SYS_setresgid: case SYS_setgroups:
    case SYS_setfsuid: case SYS_setfsgid:
        uc->uc_mcontext.regs[0] = 0;
        break;
    default:
        uc->uc_mcontext.regs[0] = (unsigned long)-1; /* -EPERM */
        break;
    }
}

/* jalankan 1 syscall di child; laporkan apakah child selamat */
static void probe(const char *name, long nr, int fake_ok) {
    pid_t p = fork();
    if (p == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_sigsys;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSYS, &sa, NULL);
        errno = 0;
        long r = syscall(nr, 0, 0, 0, 0, 0);
        /* kalau sampai sini, tak dibunuh */
        if (caught)
            printf("%-16s TRAP    ditangkap handler, ret=%ld %s\n",
                   name, r, fake_ok ? "(sukses palsu)" : "(dilewati)");
        else
            printf("%-16s LIAR    tak kena seccomp, ret=%ld errno=%d\n",
                   name, r, errno);
        fflush(stdout);
        _exit(0);
    }
    int st; waitpid(p, &st, 0);
    if (WIFSIGNALED(st))
        printf("%-16s KILL    dibunuh signal %d (RET_KILL - TIDAK BISA ditangkap)\n",
               name, WTERMSIG(st));
}

int main(void) {
    printf("=== peta seccomp Android (per-syscall, process terpisah) ===\n");
    struct { const char *n; long nr; int ok; } t[] = {
        {"setgid",     SYS_setgid,     1},
        {"setuid",     SYS_setuid,     1},
        {"setreuid",   SYS_setreuid,   1},
        {"setregid",   SYS_setregid,   1},
        {"setresuid",  SYS_setresuid,  1},
        {"setresgid",  SYS_setresgid,  1},
        {"setgroups",  SYS_setgroups,  1},
        {"setfsuid",   SYS_setfsuid,   1},
        {"setfsgid",   SYS_setfsgid,   1},
        {"chroot",     SYS_chroot,     0},
        {"mount",      SYS_mount,      0},
        {"umount2",    SYS_umount2,    0},
        {"pivot_root", SYS_pivot_root, 0},
        {"reboot",     SYS_reboot,     0},
        {"swapon",     SYS_swapon,     0},
        {"setxattr",   SYS_setxattr,   0},
        {"init_module",SYS_init_module,0},
        {"delete_module",SYS_delete_module,0},
        {"bpf",        SYS_bpf,        0},
        {"ptrace",     SYS_ptrace,     0},
        {"unshare",    SYS_unshare,    0},
        {"setns",      SYS_setns,      0},
    };
    for (size_t i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        probe(t[i].n, t[i].nr, t[i].ok);
    printf("=== selesai, semua child diproses ===\n");
    return 0;
}
