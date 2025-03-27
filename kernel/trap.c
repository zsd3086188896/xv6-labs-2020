#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
// 设置stvec寄存器指向陷阱入口
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  //设置stvec寄存器设为指向处理内核陷阱的入口
  //为什么要将陷阱处理入口设置位内核的，因为当我们从用户态陷入到内核后，在内核中又发生了中断，那么此时就应该交给内核的处理中断程序执行
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  //保存程序寄存器
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){//如果陷阱来自系统调用，就会触发syscall去处理
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    // 当前计数器保存的是ecall指令的地址，但我们要跳过这条指令执行下一条指令因此加四个字节
    //返回后我们要执行下一条指令，否则会陷入死循环
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    //中断会修改 sstatus 等寄存器，因此在完成对这些寄存器的操作前不要启用中断
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){//是否是外部中断或者软件中断，调用共devintr处理
    // ok
  } else {
    //scause寄存器表示陷阱原因
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    //r_sepc记录了触发异常的指令地址,r_stval存储的是异常相关的信息
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;  //终止进程
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  //时间片到了就让出CPU
  if(which_dev == 2)
    yield();

    //调用该函数用于恢复上下文,设置stevc
  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  //我们即将将陷阱的处理目标从 kerneltrap() 切换到 usertrap()，
  //因此在返回到用户空间之前需要禁用中断，因为只有回到用户空间后 usertrap() 才是正确的处理入口。
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  //设置用户态陷阱处理入口
  //TRAMPOLINE在用户和内核页表映射到相同的物理页
  //(uservec - trampoline)计算页内偏移量+TRAMPOLINE基址
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  //设置陷阱帧(trapframe)中的值，以供进程下次进入内核时uservec使用
  //保存当前内核页表的物理地址,确保可以正确加载内核页表
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  //设置进程内核栈的指针，内核栈用于在内核态执行时对函数的调用进行保存
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  //保存CPU的核心ID，在多线程中确保进程在正确的CPU上运行
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  //SPP位表示之前处于什么级别，但这里显示的将SPP设为0，在sret中会查看SPP发现是0返回用户，1返回内核
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  //在这里就恢复了上文中禁用的中断了，这里设置SPIE为1表示返回用户态后恢复中断的使用
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  //p->trapframe->epc += 4;这是在usertrap中执行的代码用于设置sret后应该返回到的发生中断的下一条语句的位置，这里对sepc进行设置确保可以返回到正确的执行位置
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  //这里设置satp为用户页表的位置，在后续汇编中会将他加载到实际的satp寄存器中
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  //得出用户地址空间中userret的虚拟地址
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  //执行 fn(TRAPFRAME, satp) 会跳转到用户地址空间的 userret 代码，开始执行返回用户模式的流程。
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
//检查是否为外部中断或软件中断，并处理它。
// 返回：
// ​2 表示定时器中断，
// ​1 表示其他设备中断，
// ​0 表示未识别的中断类型。
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

