/* Unigine 不链接 libGL，运行时 dlopen + dlsym => 必须拦 dlsym 才能钩到 swap */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#define MAXN 400000
static unsigned long long pre[MAXN], post[MAXN];
static volatile int n = 0;
static char outbuf[256];

static unsigned long long now_ns(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec*1000000000ull + (unsigned long long)t.tv_nsec;
}
static void dump(void){
    FILE *f = fopen(outbuf, "w");
    if(!f) return;
    int k = n; if(k > MAXN) k = MAXN;
    for(int i=0;i<k;i++) fprintf(f, "%llu %llu\n", pre[i], post[i]);
    fclose(f);
}
static void on_sig(int s){ (void)s; dump(); _exit(0); }

static void (*real_swap)(void*, unsigned long) = 0;
static void *(*real_gpa)(const unsigned char*) = 0;
static void *(*real_dlsym)(void*, const char*) = 0;

static void my_swap(void *dpy, unsigned long d){
    int i = n;
    unsigned long long a = now_ns();
    if(real_swap) real_swap(dpy, d);
    unsigned long long b = now_ns();
    if(i < MAXN){ pre[i]=a; post[i]=b; __sync_synchronize(); n = i+1; }
}
static void *my_gpa(const unsigned char *name){
    if(name && !strcmp((const char*)name, "glXSwapBuffers")) return (void*)my_swap;
    if(!real_gpa) return 0;
    return real_gpa(name);
}

void *dlsym(void *h, const char *name){
    if(!real_dlsym) return 0;
    void *r = real_dlsym(h, name);
    if(!name || !r) return r;
    if(!strcmp(name, "glXSwapBuffers")){
        if(!real_swap) real_swap = (void(*)(void*,unsigned long))r;
        return (void*)my_swap;
    }
    if(!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")){
        if(!real_gpa) real_gpa = (void*(*)(const unsigned char*))r;
        return (void*)my_gpa;
    }
    return r;
}

__attribute__((constructor)) static void init(void){
    const char *o = getenv("SWAPTS_OUT");
    snprintf(outbuf, sizeof outbuf, "%s", o ? o : "/tmp/swapts.txt");
    real_dlsym = (void*(*)(void*,const char*))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGUSR1, on_sig);
    atexit(dump);
}
