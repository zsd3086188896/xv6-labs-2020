// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];//使每一个CPU都有一个空闲链表

char* kmem_lock_names[] = {
    "kmem_cpu_0",
    "kmem_cpu_1",
    "kmem_cpu_2",
    "kmem_cpu_3",
    "kmem_cpu_4",
    "kmem_cpu_5",
    "kmem_cpu_6",
};
void
kinit()
{
  //为每个CPU的锁进行初始化
  for(int i = 0;i<NCPU;i++){
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  //获取当前CPU编号，需要关闭中断
  push_off();

  //获取CPU编号
  int cpu = cpuid();

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  //对当前CPU进行释放操作,因为使用的是当前CPU的锁，不影响其他进程
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);

  //开启中断
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

    //获取当前CPU编号，需要关闭中断
    push_off();

    //获取CPU编号
    int cpu = cpuid();
    acquire(&kmem[cpu].lock);
    if(!kmem[cpu].freelist){//当前CPU的链表没有空闲内存
        int steal_left = 64;//指定偷的页数
        for(int i = 0;i<NBUF&&i!=cpu;i++){
          acquire(&kmem[i].lock);//对当前cpu上锁
          if(!kmem[i].freelist){//当前CPU也没有空闲内存
            release(&kmem[i].lock);
            continue;
          }

          struct run* rr = kmem[i].freelist;
          while(rr&&steal_left){//循环将kmem[i]的freelist移动到kmem[cpu]中
            kmem[i].freelist = rr->next;//先跳过第一个要移动的节点到下一个节点的位置
            rr->next = kmem[cpu].freelist;//将要从偷取的链表的指针指向要移动到cpu的第一个节点的位置
            kmem[cpu].freelist = rr;//现在目标cpu的第一个节点变为的icpu的第一个节点，完成移动
            rr = kmem[i].freelist;//移动rr指针到被偷取的链表的第二个节点
            steal_left--;
          }
          release(&kmem[i].lock);

          if(steal_left==0){
            break;
          }
        }
    }
    r = kmem[cpu].freelist;
    if(r)
      kmem[cpu].freelist = r->next;
    release(&kmem[cpu].lock);

  //恢复中断
  pop_off();
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
