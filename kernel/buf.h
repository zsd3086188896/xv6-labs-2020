struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno; //标记当前缓存的磁盘块号
  struct sleeplock lock;
  uint refcnt;    //引用计数，当 refcnt > 0 时，表示该 buf 正在被某个进程使用，不能被释放或重用。当 refcnt = 0 时，表示该 buf 未被使用，可以被回收
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];

  uint64 lastuse; //当前缓冲区最后一次被访问的时间，值越小表示越久没有访问
};

