#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
//#include "vmcopyin.c"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;//虚拟页表

extern char etext[];  // kernel.ld sets this to end of kernel code.,标记内核代码段的末尾

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 * 在启动序列之前调用kvminit用于分配内核页表,直接引用物理内存
 */
// void
// kvminit()
// {
//   //取出一块物理内存，标记为0，表示当前内存可用
//   //将最高级的页目录分配内存
//   kernel_pagetable = (pagetable_t) kalloc();
//   memset(kernel_pagetable, 0, PGSIZE);

//   //将IO设备映射到内核内存中，与物理地址保持相同的位置
//   // uart registers                                     //IO设备#define UART0 0x10000000L
//   //对应到最低一级页目录
//   //PTE_R | PTE_W设置标志位
//   kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

//   // virtio mmio disk interface
//   kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

//   // CLINT
//   kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

//   // PLIC
//   kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

//   // map kernel text executable and read-only.
//   kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

//   // map kernel data and the physical RAM we'll make use of.
//   kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

//   // map the trampoline for trap entry/exit to
//   // the highest virtual address in the kernel.
//   kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
// }

/*
 * create a direct-map page table for the kernel.
 * 在启动序列之前调用kvminit用于分配内核页表,直接引用物理内存
 */
void 
kvminit(){
  kernel_pagetable = kvminit_ker();
  //我们在每个进程的内核中不是必须的但是在全局内核页表初始化的时候他还是要有的
  kvmmap(kernel_pagetable,CLINT, CLINT, 0x10000, PTE_R | PTE_W);
}
//新建kvminit用于为每个进程的内核页表初始化
//这个版本中应当创造一个新的页表而不是修改kernel_pagetable
pagetable_t 
kvminit_ker(){
  //创建一个新页表
  pagetable_t pgtble = (pagetable_t) kalloc();
  memset(pgtble, 0, PGSIZE);

  kvm_map_pagetable(pgtble);

  return pgtble;
}

