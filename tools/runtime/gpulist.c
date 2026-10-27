/* gpulist.c —— 拦截 glXGetProcAddress，记录程序请求过的 GL 函数名
 * 目的：判断目标程序是否使用 legacy 固定管线 API
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#define MAXN 4096
static char names[MAXN][64];
static unsigned long cnt[MAXN];
static int nn;

static void record(const char *s)
{
    if (!s) return;
    for (int i = 0; i < nn; i++) if (!strcmp(names[i], s)) { cnt[i]++; return; }
    if (nn < MAXN) { snprintf(names[nn], 64, "%s", s); cnt[nn] = 1; nn++; }
}

static void dump(void)
{
    const char *p = getenv("GL_LOG");
    FILE *f = fopen(p ? p : "/tmp/gllist.log", "w");
    if (!f) return;
    /* 只输出 legacy 固定管线相关的 */
    const char *kw[] = {"Color","Translate","Rotate","Scale","LoadIdentity","MatrixMode",
                        "PushMatrix","PopMatrix","Begin","End","Vertex2","Vertex3",
                        "Normal","Material","Light","Frustum","Ortho","ClientState",
                        "Uniform","Attrib","UseProgram", NULL};
    for (int i = 0; i < nn; i++) {
        int hit = 0;
        for (int k = 0; kw[k]; k++) if (strstr(names[i], kw[k])) { hit = 1; break; }
        if (hit) fprintf(f, "%-28s %lu\n", names[i], cnt[i]);
    }
    fprintf(f, "--- 共记录 %d 个不同函数名 ---\n", nn);
    fclose(f);
}
static void on_sig(int s) { (void)s; dump(); _exit(0); }
__attribute__((constructor)) static void init(void)
{
    int sg[] = {SIGTERM, SIGINT, SIGALRM};
    for (int i = 0; i < 3; i++) { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_sig; sigaction(sg[i],&sa,NULL); }
}
__attribute__((destructor)) static void fini(void) { dump(); }

void *glXGetProcAddress(const unsigned char *n)
{
    static void *(*real)(const unsigned char *);
    if (!real) real = dlsym(RTLD_NEXT, "glXGetProcAddress");
    record((const char *)n);
    return real ? real(n) : NULL;
}
void *glXGetProcAddressARB(const unsigned char *n)
{
    static void *(*real)(const unsigned char *);
    if (!real) real = dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    record((const char *)n);
    return real ? real(n) : NULL;
}
