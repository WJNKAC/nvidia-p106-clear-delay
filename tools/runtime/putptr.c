/* putptr.c —— 在 emit 函数入口用硬件断点采样 rsi（pushbuffer 写指针）
 * 用法: putptr.c <pid> <base> <off> [样本数] [间隔ms]
 * 输出: 每次命中的 rsi 值 = 当前写入地址
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <stdint.h>
#include <dirent.h>
#include <time.h>

#define DR_BASE 848
#define DR(n)   (DR_BASE+(n)*8)
#define DR7     DR(7)

static volatile sig_atomic_t g_to = 0;
static void on_alarm(int s){(void)s;g_to=1;}

static uint64_t now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"用法: %s <pid> <base> <off> [n] [gap_ms]\n",argv[0]);return 2;}
    pid_t pid=atoi(argv[1]);
    uint64_t bp=strtoull(argv[2],NULL,16)+strtoull(argv[3],NULL,16);
    int N=(argc>4)?atoi(argv[4]):40;
    int GAP=(argc>5)?atoi(argv[5]):5;

    struct sigaction sa;memset(&sa,0,sizeof sa);sa.sa_handler=on_alarm;
    sigaction(SIGALRM,&sa,NULL);setvbuf(stdout,NULL,_IONBF,0);
    printf("pid=%d bp=0x%llx N=%d gap=%dms\n",pid,(unsigned long long)bp,N,GAP);

    /* 找哪个线程执行它 */
    char path[64];snprintf(path,sizeof path,"/proc/%d/task",pid);
    DIR*d=opendir(path);if(!d){perror("opendir");return 1;}
    pid_t tids[512];int nt=0;struct dirent*e;
    while((e=readdir(d))&&nt<512){
        if(e->d_name[0]<'0'||e->d_name[0]>'9')continue;
        tids[nt++]=atoi(e->d_name);
    }closedir(d);

    pid_t target=-1;
    for(int i=0;i<nt;i++){
        if(ptrace(PTRACE_ATTACH,tids[i],NULL,NULL))continue;
        int st;if(waitpid(tids[i],&st,0)<0){ptrace(PTRACE_DETACH,tids[i],NULL,NULL);continue;}
        ptrace(PTRACE_POKEUSER,tids[i],(void*)DR(0),(void*)bp);
        ptrace(PTRACE_POKEUSER,tids[i],(void*)DR7,(void*)0x1ULL);
        g_to=0;alarm(1);
        int hit=0;
        while(!g_to){
            if(ptrace(PTRACE_CONT,tids[i],NULL,NULL))break;
            if(waitpid(tids[i],&st,0)<0){if(errno==EINTR)continue;break;}
            if(WIFSTOPPED(st)){if(WSTOPSIG(st)==SIGTRAP){hit=1;}break;}
            break;
        }
        alarm(0);
        ptrace(PTRACE_POKEUSER,tids[i],(void*)DR7,(void*)0ULL);
        ptrace(PTRACE_POKEUSER,tids[i],(void*)DR(0),(void*)0ULL);
        if(hit){target=tids[i];
            ptrace(PTRACE_DETACH,tids[i],NULL,NULL);
            printf("命中线程 tid=%d\n",target);break;}
        ptrace(PTRACE_DETACH,tids[i],NULL,NULL);
    }
    if(target<0){printf("没有线程执行该地址\n");return 1;}

    /* 正式采样：单步反复命中 */
    if(ptrace(PTRACE_ATTACH,target,NULL,NULL)){perror("attach2");return 1;}
    int st;if(waitpid(target,&st,0)<0){perror("wait2");return 1;}
    ptrace(PTRACE_POKEUSER,target,(void*)DR(0),(void*)bp);
    ptrace(PTRACE_POKEUSER,target,(void*)DR7,(void*)0x1ULL);

    printf("\n%8s %14s %12s\n","t(ms)","rsi(写入地址)","rdi(ctx)");
    uint64_t t0=now_ms();
    int got=0;
    g_to=0;alarm((N*GAP)/1000+10);
    while(!g_to && got<N){
        if(ptrace(PTRACE_CONT,target,NULL,NULL))break;
        if(waitpid(target,&st,0)<0){if(errno==EINTR)continue;break;}
        if(!WIFSTOPPED(st))break;
        if(WSTOPSIG(st)!=SIGTRAP)continue;
        struct user_regs_struct r;
        if(ptrace(PTRACE_GETREGS,target,NULL,&r)==0){
            printf("%8llu %14llx %12llx\n",
                (unsigned long long)(now_ms()-t0),
                (unsigned long long)r.rsi,(unsigned long long)r.rdi);
            got++;
        }
        usleep(GAP*1000);
    }
    alarm(0);

    ptrace(PTRACE_POKEUSER,target,(void*)DR7,(void*)0ULL);
    ptrace(PTRACE_POKEUSER,target,(void*)DR(0),(void*)0ULL);
    ptrace(PTRACE_DETACH,target,NULL,NULL);
    printf("\n采样 %d 次\n",got);
    return 0;
}
