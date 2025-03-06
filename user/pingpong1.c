#include "kernel/types.h"
#include "user/user.h"
#include "kernel/stat.h"

int main(int argc, char* argv[]){
    int pp2c[2], pc2p[2];
    pipe(pp2c);
    pipe(pc2p);

    if(fork()!=0){//父进程
        //向管道中输入一个字符
        write(pp2c[1], ".", 1);
        close(pp2c[1]);//即使关闭输入管道

        //父进程读入子进程的字符
        char buf;
        read(pc2p[0], &buf, 1);
        printf("%d: received pong\n", getpid());

        //等待结束
        wait(0);
    }
    else{
        //读入父进程的字符
        char buf;
        read(pp2c[0], &buf, 1);
        printf("%d:received ping\n", getpid());

        //向父进程写入一个字符
        write(pc2p[1], &buf, 1);
        close(pc2p[1]);
    }

    //关闭管道读端
    close(pp2c[0]);
    close(pc2p[0]);

    exit(0);
}