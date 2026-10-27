/* rpsample2.c —— ptrace 采样 RIP，按 glcore/其他库 分别统计热点
 * 修复报告 146 版的地址标记 bug（旧版把正常 0x7f.. 地址误当成带标记的）
 *
 * 用法: rpsample2 <pid> [样本数] [间隔us]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>

#define MAXB 16384

struct hit { uint64_t a; int n; };
static struct hit rhits[MAXB];  static int nr;
static struct hit chits[MAXB];  static int nc;   /* sched_yield 的调用者 */

static void add(struct hit *arr, int *n, uint64_t a)
{
    for (int i = 0; i < *n; i++) if (arr[i].a == a) { arr[i].n++; return; }
    if (*n < MAXB) { arr[*n].a = a; arr[*n].n = 1; (*n)++; }
}

static uint64_t gbase, gend, lbase, lend, ylo, yhi;

static void find_bases(pid_t pid)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        if (strstr(line, "libnvidia-glcore")) {
            if (!gbase) gbase = a;
            if (b > gend) gend = b;          /* 取所有段的 max，不能只取第一段 */
        }
        if (strstr(line, "libc.so") && !lbase) { lbase = a; lend = b; }
    }
    fclose(f);
    /* __sched_yield 在 libc 内的偏移由 objdump 测得 */
    ylo = lbase ? lbase + 0xf8640ULL : 0;
    yhi = lbase ? lbase + 0xf8661ULL : 0;
}

static int peekq(pid_t pid, uint64_t addr, uint64_t *out)
{
    uint64_t tmp = 0;
    struct iovec li = { &tmp, 8 }, ri = { (void *)addr, 8 };
    if (process_vm_readv(pid, &li, 1, &ri, 1, 0) == 8) { *out = tmp; return 0; }
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr & ~7ULL), NULL);
    if (errno) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int cmpb(const void *x, const void *y)
{
    const struct hit *a = x, *b = y;
    return b->n - a->n;
}

static void report(const char *title, struct hit *arr, int n, int caller_mode)
{
    qsort(arr, n, sizeof(struct hit), cmpb);
    printf("\n=== %s ===\n", title);
    for (int k = 0; k < n && k < 20; k++) {
        uint64_t a = arr[k].a;
        char where[80];
        if (gbase && a >= gbase && a < gend)
            snprintf(where, sizeof where, "glcore+%#llx", (unsigned long long)(a - gbase));
        else if (lbase && a >= lbase && a < lend)
            snprintf(where, sizeof where, "libc+%#llx", (unsigned long long)(a - lbase));
        else snprintf(where, sizeof where, "?", 0);
        printf("  %6d  %-16s %#18llx%s\n", arr[k].n, where,
               (unsigned long long)a, caller_mode ? "  <- yield 调用者" : "");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s <pid> [样本] [间隔us]\n", argv[0]); return 9; }
    pid_t pid = (pid_t)strtol(argv[1], 0, 0);
    int n = argc > 2 ? atoi(argv[2]) : 400;
    int us = argc > 3 ? atoi(argv[3]) : 700;

    find_bases(pid);
    if (!gbase) { fprintf(stderr, "没找到 libnvidia-glcore\n"); return 9; }
    fprintf(stderr, "[rpsample2] glcore base=%#lx libc base=%#lx yield=[%#lx,%#lx)\n",
            (unsigned long)gbase, (unsigned long)lbase,
            (unsigned long)ylo, (unsigned long)yhi);

    if (ptrace(PTRACE_SEIZE, pid, 0, 0) < 0) { perror("PTRACE_SEIZE"); return 2; }

    int ok = 0, nyield = 0;
    for (int i = 0; i < n; i++) {
        if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) < 0) break;
        int st = 0;
        if (waitpid(pid, &st, 0) < 0) break;
        if (!WIFSTOPPED(st)) break;
        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, pid, 0, &r) == 0) {
            add(rhits, &nr, r.rip); ok++;
            if (ylo && r.rip >= ylo && r.rip < yhi) {
                uint64_t v;
                if (peekq(pid, r.rsp, &v) == 0) { add(chits, &nc, v); nyield++; }
            }
        }
        if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) break;
        usleep(us);
    }
    ptrace(PTRACE_DETACH, pid, 0, 0);
    fprintf(stderr, "[rpsample2] 有效样本 %d / %d，其中停在 sched_yield 内 %d 次\n", ok, n, nyield);

    report("RIP 热点", rhits, nr, 0);
    if (nc) report("sched_yield 的调用者", chits, nc, 1);
    return 0;
}
