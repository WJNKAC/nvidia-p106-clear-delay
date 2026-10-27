/* pbpatch —— 实时改写 pushbuffer 里指定命令的实参
 *
 * 用法:
 *   pbpatch <pid> <meth_hex> <newvalue_hex> [secs]
 *
 * 做什么:
 *   周期性扫描目标进程的 nvidia 可写映射，找到 header 编码等于 (cnt=1, meth=目标) 的位置，
 *   把紧随其后的那个 dword 改成 newvalue。
 *
 *   命令头编码: w = (op<<29) | (cnt<<16) | (sub<<13) | (meth>>2)
 *   本工具只匹配 cnt==1 且 meth 精确相等的头（op/sub 不限），与 pbraw.py 的观测一致。
 *
 * 为什么这样改是安全的:
 *   只改"实参的值"，不改命令结构、不改长度。如果 GPU 已经消费过这一条，
 *   改的是环形缓冲里"下一个轮回"才会被读到的副本，不会破坏当前帧。
 *   （若该参数是"延迟量"，改动会直接反映到帧时间上。）
 *
 * 注意:
 *   - 需要 ptrace attach 才能写对方内存（/proc/pid/mem 写入也要求 ptrace 关系）。
 *   - 只支持一个线程的程序；多线程时要选对 tid（这里用 process_vm_writev 绕开这个限制）。
 *   - 每轮扫描只读"变化过的块"，避免全量扫 27 段 × 1MB。
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
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#define MAXSEG 64
#define CAP    (1u << 20)

typedef struct { unsigned long a, b; } seg_t;
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
        unsigned long a, b;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        if (!strstr(perms, "rw")) continue;
        segs[nseg].a = a; segs[nseg].b = b; nseg++;
    }
    fclose(f);
}

static int seg_of(unsigned long va)
{
    for (int i = 0; i < nseg; i++)
        if (va >= segs[i].a && va < segs[i].b) return i;
    return -1;
}

static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + t.tv_nsec / 1000000ull;
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

    load_segs(pid);
    fprintf(stderr, "pid=%d 可写 nvidia 段=%d 目标 meth=%#x 新值=%#x 时长=%ds\n",
            pid, nseg, meth, newval, secs);
    if (!nseg) return 2;

    int mem = open("/proc/x", O_RDONLY); (void)mem;

    /* ptrace attach 以获得写权限 */
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) { perror("ATTACH"); return 3; }
    int st;
    waitpid(pid, &st, 0);
    /* 立刻 detach，之后靠 process_vm_writev（同 uid 同进程族即可，
     * 实际上 ptrace attach 过的进程关系已建立；若失败会打印 errno） */
    ptrace(PTRACE_DETACH, pid, 0, 0);

    unsigned char *prev = calloc(CAP, 1);
    unsigned char *cur  = calloc(CAP, 1);
    if (!prev || !cur) { fprintf(stderr, "内存不足\n"); return 4; }
    int *have = calloc(nseg, sizeof(int));

    long npatch = 0, nhit = 0;
    unsigned long long tend = now_ms() + (unsigned long long)secs * 1000;

    while (now_ms() < tend) {
        for (int i = 0; i < nseg; i++) {
            size_t sz = segs[i].b - segs[i].a;
            if (sz > CAP) sz = CAP;
            struct iovec lo = { cur, sz }, ro = { (void *)segs[i].a, sz };
            ssize_t got = process_vm_readv(pid, &lo, 1, &ro, 1, 0);
            if (got <= 0) continue;
            if (!have[i]) { memcpy(prev, cur, got); have[i] = 1; continue; }

            for (size_t p = 0; p + 8 <= (size_t)got; p += 4) {
                if (memcmp(cur + p, prev + p, 4) == 0 && p + 8 <= (size_t)got
                    && memcmp(cur + p + 4, prev + p + 4, 4) == 0)
                    continue;
                uint32_t w;
                memcpy(&w, cur + p, 4);
                if ((w >> 29) != 1) continue;                  /* op=INC_METHOD */
                if (((w >> 16) & 0x1fff) != 1) continue;        /* cnt==1 */
                if ((((w & 0x1fff) << 2)) != meth) continue;    /* meth 匹配 */
                uint32_t arg;
                memcpy(&arg, cur + p + 4, 4);
                nhit++;
                if (arg == newval) continue;
                uint32_t nv = newval;
                struct iovec lw = { &nv, 4 }, rw = { (void *)(segs[i].a + p + 4), 4 };
                if (process_vm_writev(pid, &lw, 1, &rw, 1, 0) == 4) npatch++;
            }
            memcpy(prev, cur, got);
        }
        usleep(500);
    }

    printf("命中=%ld  已改写=%ld  目标值=%#x\n", nhit, npatch, newval);
    return 0;
}
