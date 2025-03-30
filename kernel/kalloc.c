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
} kmem;

//初始化
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);//初始化物理内存分配器，从end到物理内存结束的地址，可以分配页
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

  // Fill with junk to catch dangling refs.
  //将分配的空闲页面通过填充1，来避免后续再次使用分配后的物理页面
  memset(pa, 1, PGSIZE);

  //将物理地址强制转换成链表的节点类型
  r = (struct run*)pa;

  //将新分配的空闲节点加入到空闲链表中 
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
//在空闲链表中取出一块空闲的节点，分配给物理内存，让用户进行使用
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  //填充垃圾数据，确保分配后的空间不会被重复使用
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
