/* rpsample.c —— 用 ptrace 采样目标进程的 RIP，统计热点地址
 * 不需要 perf 权限（perf_event_paranoid 挡住时用这个）
 *
 * 用法: rpsample <pid> [样本数] [间隔us]
 * 输出: 每个 rip 命中的次数，以及换算成 libnvidia-glcore 内的偏移
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <errno.h>

#define MAXB 8192

static uint64_t va[MAXB];
static int      ct[MAXB];
static int      nb;

static void add(uint64_t a)
{
    for (int i = 0; i < nb; i++) if (va[i] == a) { ct[i]++; return; }
    if (nb < MAXB) { va[nb] = a; ct[nb] = 1; nb++; }
}

/* 从 maps 里找 glcore / libc 的装载基址 */
static uint64_t gbase, gend, lbase, lend;
static uint64_t yield_lo, yield_hi;
static char gname[256];

static void find_bases(pid_t pid)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        if (strstr(line, "libnvidia-glcore") && !gbase) {
            gbase = a; gend = b;
            char *s = strrchr(line, '/');
            snprintf(gname, sizeof gname, "%s", s ? s + 1 : "?");
        }
        if (strstr(line, "libc.so") && !lbase) { lbase = a; lend = b; }
    }
    fclose(f);
}

static int cmpb(const void *x, const void *y)
{
    int i = *(const int *)x, j = *(const int *)y;
    return ct[j] - ct[i];
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s <pid> [样本数] [间隔us]\n", argv[0]); return 9; }
    pid_t pid = (pid_t)strtol(argv[1], 0, 0);
    int n  = argc > 2 ? atoi(argv[2]) : 400;
    int us = argc > 3 ? atoi(argv[3]) : 800;

    find_bases(pid);
    if (!gbase) { fprintf(stderr, "没找到 libnvidia-glcore（进程没加载？）\n"); return 9; }
    /* sched_yield 在 libc 内的固定偏移：由 objdump 测得 0xf8640..0xf8661 */
    yield_lo = lbase ? lbase + 0xf8640ULL : 0;
    yield_hi = lbase ? lbase + 0xf8661ULL : 0;
    fprintf(stderr, "[rpsample] %s base=%#lx  libc base=%#lx  yield=[%#lx,%#lx)\n", gname,
            (unsigned long)gbase, (unsigned long)lbase,
            (unsigned long)yield_lo, (unsigned long)yield_hi);

    if (ptrace(PTRACE_SEIZE, pid, 0, 0) < 0) { perror("PTRACE_SEIZE"); return 2; }

    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) < 0) break;
        int st = 0;
        if (waitpid(pid, &st, 0) < 0) break;
        if (!WIFSTOPPED(st)) break;

        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, pid, 0, &r) == 0) {
            add(r.rip); ok++;
            /* 若停在 sched_yield 内，读栈顶 = 调用者的返回地址 */
            if (r.rip >= yield_lo && r.rip < yield_hi) {
                errno = 0;
                long v = ptrace(PTRACE_PEEKDATA, pid, (void *)r.rsp, NULL);
                if (!errno) add((uint64_t)v + 0x100000000000ULL);  /* 高位打标，区分 */
            }
        }

        if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) break;
        usleep(us);
    }
    ptrace(PTRACE_DETACH, pid, 0, 0);

    fprintf(stderr, "[rpsample] 有效样本 %d / 计划 %d\n", ok, n);

    int idx[MAXB];
    for (int i = 0; i < nb; i++) idx[i] = i;
    qsort(idx, nb, sizeof(int), cmpb);

    printf("%6s %-18s %s\n", "次数", "rip", "归属");
    for (int k = 0; k < nb && k < 25; k++) {
        int i = idx[k];
        uint64_t a = va[i]; int tagged = 0;
        if (a >> 44) { a -= 0x100000000000ULL; tagged = 1; }
        char where[64] = "?";
        if (a >= gbase && a < gend)      snprintf(where, sizeof where, "glcore+%#llx", (unsigned long long)(a - gbase));
        else if (lbase && a >= lbase && a < lend) snprintf(where, sizeof where, "libc+%#llx", (unsigned long long)(a - lbase));
        else snprintf(where, sizeof where, "other(%#llx)", (unsigned long long)a);
        printf("%6d %#18llx %s%s\n", ct[i], (unsigned long long)a, where, tagged?"   <- sched_yield 调用者":"");
    }
    return 0;
}
