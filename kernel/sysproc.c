#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
    //只修改进程的大小而不真正分配内存
  addr = myproc()->sz;
  //修改为lazy分配，只会增加大小但是不会分配新内存，返回旧的大小
  // if(growproc(n) < 0)
  //   return -1;
  struct proc*p = myproc();
  if(n>0){//如果是扩大内存就只增加大小
    p->sz+=n;
  }else if(p->sz+n>0){//如果是缩小内存，因为n为负数检查是否小于0，相加结果如果小于0说明要缩小的内存过大，从而报错
    p->sz = uvmdealloc(p->pagetable, p->sz, p->sz+n);
  }else
    return -1;
  //返回旧的内存大小
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  backtrace();//调用
  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_sigalarm(void){
  int n;
  uint64 fn;
  if(argint(0, &n)<0) //获取第一个参数
    return -1;
  if(argaddr(1, &fn)<0) //获取第二个参数
    return -1;
    
  return sigalarm(n, (void(*)())(fn));
}

uint64 sys_sigreturn(void){
  return sigreturn();
}