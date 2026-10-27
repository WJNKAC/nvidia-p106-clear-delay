/* ffcount.c —— 统计 legacy 固定管线 API 的调用次数
 * 用法: LD_PRELOAD=./ffcount.so <程序>
 * 退出时打印每个被拦截函数的调用次数
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define N(n) static unsigned long n##_n; \
    static void *n##_f; \
    static void ensure_##n(void){ if(!n##_f) n##_f = dlsym(RTLD_NEXT, #n); }

N(glColor3f) N(glColor4f) N(glColor3fv) N(glColor4fv)
N(glTranslatef) N(glTranslated) N(glRotatef) N(glScalef)
N(glLoadIdentity) N(glLoadMatrixf) N(glMultMatrixf)
N(glMatrixMode) N(glPushMatrix) N(glPopMatrix)
N(glBegin) N(glEnd) N(glVertex2f) N(glVertex3f)
N(glEnableClientState) N(glDisableClientState) N(glVertexPointer) N(glColorPointer)
N(glTexCoord2f) N(glNormal3f) N(glMaterialfv) N(glLightfv)
N(glFrustum) N(glOrtho) N(glVertexAttribPointer) N(glUseProgram)

void glColor3f(float a,float b,float c){ glColor3f_n++; ensure_glColor3f(); ((void(*)(float,float,float))glColor3f_f)(a,b,c); }
void glColor4f(float a,float b,float c,float d){ glColor4f_n++; ensure_glColor4f(); ((void(*)(float,float,float,float))glColor4f_f)(a,b,c,d); }
void glTranslatef(float a,float b,float c){ glTranslatef_n++; ensure_glTranslatef(); ((void(*)(float,float,float))glTranslatef_f)(a,b,c); }
void glRotatef(float a,float b,float c,float d){ glRotatef_n++; ensure_glRotatef(); ((void(*)(float,float,float,float))glRotatef_f)(a,b,c,d); }
void glScalef(float a,float b,float c){ glScalef_n++; ensure_glScalef(); ((void(*)(float,float,float))glScalef_f)(a,b,c); }
void glLoadIdentity(void){ glLoadIdentity_n++; ensure_glLoadIdentity(); ((void(*)(void))glLoadIdentity_f)(); }
void glMatrixMode(unsigned int m){ glMatrixMode_n++; ensure_glMatrixMode(); ((void(*)(unsigned int))glMatrixMode_f)(m); }
void glPushMatrix(void){ glPushMatrix_n++; ensure_glPushMatrix(); ((void(*)(void))glPushMatrix_f)(); }
void glPopMatrix(void){ glPopMatrix_n++; ensure_glPopMatrix(); ((void(*)(void))glPopMatrix_f)(); }
void glBegin(unsigned int m){ glBegin_n++; ensure_glBegin(); ((void(*)(unsigned int))glBegin_f)(m); }
void glEnd(void){ glEnd_n++; ensure_glEnd(); ((void(*)(void))glEnd_f)(); }
void glVertex2f(float a,float b){ glVertex2f_n++; ensure_glVertex2f(); ((void(*)(float,float))glVertex2f_f)(a,b); }
void glVertex3f(float a,float b,float c){ glVertex3f_n++; ensure_glVertex3f(); ((void(*)(float,float,float))glVertex3f_f)(a,b,c); }
void glEnableClientState(unsigned int c){ glEnableClientState_n++; ensure_glEnableClientState(); ((void(*)(unsigned int))glEnableClientState_f)(c); }
void glVertexPointer(int a,int b,unsigned int c,const void*d){ glVertexPointer_n++; ensure_glVertexPointer(); ((void(*)(int,int,unsigned int,const void*))glVertexPointer_f)(a,b,c,d); }
void glVertexAttribPointer(unsigned int i,int a,int b,unsigned char c,int d,const void*e){ glVertexAttribPointer_n++; ensure_glVertexAttribPointer(); ((void(*)(unsigned int,int,int,unsigned char,int,const void*))glVertexAttribPointer_f)(i,a,b,c,d,e); }
void glUseProgram(unsigned int p){ glUseProgram_n++; ensure_glUseProgram(); ((void(*)(unsigned int))glUseProgram_f)(p); }
void glNormal3f(float a,float b,float c){ glNormal3f_n++; ensure_glNormal3f(); ((void(*)(float,float,float))glNormal3f_f)(a,b,c); }
void glMaterialfv(unsigned int f,unsigned int p,const float*v){ glMaterialfv_n++; ensure_glMaterialfv(); ((void(*)(unsigned int,unsigned int,const float*))glMaterialfv_f)(f,p,v); }

static void dump(void);

/* 收到 SIGTERM/SIGINT/SIGALRM 时先落盘再退出，保证被 kill 也能拿到数据 */
static void on_sig(int sig) { (void)sig; dump(); _exit(0); }

__attribute__((constructor)) static void setup_sig(void)
{
    for (int i = 0; i < 3; i++) {
        int sg[] = {SIGTERM, SIGINT, SIGALRM};
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sig; sigaction(sg[i], &sa, NULL);
    }
}

__attribute__((destructor)) static void fini(void) { dump(); }

static void dump(void)
{
    FILE *f = fopen(getenv("FF_LOG") ? getenv("FF_LOG") : "/tmp/ffcount.log", "w");
    if (!f) return;
#define P(n) if (n##_n) fprintf(f, "%-24s %lu\n", #n, n##_n);
    P(glColor3f) P(glColor4f) P(glTranslatef) P(glRotatef) P(glScalef)
    P(glLoadIdentity) P(glMatrixMode) P(glPushMatrix) P(glPopMatrix)
    P(glBegin) P(glEnd) P(glVertex2f) P(glVertex3f)
    P(glEnableClientState) P(glVertexPointer) P(glVertexAttribPointer)
    P(glUseProgram) P(glNormal3f) P(glMaterialfv)
    fprintf(f, "--- end ---\n");
    fclose(f);
}
