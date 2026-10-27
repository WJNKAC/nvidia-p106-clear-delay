/* tsample.c —— 逐线程采样 RIP，找出哪个线程在 glcore 里、在干什么
 * 用法: tsample <pid> [每线程样本] [间隔us]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dirent.h>
#include <sched.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>

#define MAXT 128
static pid_t tids[MAXT]; static int nt;
struct hit { uint64_t a; int n; };
struct tstat { pid_t tid; struct hit h[64]; int nh; int samples; };
static struct tstat T[MAXT];
static uint64_t gbase, gend;

static void add(struct hit*a,int*n,uint64_t x){ for(int i=0;i<*n;i++) if(a[i].a==x){a[i].n++;return;} if(*n<64){a[*n].a=x;a[*n].n=1;(*n)++;} }
static int cmpb(const void*x,const void*y){ const struct hit*a=x,*b=y; return b->n-a->n; }

static void find_base(pid_t pid){
  char p[64]; snprintf(p,sizeof p,"/proc/%d/maps",pid); FILE*f=fopen(p,"r"); if(!f)return;
  char l[512];
  while(fgets(l,sizeof l,f)){ if(!strstr(l,"libnvidia-glcore"))continue;
    unsigned long a,off=0; if(sscanf(l,"%lx-",&a)!=1)continue;
    char*sp=strchr(l,' ');
    if(sp&&sscanf(sp+1,"%*s %lx",&off)==1&&off==0){ gbase=a; }
    unsigned long b; if(sscanf(l,"%*lx-%lx",&b)==1&&b>gend) gend=b; }
  fclose(f);
}

int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"用法: %s <pid> [样本] [间隔us]\n",argv[0]);return 9;}
  pid_t pid=strtol(argv[1],0,0);
  int n=argc>2?atoi(argv[2]):200, us=argc>3?atoi(argv[3]):300;
  find_base(pid);
  char pth[64]; snprintf(pth,sizeof pth,"/proc/%d/task",pid);
  DIR*d=opendir(pth); if(!d)return 1; struct dirent*e;
  while((e=readdir(d))&&nt<MAXT) if(e->d_name[0]>='0'&&e->d_name[0]<='9') tids[nt++]=atoi(e->d_name);
  closedir(d);
  if(ptrace(PTRACE_SEIZE,pid,0,0)<0){ perror("SEIZE"); return 2; }

  for(int k=0;k<nt;k++){
    T[k].tid=tids[k]; T[k].nh=0; T[k].samples=0;
    if(ptrace(PTRACE_INTERRUPT,tids[k],0,0)<0) continue;
    int st; if(waitpid(tids[k],&st,0)<0) continue;
    ptrace(PTRACE_CONT,tids[k],0,0);   /* 先恢复，之后逐次打断 */
    for(int i=0;i<n;i++){
      if(ptrace(PTRACE_INTERRUPT,tids[k],0,0)<0) break;
      if(waitpid(tids[k],&st,0)<0) break;
      struct user_regs_struct r;
      
      if(ptrace(PTRACE_GETREGS,tids[k],0,&r)==0){ add(T[k].h,&T[k].nh,r.rip); T[k].samples++; }
      if(ptrace(PTRACE_CONT,tids[k],0,0)<0) break;
      usleep(us);
    }
  }
  ptrace(PTRACE_DETACH,pid,0,0);

  printf("glcore base=%#lx  线程数=%d\n",(unsigned long)gbase,nt);
  for(int k=0;k<nt;k++){
    if(!T[k].samples) continue;
    qsort(T[k].h,T[k].nh,sizeof(struct hit),cmpb);
    int ing=0; for(int i=0;i<T[k].nh;i++){ uint64_t a=T[k].h[i].a; if(a>=gbase&&a<gend) ing+=T[k].h[i].n; }
    printf("\n[tid %d] 样本 %d  glcore 内 %.0f%%\n",T[k].tid,T[k].samples,100.0*ing/T[k].samples);
    for(int i=0;i<T[k].nh&&i<4;i++){ uint64_t a=T[k].h[i].a;
      printf("   %4d  %s%#llx\n",T[k].h[i].n,(a>=gbase&&a<gend)?"glcore+":"other ",
             (unsigned long long)((a>=gbase&&a<gend)?a-gbase:a)); }
  }
  return 0;
}
