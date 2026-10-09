#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void) {
    struct { const char *n; int f; } t[] = {
        {"flags=0",        0},
        {"CLONE_NEWUSER",  CLONE_NEWUSER},
        {"CLONE_NEWNS",    CLONE_NEWNS},
        {"CLONE_NEWPID",   CLONE_NEWPID},
        {"CLONE_NEWNET",   CLONE_NEWNET},
        {"CLONE_NEWUTS",   CLONE_NEWUTS},
        {"CLONE_NEWIPC",   CLONE_NEWIPC},
        {"CLONE_NEWCGROUP",CLONE_NEWCGROUP},
        {"CLONE_NEWTIME",  CLONE_NEWTIME},
    };
    printf("%-18s %-8s %s\n", "FLAG", "ERRNO", "ARTINYA");
    printf("----------------------------------------------\n");
    for (size_t i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        errno = 0;
        long r = syscall(SYS_unshare, t[i].f);
        const char *why = "-";
        if (r == 0) why = "SUKSES!";
        else if (errno == EINVAL) why = "EINVAL -> dicurigai seccomp ERRNO (kernel jarang bilang EINVAL utk flag valid)";
        else if (errno == EPERM)  why = "EPERM -> dari kernel (cap/LSM), bukan seccomp";
        else if (errno == ENOSPC) why = "ENOSPC -> max_user_namespaces=0";
        printf("%-18s %-8d %s\n", t[i].n, r == 0 ? 0 : errno, why);
    }
    /* info kernel */
    char buf[64] = {0};
    FILE *f = fopen("/proc/sys/user/max_user_namespaces", "r");
    if (f) { if (fgets(buf, sizeof buf, f)) printf("\nmax_user_namespaces = %s", buf); fclose(f); }
    else printf("\nmax_user_namespaces: %s\n", strerror(errno));
    return 0;
}
