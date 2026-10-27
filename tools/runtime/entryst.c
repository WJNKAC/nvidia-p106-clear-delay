/* entryst.c —— 在指定函数入口下硬件执行断点，统计"每个参数值"的分布
 *
 * 为什么需要它：
 *   glcore 里像 ensure_space(0x1578280/0xfbed80) 这类函数不是通过静态 call 调用的，
 *   而是通过"上下文对象里的函数指针槽"(如 [ctx+0x1a90]) 间接调用，
 *   也没有 RELATIVE 重定位。静态调用图拿不到，只能动态测。
 *
 * 用法：
 *   entryst <pid> <lib_substr> <offset> <secs> [tid] [reg]
 *     lib_substr : /proc/pid/maps 里用于匹配库的字符串，如 nvidia-glcore
 *     offset     : 相对该库基址的偏移（十六进制，可带 0x）
 *     secs       : 采样秒数
 *     tid        : 可选，指定线程；省略则自动挑最忙的那个
 *     reg        : 可选，要统计的寄存器，默认 rsi（x86-64 第二参数）
 *
 * 输出：
 *   - 总命中数、命中率（/秒）
 *   - 指定寄存器取值的前 12 个高频值
 *   - 以 2 为底的对数直方图（看数量级分布）
 *
 * 注意（踩过的坑）：
 *   1) 库基址必须取 maps 里 offset==0 的那一段，否则偏移算错。
 *   2) **只能给一个线程下断点**。给所有线程下 DR 会崩目标（追踪/断点陷阱）。
 *   3) 硬件断点最多 4 个，这里只用 1 个。
 *   4) 读寄存器必须在 PTRACE_CONT 之前（attach 状态下）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/types.h>

#define MAXV 64
static unsigned long long vals[MAXV];
static int nvals = 0;
static long nhit = 0;

static void add_val(unsigned long long v)
{
    for (int i = 0; i < nvals; i++)
        if (vals[i] == v) return;
    if (nvals < MAXV) vals[nvals++] = v;
}

/* 找库基址：必须是 offset==0 的那一段 */
static unsigned long find_base(pid_t pid, const char *sub)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long best = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b, off;
        char perms[8], dev[16], name[512] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %15s %*s %511s", &a, &b, perms, &off, dev, name) < 5)
            continue;
        if (!strstr(name, sub)) continue;
        if (off != 0) continue;
        best = a;
        break;
    }
    fclose(f);
    return best;
}

/* 挑最忙的线程：读 /proc/pid/task/<tid>/stat 的 utime+stime，取最大者（排除主线程可选） */
static pid_t busiest_tid(pid_t pid)
{
    char path[128], line[2048];
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) return pid;
    pid_t best = pid;
    unsigned long long bestv = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t tid = atoi(e->d_name);
        char p2[160];
        snprintf(p2, sizeof p2, "/proc/%d/task/%d/stat", pid, tid);
        FILE *f = fopen(p2, "r");
        if (!f) continue;
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }
        fclose(f);
        /* 跳过 comm（可能含空格），从最后一个 ')' 之后开始 */
        char *rp = strrchr(line, ')');
        if (!rp) continue;
        unsigned long long ut = 0, st = 0;
        /* state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime */
        if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2)
            continue;
        unsigned long long tot = ut + st;
        if (tot > bestv) { bestv = tot; best = tid; }
    }
    closedir(d);
    return best;
}

