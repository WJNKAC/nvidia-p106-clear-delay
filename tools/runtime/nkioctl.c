/* nkioctl.c — 在「驱动 ↔ 内核」边界上拦截 NVIDIA ioctl（LD_PRELOAD）
 *
 * 为什么这一层比 GL 层垫片好：
 *   1. 对任何应用都生效，不依赖 GL 版本、不需要拦 dlsym（ioctl 是 libc 导出符号）
 *   2. 看到的是驱动真正跟内核说的话（协议层），不是 GL 语义
 *   3. 可以**注入延迟**做因果实验，这在 GL 层做不到（GL 层改不了驱动内部）
 *
 * 用法：
 *   LD_PRELOAD=./nkioctl.so NK_MODE=count   ./app     # 只统计（零风险）
 *   LD_PRELOAD=./nkioctl.so NK_MODE=trace   ./app     # 打印每个 ioctl 的解码
 *   LD_PRELOAD=./nkioctl.so NK_MODE=dedup   ./app     # 对标记为幂等的请求做结果缓存
 *   LD_PRELOAD=./nkioctl.so NK_DELAY=0x4a:1200 ./app  # 给 nr=0x4a 注入 1200µs（因果实验）
 *   kill -USR1 <pid>                                  # 随时吐统计
 *
 * 只处理 type=='F'(0x46) 的 ioctl（NVIDIA 的 NV_IOCTL_MAGIC），其它原样透传。
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>

#define NV_MAGIC 0x46

/* _IOC 解码 */
#define IOC_NRBITS    8
#define IOC_TYPEBITS  8
#define IOC_SIZEBITS  14
#define IOC_DIRBITS   2
#define IOC_NRSHIFT   0
#define IOC_TYPESHIFT (IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT (IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT  (IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_NR(c)   (((c) >> IOC_NRSHIFT)   & ((1u << IOC_NRBITS)   - 1))
#define IOC_TYPE(c) (((c) >> IOC_TYPESHIFT) & ((1u << IOC_TYPEBITS) - 1))
#define IOC_SIZE(c) (((c) >> IOC_SIZESHIFT) & ((1u << IOC_SIZEBITS) - 1))
#define IOC_DIR(c)  (((c) >> IOC_DIRSHIFT)  & ((1u << IOC_DIRBITS)  - 1))

#define MAXNR 256

struct slot {
    unsigned int size, dir;
    unsigned long calls;
    unsigned long long sum_ns, max_ns;
    unsigned long hist[8];      /* <10µs,<50µs,<100µs,<500µs,<1ms,<5ms,<20ms,>=20ms */
    unsigned long long last_in[24];   /* 最近一次 96 字节输入，用于识别重复请求 */
    unsigned long same_in;            /* 与上一次输入完全相同的次数 */
    unsigned long out_changed;        /* 输入相同但输出（+0x14 起 8 字节）变了 */
    unsigned long long last_out;
    int have_last;
};

static struct slot slots[MAXNR];
static int inited;
static int disabled;
static int mode_count, mode_trace, mode_dedup;
static int delay_nr = -1;
static unsigned long long dump_mask;  /* NK_DUMP=0x2a,0x2b : 对这些 nr 做完整 dump */
static int dump_any;                  /* dump_mask 非空 */
static int pdu;                       /* NK_PDUMP=1 : dump 0x2a 的 params 二级结构 */
static int dump_max = 2000;     /* NK_DUMP_MAX 限制条数（按 nr 分别计） */
static int dump_cnt[64];        /* 每个 nr 各自的计数 */
static long delay_us;
static const char *log_path;
static double t_start;
static unsigned long total_calls;

static int (*real_ioctl)(int, unsigned long, ...);
static int dlsym_busy;

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static unsigned long long now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static FILE *outfp(void)
{
    if (log_path) { FILE *f = fopen(log_path, "a"); if (f) return f; }
    return stderr;
}

