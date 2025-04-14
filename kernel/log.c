#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
//头块,在提交前内存中跟踪已记录的块号
struct logheader {
  int n;            // 日志条目数（即需要修改的块数）
  int block[LOGSIZE];// 需要修改的磁盘块号数组
};

struct log {
  struct spinlock lock;
  int start;    // 日志区域在磁盘上的起始块号
  int size;     // 日志区域的总块数
  int outstanding; // how many FS sys calls are executing.前正在执行的文件系统调用数量
  int committing;  // in commit(), please wait.是否正在提交日志（若为1，其他操作需等待）
  int dev;          //日志所属的设备号
  struct logheader lh;// 日志头（记录元数据，如待提交的块号列表）
};
struct log log;

static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;//初始化日志在超级块中的位置
  log.size = sb->nlog;    //日志块的大小
  log.dev = dev;
  recover_from_log();
}

// 将已提交的块从日志（log）复制到它们的原始位置（home location）
//将修改后的事务真正应用到磁盘中
static void
install_trans(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // 读取日志块
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // 读取目标块
    memmove(dbuf->data, lbuf->data, BSIZE);  // 将日志块数据复制到目标块
    bwrite(dbuf);  // 将目标块写入磁盘
    bunpin(dbuf);  // 解引用目标块
    brelse(lbuf);  // 释放日志块缓冲区
    brelse(dbuf);  // 释放目标块缓冲区
  }
}

// Read the log header from disk into the in-memory log header
//用于崩溃恢复，检查日志头快是否又未完成的事务需要重放
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  //如果此时的lh.n大于0说明有事务需要恢复,通过调用install_trans()重放日志
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
//将头块写入磁盘：这是提交点，写入后的崩溃将导致从日志恢复重演事务的写入操作
static void
write_head(void)
{
  // 在 write_head() 之前：若崩溃，日志不完整，直接丢弃。
  // 在 write_head() 之后：若崩溃，系统重启后会根据日志头中的 hb->n 和 hb->block[] 重放事务（调用 install_trans()）。
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);//通过强制转换，操作日志头的数据
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);//将修改后的日志头块写入磁盘，标记日志已经完整的记录了
  brelse(buf);
}

// 从日志中恢复数据（用于系统崩溃后的恢复）
static void
recover_from_log(void)
{
  read_head();       // 读取磁盘上的日志头到内存
  install_trans();   // 如果事务已提交，将日志数据复制到实际磁盘位置
  log.lh.n = 0;      // 重置日志条目数（标记日志为空）
  write_head();      // 将空日志头写回磁盘，清除日志
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){//如果此时正在提交中，等待
      sleep(&log, &log.lock);
      // log.lh.n：当前日志中已记录的块数。
      // log.outstanding + 1：当前正在执行的FS调用数 + 新调用。
      // MAXOPBLOCKS：单个FS调用可能占用的最大日志块数（xv6 中通常为 10）。
      // LOGSIZE：日志区域的总块数。   
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){//防止日志空间被耗尽
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);//等待已提交的操作释放空间
    } else {
      //如果以上条件都不满足，就新增一个fs调用
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// 在每个文件系统调用的结束时被调用。
// 如果这是最后一个未完成的操作，则提交事务。
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;//减少未完成的操作计数
  if(log.committing)//如果日志有正在提交中的，触发panic
    panic("log.committing");
  if(log.outstanding == 0){//如果所有的调用都已经完成，则设置提交标记
    do_commit = 1;
    log.committing = 1;
  } else {
    // 如果还有其他未完成操作，唤醒可能等待的 begin_op()
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();              // 执行提交（不持有锁，避免睡眠死锁）
    acquire(&log.lock);    // 重新获取锁
    log.committing = 0;    // 清除提交标志
    wakeup(&log);          // 唤醒其他等待的进程
    release(&log.lock);    // 释放锁
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;
  //将事务中修改的每个块从缓冲区缓存复制到磁盘上日志槽位中。
  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // 将数据写入日志区域。
    write_head();    // 写入日志头（提交点）。
    install_trans(); // 将日志数据应用到实际位置。
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log从日志中擦除事务
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
//用log_write代替bwrite，因为bwrite会直接写入磁盘，但我们希望先将修改存入到日志中，延迟提交批量写入
void
log_write(struct buf *b)
{
  int i;

  //检查日志是否已经满了或者是否在事务中
  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    //日志合并,如果当前块号已经在其中记录，就跳过，防止重复记录
    if (log.lh.block[i] == b->blockno)   // log absorbtion
      break;
  }
  //记录块号
  log.lh.block[i] = b->blockno;//保证数组始终包含最新的块号引用，防止只有部分写入但未提交导致的崩溃恢复后不一致
  if (i == log.lh.n) {  // Add new block to log?如果是新块
    bpin(b);      //固定缓存区,防止被提前回收，当事务完成后，通过install_trans调用bunpin解引用
    log.lh.n++;
  }
  release(&log.lock);
}

