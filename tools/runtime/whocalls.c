/* whocalls.c —— 采样时回溯一帧，找出 target 函数是被哪个调用点调用的
 *
 * 原理：0x15761b0 的帧布局固定
 *   entry:  [rsp0] = 调用者返回地址
 *   push r14,r13,r12,rbp,rbx  (5×8 = 0x28)
 *   sub  rsp,0x10             (0x10)
 *   ⇒ 在 0x15761b0 内部，调用者返回地址在 [rsp + 0x38]
 *   ⇒ 若采样落在它调用的更深函数里（sched_yield），那里又 push 了返回地址 8 字节，
 *      则 rsp 再低 8 ⇒ 调用者返回地址在 [rsp + 0x40]
 *
 * 用法: whocalls <pid> <target_off> <span_bytes> [样本数] [间隔us]
 *   采样到 rip ∈ [target, target+span) 时，读出上层的返回地址并统计
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>

#define MAXB 4096
struct hit { uint64_t a; int n; };
static struct hit caller_hits[MAXB]; static int ncaller;
static struct hit site_hits[MAXB];   static int nsite;

static uint64_t gbase, gend;

static void add(struct hit *arr, int *n, uint64_t a)
{
    for (int i = 0; i < *n; i++) if (arr[i].a == a) { arr[i].n++; return; }
    if (*n < MAXB) { arr[*n].a = a; arr[*n].n = 1; (*n)++; }
}

static void find_base(pid_t pid)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "libnvidia-glcore")) continue;
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        if (!gbase) gbase = a;
        if (b > gend) gend = b;
    }
    fclose(f);
}

static int peekq(pid_t pid, uint64_t a, uint64_t *out)
{
    uint64_t t = 0;
    struct iovec li = { &t, 8 }, ri = { (void *)a, 8 };
    if (process_vm_readv(pid, &li, 1, &ri, 1, 0) == 8) { *out = t; return 0; }
    return -1;
}

static int cmpb(const void *x, const void *y)
{ const struct hit *a = x, *b = y; return b->n - a->n; }

static void report(const char *t, struct hit *arr, int n)
{
    qsort(arr, n, sizeof(struct hit), cmpb);
    printf("\n=== %s ===\n", t);
    for (int k = 0; k < n && k < 15; k++)
        printf("  %6d  %s%#llx\n", arr[k].n,
               (arr[k].a >= gbase && arr[k].a < gend) ? "glcore+" : "other ",
               (unsigned long long)(arr[k].a >= gbase && arr[k].a < gend
                                    ? arr[k].a - gbase : arr[k].a));
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <pid> <target_off> <span> [样本数] [间隔us]\n", argv[0]);
        return 9;
    }
    pid_t pid = strtol(argv[1], 0, 0);
    uint64_t off = strtoull(argv[2], 0, 0);
    uint64_t span = strtoull(argv[3], 0, 0);
    int n = argc > 4 ? atoi(argv[4]) : 3000;
    int us = argc > 5 ? atoi(argv[5]) : 150;

    find_base(pid);
    if (!gbase) { fprintf(stderr, "没找到 glcore\n"); return 9; }
    uint64_t lo = gbase + off, hi = lo + span;
    fprintf(stderr, "[whocalls] glcore base=%#lx  target=[%#lx,%#lx)\n",
            (unsigned long)gbase, (unsigned long)lo, (unsigned long)hi);

    if (ptrace(PTRACE_SEIZE, pid, 0, 0) < 0) { perror("SEIZE"); return 2; }

    int ok = 0, inside = 0;
    for (int i = 0; i < n; i++) {
        if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) < 0) break;
        int st = 0;
        if (waitpid(pid, &st, 0) < 0) break;
        if (!WIFSTOPPED(st)) break;
        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, pid, 0, &r) == 0) {
            ok++;
            /* 触发条件：rip 落在 target 内，**或者** [rsp] 指回 target
             * （后者覆盖"停在 target 调用的更深函数里"的情况，样本量最大） */
            uint64_t ret = 0;
            int have_ret = (peekq(pid, r.rsp, &ret) == 0);
            int in_rip = (r.rip >= lo && r.rip < hi);
            int ret_in = (have_ret && ret >= lo && ret < hi);
            if (in_rip || ret_in) {
                inside++;
                if (in_rip) add(site_hits, &nsite, r.rip);      /* 停在 target 的具体位置 */
                else        add(site_hits, &nsite, ret);        /* 停在它调用的哪里 */
                /* target 的调用者：帧布局决定偏移 */
                uint64_t off2 = in_rip ? 0x38 : 0x40;
                uint64_t cand;
                if (peekq(pid, r.rsp + off2, &cand) == 0)
                    add(caller_hits, &ncaller, cand);
            }
        }
        if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) break;
        usleep(us);
    }
    ptrace(PTRACE_DETACH, pid, 0, 0);
    fprintf(stderr, "[whocalls] 样本 %d，落在 target 内 %d 次\n", ok, inside);

    report("落在 target 内的返回地址（=它调用的更深函数位置）", site_hits, nsite);
    report("target 的调用者（回溯一帧）", caller_hits, ncaller);
    return 0;
}
