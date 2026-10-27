/* glbatch.c — OpenGL 状态绑定去重 / 合并垫片（LD_PRELOAD）
 *
 * 目的：本项目的实测结论是「每做一次【绑定对象 → draw】，525 驱动多花 122 µs，
 *       而单独绑定 1000 次（不跟 draw）是免费的」。所以垫片要做的不是"合并批次"
 *       这种大动作，而是**减少【绑定 → draw】这个转换发生的次数**。
 *
 * 三种模式（环境变量 GLBATCH_MODE）：
 *   count   只统计、不改行为 —— 用来量"潜力上限"（多少次绑定、多少次是冗余的、每次 draw 配几次绑定）
 *   dedupe  跳过冗余绑定（绑定值与当前已绑定值相同的调用直接吞掉）。
 *           **语义上等价于什么都没做**（把同一个对象再绑一次本来就是 no-op），
 *           因此这是唯一"零风险"的模式，默认启用。
 *   reorder 把 program / 纹理绑定延迟到 draw 前一次性下发（同一次 draw 前的多个绑定合并成一组）。
 *           当且仅当"两次 draw 之间发生了多次互相覆盖的绑定"时才有收益。
 *           ⚠ 延迟下发对"绑定后立刻修改该对象"的写法（bind → glTexImage2D）不成立，
 *           所以 reorder 模式下所有已知的"修改/查询"入口都会先 flush。
 *
 * 统计（GLBATCH_STATS=1 或 GLBATCH_LOG=<file>）：
 *   每个 API 的调用次数、被吞掉的次数、flush 次数、swap 次数，
 *   以及"每次 draw 平均配几次绑定"——这个比值就是垫片能压缩掉多少空间的直接度量。
 *
 * 构建：gcc -O2 -fPIC -shared -o glbatch.so glbatch.c -ldl
 * 使用：LD_PRELOAD=/path/glbatch.so GLBATCH_MODE=dedupe ./app
 *
 * 注意：
 *  - 只跟踪单线程单上下文（GL 的正常用法）。glXMakeCurrent 会清空缓存强制重下发。
 *  - 不认识的 GL 调用看不见，所以 reorder 模式对"未拦截的调用会观察到旧状态"这件事
 *    无法完全防御；已知的修改/查询入口都已拦截并 flush。
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <time.h>
#include <execinfo.h>

/* ---- 自带最小 GL 类型，避免依赖头文件版本 ---- */
typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float         GLfloat;
typedef ptrdiff_t     GLsizeiptr;
typedef ptrdiff_t     GLintptr;
typedef char          GLchar;
typedef double        GLdouble;
typedef unsigned int  GLbitfield;

#define GL_TEXTURE_2D       0x0DE1
#define GL_TEXTURE_3D       0x806F
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_1D       0x0DE0
#define GL_TEXTURE_RECT     0x84F5
#define GL_FRAMEBUFFER      0x8D40
#define GL_ARRAY_BUFFER     0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_TEXTURE0         0x84C0
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D

/* ---------- 真实函数指针 ---------- */
static void *(*real_ggpa)(const GLubyte *) = NULL;
static void *(*real_ggpaa)(const GLubyte *) = NULL;

#define MAXAPI 96
struct apicnt { const char *name; unsigned long calls, skipped; };

static struct apicnt api[MAXAPI];
static int napis = 0;