static void dump(void)
{
    double el = now_s() - t_start;
    FILE *f = outfp();
    fprintf(f, "\n=== nkioctl (%.1fs, %lu calls) ===\n", el, total_calls);
    fprintf(f, "%-6s %-6s %-7s %9s %11s %10s %9s  %s\n",
            "nr", "size", "dir", "calls", "sum_ms", "avg_us", "max_us", "hist(<10 <50 <100 <500 <1m <5m <20m >20m)");
    for (int i = 0; i < MAXNR; i++) {
        if (!slots[i].calls) continue;
        unsigned long long s = slots[i].sum_ns;
        fprintf(f, "0x%-4x %-6u %-7u %9lu %11.2f %10.1f %9.0f  %lu %lu %lu %lu %lu %lu %lu %lu",
                i, slots[i].size, slots[i].dir,
                slots[i].calls, s / 1e6,
                slots[i].calls ? (double)s / slots[i].calls / 1000.0 : 0,
                slots[i].max_ns / 1000.0,
                slots[i].hist[0], slots[i].hist[1], slots[i].hist[2], slots[i].hist[3],
                slots[i].hist[4], slots[i].hist[5], slots[i].hist[6], slots[i].hist[7]);
        if (slots[i].same_in)
            fprintf(f, "   [输入重复 %lu 次, 其中输出变了 %lu 次]", slots[i].same_in, slots[i].out_changed);
        fprintf(f, "\n");
    }
    fflush(f);
    if (f != stderr) fclose(f);
}

static void on_usr1(int s) { (void)s; dump(); }
__attribute__((destructor)) static void fini(void) { if (!disabled && total_calls) dump(); }

static void init(void)
{
    if (inited) {
        if (!real_ioctl) { *(void **)&real_ioctl = dlsym(RTLD_NEXT, "ioctl"); }
        return;
    }
    inited = 1;
    *(void **)&real_ioctl = dlsym(RTLD_NEXT, "ioctl");

    const char *m = getenv("NK_MODE");
    if (!m) m = "count";
    if (!strcmp(m, "off")) disabled = 1;
    else if (!strcmp(m, "trace")) { mode_trace = 1; mode_count = 1; }
    else if (!strcmp(m, "dedup")) { mode_dedup = 1; mode_count = 1; }
    else mode_count = 1;

    const char *d = getenv("NK_DELAY");
    if (d) {
        char *colon = strchr(d, ':');
        if (colon) { delay_nr = (int)strtol(d, NULL, 0); delay_us = strtol(colon + 1, NULL, 0); }
    }
    const char *dn = getenv("NK_DUMP");
    if (dn) {
        char *p = (char *)dn;
        while (*p) {
            char *end;
            long v = strtol(p, &end, 0);
            if (end == p) break;
            if (v >= 0 && v < 64) { dump_mask |= 1ULL << v; dump_any = 1; }
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
    }
    const char *dm = getenv("NK_DUMP_MAX");
    if (dm) dump_max = atoi(dm);
    log_path = getenv("NK_LOG");
    t_start = now_s();
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, NULL); }
}

