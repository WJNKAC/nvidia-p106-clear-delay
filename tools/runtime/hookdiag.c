/* 诊断：Unigine 到底通过什么路径拿到 swap / proc address */
#define _GNU_SOURCE
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

static void *(*real_dlsym)(void*, const char*);
static void *(*real_gpa)(const unsigned char*);
static int fd = -1;

static void logline(const char *tag, const char *name){
    if(fd < 0 || !name) return;
    char b[320]; size_t n = 0;
    while(*tag && n < 40) b[n++] = *tag++;
    b[n++] =  ;
    for(const char *p = name; *p && n < 300; p++) b[n++] = *p;
    b[n++] = n;
    ssize_t r = write(fd, b, n); (void)r;
}
static int is_gl(const char *s){
    if(!s) return 0;
    if(strstr(s, "glX")) return 1;
    if(strstr(s, "SwapBuffers")) return 1;
    if(strstr(s, "MakeCurrent")) return 1;
    if(strstr(s, "GetProcAddress")) return 1;
    return 0;
}
static void *my_gpa(const unsigned char *name){
    if(name) logline("GPA", (const char*)name);
    if(!real_gpa) return 0;
    return real_gpa(name);
}
void *dlsym(void *h, const char *name){
    if(!real_dlsym) return 0;
    void *r = real_dlsym(h, name);
    if(!name || !r) return r;
    if(is_gl(name)) logline("DLSYM", name);
    if(!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")){
        if(!real_gpa) real_gpa = (void*(*)(const unsigned char*))r;
        logline("HOOKED", name);
        return (void*)my_gpa;
    }
    return r;
}
__attribute__((constructor)) static void init(void){
    fd = open("/tmp/hookdiag.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    real_dlsym = (void*(*)(void*,const char*))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
}
