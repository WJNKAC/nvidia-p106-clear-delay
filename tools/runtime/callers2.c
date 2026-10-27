/* callers2.c —— 在函数入口下硬件断点，统计 [rsp] 的返回地址分布（真正的调用者）
 * 用法: callers2 <pid> <glcore_off> <样本数> <间隔us>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <errno.h>
#include <sched.h>
#define MAXB 2048
struct hit { uint64_t a; int n; };
static struct hit H[MAXB]; static int nh;
static uint64_t gbase, gend;
static void add(uint64_t a){ for(int i=0;i<nh;i++) if(H[i].a==a){H[i].n++;return;} if(nh<MAXB){H[nh].a=a;H[nh].n=1;nh++;} }
static void find_base(pid_t pid){ char p[64]; snprintf(p,sizeof p,"/proc/%d/maps",pid); FILE*f=fopen(p,"r"); if(!f)return;
  char l[512]; while(fgets(l,sizeof l,f)){ if(!strstr(l,"libnvidia-glcore"))continue; unsigned long a,b;
  if(sscanf(l,"%lx-%lx",&a,&b)!=2)continue; if(!gbase)gbase=a; if(b>gend)gend=b; } fclose(f); }
static int peekq(pid_t pid,uint64_t a,uint64_t*o){ uint64_t t=0; struct iovec li={&t,8},ri={(void*)a,8};
  if(process_vm_readv(pid,&li,1,&ri,1,0)==8){*o=t;return 0;} return -1; }
static int cmpb(const void*x,const void*y){ const struct hit*a=x,*b=y; return b->n-a->n; }
int main(int argc,char**argv){
  if(argc<3){fprintf(stderr,"用法: %s <pid> <glcore_off> [样本] [间隔us]\n",argv[0]);return 9;}
  pid_t pid=strtol(argv[1],0,0); uint64_t off=strtoull(argv[2],0,0);
  int n=argc>3?atoi(argv[3]):400; int us=argc>4?atoi(argv[4]):200;
  find_base(pid); if(!gbase){fprintf(stderr,"没找到 glcore\n");return 9;}
  uint64_t bp=gbase+off;
  /* 选 CPU 时间最大的线程 */
  char pth[64]; snprintf(pth,sizeof pth,"/proc/%d/task",pid); DIR*d=opendir(pth); if(!d)return 1;
  pid_t tids[512]; int nt=0; struct dirent*e;
  while((e=readdir(d))&&nt<512) if(e->d_name[0]>='0'&&e->d_name[0]<='9') tids[nt++]=atoi(e->d_name);
  closedir(d);
  pid_t best=pid; unsigned long long bu=0;
  for(int i=0;i<nt;i++){ char f[96]; snprintf(f,sizeof f,"/proc/%d/task/%d/stat",pid,tids[i]); FILE*fp=fopen(f,"r");
    if(!fp)continue; char b[1024]; if(!fgets(b,sizeof b,fp)){fclose(fp);continue;} fclose(fp);
    char*rp=strrchr(b,')'); if(!rp)continue; unsigned long long u=0,s2=0;
    if(sscanf(rp+2,"%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",&u,&s2)>=1) if(u+s2>bu){bu=u+s2;best=tids[i];} }
  if(ptrace(PTRACE_ATTACH,best,0,0)){perror("ATTACH");return 3;}
  int st; if(waitpid(best,&st,0)<0){ptrace(PTRACE_DETACH,best,0,0);return 3;}
  if(ptrace(PTRACE_POKEUSER,best,(void*)848,(void*)bp)||ptrace(PTRACE_POKEUSER,best,(void*)904,(void*)1ULL)){
    perror("POKE"); ptrace(PTRACE_DETACH,best,0,0); return 3; }
  int got=0,need=1;
  for(int i=0;i<n*20 && got<n;i++){
    if(need){ if(ptrace(PTRACE_CONT,best,0,0))break; need=0; }
    int r=waitpid(best,&st,WNOHANG);
    if(r==0){ sched_yield(); continue; }
    if(r<0){ if(errno==EINTR)continue; break; }
    if(WIFSTOPPED(st)){ if(WSTOPSIG(st)==SIGTRAP){ got++;
        struct user_regs_struct rr; if(ptrace(PTRACE_GETREGS,best,0,&rr)==0){ uint64_t ret=0;
          /* 目标函数入口：push r12/rbx/... 之后 rsp 会变，但入口处 [rsp] 就是调用者返回地址 */
          if(peekq(best, rr.rsp, &ret)==0 && ret) add(ret); }
        need=1; continue; }
      if(ptrace(PTRACE_CONT,best,0,(void*)(long)WSTOPSIG(st)))break; continue; }
    break; }
  ptrace(PTRACE_POKEUSER,best,(void*)904,(void*)0ULL);
  ptrace(PTRACE_POKEUSER,best,(void*)848,(void*)0ULL);
  ptrace(PTRACE_DETACH,best,0,0);
  fprintf(stderr,"[callers2] 命中 %d 次\n",got);
  qsort(H,nh,sizeof(struct hit),cmpb);
  printf("\n=== 真正的调用者 ===\n");
  for(int k=0;k<nh&&k<12;k++) printf("  %6d  %s%#llx\n",H[k].n,
      (H[k].a>=gbase&&H[k].a<gend)?"glcore+":"other ", (unsigned long long)(H[k].a>=gbase&&H[k].a<gend?H[k].a-gbase:H[k].a));
  return 0; }
