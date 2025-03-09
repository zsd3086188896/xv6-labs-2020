#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
int
main(int argc, char *argv[])
{
  int i;
  //存储待跟踪程序的名称和参数
  char *nargv[MAXARG];

  //保证trace的参数不少于三个，并且跟踪的系统调用号在0-99之间
  if(argc < 3 || (argv[1][0] < '0' || argv[1][0] > '9')){
    fprintf(2, "Usage: %s mask command\n", argv[0]);
    exit(1);
  }
  //调用trace系统调用，传入待跟踪系统调用号
  if (trace(atoi(argv[1])) < 0) {
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }
  //保存待跟踪程序的名称和参数
  for(i = 2; i < argc && i < MAXARG; i++){
    nargv[i-2] = argv[i];
  }
  //运行待跟踪的程序
  exec(nargv[0], nargv);
  exit(0);
}