static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + t.tv_nsec / 1000000ull;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "用法: %s <pid> <lib_substr> <offset> <secs> [tid] [reg]\n", argv[0]);
        return 1;
    }
    pid_t pid = atoi(argv[1]);
    const char *sub = argv[2];
    unsigned long offset = strtoul(argv[3], NULL, 16);
    int secs = atoi(argv[4]);
    pid_t tid = (argc > 5 && atoi(argv[5]) > 0) ? atoi(argv[5]) : 0;
    const char *regname = (argc > 6) ? argv[6] : "rsi";

    unsigned long base = find_base(pid, sub);
    if (!base) { fprintf(stderr, "找不到库基址（要求 offset==0 的 mapping）\n"); return 2; }
    unsigned long addr = base + offset;
    if (!tid) tid = busiest_tid(pid);

    fprintf(stderr, "库基址=0x%lx  断点=0x%lx  目标线程=%d  寄存器=%s  时长=%ds\n",
            base, addr, tid, regname, secs);

    long rc = ptrace(PTRACE_ATTACH, tid, 0, 0);
    if (rc < 0) { perror("PTRACE_ATTACH"); return 3; }
    int status;
    waitpid(tid, &status, 0);

    struct user_regs_struct r;
    if (ptrace(PTRACE_GETREGS, tid, 0, &r) < 0) { perror("GETREGS"); return 4; }

    /* DR0 = 断点地址；DR7: L0=1, R/W0=00(执行), LEN0=00 */
    if (ptrace(PTRACE_POKEUSER, tid, offsetof(struct user, u_debugreg[0]), (void *)addr) < 0) {
        perror("DR0"); return 5;
    }
    if (ptrace(PTRACE_POKEUSER, tid, offsetof(struct user, u_debugreg[7]), (void *)0x1) < 0) {
        perror("DR7"); return 6;
    }

    int which = 1;
    int useret = 0;
    int usetimer = 0;
    int usedump = 0;
    unsigned long timer_off = 0;
    unsigned long dump_off = 0;
    int dump_n = 0;
    if (!strncmp(regname, "timer:", 6)) {
        usetimer = 1;
        timer_off = strtoul(regname + 6, NULL, 16);
    }
    else if (!strncmp(regname, "dump:", 5) || !strncmp(regname, "dumprbp:", 8)) {
        /* dump:<off>:<n>  —— 从 ctx(rdi) 导出
         * dumprbp:<off>:<n> —— 从 rbp 导出（glcore 的全局派发表在 bss，运行时才填） */
        const char *spec = regname + (regname[4] == ':' ? 5 : 8);
        char *dup = strdup(spec);
        char *c = strchr(dup, ':');
        if (c) { *c = 0; dump_n = atoi(c + 1); }
        dump_off = strtoul(dup, NULL, 16);
        free(dup);
        usedump = (regname[4] == ':') ? 1 : 2;   /* 1=rdi  2=rbp */
    }
    else if (!strcmp(regname, "ret")) { useret = 1; }
    else if (!strcmp(regname, "rsi")) which = 2;
    else if (!strcmp(regname, "rdx")) which = 3;
    else if (!strcmp(regname, "rcx")) which = 4;
    else if (!strcmp(regname, "rdi")) which = 1;
    else if (!strcmp(regname, "r8")) which = 5;
    else if (!strcmp(regname, "r9")) which = 6;

    /* timer 模式的统计量 */
    unsigned long long tsum = 0, tcnt = 0, tmin = ~0ull, tmax = 0, prev = 0;
    int have_prev = 0;
    unsigned long ctx_seen = 0;

    unsigned long long t_end = now_ms() + (unsigned long long)secs * 1000;
    while (now_ms() < t_end) {
        if (ptrace(PTRACE_CONT, tid, 0, 0) < 0) break;
        if (waitpid(tid, &status, 0) < 0) break;
        if (WIFEXITED(status) || WIFSIGNALED(status)) { fprintf(stderr, "目标已退出\n"); break; }
        if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
            nhit++;
            if (ptrace(PTRACE_GETREGS, tid, 0, &r) == 0) {
                unsigned long long v = 0;
                if (usedump) {
                    if (nhit == 1) {
                        unsigned long long tbase = (usedump == 2) ? r.rbp : r.rdi;
                        printf("=== base(%s)=0x%llx  区间 [+0x%lx, +0x%lx) ===\n",
                               (usedump == 2) ? "rbp" : "rdi",
                               tbase, dump_off, dump_off + 8L*dump_n);
                        for (int k = 0; k < dump_n; k++) {
                            unsigned long a = tbase + dump_off + 8L*k;
                            errno = 0;
                            long w = ptrace(PTRACE_PEEKTEXT, tid, (void *)a, 0);
                            if (errno) { printf("  +0x%03lx  <读失败>\n", dump_off + 8L*k); continue; }
                            unsigned long long p = (unsigned long long)(unsigned long)w;
                            if (p >= base && p < base + 0x3000000ULL)
                                printf("  +0x%03lx  -> glcore+0x%-9llx\n", dump_off + 8L*k, p - base);
                            else
                                printf("  +0x%03lx  = 0x%-12llx (非本库)\n", dump_off + 8L*k, p);
                        }
                        fflush(stdout);
                    }
                } else if (usetimer) {
                    /* 读驱动自己累计的"等待耗时"字段：[ctx + off]，ctx 默认在 rdi。
                     * 相邻两次命中的差值 = 上一次等待的实际时长（驱动自测，不受 ptrace 干扰）。 */
                    ctx_seen = r.rdi;
                    errno = 0;
                    long w = ptrace(PTRACE_PEEKTEXT, tid, (void *)(r.rdi + timer_off), 0);
                    if (!errno) {
                        unsigned long long cur = (unsigned long long)(unsigned long)w;
                        if (have_prev && cur >= prev) {
                            unsigned long long d = cur - prev;
                            tsum += d; tcnt++;
                            if (d < tmin) tmin = d;
                            if (d > tmax) tmax = d;
                        }
                        prev = cur; have_prev = 1;
                    }
                } else if (useret) {
                    errno = 0;
                    long w = ptrace(PTRACE_PEEKTEXT, tid, (void *)r.rsp, 0);
                    if (!errno) {
                        unsigned long long ret = (unsigned long long)(unsigned long)w;
                        v = (ret >= base + 5) ? (ret - base - 5) : 0;
                    }
                    add_val(v);
                } else {
                    switch (which) {
                        case 1: v = r.rdi; break;
                        case 2: v = r.rsi; break;
                        case 3: v = r.rdx; break;
                        case 4: v = r.rcx; break;
                        case 5: v = r.r8;  break;
                        case 6: v = r.r9;  break;
                    }
                    add_val(v);
                }
            }
        }
    }

    ptrace(PTRACE_POKEUSER, tid, offsetof(struct user, u_debugreg[7]), (void *)0);
    ptrace(PTRACE_DETACH, tid, 0, 0);

    printf("命中总数=%ld  速率=%.0f 次/秒\n", nhit, nhit / (double)secs);
    if (usetimer) {
        printf("ctx=0x%lx  计时器偏移=+0x%lx\n", ctx_seen, timer_off);
        printf("相邻命中差值样本=%llu 个（每次差值 = 上一次等待的实际耗时）\n", tcnt);
        if (tcnt) {
            printf("  平均=%.1f ns  最小=%llu ns  最大=%llu ns\n",
                   (double)tsum / (double)tcnt, tmin, tmax);
            printf("  累计等待=%.3f ms（该窗口内）\n", (double)tsum / 1e6);
            printf("  窗口内等待占比=%.1f%%（窗口 %d 秒）\n",
                   100.0 * ((double)tsum / 1e6) / (secs * 1000.0), secs);
        }
        return 0;
    }
    printf("不同取值个数=%d（上限 %d，超出部分未记）\n", nvals, MAXV);
    printf("取值（升序）：\n");
    /* 简单排序 */
    for (int i = 0; i < nvals; i++)
        for (int j = i + 1; j < nvals; j++)
            if (vals[j] < vals[i]) { unsigned long long t = vals[i]; vals[i] = vals[j]; vals[j] = t; }
    for (int i = 0; i < nvals && i < 40; i++)
        printf("  0x%-14llx %llu\n", vals[i], vals[i]);
    return 0;
}
