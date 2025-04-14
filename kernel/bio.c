// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUFMAP_BUCKET 13//哈希桶的个数，设置为质数降低竞争
#define BUFMAP_HASH(dev, blockno)((((dev)<<27)|(blockno))%NBUFMAP_BUCKET)//哈希函数


struct {
  //struct spinlock lock;
  struct buf buf[NBUF];

  struct spinlock eviction_lock;  //驱逐锁，确保不会产生死锁的同时，避免因提前释放锁而导致，多进程重复访问buf
  //哈希表
  struct buf bufmap[NBUFMAP_BUCKET];  //哈希桶
  struct spinlock bufmap_locks[NBUFMAP_BUCKET];//给每一个桶设定一个锁，只有当两个进程同时哈希到同一个桶的时候才会发生竞争
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  //对每一个桶的锁进行初始化
  for(int i = 0;i<NBUFMAP_BUCKET;i++){
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
    bcache.bufmap[i].next = 0;
  }

  //初始化缓存区块
  for(int i = 0;i<NBUF;++i){
    struct buf* b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->lastuse = 0;
    b->refcnt = 0;

    //将所有缓存区块添加到bufmap[0]中
    b->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = b;
  }

  //初始化驱逐锁
  initlock(&bcache.eviction_lock, "bcache_eviction");
  // // Create linked list of buffers
  // //创建双向链表
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  //获取对应的桶
  uint key = BUFMAP_HASH(dev, blockno);
  //对当前的桶进行上锁
  acquire(&bcache.bufmap_locks[key]);

  // Is the block already cached?
  //在当前的桶中进行查找
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }


  //不在缓存区
  //防止死锁，先释放当前桶的锁
  release(&bcache.bufmap_locks[key]);
  //获取驱逐锁
  acquire(&bcache.eviction_lock);

  //避免在释放锁和获取驱逐锁的间隙创建了缓存区块所以再进行一次检查
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      acquire(&bcache.bufmap_locks[key]);//如果要增加引用计数，要重新获取锁
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  //如果仍然不在缓存区
  //此时不持有任何桶锁，只持有驱逐锁
  struct buf* before_least = 0; //LRU_buf的前驱节点
  uint holding_bucket = -1; //记录当前持有哪一个桶的锁
  // 循环查询所有的桶
  for(int i = 0;i<NBUFMAP_BUCKET;++i){
    acquire(&bcache.bufmap_locks[i]);//获取当前桶的锁

    int newfound = 0;//是否在当前桶中找到LRU-buf

    for(b = &bcache.bufmap[i];b->next;b = b->next){
      //当前buf未被使用
      if(b->next->refcnt==0&&(!before_least||b->next->lastuse<before_least->next->lastuse)){
        //记录前驱节点
        before_least = b;
        newfound = 1;
      }
    }
    if(!newfound){      //没有找到，释放当前桶的锁
      release(&bcache.bufmap_locks[i]);
    }else{              //找到了对应的buf，检查是否通过holding记录了当前的桶号，释放该桶锁
      if(holding_bucket!=-1){
        release(&bcache.bufmap_locks[holding_bucket]);
      }
      holding_bucket = i;
    }
  }

  if(!before_least){    //遍历完全部的桶没有找到一个LRU_BUF,表示没有空闲缓存块了
    panic("bget: no buffuers");
  }

  b = before_least->next; //现在b指向我们找到的LRU的buf，因为before_least指向buf的前驱节点，因此可以直接锁定

  if(holding_bucket!=key){  //如果想要偷的块不在key中，就要将他从原先的桶中驱逐添加到key桶中
    before_least->next = b->next; //将b也就是要偷的buf从原链表中删除
    release(&bcache.bufmap_locks[holding_bucket]);

    //将找到的LRU放到目标桶中
    acquire(&bcache.bufmap_locks[key]);
    //让b成为目标桶的头节点
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }

  //设置新buf字段
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  //释放相关的锁
  release(&bcache.bufmap_locks[key]);
  release(&bcache.eviction_lock);
  acquiresleep(&b->lock);
  return b;

  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   //标识当前缓冲区是否使用
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;//标志将从磁盘进行读取数据
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  // panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
//返回一个锁定缓冲区，其中包含指定块的内容。
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  //获取缓冲区
  b = bget(dev, blockno);
  //valid表示缓冲区是否包含块的副本，不包含会从磁盘读取
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
//如果修改了缓冲区就必须在返回之前调用bwrite重新写入磁盘
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  //引用计数--
  b->refcnt--;
  if (b->refcnt == 0) {//如果引用计数为0，重置访问时间，越小说明越久没有用
    b->lastuse = ticks;
    // // no one is waiting for it.
    // b->next->prev = b->prev;
    // b->prev->next = b->next;
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
  }
  
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}


