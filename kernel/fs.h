// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

//魔数，标识磁盘设备是否时合法的系统，超级块的magic必须与这个匹配
#define FSMAGIC 0x10203040

//直接数据块的数量,存储在innode.addr[0-11]中
#define NDIRECT 12
//索引二级间接数据块
#define NDIRECT2 13
//inode 的 addrs[12] 指向一个间接块，该块存储 256 个数据块地址。
#define NINDIRECT (BSIZE / sizeof(uint))//单个间接块能存储的块地址的数量

//单个文件支持的 ​​最大数据块数量​​（直接 + 间接）。268kb
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)//二级间接块能存储的最大块地址的数量

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // 引用该innode的目录条目数量，减为0释放其占用的内存块
  uint size;            // Size of file (bytes)
  //第13位为二级间接块
  uint addrs[NDIRECT+2];   // Data block addresses
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

