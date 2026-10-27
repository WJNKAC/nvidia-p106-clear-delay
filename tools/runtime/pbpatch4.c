/* pbpatch4 —— 只扫"活跃段"的高命中率实参改写
 *
 * 演进过程（记录下来，避免再走弯路）：
 *   pbpatch  用 process_vm_readv → 段首未映射时整段 EFAULT，命中恒为 0
 *   pbpatch2 改 pread/pwrite，但每轮扫全部 28 段 × 1MB = 28MB，
 *            慢于 GPU 消费速度（每批 118µs），改写率仅 ~7.7%
 *   pbpatch3 改为"记住命中地址"，但环形缓冲轮转会换位置，地址表迅速失效 → 完全无效
 *   pbpatch4 ★ 记住"活跃段"（实测只有 2 段在变化，共 1MB），每轮只扫这 2 段
 *
 * 用法: pbpatch4 <pid> <meth_hex> <newvalue_hex> [secs]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#define MAXSEG 64
#define CAP    (1u << 20)

typedef struct { unsigned long a, b; int active; } seg_t;
static seg_t segs[MAXSEG];
static int nseg = 0;

static void load_segs(pid_t pid)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    nseg = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && nseg < MAXSEG) {
        if (!strstr(line, "nvidia")) continue;
        unsigned long a, b; char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        if (!strstr(perms, "rw")) continue;
        segs[nseg].a = a; segs[nseg].b = b; segs[nseg].active = 0; nseg++;
    }
    fclose(f);
}

static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + t.tv_nsec / 1000000ull;
}

/* 扫一段；mark_active=1 时只统计不改，用于首轮找出活跃段 */
static long scan_seg(int fd, seg_t *s, unsigned meth, unsigned newval, int do_patch)
{
    static unsigned char buf[CAP];
    size_t sz = s->b - s->a;
    if (sz > CAP) sz = CAP;
    ssize_t got = pread(fd, buf, sz, (off_t)s->a);
    if (got <= 0) return 0;

    long hits = 0;
    for (ssize_t p = 0; p + 8 <= got; p += 4) {
        uint32_t w;
        memcpy(&w, buf + p, 4);
        if ((w >> 29) != 1) continue;
        if (((w >> 16) & 0x1fff) != 1) continue;
        if ((((w & 0x1fff) << 2)) != meth) continue;
        hits++;
        if (!do_patch) continue;
        uint32_t arg;
        memcpy(&arg, buf + p + 4, 4);
        if (arg == newval) continue;
        uint32_t nv = newval;
        if (pwrite(fd, &nv, 4, (off_t)(s->a + p + 4)) == 4)
            memcpy(buf + p + 4, &nv, 4);
    }
    return hits;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <pid> <meth_hex> <newvalue_hex> [secs]\n", argv[0]);
        return 1;
    }
    pid_t pid = atoi(argv[1]);
    unsigned meth = (unsigned)strtoul(argv[2], NULL, 16);
    unsigned newval = (unsigned)strtoul(argv[3], NULL, 16);
    int secs = (argc > 4) ? atoi(argv[4]) : 10;
    /* 第 5 个参数为 "count" 时只统计、绝不写。
     * 用途：与 patcher 并行运行时，作为"零写入"的帧率代理计数器，
     *      这样两轮之间计数器的开销完全一致，命中率才可比。 */
    int count_only = (argc > 5 && strcmp(argv[5], "count") == 0);

    load_segs(pid);
    if (!nseg) { fprintf(stderr, "无可写 nvidia 段\n"); return 2; }

    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/mem", pid);
    int fd = open(mp, O_RDWR);
    if (fd < 0) {
        if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) { perror("ATTACH"); return 3; }
        int st; waitpid(pid, &st, 0);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        fd = open(mp, O_RDWR);
        if (fd < 0) { perror("open mem"); return 4; }
    }

    /* 首轮：找出含目标命令的段（=活跃段） */
    int nact = 0;
    for (int i = 0; i < nseg; i++) {
        if (scan_seg(fd, &segs[i], meth, newval, 0) > 0) {
            segs[i].active = 1; nact++;
        }
    }
    fprintf(stderr, "段=%d 活跃段=%d 目标 meth=%#x 新值=%#x\n", nseg, nact, meth, newval);
    if (!nact) return 5;

    long npatch = 0, nhit = 0;
    unsigned long long tend = now_ms() + (unsigned long long)secs * 1000;
    while (now_ms() < tend) {
        for (int i = 0; i < nseg; i++) {
            if (!segs[i].active) continue;
            long h = scan_seg(fd, &segs[i], meth, newval, count_only ? 0 : 1);
            if (h > 0) { nhit += h; npatch += h; }
        }
    }

    printf("活跃段内命中=%ld（含已达标）  目标值=%#x\n", nhit, newval);
    return 0;
}
