/* fd2.c —— 断点 + 读字段/寄存器（修订版）
 *
 * 修掉 fielddump.c 的两个问题：
 *   1. 字段读取原来在 PTRACE_DETACH 之后执行，GETREGS 必然失败 → 移到附加期间
 *   2. ctx 不一定是 rdi（有些点 ctx 在 rbp/rbx）→ 支持 ctxreg=rbp
 *
 * 用法:
 *   fd2 <pid> <glcore_off> <秒> [ctxreg=rXX] <spec>...
 *   spec: 0x1130:u64 | reg:r12 | 0x3c0:p+0x48:u32
 *
 * 可重复运行捕获多个样本：加 samples=N
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <time.h>

#define DR_BASE 848
#define DR(n) (DR_BASE + (n)*8)
#define DR7   DR(7)

static volatile sig_atomic_t g_to = 0;
static void on_alarm(int s) { (void)s; g_to = 1; }

static uint64_t base_addr;

static uint64_t find_base(pid_t pid)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512]; uint64_t b = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "libnvidia-glcore")) continue;
        unsigned long a, off = 0;
        if (sscanf(line, "%lx-", &a) != 1) continue;
        char *sp = strchr(line, ' ');
        if (sp) sscanf(sp + 1, "%*s %lx", &off);
        if (off == 0) { b = a; break; }
        if (!b) b = a;
    }
    fclose(f);
    return b;
}

static int peekq(pid_t pid, uint64_t addr, uint64_t *out)
{
    uint64_t tmp = 0;
    struct iovec li = { &tmp, 8 }, ri = { (void *)(addr & ~7ULL), 8 };
    if (process_vm_readv(pid, &li, 1, &ri, 1, 0) == 8) { *out = tmp; return 0; }
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr & ~7ULL), NULL);
    if (errno) return -1;
    *out = (uint64_t)v; return 0;
}

static int readn(pid_t pid, uint64_t a, int w, uint64_t *out)
{
    uint64_t v, v2;
    if (peekq(pid, a, &v)) return -1;
    int sh = (int)((a & 7) * 8);
    if (sh + w * 8 <= 64) {
        *out = (v >> sh) & (w == 8 ? ~0ULL : ((1ULL << (w * 8)) - 1));
        return 0;
    }
    if (peekq(pid, a + 8, &v2)) return -1;
    __uint128_t big = ((__uint128_t)v2 << 64) | v;
    *out = (uint64_t)((big >> sh) & ((__uint128_t)1 << (w * 8)) - 1);
    return 0;
}

static uint64_t reg_get(const struct user_regs_struct *r, const char *n)
{
    if (!strcmp(n,"rax")) return r->rax;  if (!strcmp(n,"rbx")) return r->rbx;
    if (!strcmp(n,"rcx")) return r->rcx;  if (!strcmp(n,"rdx")) return r->rdx;
    if (!strcmp(n,"rsi")) return r->rsi;  if (!strcmp(n,"rdi")) return r->rdi;
    if (!strcmp(n,"rbp")) return r->rbp;  if (!strcmp(n,"rsp")) return r->rsp;
    if (!strcmp(n,"r8"))  return r->r8;   if (!strcmp(n,"r9"))  return r->r9;
    if (!strcmp(n,"r10")) return r->r10;  if (!strcmp(n,"r11")) return r->r11;
    if (!strcmp(n,"r12")) return r->r12;  if (!strcmp(n,"r13")) return r->r13;
    if (!strcmp(n,"r14")) return r->r14;  if (!strcmp(n,"r15")) return r->r15;
    if (!strcmp(n,"rip")) return r->rip;
    return 0;
}

struct spec { int isreg; char name[32]; uint64_t off, sub; int deref, width; char txt[64]; };
static struct spec specs[32];
static int nspec;

static int parse_spec(const char *s, struct spec *sp)
{
    memset(sp, 0, sizeof *sp);
    snprintf(sp->txt, sizeof sp->txt, "%s", s);
    char buf[256]; snprintf(buf, sizeof buf, "%s", s);
    char *c = strrchr(buf, ':');
    if (!c) return -1;
    *c = 0;
    const char *w = c + 1;
    if (!strcmp(w,"u8")) sp->width = 1;
    else if (!strcmp(w,"u16")) sp->width = 2;
    else if (!strcmp(w,"u32")) sp->width = 4;
    else if (!strcmp(w,"u64")) sp->width = 8;
    else return -1;
    if (!strncmp(buf, "reg:", 4)) {
        sp->isreg = 1;
        snprintf(sp->name, sizeof sp->name, "%s", buf + 4);
        return 0;
    }
    char *pd = strstr(buf, ":p+");
    if (pd) { *pd = 0; sp->deref = 1; sp->sub = strtoull(pd + 3, NULL, 0); }
    sp->off = strtoull(buf, NULL, 0);
    return 0;
}

/* 在附加状态下打印所有 spec */
static void dump_all(pid_t tid, uint64_t ctx)
{
    struct user_regs_struct r;
    int got = (ptrace(PTRACE_GETREGS, tid, NULL, &r) == 0);
    if (!got) { printf("  <GETREGS 失败>\n"); return; }
    for (int k = 0; k < nspec; k++) {
        if (specs[k].isreg) {
            printf("  %-16s = %#llx (%llu)\n", specs[k].txt,
                   (unsigned long long)reg_get(&r, specs[k].name),
                   (unsigned long long)reg_get(&r, specs[k].name));
            continue;
        }
        uint64_t a = ctx + specs[k].off, v = 0;
        if (specs[k].deref) {
            uint64_t p = 0;
            if (readn(tid, a, 8, &p) || !p) { printf("  %-16s = <ptr 空/失败>\n", specs[k].txt); continue; }
            if (readn(tid, p + specs[k].sub, specs[k].width, &v) == 0)
                printf("  %-16s = %#llx  ([%#llx]+%#llx)\n", specs[k].txt,
                       (unsigned long long)v, (unsigned long long)p,
                       (unsigned long long)specs[k].sub);
            else printf("  %-16s = <解引用失败>\n", specs[k].txt);
        } else {
            if (readn(tid, a, specs[k].width, &v) == 0)
                printf("  %-16s = %#llx (%llu)\n", specs[k].txt,
                       (unsigned long long)v, (unsigned long long)v);
            else printf("  %-16s = <读失败>\n", specs[k].txt);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <pid> <off> <秒> [ctxreg=rXX] [samples=N] <spec>...\n", argv[0]);
        return 2;
    }
    pid_t pid = (pid_t)strtol(argv[1], 0, 0);
    uint64_t off = strtoull(argv[2], 0, 0);
    int tmo = atoi(argv[3]);
    char ctxreg[16] = "rdi";
    int samples = 1;

    for (int i = 4; i < argc; i++) {
        if (!strncmp(argv[i], "ctxreg=", 7)) { snprintf(ctxreg, sizeof ctxreg, "%s", argv[i] + 7); continue; }
        if (!strncmp(argv[i], "samples=", 8)) { samples = atoi(argv[i] + 8); continue; }
        if (parse_spec(argv[i], &specs[nspec]) == 0) nspec++;
        else fprintf(stderr, "跳过 spec: %s\n", argv[i]);
    }

    base_addr = find_base(pid);
    if (!base_addr) { fprintf(stderr, "没找到 glcore\n"); return 9; }
    uint64_t bp = base_addr + off;
    printf("pid=%d base=%#llx bp=%#llx ctxreg=%s samples=%d\n",
           pid, (unsigned long long)base_addr, (unsigned long long)bp, ctxreg, samples);

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    char pth[64]; snprintf(pth, sizeof pth, "/proc/%d/task", pid);
    DIR *d = opendir(pth);
    if (!d) { perror("opendir"); return 1; }
    pid_t tids[512]; int nt = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nt < 512)
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids[nt++] = atoi(e->d_name);
    closedir(d);

    /* 选 CPU 时间最大的线程 */
    pid_t best = pid; unsigned long long bestut = 0;
    for (int i = 0; i < nt; i++) {
        char f[96]; snprintf(f, sizeof f, "/proc/%d/task/%d/stat", pid, tids[i]);
        FILE *fp = fopen(f, "r"); if (!fp) continue;
        char b[1024]; if (!fgets(b, sizeof b, fp)) { fclose(fp); continue; }
        fclose(fp);
        char *rp2 = strrchr(b, ')'); if (!rp2) continue;
        unsigned long long ut = 0, st2 = 0;
        if (sscanf(rp2 + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st2) >= 1)
            if (ut + st2 > bestut) { bestut = ut + st2; best = tids[i]; }
    }

    if (ptrace(PTRACE_ATTACH, best, NULL, NULL)) { perror("ATTACH"); return 3; }
    int st; if (waitpid(best, &st, 0) < 0) { ptrace(PTRACE_DETACH, best, NULL, NULL); return 3; }
    if (ptrace(PTRACE_POKEUSER, best, (void *)DR(0), (void *)bp) ||
        ptrace(PTRACE_POKEUSER, best, (void *)DR7, (void *)0x1ULL)) {
        perror("POKEUSER"); ptrace(PTRACE_DETACH, best, NULL, NULL); return 3;
    }

    int got = 0, need_cont = 1;
    g_to = 0; alarm(tmo);
    while (!g_to && got < samples) {
        if (need_cont) {
            if (ptrace(PTRACE_CONT, best, NULL, NULL)) { perror("CONT"); break; }
            need_cont = 0;
        }
        int r = waitpid(best, &st, WNOHANG);
        if (r == 0) { usleep(200); continue; }
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (WIFSTOPPED(st)) {
            if (WSTOPSIG(st) == SIGTRAP) {
                struct user_regs_struct rr;
                if (ptrace(PTRACE_GETREGS, best, NULL, &rr) == 0) {
                    uint64_t ctx = reg_get(&rr, ctxreg);
                    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
                      printf("\n--- 样本 %d  t=%.6f  ctx(%s)=%#llx ---\n", ++got,
                             ts.tv_sec + ts.tv_nsec/1e9, ctxreg, (unsigned long long)ctx); }
                    dump_all(best, ctx);
                }
                need_cont = 1;
                continue;
            }
            if (ptrace(PTRACE_CONT, best, NULL, (void *)(long)WSTOPSIG(st))) break;
            continue;
        }
        break;
    }
    alarm(0);

    ptrace(PTRACE_POKEUSER, best, (void *)DR7, (void *)0ULL);
    ptrace(PTRACE_POKEUSER, best, (void *)DR(0), (void *)0ULL);
    ptrace(PTRACE_DETACH, best, NULL, NULL);
    if (!got) printf("未命中\n");
    return 0;
}
