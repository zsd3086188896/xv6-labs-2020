// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"


#define PA2PGREF_ID(p) (((p)-KERNBASE)/PGSIZE)//由物理地址获取物理页id
#define PGREF_MAX_ENTRIES PA2PGREF_ID(PHYSTOP)//物理页数上限

int pagecount[PGREF_MAX_ENTRIES];//物理页引用计数数组
struct spinlock pgreflock;      //用于引用计数数组的锁

#define PA2PGREF(p) pagecount[PA2PGREF_ID((uint64)(p))]//获取当前物理页面的引用计数

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;


void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
  initlock(&pgreflock, "pgref");//初始化锁
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  //对临界区进行上锁
  acquire(&pgreflock);
  if(--PA2PGREF(pa)<=0){//当当前页面的引用计数为0时才进行回收
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;//获取当前要释放物理页的在链表中的节点
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
  release(&pgreflock);//释放锁
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
    PA2PGREF(r) = 1;//将新分配的物理页的引用数设置为1
  return (void*)r;
}

//物理页引用计数加1
void addpage(void* pa){
  acquire(&pgreflock);
  PA2PGREF(pa)++;//引用计数加1
  release(&pgreflock);
}

//写时复制一个新地址返回
void *kcopy_n_deref(void* pa){
  acquire(&pgreflock);

  //当前物理页的引用计数为1，就无需分配新的物理页
  if(PA2PGREF(pa)<=1){
    release(&pgreflock);
    return pa;
  }

  //分配新的物理页，并复制旧页的数据到新页
  uint64 newpa = (uint64)kalloc();
  if(newpa==0){
    release(&pgreflock);//内存不够
    return 0;
  }
  memmove((void*)newpa, (void*)pa, PGSIZE);//复制

  PA2PGREF(pa)--;//对旧页的引用计数--

  release(&pgreflock);
  return (void*)newpa;//返回新地址空间
}