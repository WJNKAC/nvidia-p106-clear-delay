/* bc2.c —— 通用断点计数器：任意库 + 任意偏移
 * 用法: bc2 <pid> <库名子串> <offset_hex> <秒数>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#define DR0 848
#define DR7 904
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static uint64_t find_base(pid_t pid,const char*sub){
  char p[64]; snprintf(p,sizeof p,"/proc/%d/maps",pid); FILE*f=fopen(p,"r"); if(!f)return 0;
  char l[512]; uint64_t base=0;
  while(fgets(l,sizeof l,f)){ if(!strstr(l,sub))continue;
    unsigned long a,off=0; if(sscanf(l,"%lx-",&a)!=1)continue;
    char*sp=strchr(l,' ');
    if(sp&&sscanf(sp+1,"%*s %lx",&off)==1&&off==0){ base=a; break; } }
  fclose(f); return base; }
int main(int argc,char**argv){
  if(argc<5){fprintf(stderr,"用法: %s <pid> <lib子串> <off> <秒>\n",argv[0]);return 9;}
  pid_t pid=strtol(argv[1],0,0); const char*sub=argv[2];
  uint64_t off=strtoull(argv[3],0,0); double secs=atof(argv[4]);
  uint64_t base=find_base(pid,sub);
  if(!base){ fprintf(stderr,"没找到 %s\n",sub); return 9; }
  uint64_t bp=base+off;
  char pth[64]; snprintf(pth,sizeof pth,"/proc/%d/task",pid); DIR*d=opendir(pth); if(!d)return 1;
  pid_t tids[512]; int nt=0; struct dirent*e;
  while((e=readdir(d))&&nt<512) if(e->d_name[0]>='0'&&e->d_name[0]<='9') tids[nt++]=atoi(e->d_name);
  closedir(d);
  /* 只挑 CPU 时间最大的单线程：给所有线程同时下断点会让目标崩溃 */
  pid_t best=pid; unsigned long long bu=0;
  for(int k=0;k<nt;k++){
    char f[96]; snprintf(f,sizeof f,"/proc/%d/task/%d/stat",pid,tids[k]);
    FILE*fp=fopen(f,"r"); if(!fp)continue;
    char b[1024]; if(!fgets(b,sizeof b,fp)){fclose(fp);continue;} fclose(fp);
    char*rp=strrchr(b,')'); if(!rp)continue;
    unsigned long long u=0,s2=0;
    if(sscanf(rp+2,"%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",&u,&s2)>=1)
      if(u+s2>bu){bu=u+s2;best=tids[k];}
  }
  if(argc>5){ tids[0]=(pid_t)strtol(argv[5],0,0); nt=1; }
  else { tids[0]=best; nt=1; }
  long total=0;
  for(int k=0;k<nt;k++){
    pid_t t=tids[k];
    if(ptrace(PTRACE_ATTACH,t,0,0)) continue;
    int st; if(waitpid(t,&st,0)<0){ ptrace(PTRACE_DETACH,t,0,0); continue; }
    if(ptrace(PTRACE_POKEUSER,t,(void*)DR0,(void*)bp)||ptrace(PTRACE_POKEUSER,t,(void*)DR7,(void*)1ULL)){
      ptrace(PTRACE_DETACH,t,0,0); continue; }
    double t0=now_s(); int need=1;
    while(now_s()-t0<secs){
      if(need){ if(ptrace(PTRACE_CONT,t,0,0))break; need=0; }
      int r=waitpid(t,&st,WNOHANG);
      if(r==0){ sched_yield(); continue; }
      if(r<0){ if(errno==EINTR)continue; break; }
      if(WIFSTOPPED(st)){
        if(WSTOPSIG(st)==SIGTRAP){ total++; need=1; }
        else { if(ptrace(PTRACE_CONT,t,0,(void*)(long)WSTOPSIG(st)))break; }
        continue; }
      break; }
    ptrace(PTRACE_POKEUSER,t,(void*)DR7,(void*)0ULL);
    ptrace(PTRACE_POKEUSER,t,(void*)DR0,(void*)0ULL);
    ptrace(PTRACE_DETACH,t,0,0);
  }
  printf("tid=%d ",(int)tids[0]);
  printf("命中 %ld 次 / %.1fs = %.1f 次/秒\n",total,secs,total/secs);
  return 0; }
