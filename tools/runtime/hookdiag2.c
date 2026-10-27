/* 诊断 + 计数：Unigine 到底通过什么路径呈现每一帧？
 *
 * 目的：
 *   1) 记录 Unigine 通过 dlsym / glXGetProcAddress 查了哪些 GL 相关符号
 *      —— 判定它拿 swap 的真实路径
 *   2) 同时把 X11 的呈现类函数全部挂钩并计数
 *      —— libUnigine 的 NEEDED 里有 libX11.so.6，所以这些是 PLT 直连，LD_PRELOAD 必定能拦到
 *
 * 哪个计数器 ≈ 1/帧，哪个就是"帧边界"，可以直接拿来做帧时间直方图。
 */
#define _GNU_SOURCE
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

/* ---------- 诊断：dlsym / glXGetProcAddress 查询了哪些符号 ---------- */
static void *(*real_dlsym)(void *, const char *);
static void *(*real_gpa)(const unsigned char *);
static int dfd = -1;

static void logline(const char *tag, const char *name)
{
    if (dfd < 0 || !name) return;
    char b[400];
    size_t n = 0;
    while (*tag && n < 32) b[n++] = *tag++;
    b[n++] = ' ';
    for (const char *p = name; *p && n < 380; p++) b[n++] = *p;
    b[n++] = '\n';
    ssize_t r = write(dfd, b, n);
    (void)r;
}

static int is_gl(const char *s)
{
    if (!s) return 0;
    if (strstr(s, "glX")) return 1;
    if (strstr(s, "SwapBuffers")) return 1;
    if (strstr(s, "MakeCurrent")) return 1;
    if (strstr(s, "GetProcAddress")) return 1;
    if (strstr(s, "PutImage")) return 1;
    return 0;
}

static void *my_gpa(const unsigned char *name)
{
    if (name) logline("GPA", (const char *)name);
    if (!real_gpa) return 0;
    return real_gpa(name);
}

void *dlsym(void *h, const char *name)
{
    if (!real_dlsym) return 0;
    void *r = real_dlsym(h, name);
    if (!name || !r) return r;
    if (is_gl(name)) logline("DLSYM", name);
    if (!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")) {
        if (!real_gpa) real_gpa = (void *(*)(const unsigned char *))r;
        logline("HOOKED", name);
        return (void *)my_gpa;
    }
    return r;
}

/* ---------- 计数 + 时间戳 ---------- */
#define MAXTS 200000
static unsigned long long ts[MAXTS];
static volatile int nts = 0;
static volatile long n_swap = 0, n_sync = 0, n_flush = 0, n_put = 0, n_shmput = 0;
static const char *outpath = "/tmp/present_ts.txt";

static unsigned long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}

static void record(void)
{
    int i = nts;
    if (i < MAXTS) { ts[i] = now_ns(); __sync_synchronize(); nts = i + 1; }
}

static void dump(void)
{
    FILE *f = fopen(outpath, "w");
    if (!f) return;
    fprintf(f, "# swap=%ld sync=%ld flush=%ld put=%ld shmput=%ld samples=%d\n",
            n_swap, n_sync, n_flush, n_put, n_shmput, nts);
    int k = nts; if (k > MAXTS) k = MAXTS;
    for (int i = 0; i < k; i++) fprintf(f, "%llu\n", ts[i]);
    fclose(f);
}

static void on_sig(int s) { (void)s; dump(); _exit(0); }

/* ---------- X11 呈现类函数（PLT 直连，可靠拦截） ---------- */
typedef int    (*PFN_PUT)(void *, unsigned long, void *, void *, int, int, int, int, unsigned, unsigned);
typedef int    (*PFN_SHMPUT)(void *, unsigned long, void *, void *, int, int, int, int, unsigned, unsigned, int);
typedef int    (*PFN_SYNC)(void *, int);
typedef int    (*PFN_FLUSH)(void *);

int XPutImage(void *d, unsigned long dr, void *gc, void *img,
              int a, int b, int c, int e, unsigned f, unsigned g)
{
    static PFN_PUT real;
    if (!real) real = (PFN_PUT)dlsym(RTLD_NEXT, "XPutImage");
    n_put++; record();
    if (!real) return 0;
    return real(d, dr, gc, img, a, b, c, e, f, g);
}

int XShmPutImage(void *d, unsigned long dr, void *gc, void *img,
                 int a, int b, int c, int e, unsigned f, unsigned g, int send)
{
    static PFN_SHMPUT real;
    if (!real) real = (PFN_SHMPUT)dlsym(RTLD_NEXT, "XShmPutImage");
    n_shmput++; record();
    if (!real) return 0;
    return real(d, dr, gc, img, a, b, c, e, f, g, send);
}

int XSync(void *d, int discard)
{
    static PFN_SYNC real;
    if (!real) real = (PFN_SYNC)dlsym(RTLD_NEXT, "XSync");
    n_sync++;
    if (!real) return 0;
    return real(d, discard);
}

int XFlush(void *d)
{
    static PFN_FLUSH real;
    if (!real) real = (PFN_FLUSH)dlsym(RTLD_NEXT, "XFlush");
    n_flush++;
    if (!real) return 0;
    return real(d);
}

/* ---------- 如果 swap 是通过 dlsym/gpa 拿的，也接上 ---------- */
static void (*real_swap)(void *, unsigned long);

static void my_swap(void *dpy, unsigned long dr)
{
    n_swap++; record();
    if (real_swap) real_swap(dpy, dr);
}

__attribute__((constructor)) static void init(void)
{
    const char *o = getenv("HOOKDIAG_OUT");
    if (o) outpath = o;
    dfd = open("/tmp/hookdiag.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    atexit(dump);
}
