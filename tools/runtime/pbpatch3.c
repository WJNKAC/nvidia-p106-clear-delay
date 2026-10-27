/* pbpatch3 —— 高命中率版 pushbuffer 实参改写
 *
 * 与 pbpatch2 的区别：上一版每轮全量扫描（28 段 × 1MB），改写率只有 ~7.7%，
 * 因为 GPU 消费速度（每批 118 µs）比扫描一圈还快。
 *
 * 本版策略：
 *   1. 全量扫描只做一次，把所有命中地址记下来（环形缓冲会重复使用同一位置）
 *   2. 之后每轮只做"逐个 8 字节 pread 校验 + pwrite"，代价是"命中数 × 8 字节"，
 *      比扫 28 MB 快三个数量级
 *   3. 校验时确认 header 仍是 (op=1,cnt=1,meth=目标)，不匹配则把该地址从表里删掉并重新全扫
 *
 * 用法: pbpatch3 <pid> <meth_hex> <newvalue_hex> [secs]
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
#define MAXADDR 200000

typedef struct { unsigned long a, b; } seg_t;
static seg_t segs[MAXSEG];
static int nseg = 0;
static unsigned long addrs[MAXADDR];
static int naddr = 0;

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
        segs[nseg].a = a; segs[nseg].b = b; nseg++;
    }
    fclose(f);
}

static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + t.tv_nsec / 1000000ull;
}

/* 全量扫描一遍，收集命中地址 */
static int scan_all(int fd, unsigned meth)
{
    static unsigned char buf[CAP];
    naddr = 0;
    for (int i = 0; i < nseg; i++) {
        size_t sz = segs[i].b - segs[i].a;
        if (sz > CAP) sz = CAP;
        ssize_t got = pread(fd, buf, sz, (off_t)segs[i].a);
        if (got <= 0) continue;
        for (ssize_t p = 0; p + 8 <= got; p += 4) {
            uint32_t w;
            memcpy(&w, buf + p, 4);
            if ((w >> 29) != 1) continue;
            if (((w >> 16) & 0x1fff) != 1) continue;
            if ((((w & 0x1fff) << 2)) != meth) continue;
            if (naddr < MAXADDR) addrs[naddr++] = segs[i].a + p;
        }
    }
    return naddr;
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

    int n0 = scan_all(fd, meth);
    fprintf(stderr, "段=%d 首次扫描命中地址=%d 目标 meth=%#x 新值=%#x\n", nseg, n0, meth, newval);

    long npatch = 0, nverify = 0;
    unsigned long long tend = now_ms() + (unsigned long long)secs * 1000;
    int rescan = 0;

    while (now_ms() < tend) {
        if (naddr == 0) {
            if (rescan++ > 200) break;
            usleep(10000);
            scan_all(fd, meth);
            continue;
        }
        int alive = 0;
        for (int k = 0; k < naddr; k++) {
            unsigned long a = addrs[k];
            uint32_t pair[2];
            if (pread(fd, pair, 8, (off_t)a) != 8) { addrs[alive++] = a; continue; }
            uint32_t w = pair[0];
            /* 校验 header 仍是目标命令 */
            if ((w >> 29) != 1 || ((w >> 16) & 0x1fff) != 1
                || (((w & 0x1fff) << 2)) != meth) {
                continue;   /* 该位置已被别的命令占用，丢弃 */
            }
            nverify++;
            if (pair[1] != newval) {
                uint32_t nv = newval;
                if (pwrite(fd, &nv, 4, (off_t)(a + 4)) == 4) npatch++;
            }
            addrs[alive++] = a;
        }
        naddr = alive;
    }

    printf("校验通过=%ld  已改写=%ld  剩余地址=%d  目标值=%#x\n", nverify, npatch, naddr, newval);
    return 0;
}
