/* fielddump.c —— 用 ptrace 硬件执行断点捕获 ctx，然后按需 dump 任意字段
 * 零字节修改目标进程（只在调试寄存器上设断点）。
 *
 * 用法:
 *   fielddump <pid> <base_hex> <bp_off_hex> [tmo_sec] <spec> [spec ...]
 *   spec 形式:
 *     0x21c:u32           读 [ctx+0x21c] 的 32 位
 *     0x22f2c:u32         读 [ctx+0x22f2c]
 *     0x3c0:p+0x48:u32    读 [[ctx+0x3c0]+0x48]（一级指针解引用）
 *     0x1240:u64          读 [ctx+0x1240] 的 64 位
 *
 * 断点命中后 rdi 即 ctx（glcore 内部这些函数都以 ctx 为第一个参数）。
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
#include <sys/uio.h>

#define DR_BASE 848
#define DR(n)   (DR_BASE + (n)*8)
#define DR7     DR(7)

static volatile sig_atomic_t g_to = 0;
static void on_alarm(int s) { (void)s; g_to = 1; }

static int peek(pid_t pid, uint64_t addr, uint64_t *out)
{
    errno = 0;
    long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr & ~7ULL), NULL);
    if (errno) {
        /* 回退到 process_vm_readv（对已停止的 tracee 更稳） */
        uint64_t tmp = 0;
        struct iovec li = { &tmp, 8 }, ri = { (void *)(addr & ~7ULL), 8 };
        ssize_t n = process_vm_readv(pid, &li, 1, &ri, 1, 0);
        if (n == 8) { *out = tmp; return 0; }
        fprintf(stderr, "    [peek %#llx 失败 errno=%d(%s) vm=%zd]\n",
                (unsigned long long)(addr & ~7ULL), errno, strerror(errno), n);
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

/* 读任意宽度（本项目里都对齐到 8 以内，用 shift 处理） */
static int readn(pid_t pid, uint64_t addr, int width, uint64_t *out)
{
    uint64_t v;
    if (peek(pid, addr, &v)) return -1;
    int shift = (int)((addr & 7) * 8);
    if (shift + width * 8 <= 64) {
        *out = (v >> shift) & ((width == 8) ? ~0ULL : ((1ULL << (width * 8)) - 1));
        return 0;
    }
    /* 跨 8 字节边界 */
    uint64_t v2;
    if (peek(pid, addr + 8, &v2)) return -1;
    __uint128_t big = ((__uint128_t)v2 << 64) | v;
    *out = (uint64_t)((big >> shift) & ((width == 8) ? ~(__uint128_t)0 : (((__uint128_t)1 << (width * 8)) - 1)));
    return 0;
}


/* 从 /proc/<pid>/maps 取 libnvidia-glcore 的装载基址 */
static uint64_t find_glcore_base(pid_t pid)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512]; uint64_t base = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "libnvidia-glcore")) continue;
        unsigned long a;
        if (sscanf(line, "%lx-", &a) == 1) {
            /* 取第一个映射的起始，且偏移为 0 的那段才是真实基址 */
            char *dash = strchr(line, '-');
            char *sp = strchr(line, ' ');
            unsigned long off = 0;
            /* 格式: start-end perms offset dev inode path */
            if (dash && sp) sscanf(sp + 1, "%*s %lx", &off);
            if (off == 0) { base = a; break; }
            if (!base) base = a;
        }
    }
    fclose(f);
    return base;
}

struct spec { uint64_t off; int deref; uint64_t sub; int width; const char *txt; char *regname; };
static struct spec specs[32];
static int nspec;

