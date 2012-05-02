#include <float.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>



// The different type of sleep that are supported
enum sleep_type {
  SLEEP_TYPE_NONE,
  SLEEP_TYPE_SELECT,
  SLEEP_TYPE_POLL,
  SLEEP_TYPE_USLEEP,
  SLEEP_TYPE_YIELD,
  SLEEP_TYPE_PTHREAD_COND,
  SLEEP_TYPE_NANOSLEEP,
};

// Function type for doing work with a sleep
typedef long long *(*work_func)(const int sleep_time, const int num_iterations, const int work_size);

// Information passed to the thread
struct thread_info {
  int sleep_time;
  int num_iterations;
  int work_size;
  work_func func;
};

// In order to make SLEEP_TYPE a run-time parameter function pointers are used.
// The function pointer could have been to the sleep function being used, but
// then that would mean an extra function call inside of the "work loop" and I
// wanted to keep the measurements as tight as possible and the extra work being
// done to be as small/controlled as possible so instead the work is declared as
// a seriees of macros that are called in all of the sleep functions. The code
// is a bit uglier this way, but I believe it results in a more accurate test.

// Fill in a buffer with random numbers (taken from latt.c by Jens Axboe <jens.axboe@oracle.com>)
#define DECLARE_WORK() \
  int *buf; \
  int pseed; \
  int inum, bnum; \
  struct timeval before, after; \
  long long *diff; \
  buf = calloc(work_size, sizeof(int)); \
  diff = malloc(sizeof(long long)); \
  gettimeofday(&before, NULL)
  
#define DO_WORK(SLEEP_FUNC) \
  for (inum=0; inum<num_iterations; ++inum) { \
    SLEEP_FUNC \
     \
    pseed = 1; \
    for (bnum=0; bnum<work_size; ++bnum) { \
      pseed = pseed * 1103515245 + 12345; \
      buf[bnum] = (pseed / 65536) % 32768; \
    } \
  } \

#define FINISH_WORK() \
  gettimeofday(&after, NULL); \
  *diff = 1000000LL * (after.tv_sec - before.tv_sec); \
  *diff += after.tv_usec - before.tv_usec; \
  free(buf); \
  return diff

long long *do_work_nosleep(const int sleep_time, const int num_iterations, const int work_size)
{
  DECLARE_WORK();

  // Let the compiler know that sleep_time isn't used in this function
  (void)sleep_time;

  DO_WORK();

  FINISH_WORK();
}

long long *do_work_select(const int sleep_time, const int num_iterations, const int work_size)
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

long long *do_work_poll(const int sleep_time, const int num_iterations, const int work_size)
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

long long *do_work_usleep(const int sleep_time, const int num_iterations, const int work_size)
{
  DECLARE_WORK();

  DO_WORK(
    usleep(sleep_time);
    );

  FINISH_WORK();
}

long long *do_work_yield(const int sleep_time, const int num_iterations, const int work_size)
{
  DECLARE_WORK();

  // Let the compiler know that sleep_time isn't used in this function
  (void)sleep_time;

  DO_WORK(
    sched_yield();
    );

  FINISH_WORK();
}

long long *do_work_pthread_cond(const int sleep_time, const int num_iterations, const int work_size)
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

long long *do_work_nanosleep(const int sleep_time, const int num_iterations, const int work_size)
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

void *do_test(void *arg)
{
  const struct thread_info *tinfo = (struct thread_info *)arg;

  // Call the function to do the work
  return (*tinfo->func)(tinfo->sleep_time, tinfo->num_iterations, tinfo->work_size);
}