/* 调试用：崩溃时打印调用栈（GLBATCH_BT=1 才启用） */
static void segv(int sig)
{
    void *bt[40];
    int n = backtrace(bt, 40);
    fprintf(stderr, "\n!! glbatch: 捕获信号 %d，调用栈 %d 层\n", sig, n);
    backtrace_symbols_fd(bt, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

static int  mode_count = 0, mode_dedupe = 0, mode_reorder = 0;
static int  stats_on = 0;
static const char *log_path = NULL;
static int  inited = 0;
static int  disabled = 0;
static int  min_mode = 0;
static FILE *trace_fp = NULL;
static long trace_n = 0;

static unsigned long n_swap = 0, n_draw = 0, n_flush = 0, n_bind = 0;
static unsigned long n_elide = 0, n_deliver = 0, n_group2plus = 0;

static double t_start = 0.0;
static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* kill -USR1 <pid> 随时吐一份累计统计（用来算 fps = Δswaps/Δt） */
static void on_usr1(int sig)
{
    (void)sig;
    double el = now_s() - t_start;
    FILE *f = log_path ? fopen(log_path, "a") : stderr;
    if (!f) f = stderr;
    fprintf(f, "[usr1] t=%.2fs swaps=%lu draws=%lu binds=%lu elided=%lu deliver=%lu flush=%lu fps=%.2f\n",
            el, n_swap, n_draw, n_bind, n_elide, n_deliver, n_flush,
            el > 0 ? (double)n_swap / el : 0.0);
    fflush(f);
    if (f != stderr) fclose(f);
}

static int idx_of(const char *name)
{
    for (int i = 0; i < napis; i++)
        if (api[i].name == name) return i;         /* 指针相等：名字都是字面量 */
    if (napis >= MAXAPI) return -1;
    api[napis].name = name; api[napis].calls = 0; api[napis].skipped = 0;
    return napis++;
}

/* ---------- 状态槽 ---------- */
enum {
    S_PROG = 0,
    S_FBO,
    S_RFBO,
    S_VAO,
    S_ARRAYBUF,
    S_ELEMBUF,
    S_PIPELINE,
    S_PROGBUF,
    S_UBO,
    S_TEXBASE,                   /* 之后是每 (unit,target) 一个槽 */
    S_MAX = S_TEXBASE + 8 * 6
};

#define NUNIT 8
#define NTGT  6

static const GLenum tgt_of[NTGT] = {
    GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY,
    GL_TEXTURE_CUBE_MAP, GL_TEXTURE_1D, GL_TEXTURE_RECT
};

static GLuint cur[S_MAX];        /* 应用"期望"的状态 */
static GLuint drv[S_MAX];        /* 已经下发给驱动的状态 */
static int    pend[S_MAX];       /* reorder 模式：待下发 */
static int    npend = 0;
static GLuint active_unit = 0;

#define INVALID 0xFFFFFFFFu
static void state_reset(void)
{
    for (int i = 0; i < S_MAX; i++) { cur[i] = INVALID; drv[i] = INVALID; pend[i] = 0; }
    npend = 0;
    active_unit = 0;
}

static int tgt_slot(GLenum target)
{
    int t = -1;
    for (int i = 0; i < NTGT; i++) if (tgt_of[i] == target) { t = i; break; }
    if (t < 0) return -1;
    int u = (active_unit < NUNIT) ? (int)active_unit : NUNIT - 1;
    return S_TEXBASE + u * NTGT + t;
}

static void bind_real(const char *name, GLuint v);
static void *wrap_lookup(const char *n);

/* ---------- 懒加载 ---------- */
/* 变量名 r_UseProgram → 真实符号 glUseProgram；glX* 三个特例单独处理 */
static void *ld_sym(const char *var)
{
    const char *n = var + 2;                 /* 跳过 "r_" */
    char buf[128];
    if (!strcmp(n, "SwapBuffers"))                snprintf(buf, sizeof buf, "glXSwapBuffers");
    else if (!strcmp(n, "MakeCurrent"))           snprintf(buf, sizeof buf, "glXMakeCurrent");
    else if (!strcmp(n, "MakeCurrentAttribs"))    snprintf(buf, sizeof buf, "glXMakeCurrentReadSGI");
    else                                          snprintf(buf, sizeof buf, "gl%s", n);
    return dlsym(RTLD_NEXT, buf);
}
#define LOAD(fn) do { if (!(fn)) *(void **)(&(fn)) = ld_sym(#fn); } while (0)

static void (*r_UseProgram)(GLuint);
static void (*r_BindTexture)(GLenum, GLuint);
static void (*r_ActiveTexture)(GLenum);
static void (*r_BindFramebuffer)(GLenum, GLuint);
static void (*r_BindRenderbuffer)(GLenum, GLuint);
static void (*r_BindVertexArray)(GLuint);
static void (*r_BindBuffer)(GLenum, GLuint);
static void (*r_BindBufferBase)(GLenum, GLuint, GLuint);
static void (*r_BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static void (*r_BindSampler)(GLuint, GLuint);
static void (*r_BindProgramPipeline)(GLuint);
static void (*r_DrawArrays)(GLenum, GLint, GLsizei);
static void (*r_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*r_DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
static void (*r_DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static void (*r_DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void (*r_MultiDrawArrays)(GLenum, const GLint *, const GLsizei *, GLsizei);
static void (*r_MultiDrawElements)(GLenum, const GLsizei *, GLenum, const void *const *, GLsizei);
static void (*r_Flush)(void);
static void (*r_Finish)(void);
static void (*r_Clear)(GLbitfield);
static void (*r_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*r_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*r_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*r_TexParameteri)(GLenum, GLenum, GLint);
static void (*r_GenerateMipmap)(GLenum);
static void (*r_BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*r_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void (*r_Uniform1f)(GLint, GLfloat);
static void (*r_Uniform2f)(GLint, GLfloat, GLfloat);
static void (*r_Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
static void (*r_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void (*r_Uniform1i)(GLint, GLint);
static void (*r_UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
static void (*r_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*r_DrawBuffers)(GLsizei, const GLenum *);
static void (*r_LinkProgram)(GLuint);
static void (*r_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void (*r_Scissor)(GLint, GLint, GLsizei, GLsizei);
static void (*r_GetIntegerv)(GLenum, GLint *);
static void (*r_DeleteTextures)(GLsizei, const GLuint *);
static void (*r_DeleteProgram)(GLuint);
static void (*r_DeleteBuffers)(GLsizei, const GLuint *);
static void (*r_DeleteFramebuffers)(GLsizei, const GLuint *);
static void (*r_DeleteVertexArrays)(GLsizei, const GLuint *);
static void *(*r_MapBuffer)(GLenum, GLenum);
static GLboolean (*r_UnmapBuffer)(GLenum);
static void (*r_SwapBuffers)(void *, unsigned long);
static int  (*r_MakeCurrent)(void *, unsigned long, void *);
static int  (*r_MakeCurrentAttribs)(void *, unsigned long, void *, const int *);

static void load_all(void)
{
    if (inited) {
        /* 引擎可能在我们第一次解析之后才 dlopen("libGL.so.1")，
         * 那一轮拿到的是 NULL。这里只补关键的那几个，不做整体重扫。 */
        if (!r_UseProgram || !r_BindTexture || !r_ActiveTexture ||
            !r_DrawArrays || !r_BindFramebuffer || !r_GetIntegerv) {
            LOAD(r_UseProgram);      LOAD(r_BindTexture);    LOAD(r_ActiveTexture);
            LOAD(r_DrawArrays);      LOAD(r_BindFramebuffer);LOAD(r_GetIntegerv);
            LOAD(r_BindVertexArray); LOAD(r_BindBuffer);     LOAD(r_Flush);
            LOAD(r_Finish);          LOAD(r_DrawElements);   LOAD(r_TexImage2D);
            LOAD(r_TexParameteri);   LOAD(r_BufferData);     LOAD(r_Viewport);
            LOAD(r_SwapBuffers);     LOAD(r_MakeCurrent);
            if (getenv("GLBATCH_DEBUG") && r_UseProgram && !r_GetIntegerv)
                fprintf(stderr, "glbatch: 补解析后仍缺 glGetIntegerv\n");
        }
        return;
    }
    inited = 1;

    const char *m = getenv("GLBATCH_MODE");
    if (!m) m = "dedupe";
    if (!strcmp(m, "off")) disabled = 1;
    else if (!strcmp(m, "count")) mode_count = 1;
    else if (!strcmp(m, "dedupe") || !strcmp(m, "safe")) { mode_dedupe = 1; mode_count = 1; }
    else if (!strcmp(m, "reorder")) { mode_reorder = 1; mode_count = 1; }
    else { mode_dedupe = 1; mode_count = 1; }

    min_mode = (getenv("GLBATCH_MIN") != NULL);
    if (getenv("GLBATCH_TRACE")) trace_fp = fopen(getenv("GLBATCH_TRACE"), "w");
    stats_on = (getenv("GLBATCH_STATS") != NULL) || (getenv("GLBATCH_LOG") != NULL);
    log_path = getenv("GLBATCH_LOG");
    t_start = now_s();
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1;
      sigaction(SIGUSR1, &sa, NULL); }
    if (getenv("GLBATCH_BT"))
        { signal(SIGSEGV, segv); signal(SIGABRT, segv); signal(SIGBUS, segv); }

    LOAD(r_UseProgram); LOAD(r_BindTexture); LOAD(r_ActiveTexture);
    LOAD(r_BindFramebuffer); LOAD(r_BindRenderbuffer); LOAD(r_BindVertexArray);
    LOAD(r_BindBuffer); LOAD(r_BindBufferBase); LOAD(r_BindBufferRange);
    LOAD(r_BindSampler); LOAD(r_BindProgramPipeline);
    LOAD(r_DrawArrays); LOAD(r_DrawElements); LOAD(r_DrawArraysInstanced);
    LOAD(r_DrawElementsInstanced); LOAD(r_DrawRangeElements);
    LOAD(r_MultiDrawArrays); LOAD(r_MultiDrawElements);
    LOAD(r_Flush); LOAD(r_Finish); LOAD(r_Clear); LOAD(r_ReadPixels);
    LOAD(r_TexImage2D); LOAD(r_TexSubImage2D); LOAD(r_TexParameteri); LOAD(r_GenerateMipmap);
    LOAD(r_BufferData); LOAD(r_BufferSubData);
    LOAD(r_Uniform1f); LOAD(r_Uniform2f); LOAD(r_Uniform3f); LOAD(r_Uniform4f);
    LOAD(r_Uniform1i); LOAD(r_UniformMatrix4fv);
    LOAD(r_FramebufferTexture2D); LOAD(r_DrawBuffers); LOAD(r_LinkProgram);
    LOAD(r_Viewport); LOAD(r_Scissor); LOAD(r_GetIntegerv);
    LOAD(r_DeleteTextures); LOAD(r_DeleteProgram); LOAD(r_DeleteBuffers);
    LOAD(r_DeleteFramebuffers); LOAD(r_DeleteVertexArrays);
    LOAD(r_MapBuffer); LOAD(r_UnmapBuffer);
    LOAD(r_SwapBuffers); LOAD(r_MakeCurrent); LOAD(r_MakeCurrentAttribs);

    state_reset();

    if (getenv("GLBATCH_DEBUG")) {
        struct { const char *n; void *p; } chk[] = {
            {"glUseProgram",(void*)r_UseProgram}, {"glBindTexture",(void*)r_BindTexture},
            {"glDrawArrays",(void*)r_DrawArrays}, {"glClear",(void*)r_Clear},
            {"glReadPixels",(void*)r_ReadPixels}, {"glMapBuffer",(void*)r_MapBuffer},
            {"glXSwapBuffers",(void*)r_SwapBuffers}, {"glTexImage2D",(void*)r_TexImage2D},
            {"glViewport",(void*)r_Viewport}, {"glGetIntegerv",(void*)r_GetIntegerv},
            {NULL,NULL}
        };
        for (int i = 0; chk[i].n; i++)
            if (!chk[i].p) fprintf(stderr, "glbatch: 未解析到 %s\n", chk[i].n);
    }
}

/* ---------- 统计输出 ---------- */
static void dump_stats(void)
{
    if (!stats_on) return;
    FILE *f = log_path ? fopen(log_path, "w") : stderr;
    if (!f) f = stderr;
    fprintf(f, "\n=== glbatch stats ===\n");
    fprintf(f, "mode=%s  draws=%lu  swaps=%lu  binds=%lu  elided=%lu  deliver=%lu  flush=%lu  groups>=2=%lu\n",
            mode_reorder ? "reorder" : (mode_dedupe ? "dedupe" : "count"),
            n_draw, n_swap, n_bind, n_elide, n_deliver, n_flush, n_group2plus);
    if (n_swap)
        fprintf(f, "per-frame: draws=%.1f binds=%.1f elided=%.1f deliver=%.1f\n",
                (double)n_draw / n_swap, (double)n_bind / n_swap,
                (double)n_elide / n_swap, (double)n_deliver / n_swap);
    if (n_draw)
        fprintf(f, "binds per draw = %.2f   (elided share = %.1f%%)\n",
                (double)n_bind / n_draw,
                n_bind ? 100.0 * n_elide / n_bind : 0.0);
    fprintf(f, "%-28s %10s %10s\n", "api", "calls", "skipped");
    for (int i = 0; i < napis; i++)
        if (api[i].calls)
            fprintf(f, "%-28s %10lu %10lu\n", api[i].name, api[i].calls, api[i].skipped);
    if (f != stderr) fclose(f);
}

__attribute__((destructor)) static void fini(void) { dump_stats(); }

/* ---------- 核心：状态提交 ---------- */
/* 真正调用驱动。调用方决定"要不要跳过"。 */
static void call_real(int s, GLuint v)
{
    n_deliver++;
    if (trace_fp && trace_n++ < 4000) fprintf(trace_fp, "call slot=%d v=%u\n", s, v);
    switch (s) {
    case S_PROG:     if (r_UseProgram) r_UseProgram(v); break;
    case S_FBO:      if (r_BindFramebuffer) r_BindFramebuffer(GL_FRAMEBUFFER, v); break;
    case S_RFBO:     if (r_BindRenderbuffer) r_BindRenderbuffer(0x8D41, v); break;
    case S_VAO:      if (r_BindVertexArray) r_BindVertexArray(v); break;
    case S_ARRAYBUF: if (r_BindBuffer) r_BindBuffer(GL_ARRAY_BUFFER, v); break;
    case S_ELEMBUF:  if (r_BindBuffer) r_BindBuffer(GL_ELEMENT_ARRAY_BUFFER, v); break;
    case S_PIPELINE: if (r_BindProgramPipeline) r_BindProgramPipeline(v); break;
    default:
        if (s >= S_TEXBASE) {
            int o = s - S_TEXBASE;
            int u = o / NTGT, t = o % NTGT;
            /* 保存/恢复必须问驱动当前的 active unit，不能信我们自己的记录
             * （应用可能用 glActiveTextureARB 或别的路径切换，我们会掉队） */
            GLint prev = 0;
            if (r_GetIntegerv) r_GetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &prev);
            if (r_ActiveTexture) r_ActiveTexture(GL_TEXTURE0 + u);
            if (r_BindTexture) r_BindTexture(tgt_of[t], v);
            if (r_ActiveTexture && prev) r_ActiveTexture((GLenum)prev);
        }
        break;
    }
}

/* dedupe / reorder：与"已下发值"相同则跳过 */
static void deliver_slot(int s)
{
    GLuint v = cur[s];
    if (drv[s] == v) return;
    drv[s] = v;
    call_real(s, v);
}

static void flush_pending(void)
{
    if (!npend) return;
    int g = npend;
    for (int s = 0; s < S_MAX; s++)
        if (pend[s]) { pend[s] = 0; deliver_slot(s); }
    if (g >= 2) n_group2plus++;
    npend = 0;
    n_flush++;
}

/* reorder 模式：把"绑定"记成待下发；其余模式立即下发 */
static void set_slot(int s, GLuint v, const char *name)
{
    if (s < 0) return;
    n_bind++;
    if (cur[s] == v) {                        /* 冗余绑定：本来就没效果 */
        n_elide++;
        int i = idx_of(name); if (i >= 0) api[i].skipped++;
        return;
    }
    cur[s] = v;
    if (mode_reorder) { pend[s] = 1; npend++; return; }
    if (mode_dedupe) { deliver_slot(s); return; }
    /* count 模式：不改行为，照常转发由调用方负责（这里不会被调用） */
}

/* count 模式下也必须真的下发，但不下发就没法统计"冗余"。
 * 做法：count 模式仍走 dedupe 的记录逻辑，但**总是转发**，
 * 这样得到的是"如果不做任何优化，会有多少次冗余绑定"。 */
/* ---------- 统一的绑定记账 ----------
 * bind_begin 返回 1 表示"这次绑定与当前已下发的值相同"（即冗余）。
 * 三种模式的差别只在于"不冗余时什么时候真的下发"。 */
static int bind_begin(int slot, GLuint v, const char *name)
{
    int i = idx_of(name);
    if (i >= 0) api[i].calls++;
    if (slot < 0) return 0;
    n_bind++;
    if (cur[slot] == v) {
        n_elide++;
        if (i >= 0) api[i].skipped++;
        return 1;
    }
    cur[slot] = v;
    return 0;
}

static void bind_end(int slot, int dup, int immediate)
{
    /* count 模式必须无条件转发，否则它就不是"不改行为"的基线了 */
    if (mode_count && !mode_dedupe && !mode_reorder) {
        drv[slot] = cur[slot];
        call_real(slot, cur[slot]);
        return;
    }
    if (dup) return;
    if (mode_reorder && !immediate) { if (!pend[slot]) { pend[slot] = 1; npend++; } return; }
    deliver_slot(slot);
}

/* ---------- 拦截：状态绑定 ---------- */
void glUseProgram(GLuint p)
{
    load_all();
    int dup = bind_begin(S_PROG, p, "glUseProgram");
    bind_end(S_PROG, dup, 0);
}

void glActiveTextureARB(GLenum u);
void glActiveTexture(GLenum u)
{
    load_all(); int i = idx_of("glActiveTexture"); if (i >= 0) api[i].calls++;
    active_unit = (u >= GL_TEXTURE0) ? (u - GL_TEXTURE0) : 0;
    if (active_unit >= NUNIT) active_unit = NUNIT - 1;
    if (r_ActiveTexture) r_ActiveTexture(u);
}

void glActiveTextureARB(GLenum u) { glActiveTexture(u); }

void glBindTexture(GLenum target, GLuint tex)
{
    load_all();
    int s = tgt_slot(target);
    if (s < 0) {                                   /* 不跟踪的 target：直接转发 */
        int i = idx_of("glBindTexture(其他)"); if (i >= 0) api[i].calls++;
        if (r_BindTexture) r_BindTexture(target, tex);
        return;
    }
    int dup = bind_begin(s, tex, "glBindTexture");
    bind_end(s, dup, 0);
}

void glBindFramebuffer(GLenum target, GLuint fb)
{
    load_all();
    /* GL_DRAW/READ_FRAMEBUFFER 是分开的两个绑定点，改写成 GL_FRAMEBUFFER 会破坏
     * "只绑 draw 侧"的语义（FBO 完整性检查会因此失败），所以这两种 target 直接透传。 */
    if (target != GL_FRAMEBUFFER) {
        int i = idx_of("glBindFramebuffer(draw/read)");
        if (i >= 0) api[i].calls++;
        n_bind++; flush_pending();
        if (r_BindFramebuffer) r_BindFramebuffer(target, fb);
        return;
    }
    int dup = bind_begin(S_FBO, fb, "glBindFramebuffer");
    if (!dup && mode_reorder) flush_pending();
    bind_end(S_FBO, dup, 1);
}
void glBindFramebufferEXT(GLenum target, GLuint fb) { glBindFramebuffer(target, fb); }

void glBindRenderbuffer(GLenum target, GLuint rb)
{
    load_all();
    int dup = bind_begin(S_RFBO, rb, "glBindRenderbuffer");
    if (!dup && mode_reorder) flush_pending();
    bind_end(S_RFBO, dup, 1);
}
void glBindRenderbufferEXT(GLenum t, GLuint r) { glBindRenderbuffer(t, r); }

void glBindVertexArray(GLuint va)
{
    load_all();
    int dup = bind_begin(S_VAO, va, "glBindVertexArray");
    if (!dup && mode_reorder) flush_pending();
    bind_end(S_VAO, dup, 1);
}

void glBindBuffer(GLenum target, GLuint b)
{
    load_all();
    int s = (target == GL_ARRAY_BUFFER) ? S_ARRAYBUF
          : (target == GL_ELEMENT_ARRAY_BUFFER) ? S_ELEMBUF : -1;
    if (s < 0) {
        int i = idx_of("glBindBuffer(其他)"); if (i >= 0) api[i].calls++;
        if (r_BindBuffer) r_BindBuffer(target, b);
        return;
    }
    int dup = bind_begin(s, b, "glBindBuffer");
    bind_end(s, dup, 1);                            /* buffer 绑定可能是"修改"的前置，立即下发 */
}

void glBindBufferBase(GLenum target, GLuint index, GLuint b)
{
    load_all(); int i = idx_of("glBindBufferBase"); if (i >= 0) api[i].calls++;
    n_bind++; flush_pending();
    if (r_BindBufferBase) r_BindBufferBase(target, index, b);
}
void glBindBufferRange(GLenum target, GLuint index, GLuint b, GLintptr o, GLsizeiptr l)
{
    load_all(); int i = idx_of("glBindBufferRange"); if (i >= 0) api[i].calls++;
    n_bind++; flush_pending();
    if (r_BindBufferRange) r_BindBufferRange(target, index, b, o, l);
}

void glBindSampler(GLuint u, GLuint s)
{
    load_all(); int i = idx_of("glBindSampler"); if (i >= 0) api[i].calls++;
    n_bind++; flush_pending();
    if (r_BindSampler) r_BindSampler(u, s);
}

void glBindProgramPipeline(GLuint p)
{
    load_all();
    int dup = bind_begin(S_PIPELINE, p, "glBindProgramPipeline");
    if (!dup && mode_reorder) flush_pending();
    bind_end(S_PIPELINE, dup, 1);
}

/* ---------- 拦截：draw（触发下发） ---------- */
static void pre_draw(const char *name)
{
    load_all();
    int i = idx_of(name); if (i >= 0) api[i].calls++;
    n_draw++;
    flush_pending();
}

void glDrawArrays(GLenum m, GLint f, GLsizei c)
{ pre_draw("glDrawArrays"); if (r_DrawArrays) r_DrawArrays(m, f, c); }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i)
{ pre_draw("glDrawElements"); if (r_DrawElements) r_DrawElements(m, c, t, i); }
void glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n)
{ pre_draw("glDrawArraysInstanced"); if (r_DrawArraysInstanced) r_DrawArraysInstanced(m, f, c, n); }
void glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n)
{ pre_draw("glDrawElementsInstanced"); if (r_DrawElementsInstanced) r_DrawElementsInstanced(m, c, t, i, n); }
void glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i)
{ pre_draw("glDrawRangeElements"); if (r_DrawRangeElements) r_DrawRangeElements(m, s, e, c, t, i); }
void glMultiDrawArrays(GLenum m, const GLint *f, const GLsizei *c, GLsizei n)
{ pre_draw("glMultiDrawArrays"); if (r_MultiDrawArrays) r_MultiDrawArrays(m, f, c, n); }
void glMultiDrawElements(GLenum m, const GLsizei *c, GLenum t, const void *const *i, GLsizei n)
{ pre_draw("glMultiDrawElements"); if (r_MultiDrawElements) r_MultiDrawElements(m, c, t, i, n); }

/* ---------- 拦截：刷新 / 帧边界 ---------- */
void glFlush(void)
{ load_all(); int i = idx_of("glFlush"); if (i >= 0) api[i].calls++; flush_pending(); if (r_Flush) r_Flush(); }
void glFinish(void)
{ load_all(); int i = idx_of("glFinish"); if (i >= 0) api[i].calls++; flush_pending(); if (r_Finish) r_Finish(); }

void glXSwapBuffers(void *dpy, unsigned long drawable)
{
    load_all();
    int i = idx_of("glXSwapBuffers"); if (i >= 0) api[i].calls++;
    flush_pending();
    if (!r_SwapBuffers) r_SwapBuffers = dlsym(RTLD_NEXT, "glXSwapBuffers");
    n_swap++;
    if (r_SwapBuffers) r_SwapBuffers(dpy, drawable);
}

int glXMakeCurrent(void *dpy, unsigned long drawable, void *ctx)
{
    load_all();
    int i = idx_of("glXMakeCurrent"); if (i >= 0) api[i].calls++;
    flush_pending();
    if (!r_MakeCurrent) r_MakeCurrent = dlsym(RTLD_NEXT, "glXMakeCurrent");
    int rc = r_MakeCurrent ? r_MakeCurrent(dpy, drawable, ctx) : 0;
    state_reset();                       /* 换上下文：缓存全部作废，强制重下发 */
    return rc;
}

int glXMakeCurrentReadSGI(void *dpy, unsigned long d, unsigned long r, void *ctx)
{
    return glXMakeCurrent(dpy, d, ctx);
}

/* ---------- 拦截：会观察/修改绑定对象状态的入口（先 flush） ---------- */
#define FLUSH_WRAP_VOID(fn, realfn, params, args, counter_name)             \
    void fn params { load_all(); int i = idx_of(counter_name);              \
        if (i >= 0) api[i].calls++; flush_pending();                        \
        if (realfn) realfn args; }

FLUSH_WRAP_VOID(glClear, r_Clear, (GLbitfield m), (m), "glClear")
FLUSH_WRAP_VOID(glReadPixels, r_ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p),
                (x, y, w, h, f, t, p), "glReadPixels")
FLUSH_WRAP_VOID(glTexImage2D, r_TexImage2D, (GLenum tg, GLint l, GLint if_, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum t, const void *p),
                (tg, l, if_, w, h, b, f, t, p), "glTexImage2D")
FLUSH_WRAP_VOID(glTexSubImage2D, r_TexSubImage2D, (GLenum tg, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, const void *p),
                (tg, l, x, y, w, h, f, t, p), "glTexSubImage2D")
FLUSH_WRAP_VOID(glTexParameteri, r_TexParameteri, (GLenum tg, GLenum pn, GLint p), (tg, pn, p), "glTexParameteri")
FLUSH_WRAP_VOID(glGenerateMipmap, r_GenerateMipmap, (GLenum tg), (tg), "glGenerateMipmap")
FLUSH_WRAP_VOID(glBufferData, r_BufferData, (GLenum tg, GLsizeiptr s, const void *d, GLenum u), (tg, s, d, u), "glBufferData")
FLUSH_WRAP_VOID(glBufferSubData, r_BufferSubData, (GLenum tg, GLintptr o, GLsizeiptr s, const void *d), (tg, o, s, d), "glBufferSubData")
FLUSH_WRAP_VOID(glUniform1f, r_Uniform1f, (GLint l, GLfloat a), (l, a), "glUniform1f")
FLUSH_WRAP_VOID(glUniform2f, r_Uniform2f, (GLint l, GLfloat a, GLfloat b), (l, a, b), "glUniform2f")
FLUSH_WRAP_VOID(glUniform3f, r_Uniform3f, (GLint l, GLfloat a, GLfloat b, GLfloat c), (l, a, b, c), "glUniform3f")
FLUSH_WRAP_VOID(glUniform4f, r_Uniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d), (l, a, b, c, d), "glUniform4f")
FLUSH_WRAP_VOID(glUniform1i, r_Uniform1i, (GLint l, GLint a), (l, a), "glUniform1i")
FLUSH_WRAP_VOID(glUniformMatrix4fv, r_UniformMatrix4fv, (GLint l, GLsizei c, GLboolean t, const GLfloat *v), (l, c, t, v), "glUniformMatrix4fv")
FLUSH_WRAP_VOID(glFramebufferTexture2D, r_FramebufferTexture2D, (GLenum a, GLenum b, GLenum c, GLuint d, GLint e), (a, b, c, d, e), "glFramebufferTexture2D")
FLUSH_WRAP_VOID(glDrawBuffers, r_DrawBuffers, (GLsizei n, const GLenum *b), (n, b), "glDrawBuffers")
FLUSH_WRAP_VOID(glLinkProgram, r_LinkProgram, (GLuint p), (p), "glLinkProgram")
FLUSH_WRAP_VOID(glViewport, r_Viewport, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h), "glViewport")
FLUSH_WRAP_VOID(glScissor, r_Scissor, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h), "glScissor")
FLUSH_WRAP_VOID(glDeleteTextures, r_DeleteTextures, (GLsizei n, const GLuint *t), (n, t), "glDeleteTextures")
FLUSH_WRAP_VOID(glDeleteProgram, r_DeleteProgram, (GLuint p), (p), "glDeleteProgram")
FLUSH_WRAP_VOID(glDeleteBuffers, r_DeleteBuffers, (GLsizei n, const GLuint *b), (n, b), "glDeleteBuffers")
FLUSH_WRAP_VOID(glDeleteFramebuffers, r_DeleteFramebuffers, (GLsizei n, const GLuint *f), (n, f), "glDeleteFramebuffers")
FLUSH_WRAP_VOID(glDeleteVertexArrays, r_DeleteVertexArrays, (GLsizei n, const GLuint *v), (n, v), "glDeleteVertexArrays")

void *glMapBuffer(GLenum t, GLenum a)
{
    load_all(); int i = idx_of("glMapBuffer"); if (i >= 0) api[i].calls++;
    flush_pending();
    if (!r_MapBuffer) r_MapBuffer = dlsym(RTLD_NEXT, "glMapBuffer");
    return r_MapBuffer ? r_MapBuffer(t, a) : NULL;
}
GLboolean glUnmapBuffer(GLenum t)
{
    load_all(); int i = idx_of("glUnmapBuffer"); if (i >= 0) api[i].calls++;
    flush_pending();
    if (!r_UnmapBuffer) r_UnmapBuffer = dlsym(RTLD_NEXT, "glUnmapBuffer");
    return r_UnmapBuffer ? r_UnmapBuffer(t) : 1;
}

/* glGet*：查询类，必须保证驱动侧状态已同步 */
void glGetIntegerv(GLenum p, GLint *d)
{
    load_all(); int i = idx_of("glGetIntegerv"); if (i >= 0) api[i].calls++;
    flush_pending();
    if (!r_GetIntegerv) r_GetIntegerv = dlsym(RTLD_NEXT, "glGetIntegerv");
    if (r_GetIntegerv) r_GetIntegerv(p, d);
    else if (d) *d = 0;
}

/* ---------- glXGetProcAddress：让"动态解析入口"的应用也走垫片 ---------- */
static void *wrap_lookup(const char *n)
{
    if (trace_fp && trace_n < 4000) fprintf(trace_fp, "lookup %s\n", n);
    if (min_mode) {          /* 保守模式：只包绑定/绘制/刷新，不碰查询与修改类 */
        static const char *keep[] = {
            "glUseProgram","glBindTexture","glBindFramebuffer","glBindFramebufferEXT",
            "glBindRenderbuffer","glBindRenderbufferEXT","glBindVertexArray","glBindBuffer",
            "glBindSampler","glBindProgramPipeline","glActiveTexture","glActiveTextureARB",
            "glDrawArrays","glDrawElements","glDrawArraysInstanced","glDrawElementsInstanced",
            "glDrawRangeElements","glMultiDrawArrays","glMultiDrawElements",
            "glFlush","glFinish", NULL
        };
        for (int i = 0; keep[i]; i++) if (!strcmp(n, keep[i])) goto kept;
        return NULL;
    }
kept:
    if (!strcmp(n, "glUseProgram")) return (void *)glUseProgram;
    if (!strcmp(n, "glActiveTexture")) return (void *)glActiveTexture;
    if (!strcmp(n, "glActiveTextureARB")) return (void *)glActiveTexture;
    if (!strcmp(n, "glBindTexture")) return (void *)glBindTexture;
    if (!strcmp(n, "glBindFramebuffer")) return (void *)glBindFramebuffer;
    if (!strcmp(n, "glBindFramebufferEXT")) return (void *)glBindFramebufferEXT;
    if (!strcmp(n, "glBindRenderbuffer") || !strcmp(n, "glBindRenderbufferEXT")) return (void *)glBindRenderbuffer;
    if (!strcmp(n, "glBindVertexArray")) return (void *)glBindVertexArray;
    if (!strcmp(n, "glBindBuffer")) return (void *)glBindBuffer;
    if (!strcmp(n, "glBindBufferBase")) return (void *)glBindBufferBase;
    if (!strcmp(n, "glBindBufferRange")) return (void *)glBindBufferRange;
    if (!strcmp(n, "glBindSampler")) return (void *)glBindSampler;
    if (!strcmp(n, "glBindProgramPipeline")) return (void *)glBindProgramPipeline;
    if (!strcmp(n, "glDrawArrays")) return (void *)glDrawArrays;
    if (!strcmp(n, "glDrawElements")) return (void *)glDrawElements;
    if (!strcmp(n, "glDrawArraysInstanced")) return (void *)glDrawArraysInstanced;
    if (!strcmp(n, "glDrawElementsInstanced")) return (void *)glDrawElementsInstanced;
    if (!strcmp(n, "glDrawRangeElements")) return (void *)glDrawRangeElements;
    if (!strcmp(n, "glMultiDrawArrays")) return (void *)glMultiDrawArrays;
    if (!strcmp(n, "glMultiDrawElements")) return (void *)glMultiDrawElements;
    if (!strcmp(n, "glFlush")) return (void *)glFlush;
    if (!strcmp(n, "glFinish")) return (void *)glFinish;
    if (!strcmp(n, "glClear")) return (void *)glClear;
    if (!strcmp(n, "glReadPixels")) return (void *)glReadPixels;
    if (!strcmp(n, "glTexImage2D")) return (void *)glTexImage2D;
    if (!strcmp(n, "glTexSubImage2D")) return (void *)glTexSubImage2D;
    if (!strcmp(n, "glTexParameteri")) return (void *)glTexParameteri;
    if (!strcmp(n, "glGenerateMipmap")) return (void *)glGenerateMipmap;
    if (!strcmp(n, "glBufferData")) return (void *)glBufferData;
    if (!strcmp(n, "glBufferSubData")) return (void *)glBufferSubData;
    if (!strcmp(n, "glUniform1f")) return (void *)glUniform1f;
    if (!strcmp(n, "glUniform2f")) return (void *)glUniform2f;
    if (!strcmp(n, "glUniform3f")) return (void *)glUniform3f;
    if (!strcmp(n, "glUniform4f")) return (void *)glUniform4f;
    if (!strcmp(n, "glUniform1i")) return (void *)glUniform1i;
    if (!strcmp(n, "glUniformMatrix4fv")) return (void *)glUniformMatrix4fv;
    if (!strcmp(n, "glFramebufferTexture2D")) return (void *)glFramebufferTexture2D;
    if (!strcmp(n, "glDrawBuffers")) return (void *)glDrawBuffers;
    if (!strcmp(n, "glLinkProgram")) return (void *)glLinkProgram;
    if (!strcmp(n, "glViewport")) return (void *)glViewport;
    if (!strcmp(n, "glScissor")) return (void *)glScissor;
    if (!strcmp(n, "glGetIntegerv")) return (void *)glGetIntegerv;
    if (!strcmp(n, "glDeleteTextures")) return (void *)glDeleteTextures;
    if (!strcmp(n, "glDeleteProgram")) return (void *)glDeleteProgram;
    if (!strcmp(n, "glDeleteBuffers")) return (void *)glDeleteBuffers;
    if (!strcmp(n, "glDeleteFramebuffers")) return (void *)glDeleteFramebuffers;
    if (!strcmp(n, "glDeleteVertexArrays")) return (void *)glDeleteVertexArrays;
    if (!strcmp(n, "glMapBuffer")) return (void *)glMapBuffer;
    if (!strcmp(n, "glUnmapBuffer")) return (void *)glUnmapBuffer;
    return NULL;
}


/* ---------- 拦截 dlsym ----------
 * 很多引擎（含 Unigine）不走 PLT、也不走 glXGetProcAddress，
 * 而是 dlopen("libGL.so.1") 之后按句柄 dlsym 拿入口，那样会完全绕过 LD_PRELOAD。
 * 这里把 dlsym 也接住：凡是我们要包的名字，一律返回我们自己的实现。
 * 引导问题用 dlvsym 解决（dlvsym 本身不被插桩，能直接落到 libc）。 */
extern void *dlvsym(void *handle, const char *symbol, const char *version);
static void *(*real_dlsym)(void *, const char *) = NULL;
static int dlsym_busy = 0;

void *dlsym(void *handle, const char *name)
{
    if (!real_dlsym) {
        real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym)
            real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        if (!real_dlsym)
            real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    }
    if (!real_dlsym) return NULL;
    /* RTLD_NEXT 是"库自己找真实实现"用的，必须直通，否则会拿回我们的包装 → 自递归 */
    if (name && handle != RTLD_NEXT && !dlsym_busy) {
        dlsym_busy = 1;
        void *w = wrap_lookup(name);   /* 只在 GL 名字上生效 */
        dlsym_busy = 0;
        if (w) return w;
    }
    return real_dlsym(handle, name);
}

void *glXGetProcAddress(const GLubyte *name)
{
    if (!real_ggpa) real_ggpa = dlsym(RTLD_NEXT, "glXGetProcAddress");
    load_all();
    if (name) { void *w = wrap_lookup((const char *)name); if (w) return w; }
    return real_ggpa ? real_ggpa(name) : NULL;
}

void *glXGetProcAddressARB(const GLubyte *name)
{
    if (!real_ggpaa) real_ggpaa = dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    load_all();
    if (name) { void *w = wrap_lookup((const char *)name); if (w) return w; }
    return real_ggpaa ? real_ggpaa(name) : NULL;
}