//初始化页表映射函数
void
kvm_map_pagetable(pagetable_t pgtbl){
    //将IO设备映射到内核内存中，与物理地址保持相同的位置
  // uart registers                                     //IO设备#define UART0 0x10000000L
  //对应到最低一级页目录
  //PTE_R | PTE_W设置标志位
  kvmmap(pgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(pgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  //与本实验的页表映射有冲突
  // // CLINT
  // kvmmap(pgtbl,CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(pgtbl,PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(pgtbl,KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(pgtbl,(uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(pgtbl,TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

}


// Switch h/w page table register to the kernel's page table,
// and enable paging.

void
kvminithart()
{
  //设置satp寄存器的初始位置
  //这条指令过后所有的内存地址都变成了虚拟内存地址
  //将根页表的物理内存写入satp
  //kernel_pagetable是内核页表的物理地址，MAKE_SATP将他转换成PPN号，再通过w_satp其中的一段
  //汇编函数csrw satp, %0 ，将构造好的PPN传入satp寄存器
  w_satp(MAKE_SATP(kernel_pagetable));
  //sfence.vma zero确保再切换进程后不会使用旧的TLB，同时使分页立即生效
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.

//模拟MMU地址转换机制
pte_t*
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];//找到当前虚拟地址在页表中对应的页表项
    if(*pte & PTE_V) {                    //如果当前位置有效
      pagetable = (pagetable_t)PTE2PA(*pte);//将页表项转换成对应的物理地址
    } else {
      //如果其中的一个页不存在，则进行分配
      //如果不允许分配，并且从kalloc分配内存失败直接返回
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      //如果允许分配，将刚才得到的内存置为0
      memset(pagetable, 0, PGSIZE);
      //将当前分配好的物理地址转换位页表项,并且设置标志位
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  //返回第三级的PTE,也就是最终的到物理地址
  //最终返回PTE
  return &pagetable[PX(0, va)];
}

//将用户页表拷贝到内核页表中
int 
kvmcopymappings(pagetable_t src, pagetable_t dst, uint64 start, uint64 sz){
  pte_t* pte;
  uint64 pa, i;
  uint flags;

  //将页面进行向上取证对齐到对应的页边界
  for(i = PGROUNDUP(start);i<start+sz;i+=PGSIZE){
    //未找到对应的页表项
    if((pte = walk(src, i, 0))==0){
      panic("kvmcopymapings:pte should exists");
    }
    //当前页表项无效
    if((*pte&PTE_V)==0){
      panic("kvmcopymappings:page not present");
    }
    pa = PTE2PA(*pte);

    //将该页的权限设为非用户页，因为内核无法直接访问用户页
    //取出标志位设置
    flags= PTE_FLAGS(*pte)&~PTE_U;
    if(mappages(dst, i, PGSIZE, pa, flags)!=0){
      goto err;
    }
  }
  return 0;

err:
  //解除目标页表中已经映射的页表项
  uvmunmap(dst, PGROUNDUP(start), (i-PGROUNDUP(start))/PGSIZE, 0);
  return -1;
}

//缩减内存函数
uint64
kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz){
  if(newsz>=oldsz){
    return oldsz;
  }
  if(PGROUNDUP(newsz)<PGROUNDUP(oldsz)){
    int npags = (PGROUNDUP(oldsz)-PGROUNDUP(newsz))/PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npags, 0);
  }

  return newsz;
}
// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
//查找一个虚拟地址对应的物理地址
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
//向内核页添加映射，只有在启动时使用
void
kvmmap(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
//虚拟地址转换物理地址，仅限于栈上内存转换
uint64
kvmpa(pagetable_t pagetable, uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
//为从虚拟地址 va 开始的虚拟地址创建页表项（PTEs），这些页表项指向从物理地址 pa 开始的物理地址。
//va 和 size 可能不是页对齐的。成功时返回 0，如果 walk() 无法分配所需的页表页，则返回 -1
//用于物理地址映射到虚拟地址
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);//映射到最近的页面边界，确保是从一个完整的页开始
  last = PGROUNDDOWN(va + size - 1);//最后一个页的边界
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
      //物理地址转换为PTE并设置有效位
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)//相等说明全部映射完退出
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  //要移除的映射没有对齐页边界
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

    //npages是要回收的页的数量
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    //没有对应的物理内存映射
    if((pte = walk(pagetable, a, 0)) == 0)
      //panic("uvmunmap: walk");    //遇到不存在的页表跳过
      continue;
    //有效位为0
    if((*pte & PTE_V) == 0)
      //panic("uvmunmap: not mapped");
      continue;
    //PTE_FLAGS是屏蔽物理页号的位置，只剩下标志位
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    //允许进行释放，调用kfree回收空闲物理页，加入到空闲链表中
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
//创建一个空闲内存给用户
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
//将用户初始化代码加载到页表的地址 0 处，  
//用于第一个进程。  
//`sz` 必须小于一页的大小。
//分配一个物理页用来存储用户进程的初始数据
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);//将src中的代码数据移动到分配好的物理内存中
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

    //向上取整得到最接近的页边界
  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    //每次以一个页大小进行扩大，如果分配失败进行回收刚才分配的内存
    //当要分配三页，前两页正常分配，第三页分配失败，将包括前两页全部回滚到未分配之前的oldsz
    if(mem == 0){//分配失败
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    //分配成功后调用mappages将物理页映射到页表中对应的虚拟地址处
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
//oldesz = a, newsz = oldesz
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  //如果new大于old表示不需要缩小
  if(newsz >= oldsz)
    return oldsz;

    //否则对向上取整的new和old检查是否new小于old，如果是，计算要释放的页数、
   //用uvmunmap进行回收释放物理页
   //当要分配三页，前两页正常分配，第三页分配失败，将包括前两页全部回滚到未分配之前的oldsz
  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
//递归释放页表结构
//最后不会释放叶子节点对应的物理地址
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i]; 
    //(pte & (PTE_R|PTE_W|PTE_X)) == 0，如果这些标志位没有被设置，表示这个pte指向下一级页表，只有有效位有效且存在权限位
    //说明不是一个叶子节点,需要递归继续向下查找
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

//打印页表
int
print_page(pagetable_t pagetable, int depth){
  for(int i = 0;i<512;i++){
    pte_t pte = pagetable[i];
    if(pte&PTE_V){
      printf("..");
      for(int j = 0;j<depth;j++){
        printf("..");
      }
      printf("%d：pte %p pa %p\n", i, pte, PTE2PA(pte));
      //递归遍历查找下一级页表
      if((pte&(PTE_R|PTE_W|PTE_X))==0){
        uint64 child = PTE2PA(pte);//转换成物理地址
        print_page((pagetable_t)child, depth+1);
      }
    }
  }
  return 0;
}