int main(int argc, char **argv)
{
  if (argc <= 6) {
    printf("Usage: %s <sleep_time> <outer_iterations> <inner_iterations> <work_size> <num_threads> <sleep_type>\n", argv[0]);
    printf("  outer_iterations: Number of iterations for each thread (used to calculate statistics)\n");
    printf("  inner_iterations: Number of work/sleep cycles performed in each thread (used to improve consistency/observability))\n");
    printf("  work_size: Number of array elements (in kb) that are filled with psuedo-random numbers\n");
    printf("  num_threads: Number of threads to spawn and perform work/sleep cycles in\n");
    printf("  sleep_type: 0=none 1=select 2=poll 3=usleep 4=yield 5=pthread_cond 6=nanosleep\n");
    return -1;
  }

  struct thread_info tinfo;
  int outer_iterations;
  int sleep_type;
  int s, inum, tnum, num_threads;
  pthread_attr_t attr;
  pthread_t *threads;
  long long *res;
  long long *times;

  // Get the parameters
  tinfo.sleep_time = atoi(argv[1]);
  outer_iterations = atoi(argv[2]);
  tinfo.num_iterations = atoi(argv[3]);
  tinfo.work_size = atoi(argv[4]) * 1024;
  num_threads = atoi(argv[5]);
  sleep_type = atoi(argv[6]);
  switch (sleep_type) {
    case SLEEP_TYPE_NONE:   tinfo.func = &do_work_nosleep; break;
    case SLEEP_TYPE_SELECT: tinfo.func = &do_work_select;  break;
    case SLEEP_TYPE_POLL:   tinfo.func = &do_work_poll;    break;
    case SLEEP_TYPE_USLEEP: tinfo.func = &do_work_usleep;  break;
    case SLEEP_TYPE_YIELD:  tinfo.func = &do_work_yield;   break;
    case SLEEP_TYPE_PTHREAD_COND:  tinfo.func = &do_work_pthread_cond;   break;
    case SLEEP_TYPE_NANOSLEEP:  tinfo.func = &do_work_nanosleep;   break;
    default:
      printf("Invalid sleep type: %d\n", sleep_type);
      return -7;
  }

  // Initialize the thread creation attributes
  s = pthread_attr_init(&attr);
  if (s != 0) {
    printf("Error initializing thread attributes\n");
    return -2;
  }

  // Allocate the memory to track the threads
  threads = calloc(num_threads, sizeof(pthread_t));
  times = calloc(num_threads, sizeof(unsigned long long));
  if (threads == NULL) {
    printf("Error allocating memory to track threads\n");
    return -3;
  }

  // Calculate the statistics of the processing
  float min_time = FLT_MAX;
  float max_time = -FLT_MAX;
  float avg_time = 0;
  float prev_avg_time = 0;
  float stddev_time = 0;

  // Perform the requested number of outer iterations
  for (inum=0; inum<outer_iterations; ++inum) {
    // Start all of the threads
    for (tnum=0; tnum<num_threads; ++tnum) {
      s = pthread_create(&threads[tnum], &attr, &do_test, &tinfo);

      if (s != 0) {
        printf("Error starting thread\n");
        return -4;
      }
    }

    // Clean up the thread creation attributes
    s = pthread_attr_destroy(&attr);
    if (s != 0) {
      printf("Error cleaning up thread attributes\n");
      return -5;
    }

    // Wait for all the threads to finish
    for (tnum=0; tnum<num_threads; ++tnum) {
      s = pthread_join(threads[tnum], (void **)(&res));

      if (s != 0) {
        printf("Error waiting for thread\n");
        return -6;
      }

      // Save the time
      times[tnum] = *res;

      // And clean it up
      free(res);
    }

    // Update the statistics
    for (tnum=0; tnum<num_threads; ++tnum) {
      if (times[tnum] < min_time)
        min_time = times[tnum];
      if (times[tnum] > max_time)
        max_time = times[tnum];
      avg_time += (times[tnum] - avg_time) / (float)(tnum + 1);
      stddev_time += (times[tnum] - prev_avg_time) * (times[tnum] - avg_time);
      prev_avg_time = avg_time;
    }
  }

  // Finish the calculation of the standard deviation
  stddev_time = sqrtf(stddev_time / ((outer_iterations * num_threads) - 1));

  // Print out the statistics of the times
  printf("time_per_iteration: min: %.1f us avg: %.1f us max: %.1f us stddev: %.1f us\n",
      min_time / tinfo.num_iterations,
      avg_time / tinfo.num_iterations,
      max_time / tinfo.num_iterations,
      stddev_time / tinfo.num_iterations);

  // Clean up the allocated threads
  free(threads);

  return 0;
}
