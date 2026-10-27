/* nodelay.c —— 把 525 glcore 里"20µs 自旋等待"的时限改成 0
 *
 * 目标指令(525.116.03, ELF vaddr 0x15761e6):
 *     4c 8d 60 14     lea r12,[rax+0x14]      ; deadline = now + 20(µs)
 * 把它改成 4c 8d 60 00 → deadline = now，循环立刻退出，等于去掉这次退避。
 *
 * 为什么要后台线程：glcore 是 glX 首次调用时才 dlopen 的，
 * constructor 阶段还不存在。所以起一个线程轮询 dl_iterate_phdr。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <link.h>
#include <pthread.h>

static volatile int found;
static uintptr_t base;
static unsigned long long off = 0x15761e9ULL;   /* imm8 所在位置 */
static int val = 0;

static int cb(struct dl_phdr_info *info, size_t sz, void *d)
{
    (void)sz; (void)d;
    if (info->dlpi_name && strstr(info->dlpi_name, "libnvidia-glcore")) {
        base = (uintptr_t)info->dlpi_addr;
        found = 1;
        return 1;
    }
    return 0;
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 600; i++) {          /* 最多等 30 s */
        dl_iterate_phdr(cb, 0);
        if (found) break;
        usleep(50000);
    }
    if (!found) { fprintf(stderr, "[nodelay] 30s 内没等到 glcore，未打补钉\n"); return 0; }

    uintptr_t a = base + off;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t pg = a & ~((uintptr_t)ps - 1);
    if (mprotect((void *)pg, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("[nodelay] mprotect");
        return 0;
    }
    unsigned char *p = (unsigned char *)a;
    fprintf(stderr, "[nodelay] base=%#lx off=%#llx addr=%p  上下文: %02x %02x %02x | %02x\n",
            (unsigned long)base, off, (void *)a, p[-3], p[-2], p[-1], p[0]);
    unsigned char before = p[0];
    p[0] = (unsigned char)val;
    __builtin___clear_cache((char *)pg, (char *)(pg + ps));
    fprintf(stderr, "[nodelay] 已写入：%02x -> %02x\n", before, p[0]);
    return 0;
}

__attribute__((constructor)) static void init(void)
{
    const char *o = getenv("ND_OFF");
    if (o) off = strtoull(o, 0, 0);
    const char *v = getenv("ND_VAL");
    if (v) val = atoi(v);
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
}
