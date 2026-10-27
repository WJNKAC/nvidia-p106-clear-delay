/* pbpatch2 —— 用 /proc/pid/mem 改写 pushbuffer 里指定命令的实参
 *
 * 与 pbpatch 的区别：上一个版本用 process_vm_readv/writev，在部分映射段上会返回
 * EFAULT（段首未映射），导致整个段被跳过、命中数恒为 0。
 * 这里改用 pread/pwrite(/proc/pid/mem)，它逐页处理，遇未映射页只影响该页。
 *
 * 用法: pbpatch2 <pid> <meth_hex> <newvalue_hex> [secs] [interval_us]
 *
 * 匹配规则（与 pbraw.py 观测一致）:
 *   op == 1 (INC_METHOD), cnt == 1, meth == 目标
 *   命中后把下一个 dword 改成 newvalue
 *
 * 安全性: 只改实参值，不改命令结构/长度。
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

static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + t.tv_nsec / 1000000ull;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <pid> <meth_hex> <newvalue_hex> [secs] [interval_us]\n", argv[0]);
        return 1;
    }
    pid_t pid = atoi(argv[1]);
    unsigned meth = (unsigned)strtoul(argv[2], NULL, 16);
    unsigned newval = (unsigned)strtoul(argv[3], NULL, 16);
    int secs = (argc > 4) ? atoi(argv[4]) : 10;
    int ivl  = (argc > 5) ? atoi(argv[5]) : 300;

    load_segs(pid);
    fprintf(stderr, "pid=%d 可写 nvidia 段=%d 目标 meth=%#x 新值=%#x\n", pid, nseg, meth, newval);
    if (!nseg) return 2;

    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/mem", pid);
    int fd = open(mp, O_RDWR);
    if (fd < 0) {
        /* 没有 ptrace 关系时该文件不可写，attach 一次再 detach 即可建立关系 */
        if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) { perror("ATTACH"); return 3; }
        int st; waitpid(pid, &st, 0);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        fd = open(mp, O_RDWR);
        if (fd < 0) { perror("open mem"); return 4; }
    }

    unsigned char *prev = calloc(CAP, 1);
    unsigned char *cur  = calloc(CAP, 1);
    int *have = calloc(nseg, sizeof(int));
    if (!prev || !cur || !have) { fprintf(stderr, "内存不足\n"); return 5; }

    long npatch = 0, nhit = 0, nseg_ok = 0;
    unsigned long long tend = now_ms() + (unsigned long long)secs * 1000;

    while (now_ms() < tend) {
        for (int i = 0; i < nseg; i++) {
            size_t sz = segs[i].b - segs[i].a;
            if (sz > CAP) sz = CAP;
            ssize_t got = pread(fd, cur, sz, (off_t)segs[i].a);
            if (got <= 0) continue;
            if (!have[i]) { memcpy(prev, cur, got); have[i] = 1; nseg_ok++; continue; }

            for (size_t p = 0; p + 8 <= (size_t)got; p += 4) {
                uint32_t w;
                memcpy(&w, cur + p, 4);
                if ((w >> 29) != 1) continue;
                if (((w >> 16) & 0x1fff) != 1) continue;
                if ((((w & 0x1fff) << 2)) != meth) continue;
                uint32_t arg;
                memcpy(&arg, cur + p + 4, 4);
                nhit++;
                if (arg == newval) continue;
                uint32_t nv = newval;
                if (pwrite(fd, &nv, 4, (off_t)(segs[i].a + p + 4)) == 4) {
                    npatch++;
                    /* 同步更新本地副本，避免同一轮重复计数 */
                    memcpy(cur + p + 4, &nv, 4);
                }
            }
            memcpy(prev, cur, got);
        }
        usleep(ivl);
    }

    fprintf(stderr, "已建立基线段=%ld\n", nseg_ok);
    printf("命中=%ld  已改写=%ld  目标值=%#x\n", nhit, npatch, newval);
    return 0;
}