static int parse_spec(const char *s, struct spec *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->txt = s;
    char buf[256]; snprintf(buf, sizeof buf, "%s", s);
    char *p = buf;

    char *colon = strrchr(p, ':');
    if (!colon) return -1;
    *colon = 0;
    if (!strncmp(p, "reg:", 4)) {           /* reg:r12 形式 */
        sp->regname = strdup(p + 4);
        sp->off = 0; sp->deref = 0;
        return 0;
    }
    if (!strcmp(colon + 1, "u8")) sp->width = 1;
    else if (!strcmp(colon + 1, "u16")) sp->width = 2;
    else if (!strcmp(colon + 1, "u32")) sp->width = 4;
    else if (!strcmp(colon + 1, "u64")) sp->width = 8;
    else return -1;

    char *pd = strstr(p, ":p+");
    if (pd) {
        *pd = 0;
        sp->deref = 1;
        sp->sub = strtoull(pd + 3, NULL, 0);
    }
    sp->off = strtoull(p, NULL, 0);
    return 0;
}

static int try_thread(pid_t tid, uint64_t bp, int tmo, uint64_t *ctx_out)
{
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL)) return -1;
    int st;
    if (waitpid(tid, &st, 0) < 0) { ptrace(PTRACE_DETACH, tid, NULL, NULL); return -1; }

    if (ptrace(PTRACE_POKEUSER, tid, (void *)DR(0), (void *)bp) ||
        ptrace(PTRACE_POKEUSER, tid, (void *)DR7, (void *)0x1ULL)) {
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
        return -1;
    }

    int hit = 0;
    g_to = 0;
    alarm(tmo);
    while (!g_to) {
        if (ptrace(PTRACE_CONT, tid, NULL, NULL)) break;
        if (waitpid(tid, &st, 0) < 0) { if (errno == EINTR) continue; break; }
        if (WIFSTOPPED(st)) {
            if (WSTOPSIG(st) == SIGTRAP) { hit = 1; break; }
            continue;                       /* 其它信号原样放行 */
        }
        break;
    }
    alarm(0);

    if (hit) {
        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, tid, NULL, &r) == 0) *ctx_out = r.rdi;
    }

    ptrace(PTRACE_POKEUSER, tid, (void *)DR7, (void *)0ULL);
    ptrace(PTRACE_POKEUSER, tid, (void *)DR(0), (void *)0ULL);
    ptrace(PTRACE_DETACH, tid, NULL, NULL);
    return hit;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "用法: %s <pid> <base> <bp_off> <tmo> <spec>...\n"
                        "  spec 例: 0x21c:u32   0x3c0:p+0x48:u32   0x1240:u64\n", argv[0]);
        return 2;
    }
    pid_t pid = atoi(argv[1]);
    uint64_t base = strtoull(argv[2], NULL, 16);
    if (!base) {
        base = find_glcore_base(pid);
        if (!base) { fprintf(stderr, "没能从 maps 找到 glcore 基址\n"); return 9; }
    }
    uint64_t bp = base + strtoull(argv[3], NULL, 16);
    int tmo = atoi(argv[4]);
    int count_mode = 0, count_secs = 5, argstart = 5;
    if (argc > 5 && !strncmp(argv[5], "count:", 6)) {
        count_mode = 1; count_secs = atoi(argv[5] + 6); argstart = 6;
    }
    for (int i = argstart; i < argc && nspec < 32; i++) {
        if (parse_spec(argv[i], &specs[nspec]) == 0) nspec++;
        else fprintf(stderr, "跳过无法解析的 spec: %s\n", argv[i]);
    }

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("pid=%d base=0x%llx bp=0x%llx tmo=%ds specs=%d\n", pid, (unsigned long long)base, (unsigned long long)bp, tmo, nspec);

    char pth[64];
    snprintf(pth, sizeof pth, "/proc/%d/task", pid);
    DIR *d = opendir(pth);
    if (!d) { perror("opendir"); return 1; }
    pid_t tids[512]; int nt = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nt < 512)
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids[nt++] = atoi(e->d_name);
    closedir(d);

    for (int i = 0; i < nt; i++) {
        uint64_t ctx = 0;
        int r = try_thread(tids[i], bp, tmo, &ctx);
        if (r > 0) {
            if (count_mode) {
                printf("HIT tid=%d ctx=0x%llx  ctx+0x21c=", tids[i], (unsigned long long)ctx);
                { uint64_t v = 0; if (readn(tids[i], ctx + 0x21c, 4, &v) == 0)
                      printf("%llu", (unsigned long long)v); else printf("?"); }
                printf("\n");
                return 0;
            }
            printf("HIT tid=%d ctx=0x%llx\n", tids[i], (unsigned long long)ctx);
            for (int k = 0; k < nspec; k++) {
                uint64_t addr = ctx + specs[k].off, v = 0;
                if (specs[k].regname) {
                    struct user_regs_struct rr2;
                    uint64_t rv = 0; int have = 0;
                    if (ptrace(PTRACE_GETREGS, tids[i], NULL, &rr2) == 0) {
                        const char *rn = specs[k].regname;
                        if (!strcmp(rn,"rax")) { rv = rr2.rax; have=1; }
                        else if (!strcmp(rn,"rbx")) { rv = rr2.rbx; have=1; }
                        else if (!strcmp(rn,"rcx")) { rv = rr2.rcx; have=1; }
                        else if (!strcmp(rn,"rdx")) { rv = rr2.rdx; have=1; }
                        else if (!strcmp(rn,"rsi")) { rv = rr2.rsi; have=1; }
                        else if (!strcmp(rn,"rdi")) { rv = rr2.rdi; have=1; }
                        else if (!strcmp(rn,"rbp")) { rv = rr2.rbp; have=1; }
                        else if (!strcmp(rn,"rsp")) { rv = rr2.rsp; have=1; }
                        else if (!strcmp(rn,"r8"))  { rv = rr2.r8;  have=1; }
                        else if (!strcmp(rn,"r9"))  { rv = rr2.r9;  have=1; }
                        else if (!strcmp(rn,"r10")) { rv = rr2.r10; have=1; }
                        else if (!strcmp(rn,"r11")) { rv = rr2.r11; have=1; }
                        else if (!strcmp(rn,"r12")) { rv = rr2.r12; have=1; }
                        else if (!strcmp(rn,"r13")) { rv = rr2.r13; have=1; }
                        else if (!strcmp(rn,"r14")) { rv = rr2.r14; have=1; }
                        else if (!strcmp(rn,"r15")) { rv = rr2.r15; have=1; }
                        else if (!strcmp(rn,"rip")) { rv = rr2.rip; have=1; }
                    }
                    if (have) printf("  %-18s = %#llx (%llu)   [寄存器]\n", specs[k].txt,
                                     (unsigned long long)rv, (unsigned long long)rv);
                    else printf("  %-18s = <未知寄存器>\n", specs[k].txt);
                    continue;
                }
                int rc;
                if (specs[k].deref) {
                    uint64_t ptr = 0;
                    if (readn(tids[i], addr, 8, &ptr) || !ptr) {
                        printf("  %-18s = <指针读失败或为空>\n", specs[k].txt);
                        continue;
                    }
                    rc = readn(tids[i], ptr + specs[k].sub, specs[k].width, &v);
                    if (rc == 0)
                        printf("  %-18s = %#llx   ([%#llx+%#llx])\n", specs[k].txt,
                               (unsigned long long)v, (unsigned long long)ptr,
                               (unsigned long long)specs[k].sub);
                    else
                        printf("  %-18s = <解引用读失败 ptr=%#llx>\n", specs[k].txt,
                               (unsigned long long)ptr);
                } else {
                    rc = readn(tids[i], addr, specs[k].width, &v);
                    if (rc == 0)
                        printf("  %-18s = %#llx (%llu)\n", specs[k].txt,
                               (unsigned long long)v, (unsigned long long)v);
                    else
                        printf("  %-18s = <读失败>\n", specs[k].txt);
                }
            }
            return 0;
        }
    }
    printf("所有线程未命中\n");
    return 1;
}
