#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#define MAXN 300000
static unsigned long long pre[MAXN], post[MAXN];
static volatile int n = 0;
static const char *out;

static unsigned long long now_ns(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec*1000000000ull + (unsigned long long)t.tv_nsec;
}
static void dump(void){
    if(!out) return;
    FILE *f = fopen(out, "w");
    if(!f) return;
    int k = n; if(k > MAXN) k = MAXN;
    for(int i=0;i<k;i++) fprintf(f, "%llu %llu\n", pre[i], post[i]);
    fclose(f);
}
static void on_sig(int s){ (void)s; dump(); _exit(0); }

__attribute__((constructor)) static void init(void){
    out = getenv("SWAPTS_OUT");
    if(!out) out = "/tmp/swapts.txt";
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGUSR1, on_sig);
    atexit(dump);
}

void glXSwapBuffers(void *dpy, unsigned long drawable){
    static void (*real)(void*, unsigned long) = 0;
    if(!real){ real = (void(*)(void*,unsigned long))dlsym(RTLD_NEXT, "glXSwapBuffers"); if(!real) return; }
    int i = n;
    unsigned long long a = now_ns();
    real(dpy, drawable);
    unsigned long long b = now_ns();
    if(i < MAXN){ pre[i] = a; post[i] = b; __sync_synchronize(); n = i+1; }
}
