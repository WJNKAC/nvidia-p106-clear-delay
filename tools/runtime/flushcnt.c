/* flushcnt —— 统计 XFlush 调用速率，作为"帧率"的相对量度
 *
 * 为什么用它：
 *   primus 的 libGL.so.1 对外只有 XFlush 一个 UND 的 X 调用（objdump -T 可验证），
 *   呈现路径每帧至少触发一次。它的调用速率与帧率同向变化，
 *   足够做 base vs patched 的相对比较（我们不需要绝对 FPS）。
 *
 * 为什么不用 glXSwapBuffers 钩子：
 *   Unigine 运行时 dlopen("libGL.so.1") + dlsym，且走 glvnd dispatch；
 *   实测 LD_PRELOAD 同名符号与 dlsym 拦截都拿不到（报告 160 §7、168）。
 *
 * 输出：进程结束/收到 SIGTERM 时把计数写到文件，并打印速率。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <signal.h>

static volatile long long n_flush = 0;
static volatile long long n_sync = 0;
static double t_start = 0;
static char outpath[256] = "/tmp/flushcnt.txt";

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void dump(void)
{
    double el = now_s() - t_start;
    FILE *f = fopen(outpath, "a");
    if (f) {
        fprintf(f, "XFlush=%lld XSync=%lld elapsed=%.2f rate=%.1f/s\n",
                n_flush, n_sync, el, el > 0 ? n_flush / el : 0);
        fclose(f);
    }
}

static void on_sig(int s) { (void)s; dump(); _exit(0); }

typedef int (*PFN_FLUSH)(void *);
typedef int (*PFN_SYNC)(void *, int);

int XFlush(void *dpy)
{
    static PFN_FLUSH real;
    if (!real) real = (PFN_FLUSH)dlsym(RTLD_NEXT, "XFlush");
    __sync_fetch_and_add(&n_flush, 1);
    if (!real) return 0;
    return real(dpy);
}

int XSync(void *dpy, int discard)
{
    static PFN_SYNC real;
    if (!real) real = (PFN_SYNC)dlsym(RTLD_NEXT, "XSync");
    __sync_fetch_and_add(&n_sync, 1);
    if (!real) return 0;
    return real(dpy, discard);
}

__attribute__((constructor)) static void init(void)
{
    const char *o = getenv("FLUSHCNT_OUT");
    if (o) snprintf(outpath, sizeof outpath, "%s", o);
    t_start = now_s();
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    atexit(dump);
}
