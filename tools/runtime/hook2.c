/* hook2.c —— 基于已验证可用的 putptr 探测逻辑，加 patch 功能
 * 用法: hook2 <pid> <base> <off> <窗口字节> <最多停次数> [新sub] [间隔ms]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <time.h>

#define DR_BASE 848
#define DR(n) (DR_BASE+(n)*8)
#define DR7   DR(7)

static volatile sig_atomic_t g_to=0;
static void on_alarm(int s){(void)s;g_to=1;}
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}

static int peek(pid_t p,uint64_t a,uint64_t*o){
    errno=0;long v=ptrace(PTRACE_PEEKDATA,p,(void*)a,NULL);
    if(errno)return -1;*o=(uint64_t)v;return 0;}
static int poke(pid_t p,uint64_t a,uint64_t v){return ptrace(PTRACE_POKEDATA,p,(void*)a,(void*)v);}

int main(int argc,char**argv){
    if(argc<6){fprintf(stderr,"用法: %s <pid> <base> <off> <win> <maxN> [newsub] [gapms]\n",argv[0]);return 2;}
    pid_t pid=atoi(argv[1]);
    uint64_t bp=strtoull(argv[2],NULL,16)+strtoull(argv[3],NULL,16);
    int WIN=atoi(argv[4]); int MAXN=atoi(argv[5]);
    int NEWSUB=(argc>6)?atoi(argv[6]):6;
    int GAP=(argc>7)?atoi(argv[7]):0;
    const char *MODE=(argc>8)?argv[8]:"sub";   /* sub | cnt */
    if(WIN<32)WIN=32; WIN=(WIN/8)*8;

    struct sigaction sa;memset(&sa,0,sizeof sa);sa.sa_handler=on_alarm;
    sigaction(SIGALRM,&sa,NULL);setvbuf(stdout,NULL,_IONBF,0);
    printf("pid=%d bp=0x%llx win=%d maxN=%d newsub=%d gap=%dms mode=%s\n",
           pid,(unsigned long long)bp,WIN,MAXN,NEWSUB,GAP,MODE);

    char path[64];snprintf(path,sizeof path,"/proc/%d/task",pid);
    DIR*d=opendir(path);if(!d){perror("opendir");return 1;}
    pid_t tids[512];int nt=0;struct dirent*e;
    while((e=readdir(d))&&nt<512){if(e->d_name[0]<'0'||e->d_name[0]>'9')continue;tids[nt++]=atoi(e->d_name);}
    closedir(d);
    /* 按 CPU 时间降序：渲染线程最忙，先试它 */
    double cpu[512];
    for(int i=0;i<nt;i++){
        char f[96]; snprintf(f,sizeof f,"/proc/%d/task/%d/stat",pid,tids[i]);
        FILE*fp=fopen(f,"r"); cpu[i]=0;
        if(fp){ char buf[1024]; if(fgets(buf,sizeof buf,fp)){
            char*p2=strrchr(buf,')'); if(p2){
                unsigned long u=0,st2=0;
                if(sscanf(p2+2,"%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",&u,&st2)==2)
                    cpu[i]=(double)(u+st2);
            }}
            fclose(fp);}
    }
    for(int i=0;i<nt;i++)for(int j=i+1;j<nt;j++)
        if(cpu[j]>cpu[i]){double t=cpu[i];cpu[i]=cpu[j];cpu[j]=t;
            pid_t tt=tids[i];tids[i]=tids[j];tids[j]=tt;}
    printf("线程数 %d（已按CPU时间排序）\n",nt);

    pid_t tgt=-1;
    for(int i=0;i<nt;i++){
        if(ptrace(PTRACE_ATTACH,tids[i],NULL,NULL))continue;
        int st;if(waitpid(tids[i],&st,0)<0){ptrace(PTRACE_DETACH,tids[i],NULL,NULL);continue;}
        ptrace(PTRACE_POKEUSER,tids[i],(void*)DR(0),(void*)bp);
        ptrace(PTRACE_POKEUSER,tids[i],(void*)DR7,(void*)0x1ULL);
        g_to=0;alarm(1);int hit=0;
        while(!g_to && !hit){
            if(ptrace(PTRACE_CONT,tids[i],NULL,NULL))break;
            if(waitpid(tids[i],&st,0)<0){if(errno==EINTR)continue;break;}
            if(!WIFSTOPPED(st))break;                 /* 线程退出 */
            if(WSTOPSIG(st)==SIGTRAP){hit=1;break;}   /* 命中断点 */
            /* 被其它信号停住：原样转发并继续，别放弃 */
            if(ptrace(PTRACE_CONT,tids[i],NULL,(void*)(long)WSTOPSIG(st)))break;
            if(waitpid(tids[i],&st,0)<0){if(errno==EINTR)continue;break;}
            if(WIFSTOPPED(st)&&WSTOPSIG(st)==SIGTRAP)hit=1;
        }
        alarm(0);
        if(!hit){
            ptrace(PTRACE_POKEUSER,tids[i],(void*)DR7,(void*)0ULL);
            ptrace(PTRACE_POKEUSER,tids[i],(void*)DR(0),(void*)0ULL);
            ptrace(PTRACE_DETACH,tids[i],NULL,NULL);
            continue;
        }
        tgt=tids[i];
        printf("命中 tid=%d（CPU时间=%.0f ticks，保持 attach）\n",tgt,cpu[i]);
        /* 此时停在断点处，EIP=bp。清 DR 避免立刻重触发，再单步过这条指令 */
        ptrace(PTRACE_POKEUSER,tgt,(void*)DR7,(void*)0ULL);
        ptrace(PTRACE_POKEUSER,tgt,(void*)DR(0),(void*)0ULL);
        if(ptrace(PTRACE_SINGLESTEP,tgt,NULL,NULL)==0) waitpid(tgt,&st,0);
        /* 重新武装 */
        ptrace(PTRACE_POKEUSER,tgt,(void*)DR(0),(void*)bp);
        ptrace(PTRACE_POKEUSER,tgt,(void*)DR7,(void*)0x1ULL);
        break;
    }
    if(tgt<0){printf("没有线程执行该地址\n");return 1;}

    long stops=0,patched=0,cmds=0;
    uint64_t t0=now_ms();
    g_to=0;alarm(15);
    while(!g_to && stops<MAXN){
        int st;
        if(ptrace(PTRACE_CONT,tgt,NULL,NULL))break;
        if(waitpid(tgt,&st,0)<0){if(errno==EINTR)continue;break;}
        if(!WIFSTOPPED(st)){printf("目标停止\n");break;}
        if(WSTOPSIG(st)!=SIGTRAP)continue;

        struct user_regs_struct r;
        if(ptrace(PTRACE_GETREGS,tgt,NULL,&r))break;
        uint64_t put=r.rsi;
        stops++;

        uint64_t start=(put>WIN)?(put-WIN):put;
        if(strcmp(MODE,"fill")==0){
            /* 整段填垃圾：如果 GPU 读这块，命令流必然全废 */
            for(uint64_t a=start;a<put;a+=8){ if(poke(tgt,a,0xffffffffffffffffULL)==0) patched++; }
            cmds+=(put-start)/8;
            if(stops%20==0)
                printf("  fill#%ld [0x%llx,0x%llx) 填%lld字\n",stops,
                       (unsigned long long)start,(unsigned long long)put,
                       (long long)((put-start)/8));
            if(GAP>0)usleep(GAP*1000);
            continue;
        }
        for(uint64_t a=start;a<put;a+=8){
            uint64_t w;
            if(peek(tgt,a,&w))continue;
            for(int k=0;k<2;k++){
                uint32_t v=(uint32_t)(w>>(k*32));
                uint32_t op=v>>29;
                if(op==0||op==1){
                    uint32_t cnt=(v>>16)&0x1fff, sub=(v>>13)&7;
                    if(cnt>=1&&cnt<=64){
                        cmds++;
                        int want = (strcmp(MODE,"sub")==0) ? (sub==0) : 1;
                        if(want){
                            uint32_t nv;
                            if(strcmp(MODE,"sub")==0)
                                nv=(v&~(uint32_t)(0x7u<<13))|((uint32_t)NEWSUB<<13);
                            else {
                                /* cnt 字段 +1（模 0x1fff），必然打乱参数个数 => 命令流错位 */
                                uint32_t nc=((cnt+1)&0x1fff);
                                nv=(v&~(uint32_t)(0x1fffu<<16))|(nc<<16);
                            }
                            uint64_t nw;
                            if(k==0)nw=(w&0xffffffff00000000ULL)|nv;
                            else    nw=(w&0x00000000ffffffffULL)|((uint64_t)nv<<32);
                            if(poke(tgt,a,nw)==0)patched++;
                        }
                    }
                }
            }
        }
        if(stops%100==0)
            printf("  停%ld 经过%llums 命令%ld 改写%ld put=0x%llx\n",
                   stops,(unsigned long long)(now_ms()-t0),cmds,patched,(unsigned long long)put);
        if(GAP>0)usleep(GAP*1000);
    }
    alarm(0);
    printf("\n结束：停%ld 次，命令%ld 条，改写%ld 处，耗时%llums\n",
           stops,cmds,patched,(unsigned long long)(now_ms()-t0));

    ptrace(PTRACE_POKEUSER,tgt,(void*)DR7,(void*)0ULL);
    ptrace(PTRACE_POKEUSER,tgt,(void*)DR(0),(void*)0ULL);
    ptrace(PTRACE_DETACH,tgt,NULL,NULL);
    return 0;
}
