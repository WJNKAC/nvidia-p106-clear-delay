/* patchpb.so —— 对 libnvidia-glcore 打任意字节补丁（运行时，不改文件）
 *
 * 用法：PATCH="0x123874a=4839c090;0x1238773=4839c090" LD_PRELOAD=./patchpb.so ./n83b ...
 *   - 每项 "ELF虚拟地址=十六进制字节串"，十六进制不加 0x，长度任意
 *   - 地址就是 objdump 显示的地址（glcore 的第一个 LOAD 段 vaddr 为 0，故 vaddr==文件偏移）
 *
 * 设计要点：
 *   1. glcore 是 glX 首次调用时才 dlopen 的 → constructor 阶段不存在，
 *      必须起后台线程 dl_iterate_phdr 轮询
 *   2. 每次改写都打印"原字节 → 新字节"，作为补丁真的落地的证据
 *   3. 支持把原字节打印出来，便于人工核对是否改在了正确位置
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

#define MAXP 32
struct patch { uintptr_t off; unsigned char bytes[32]; int len; unsigned char old[32]; int done; };
static struct patch pats[MAXP];
static int npats;
static volatile int found;
static uintptr_t base;

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

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse(void)
{
    const char *e = getenv("PATCH");
    if (!e) return;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = 0;
    for (char *tok = strtok_r(buf, ";", &save); tok && npats < MAXP;
         tok = strtok_r(0, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        struct patch *p = &pats[npats];
        p->off = strtoull(tok, 0, 0);
        const char *h = eq + 1;
        p->len = 0;
        while (h[0] && h[1] && p->len < (int)sizeof p->bytes) {
            int a = hexval(h[0]), b = hexval(h[1]);
            if (a < 0 || b < 0) break;
            p->bytes[p->len++] = (unsigned char)(a * 16 + b);
            h += 2;
        }
        if (p->len > 0) npats++;
    }
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1200; i++) {          /* 最多等 60 s */
        dl_iterate_phdr(cb, 0);
        if (found) break;
        usleep(50000);
    }
    if (!found) { fprintf(stderr, "[patchpb] 60s 内没等到 glcore，未打补丁\n"); return 0; }
    if (!npats) { fprintf(stderr, "[patchpb] 没有 PATCH 环境变量\n"); return 0; }

    long ps = sysconf(_SC_PAGESIZE);
    fprintf(stderr, "[patchpb] glcore base=%#lx  共 %d 处补丁\n", (unsigned long)base, npats);

    for (int i = 0; i < npats; i++) {
        struct patch *p = &pats[i];
        uintptr_t a = base + p->off;
        uintptr_t pg = a & ~((uintptr_t)ps - 1);
        uintptr_t endpg = (a + p->len - 1) & ~((uintptr_t)ps - 1);
        size_t span = (size_t)(endpg - pg + ps);
        if (mprotect((void *)pg, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            perror("[patchpb] mprotect"); continue;
        }
        unsigned char *q = (unsigned char *)a;
        memcpy(p->old, q, p->len);
        memcpy(q, p->bytes, p->len);
        __builtin___clear_cache((char *)pg, (char *)(pg + span));

        fprintf(stderr, "[patchpb] %#llx  原文 ", (unsigned long long)p->off);
        for (int k = 0; k < p->len; k++) fprintf(stderr, "%02x", p->old[k]);
        fprintf(stderr, "  ->  ");
        for (int k = 0; k < p->len; k++) fprintf(stderr, "%02x", p->bytes[k]);
        fprintf(stderr, "   %s\n", memcmp(p->old, p->bytes, p->len) ? "[已改写]" : "[本来相同]");
        p->done = 1;
    }
    return 0;
}

__attribute__((constructor)) static void init(void)
{
    parse();
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) == 0) pthread_detach(t);
}