static void hist_add(struct slot *s, unsigned long long ns)
{
    if (ns < 10000) s->hist[0]++;
    else if (ns < 50000) s->hist[1]++;
    else if (ns < 100000) s->hist[2]++;
    else if (ns < 500000) s->hist[3]++;
    else if (ns < 1000000) s->hist[4]++;
    else if (ns < 5000000) s->hist[5]++;
    else if (ns < 20000000) s->hist[6]++;
    else s->hist[7]++;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    init();
    if (disabled || !real_ioctl) return real_ioctl ? real_ioctl(fd, request, arg) : -1;

    if (IOC_TYPE(request) != NV_MAGIC)          /* 不是 NVIDIA，透传 */
        return real_ioctl(fd, request, arg);

    unsigned int nr = IOC_NR(request);
    unsigned int size = IOC_SIZE(request);
    if (nr >= MAXNR) return real_ioctl(fd, request, arg);

    struct slot *s = &slots[nr];
    s->size = size;
    s->dir  = IOC_DIR(request);

    /* 识别"同一请求重复"：比对输入区（前 96 字节） */
    if (arg && size >= 8 && mode_count) {
        unsigned long long in[12] = {0};
        size_t cp = size < 96 ? size : 96;
        memcpy(in, arg, cp);
        if (s->have_last && !memcmp(in, s->last_in, cp)) s->same_in++;
        memcpy(s->last_in, in, cp);
        s->have_last = 1;
    }

    if (mode_trace)
        fprintf(stderr, "[nkioctl] nr=0x%x size=%u dir=%u arg=%p%s\n",
                nr, size, IOC_DIR(request), arg,
                (delay_nr == (int)nr) ? "  <-- 注入延迟" : "");

    if (delay_nr == (int)nr)
        usleep(delay_us);

    /* ---- dump 模式：把调用前后的参数都记下来 ---- */
    unsigned char before[256];
    size_t dlen = 0;
    int want_dump = dump_any && ((dump_mask >> nr) & 1ULL) && arg && size
                    && dump_cnt[nr & 63] < dump_max;
    if (want_dump) {
        dlen = size < sizeof(before) ? size : sizeof(before);
        memcpy(before, arg, dlen);
    }

    unsigned long long t0 = now_ns();
    int rc = real_ioctl(fd, request, arg);
    unsigned long long d = now_ns() - t0;

    if (want_dump) {
        FILE *f = outfp();
        dump_cnt[nr & 63]++;
        unsigned int u[16] = {0};
        unsigned long long pq = 0;
        for (size_t i = 0; i + 4 <= dlen && i / 4 < 16; i += 4)
            memcpy(&u[i / 4], before + i, 4);
        if (dlen >= 0x18) memcpy(&pq, before + 0x10, 8);

        /* 结构化摘要行：便于跨版本对齐比较（脚本只读这一行） */
        if (nr == 0x2a && dlen >= 0x20) {
            fprintf(f, "CTRL  #%-3d hClient=%08x hObject=%08x cmd=%08x flags=%08x "
                       "params=%llx psize=%u status=%08x rc=%d us=%.1f\n",
                    dump_cnt[nr & 63], u[0], u[1], u[2], u[3], pq, u[6], u[7], rc, d / 1000.0);
        } else if (nr == 0x2b && dlen >= 0x18) {
            unsigned int hnew_after = u[2];
            if (dlen > 8) memcpy(&hnew_after, (char *)arg + 8, 4);
            fprintf(f, "ALLOC #%-3d hRoot=%08x hParent=%08x hNew=%08x hNewOut=%08x "
                       "hClass=%08x parms=%llx rc=%d us=%.1f\n",
                    dump_cnt[nr & 63], u[0], u[1], u[2], hnew_after, u[3], pq, rc, d / 1000.0);
        }

        fprintf(f, "[DUMP #%d] nr=0x%x size=%u rc=%d us=%.1f\n", dump_cnt[nr & 63], nr, size, rc, d / 1000.0);
        fprintf(f, "  BEFORE:");
        for (size_t i = 0; i < dlen; i++) {
            if (i % 16 == 0) fprintf(f, "\n   +%03zx:", i);
            fprintf(f, " %02x", before[i]);
        }
        fprintf(f, "\n  AFTER :");
        for (size_t i = 0; i < dlen; i++) {
            if (i % 16 == 0) fprintf(f, "\n   +%03zx:", i);
            fprintf(f, " %02x", ((unsigned char *)arg)[i]);
        }
        fprintf(f, "\n  u32   :");
        for (size_t i = 0; i + 4 <= dlen; i += 4) {
            unsigned int v; memcpy(&v, before + i, 4);
            fprintf(f, " [%02zx]=%08x", i, v);
            if ((i/4) % 4 == 3) fprintf(f, "\n          ");
        }
        /* 二级指针：NVOS54.params / NVOS21.pAllocParms 指向的结构 */
        if (dlen >= 0x18 && pq > 0x10000 && pq < 0x800000000000ULL
            && (nr != 0x2a || pdu)) {
            int n = (nr == 0x2a) ? 48 : 192;
            fprintf(f, "\n  PARMS @%llx:", pq);
            const unsigned char *q = (const unsigned char *)pq;
            for (int i = 0; i < n; i++) {
                if (i % 16 == 0) fprintf(f, "\n   +%03x:", i);
                fprintf(f, " %02x", q[i]);
            }
        }
        fprintf(f, "\n");
        fflush(f);
        if (f != stderr) fclose(f);
    }

    s->calls++;
    s->sum_ns += d;
    if (d > s->max_ns) s->max_ns = d;
    hist_add(s, d);
    total_calls++;

    /* 输入相同但输出变了 —— 说明这不是幂等查询，不能缓存 */
    if (arg && size >= 0x20) {
        unsigned long long o = 0;
        memcpy(&o, (char *)arg + 0x14, 8);
        if (s->same_in && o != s->last_out) s->out_changed++;
        s->last_out = o;
    }
    return rc;
}
