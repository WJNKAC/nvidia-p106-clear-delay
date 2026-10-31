#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"
    "void main(){ gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *FS =
    "#version 330 core\n"
    "out vec4 c;\n"
    "void main(){ c = vec4(0.2, 0.4, 0.8, 1.0); }\n";
static GLuint mkshader(GLenum t, const char *src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed:\n%s\n", log);
        exit(3);
    }
    return s;
}
int main(int argc, char **argv) {
    int nBatch = 400, nFrame = 200, nWarm = 20, syncEach = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) nBatch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f")) nFrame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) nWarm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) syncEach = 1;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "cannot open display\n"); return 2; }
    int attribs[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT,
                      GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
                      GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                      None };
    int n = 0;
    GLXFBConfig *fbcs = glXChooseFBConfig(dpy, DefaultScreen(dpy), attribs, &n);
    if (!fbcs || n == 0) { fprintf(stderr, "no pbuffer fbconfig\n"); return 2; }
    /* pbuffer-only 的 fbconfig 没有关联 visual，直接用 fbconfig 建上下文 */
    GLXContext ctx = glXCreateNewContext(dpy, fbcs[0], GLX_RGBA_TYPE, NULL, True);
    if (!ctx) { fprintf(stderr, "no context\n"); return 2; }
    int pbattr[] = { GLX_PBUFFER_WIDTH, 256, GLX_PBUFFER_HEIGHT, 256, None };
    GLXPbuffer pb = glXCreatePbuffer(dpy, fbcs[0], pbattr);
    if (!pb) { fprintf(stderr, "no pbuffer\n"); return 2; }
    if (!glXMakeCurrent(dpy, pb, ctx)) { fprintf(stderr, "makecurrent failed\n"); return 2; }
    printf("GL_RENDERER  : %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VENDOR    : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_VERSION   : %s\n", (const char *)glGetString(GL_VERSION));
    GLuint vs = mkshader(GL_VERTEX_SHADER, VS);
    GLuint fs = mkshader(GL_FRAGMENT_SHADER, FS);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(prog, sizeof log, NULL, log);
               fprintf(stderr, "link failed:\n%s\n", log); return 3; }
    glUseProgram(prog);
    static const GLfloat tri[] = { -1.f, -1.f,  3.f, -1.f,  -1.f, 3.f };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glViewport(0, 0, 256, 256);
    for (int f = 0; f < nWarm; f++) {
        for (int b = 0; b < nBatch; b++) {
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFlush();
            if (syncEach) glFinish();
        }
        glFinish();
    }
    double *ms = malloc(sizeof(double) * nFrame);
    double t0 = now_ms();
    for (int f = 0; f < nFrame; f++) {
        double a = now_ms();
        for (int b = 0; b < nBatch; b++) {
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFlush();
            if (syncEach) glFinish();
        }
        glFinish();
        ms[f] = now_ms() - a;
    }
    double total = now_ms() - t0;
    qsort(ms, nFrame, sizeof(double), cmpd);
    double p50 = ms[nFrame / 2];
    double p95 = ms[(int)(nFrame * 0.95)];
    printf("sync_each    : %d\n", syncEach);
    printf("batches/frame: %d\n", nBatch);
    printf("total_p50    : %.3f ms\n", p50);
    printf("total_p95    : %.3f ms\n", p95);
    printf("per_batch    : %.1f us\n", p50 * 1000.0 / nBatch);
    printf("fps          : %.2f\n", 1000.0 / p50);
    printf("wall         : %.1f ms (%d frames)\n", total, nFrame);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyPbuffer(dpy, pb);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}
