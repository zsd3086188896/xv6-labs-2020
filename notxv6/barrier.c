#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

static int nthread = 1;
static int round = 0;

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  //统计当前到达屏障的线程数，当这个线程数等于总线程数释放
  int nthread;      // Number of threads that have reached this round of the barrier
  int round;     // Barrier round
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

static void 
barrier()
{
    //在这里上锁防止有线程不会被唤醒
    pthread_mutex_lock(&bstate.barrier_mutex);
    bstate.nthread++;//当一个线程进入，线程数应该+1
    if(bstate.nthread!=nthread){//当其中的线程没有达到总线程的数量，将当前进程睡眠
        pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
    }else{
        bstate.round++;//将轮数加1
        bstate.nthread = 0;//并重置线程数
        pthread_cond_broadcast(&bstate.barrier_cond);//唤醒全部线程
    }
    pthread_mutex_unlock(&bstate.barrier_mutex);
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;   //读取全局的屏障的当前轮次
    assert (i == t);
    barrier();              //等待所有线程到达此处
    usleep(random() % 100);
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}