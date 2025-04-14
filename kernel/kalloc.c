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

//extern表示这是一个声明，end是一个地址标记，指向内核之后的首地址，标记整个内核代码的结束位置
//可以看作是一个指针 char*
extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

//空闲链表结构体
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

//空闲范围
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);                       //#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))  //向上取整
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
//释放一个物理页面，将他添加到空闲链表中
//回收不用的物理页面
void
kfree(void *pa)//*pa物理地址作为参数
{
  //创建一个节点
  struct run *r;

  //(uint64)pa % PGSIZE) != 0确保页面要对齐， (char*)pa < end 是否在内核代码的空间分配
  //(uint64)pa >= PHYSTOP是否超出物理内存地址
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  //获取当前CPU编号，需要关闭中断
  push_off();

  //获取CPU编号
  int cpu = cpuid();

  // Fill with junk to catch dangling refs.
  //将分配的空闲页面通过填充1，来避免后续再次使用分配后的物理页面
  memset(pa, 1, PGSIZE);

  //将物理地址强制转换成链表的节点类型
  r = (struct run*)pa;

  push_off();
  int cpu = cpuid();

  //将新分配的空闲节点加入到空闲链表中 
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
//在空闲链表中取出一块空闲的节点，分配给物理内存，让用户进行使用
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
