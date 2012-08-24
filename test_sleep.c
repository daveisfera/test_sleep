#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>


// The different type of sleep that are supported
enum sleep_type {
  SLEEP_TYPE_NONE,
  SLEEP_TYPE_YIELD,
  SLEEP_TYPE_SELECT,
  SLEEP_TYPE_POLL,
  SLEEP_TYPE_USLEEP,
  SLEEP_TYPE_PTHREAD_COND,
  SLEEP_TYPE_NANOSLEEP,
};

// Function type for doing work with a sleep
typedef int (*work_func)(const int sleep_time, const int num_iterations, const int work_size);

// In order to make SLEEP_TYPE a run-time parameter function pointers are used.
// The function pointer could have been to the sleep function being used, but
// then that would mean an extra function call inside of the "work loop" and I
// wanted to keep the measurements as tight as possible and the extra work being
// done to be as small/controlled as possible so instead the work is declared as
// a seriees of macros that are called in all of the sleep functions. The code
// is a bit uglier this way, but I believe it results in a more accurate test.

// Fill in a buffer with random numbers (taken from latt.c by Jens Axboe <jens.axboe@oracle.com>)
#define DECLARE_FUNC(NAME) int do_work_##NAME(const int sleep_time, const int num_iterations, const int work_size)

#define DECLARE_WORK() \
  int pseed; \
  int inum, bnum; \
  pseed = 0; \
  
#define DO_WORK(SLEEP_FUNC) \
  for (inum=0; inum<num_iterations; ++inum) { \
    SLEEP_FUNC \
     \
    pseed = 1; \
    for (bnum=1; bnum<work_size; ++bnum) \
      pseed = pseed * 1103515245 + 12345; \
  }

#define FINISH_WORK() \
  return pseed

DECLARE_FUNC(nosleep)
{
  DECLARE_WORK();

  // Let the compiler know that sleep_time isn't used in this function
  (void)sleep_time;

  DO_WORK();

  FINISH_WORK();
}

DECLARE_FUNC(select)
{
  struct timeval ts;
  DECLARE_WORK();

  DO_WORK(
    ts.tv_sec = 0;
    ts.tv_usec = sleep_time;
    select(0, 0, 0, 0, &ts);
    );

  FINISH_WORK();
}

DECLARE_FUNC(poll)
{
  struct pollfd pfd;
  const int sleep_time_ms = sleep_time / 1000;
  DECLARE_WORK();

  pfd.fd = 0;
  pfd.events = 0;

  DO_WORK(
    poll(&pfd, 1, sleep_time_ms);
    );

  FINISH_WORK();
}

DECLARE_FUNC(usleep)
{
  DECLARE_WORK();

  DO_WORK(
    usleep(sleep_time);
    );

  FINISH_WORK();
}

DECLARE_FUNC(yield)
{
  DECLARE_WORK();

  // Let the compiler know that sleep_time isn't used in this function
  (void)sleep_time;

  DO_WORK(
    sched_yield();
    );

  FINISH_WORK();
}

DECLARE_FUNC(pthread_cond)
{
  pthread_cond_t cond  = PTHREAD_COND_INITIALIZER;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  struct timespec ts;
  const int sleep_time_ns = sleep_time * 1000;
  DECLARE_WORK();

  pthread_mutex_lock(&mutex);

  DO_WORK(
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += sleep_time_ns;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&cond, &mutex, &ts);
    );

  pthread_mutex_unlock(&mutex);

  pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&mutex);

  FINISH_WORK();
}

DECLARE_FUNC(nanosleep)
{
  struct timespec req, rem;
  const int sleep_time_ns = sleep_time * 1000;
  DECLARE_WORK();

  DO_WORK(
    req.tv_sec = 0;
    req.tv_nsec = sleep_time_ns;
    nanosleep(&req, &rem);
    );

  FINISH_WORK();
}

int main(int argc, char **argv)
{
  if (argc < 5) {
    printf("Usage: %s <sleep_time> <num_iterations> <work_size> <sleep_type>\n", argv[0]);
    printf("  num_iterations: Number of work/sleep cycles performed (used to improve consistency/observability))\n");
    printf("  work_size: Number of iterations (in k) that the psuedo-random number calculation is performed\n");
    printf("  sleep_type: 0=none 1=yield 2=select 3=poll 4=usleep 5=pthread_cond 6=nanosleep\n");
    return -1;
  }

  int sleep_time;
  int num_iterations;
  int work_size;
  int sleep_type;
  work_func func;

  // Get the parameters
  sleep_time = atoi(argv[1]);
  num_iterations = atoi(argv[2]);
  work_size = atoi(argv[3]) * 1024;
  sleep_type = atoi(argv[4]);
  switch (sleep_type) {
    case SLEEP_TYPE_NONE:   func = &do_work_nosleep; break;
    case SLEEP_TYPE_SELECT: func = &do_work_select;  break;
    case SLEEP_TYPE_POLL:   func = &do_work_poll;    break;
    case SLEEP_TYPE_USLEEP: func = &do_work_usleep;  break;
    case SLEEP_TYPE_YIELD:  func = &do_work_yield;   break;
    case SLEEP_TYPE_PTHREAD_COND:  func = &do_work_pthread_cond;   break;
    case SLEEP_TYPE_NANOSLEEP:  func = &do_work_nanosleep;   break;
    default:
      printf("Invalid sleep type: %d\n", sleep_type);
      return -7;
  }

  // Do the work
  func(sleep_time, num_iterations, work_size);

  return 0;
}
