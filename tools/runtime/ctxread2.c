/* ctxread.c —— ptrace 硬件执行断点读 glcore 内部 ctx 指针（不改目标任何字节）
 * 用法: ctxread <pid> <base_hex> <off_hex> [超时秒]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <stdint.h>

#define DR_BASE 848
#define DR(n)   (DR_BASE + (n)*8)
#define DR7     DR(7)
#define DR6     DR(6)

static volatile sig_atomic_t g_to = 0;
static void on_alarm(int s) { (void)s; g_to = 1; }

static int peek(pid_t pid, uint64_t addr, uint64_t *out)
{
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
    if (errno) return -1;
    *out = (uint64_t)v; return 0;
}
static int peek32(pid_t pid, uint64_t addr, uint32_t *out)
{
    uint64_t v;
    if (peek(pid, addr & ~7ULL, &v)) return -1;
    *out = (uint32_t)((v >> ((addr & 7) * 8)) & 0xffffffffULL);
    return 0;
}
static int peek8(pid_t pid, uint64_t addr, uint8_t *out)
{
    uint64_t v;
    if (peek(pid, addr & ~7ULL, &v)) return -1;
    *out = (uint8_t)((v >> ((addr & 7) * 8)) & 0xff);
    return 0;
}

static int try_thread(pid_t tid, uint64_t bp, int tmo)
{
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL)) return -1;
    int st;
    if (waitpid(tid, &st, 0) < 0) { ptrace(PTRACE_DETACH, tid, NULL, NULL); return -1; }

    if (ptrace(PTRACE_POKEUSER, tid, (void *)DR(0), (void *)bp) ||
        ptrace(PTRACE_POKEUSER, tid, (void *)DR7, (void *)0x1ULL)) {
        ptrace(PTRACE_DETACH, tid, NULL, NULL); return -1;
    }

    int hit = 0;
    g_to = 0;
    alarm(tmo);
    while (!g_to) {
        if (ptrace(PTRACE_CONT, tid, NULL, NULL)) break;
        if (waitpid(tid, &st, 0) < 0) { if (errno == EINTR) continue; break; }
        if (WIFSTOPPED(st)) {
            if (WSTOPSIG(st) == SIGTRAP) { hit = 1; break; }
            continue;
        }
        break;
    }
    alarm(0);

    if (hit) {
        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, tid, NULL, &r) == 0) {
            uint64_t ctx = r.rdi, obj = 0, pw = 0;
            uint32_t v104 = 0, v14 = 0; uint8_t vbf8 = 0;
            if (peek(tid, ctx + 0x1300, &obj) == 0 && obj) {
                peek32(tid, obj + 0x104, &v104);
                peek32(tid, obj + 0x14, &v14);
                peek8(tid, obj + 0xbf8, &vbf8);
                peek(tid, ctx + 0x1130, &pw);
                printf("ctx=0x%llx obj=0x%llx v104=%u vbf8=0x%x v14=%u W=0x%llx\n",
                       (unsigned long long)ctx, (unsigned long long)obj, v104, vbf8, v14,
                       (unsigned long long)pw);
            } else {
                printf("HIT tid=%d ctx=0x%llx 但 +0x1300 读不到\n", tid, (unsigned long long)ctx);
            }
        }
    }

    ptrace(PTRACE_POKEUSER, tid, (void *)DR7, (void *)0ULL);
    ptrace(PTRACE_POKEUSER, tid, (void *)DR(0), (void *)0ULL);
    ptrace(PTRACE_DETACH, tid, NULL, NULL);
    return hit;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "用法: %s <pid> <base> <off> [tmo]\n", argv[0]); return 2; }
    pid_t pid = atoi(argv[1]);
    uint64_t bp = strtoull(argv[2], NULL, 16) + strtoull(argv[3], NULL, 16);
    int tmo = (argc > 4) ? atoi(argv[4]) : 1;

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("pid=%d bp=0x%llx tmo=%ds\n", pid, (unsigned long long)bp, tmo);

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) { perror("opendir"); return 1; }

    /* 先按线程 CPU 时间排序，优先试最忙的 */
    pid_t tids[512]; int nt = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nt < 512) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        tids[nt++] = atoi(e->d_name);
    }
    closedir(d);
    printf("线程数 %d\n", nt);

    for (int i = 0; i < nt; i++) {
        printf("  试 tid=%d ... ", tids[i]);
        int r = try_thread(tids[i], bp, tmo);
        if (r > 0) { printf("HIT\n"); return 0; }
        printf("miss(%d)\n", r);
    }
    printf("所有线程未命中\n");
    return 1;
}