int vmprint(pagetable_t pagetable){
  printf("page table %p\n", pagetable);
  return print_page(pagetable, 0);
}
// Free user memory pages,
// then free page-table pages.
//释放用户内存页面,
//然后释放页表页面。
//完整释放用户内存空间以及页表结构
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  //释放用户内存(数据，代码，堆栈等部分)就是叶子节点，之后再释放页表页面的内存
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
//给定一个父进程的页表，将其内存复制到子进程的页表中。
//复制内容包括页表和物理内存。
//成功时返回 0，失败时返回 -1。
//在失败时会释放已分配的页面。
//父进程创建子进程时会子进程会复制父进程的页表，并此时都会指向同一个物理地址，这些页会被标记位只读
//当父子进程发生写的操作的时候，操作系统才会重新为子进程开辟一个新的页表，并分配一块新的物理内存
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      //panic("uvmcopy: pte should exist");
      continue;//惰性分配导致某些PTE未分配则跳过
    if((*pte & PTE_V) == 0)
      //panic("uvmcopy: page not present");
      continue;
    pa = PTE2PA(*pte);//将父进程的对应物理地址的PTE赋给pa
    flags = PTE_FLAGS(*pte);//取出PTE的十位的标志位
    if((mem = kalloc()) == 0)//没有空闲的物理内存
      goto err;
    memmove(mem, (char*)pa, PGSIZE);//复制父进程的页表和物理内存
    //将新页面进行映射
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
//将一个页标记为只能由内核进行访问
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
//从内核拷贝到用户
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)//将从内核的src拷贝到用户空间的虚拟地址dstva
{
  uint64 n, va0, pa0;

  if(uvmshouldallocate(dstva)){//如果遇到没有分配的地址空间马上进行分配
    uvmlazyallocate(dstva);
  }
  //用循环解决了跨页的情况
  while(len > 0){
    va0 = PGROUNDDOWN(dstva);//计算当前页的起始虚拟地址
    pa0 = walkaddr(pagetable, va0);//找到对应的物理地址
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);//计算需要赋值的字节数
    if(n > len)
      n = len;
    //用户地址的虚拟空间就是物理地址加上页内偏移
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}


extern int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
extern int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);
// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
//将用户空间虚拟地址srcva复制len个字节到内核空间dst指针指向的位置
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  if(uvmshouldallocate((uint64)dst)){//如果遇到没有分配的地址空间马上进行分配
    uvmlazyallocate((uint64)dst);
  }
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);//计算需要赋值的字节数
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;//下一轮循环处理下一个页面
  }
  return 0;
  //return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
//将一个以 \0 结尾的字符串从用户空间拷贝到内核空间。
//从给定页表中的虚拟地址 srcva 开始，将字节拷贝到内核的 dst 中，直到遇到 \0 或者达到最大长度 max。
//成功时返回 0，失败时返回 -1。
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
//   uint64 n, va0, pa0;
//   int got_null = 0;

//   while(got_null == 0 && max > 0){
//     va0 = PGROUNDDOWN(srcva);
//     pa0 = walkaddr(pagetable, va0);
//     if(pa0 == 0)
//       return -1;
//     n = PGSIZE - (srcva - va0);
//     if(n > max)
//       n = max;

//     char *p = (char *) (pa0 + (srcva - va0));
//     while(n > 0){
//       if(*p == '\0'){
//         *dst = '\0';
//         got_null = 1;
//         break;
//       } else {
//         *dst = *p;
//       }
//       --n;
//       --max;
//       p++;
//       dst++;
//     }

//     srcva = va0 + PGSIZE;
//   }
//   if(got_null){
//     return 0;
//   } else {
//     return -1;
//   }
  return copyinstr_new(pagetable, dst, srcva, max);
}

//递归释放内核页面
void kvm_free_kernelpgtbl(pagetable_t pgtbl){
  for(int i = 0;i<512;i++){
    pte_t pte = pgtbl[i];
    uint64 child = PTE2PA(pte);
    if((pte&PTE_V)&&(pte&(PTE_W|PTE_X|PTE_U))==0){
      kvm_free_kernelpgtbl((pagetable_t)child);
      pgtbl[i] = 0;
    }
  }
  kfree((void*)pgtbl);
}

#include"spinlock.h"
#include"proc.h"
//检查当前分配的虚拟地址是否还没有实际分配
int uvmshouldallocate(uint64 va){
  pte_t* pte;
  struct proc*p = myproc();
  return va<p->sz     //确保地址在进程空间中
        &&PGROUNDDOWN(va)!=r_sp() //确保地址不在栈的保护区域
        &&(((pte = walk(p->pagetable, va, 0))==0)||((*pte & PTE_V)==0));  //确保真的没有进行分配
}

//给虚拟地址分配和映射物理内存
void uvmlazyallocate(uint64 va){
  struct proc*p = myproc();
  char *pa = kalloc();
  if(pa==0){
    printf("lazy alloc：out of memory\n");
    p->killed = 1;
  }else{
    if(mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, (uint64)pa, PTE_W|PTE_X|PTE_R|PTE_U)!=0){
      printf("lazy alloc: failed to map page\n");
          kfree(pa);
          p->killed = 1;
    }
  }
}