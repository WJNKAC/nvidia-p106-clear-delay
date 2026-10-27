/* bpcount.c —— 统计某函数在 N 秒内被调用的次数（硬件执行断点，零字节修改）
 *
 * 用途：判断"每帧调一次"还是"每对调一次"。
 * 因为 n83b 的每次 glMapBuffer 之间跑 1000 对 (绑定,draw)，
 * 若某函数在 M3 里被调用 ~1000×帧数 次，就是"每对调一次"。
 *
 * 用法: bpcount <pid> <glcore_off> <秒数>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sched.h>

#define DR_BASE 848
#define DR(n) (DR_BASE + (n)*8)
#define DR7   DR(7)

static uint64_t find_base(pid_t pid)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512]; uint64_t base = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "libnvidia-glcore")) continue;
        unsigned long a, off = 0;
        if (sscanf(line, "%lx-", &a) != 1) continue;
        /* 必须取 offset==0 的映射作为装载基址；只看第一段会取错 */
        char *sp = strchr(line, ' ');
        if (sp && sscanf(sp + 1, "%*s %lx", &off) == 1 && off == 0) { base = a; break; }
    }
    fclose(f);
    return base;
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "用法: %s <pid> <glcore_off> <秒数>\n", argv[0]); return 2; }
    pid_t pid = (pid_t)strtol(argv[1], 0, 0);
    uint64_t off = strtoull(argv[2], 0, 0);
    double secs = atof(argv[3]);

    uint64_t base = find_base(pid);
    if (!base) { fprintf(stderr, "没找到 glcore 基址\n"); return 9; }
    uint64_t bp = base + off;

    /* 选 CPU 时间最大的线程（渲染线程） */
    char pth[64]; snprintf(pth, sizeof pth, "/proc/%d/task", pid);
    DIR *d = opendir(pth);
    if (!d) { perror("opendir"); return 1; }
    struct dirent *e; pid_t tids[512]; int nt = 0;
    while ((e = readdir(d)) && nt < 512)
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids[nt++] = atoi(e->d_name);
    closedir(d);

    pid_t best = 0; unsigned long long bestut = 0;
    for (int i = 0; i < nt; i++) {
        char f[96]; snprintf(f, sizeof f, "/proc/%d/task/%d/stat", pid, tids[i]);
        FILE *fp = fopen(f, "r"); if (!fp) continue;
        char buf[1024]; if (!fgets(buf, sizeof buf, fp)) { fclose(fp); continue; }
        fclose(fp);
        char *rp = strrchr(buf, ')');
        if (!rp) continue;
        unsigned long long ut = 0, stt = 0;
        if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &stt) >= 1) {
            if (ut + stt > bestut) { bestut = ut + stt; best = tids[i]; }
        }
    }
    if (!best) best = pid;

    printf("pid=%d base=%#llx bp=%#llx  追踪 tid=%d  窗口 %.1fs\n",
           pid, (unsigned long long)base, (unsigned long long)bp, best, secs);

    if (ptrace(PTRACE_ATTACH, best, NULL, NULL)) { perror("ATTACH"); return 3; }
    int st; if (waitpid(best, &st, 0) < 0) { ptrace(PTRACE_DETACH, best, NULL, NULL); return 3; }
    if (ptrace(PTRACE_POKEUSER, best, (void *)DR(0), (void *)bp) ||
        ptrace(PTRACE_POKEUSER, best, (void *)DR7, (void *)0x1ULL)) {
        perror("POKEUSER"); ptrace(PTRACE_DETACH, best, NULL, NULL); return 3;
    }

    double t0 = now_s();
    long hits = 0;
    int need_cont = 1;
    while (now_s() - t0 < secs) {
        if (need_cont) {
            if (ptrace(PTRACE_CONT, best, NULL, NULL)) { perror("CONT"); break; }
            need_cont = 0;
        }
        int r = waitpid(best, &st, WNOHANG);
        if (r == 0) { sched_yield(); continue; }    /* 还在跑；不能 usleep，否则限速约3300/s */
        if (r < 0) { if (errno == EINTR) continue; perror("waitpid"); break; }
        if (WIFSTOPPED(st)) {
            if (WSTOPSIG(st) == SIGTRAP) hits++;
            else {                                    /* 其它信号原样放行 */
                if (ptrace(PTRACE_CONT, best, NULL, (void *)(long)WSTOPSIG(st))) break;
                continue;                             /* 已 CONT，无需再发 */
            }
            need_cont = 1;
            continue;
        }
        break;
    }
    double el = now_s() - t0;

    ptrace(PTRACE_POKEUSER, best, (void *)DR7, (void *)0ULL);
    ptrace(PTRACE_POKEUSER, best, (void *)DR(0), (void *)0ULL);
    ptrace(PTRACE_DETACH, best, NULL, NULL);

    printf("命中 %ld 次 / %.2fs = %.1f 次/秒\n", hits, el, hits / el);
    return 0;
}
