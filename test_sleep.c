#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/time.h>


// Apparently GLIBC doesn't provide a wrapper for this function so provide it here
#ifndef HAS_GETTID
pid_t gettid(void)
{
  return syscall(SYS_gettid);
}
#endif


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

// Information returned by the processing thread
struct thread_res {
  long long clock;
  long long user;
  long long sys;
};

// Function type for doing work with a sleep
typedef struct thread_res *(*work_func)(const int pid, const int sleep_time, const int num_iterations, const int work_size);

// Information passed to the thread
struct thread_info {
  pid_t pid;
  int sleep_time;
  int num_iterations;
  int work_size;
  work_func func;
};


inline void get_thread_times(pid_t pid, pid_t tid, unsigned long long *utime, unsigned long long *stime)
{
  char filename[FILENAME_MAX];
  FILE *f;

  sprintf(filename, "/proc/%d/task/%d/stat", pid, tid);
  f = fopen(filename, "r");

  if (f == NULL) {
    *utime = 0;
    *stime = 0;
    return;
  }

  if (fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %Lu %Lu", utime, stime) != 2)
    printf("Error reading thread times for pid %u tid %u\n", pid, tid);

  fclose(f);
}

// In order to make SLEEP_TYPE a run-time parameter function pointers are used.
// The function pointer could have been to the sleep function being used, but
// then that would mean an extra function call inside of the "work loop" and I
// wanted to keep the measurements as tight as possible and the extra work being
// done to be as small/controlled as possible so instead the work is declared as
// a seriees of macros that are called in all of the sleep functions. The code
// is a bit uglier this way, but I believe it results in a more accurate test.

// Fill in a buffer with random numbers (taken from latt.c by Jens Axboe <jens.axboe@oracle.com>)
#define DECLARE_FUNC(NAME) struct thread_res *do_work_##NAME(const int pid, const int sleep_time, const int num_iterations, const int work_size)

#define DECLARE_WORK() \
  int *buf; \
  int pseed; \
  int inum, bnum; \
  pid_t tid; \
  struct timeval clock_before, clock_after; \
  unsigned long long user_before, user_after; \
  unsigned long long sys_before, sys_after; \
  struct thread_res *diff; \
  tid = gettid(); \
  buf = malloc(work_size * sizeof(*buf)); \
  diff = malloc(sizeof(*diff)); \
  get_thread_times(pid, tid, &user_before, &sys_before); \
  gettimeofday(&clock_before, NULL)
  
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
  gettimeofday(&clock_after, NULL); \
  get_thread_times(pid, tid, &user_after, &sys_after); \
  diff->clock = 1000000LL * (clock_after.tv_sec - clock_before.tv_sec); \
  diff->clock += clock_after.tv_usec - clock_before.tv_usec; \
  diff->user = user_after - user_before; \
  diff->sys = sys_after - sys_before; \
  free(buf); \
  return diff

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

void *do_test(void *arg)
{
  const struct thread_info *tinfo = (struct thread_info *)arg;

  // Call the function to do the work
  return (*tinfo->func)(tinfo->pid, tinfo->sleep_time, tinfo->num_iterations, tinfo->work_size);
}

struct thread_res_stats {
  double min;
  double max;
  double avg;
  double stddev;
  double prev_avg;
};

#ifdef LLONG_MAX
  #define THREAD_RES_STATS_INITIALIZER {LLONG_MAX, LLONG_MIN, 0, 0, 0}
#else
  #define THREAD_RES_STATS_INITIALIZER {LONG_MAX, LONG_MIN, 0, 0, 0}
#endif

void update_stats(struct thread_res_stats *stats, double value, int num_samples, int num_iterations, double scale_to_usecs)
{
  // Calculate the average time per iteration
  double value_per_iteration = value * scale_to_usecs / num_iterations;

  // Update the max and min
  if (value_per_iteration < stats->min)
    stats->min = value_per_iteration;
  if (value_per_iteration > stats->max)
    stats->max = value_per_iteration;
  // Update the average
  stats->avg += (value_per_iteration - stats->avg) / (double)(num_samples);
  // Update the standard deviation
  stats->stddev += (value_per_iteration - stats->prev_avg) * (value_per_iteration - stats->avg);
  // And record the current average for use in the next update
  stats->prev_avg= stats->avg;
}

void print_stats(const char *name, const struct thread_res_stats *stats)
{
  printf("%s: min: %.1f us avg: %.1f us max: %.1f us stddev: %.1f us\n",
      name,
      stats->min,
      stats->avg,
      stats->max,
      stats->stddev);
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
  int does_sleep;
  int s, inum, tnum, num_samples, num_threads;
  pthread_attr_t attr;
  pthread_t *threads;
  struct thread_res *res;
  struct thread_res **times;
  // Track the stats for each of the measurements
  struct thread_res_stats stats_clock = THREAD_RES_STATS_INITIALIZER;
  struct thread_res_stats stats_user = THREAD_RES_STATS_INITIALIZER;
  struct thread_res_stats stats_sys = THREAD_RES_STATS_INITIALIZER;
  // Calculate the conversion factor from clock_t to seconds
  const long clocks_per_sec = sysconf(_SC_CLK_TCK);
  const double clocks_to_usec = 1000000 / (double)clocks_per_sec;
  // Get the number of CPUs and online CPUs
  const long num_online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

  // Get the parameters
  tinfo.pid = getpid();
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

  // Check if this sleep type actually sleeps
  does_sleep = !((sleep_type == SLEEP_TYPE_NONE) || (sleep_type == SLEEP_TYPE_YIELD));

  // Initialize the thread creation attributes
  s = pthread_attr_init(&attr);
  if (s != 0) {
    printf("Error initializing thread attributes\n");
    return -2;
  }

  // Allocate the memory to track the threads
  threads = calloc(num_threads, sizeof(*threads));
  times = calloc(num_threads, sizeof(*times));
  if (threads == NULL) {
    printf("Error allocating memory to track threads\n");
    return -3;
  }

  // Initialize the number of samples
  num_samples = 0;
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

    // Wait for all the threads to finish
    for (tnum=0; tnum<num_threads; ++tnum) {
      s = pthread_join(threads[tnum], (void **)(&res));

      if (s != 0) {
        printf("Error waiting for thread\n");
        return -6;
      }

      // Save the result for processing when they're all done
      times[tnum] = res;
    }

    // Calculate the scalar for the clock
    // NOTE: The purpose of this scalar is to "flatten" the time measured by
    // gettimeofday so it needs to account for the initial flat part of the
    // curve that occurs when there are enough CPUs to handle all of the threads
    // and the linear part of the curve that starts once there are more threads
    // than CPUs
    double clock_scalar;
    // If this sleep type actually sleeps (i.e. doubles the wall time)
    if (does_sleep) {
      // If there's enough CPUs for half the threads to be working and half to be sleeping
      if (num_threads <= 2 * num_online_cpus) {
        // Then the scalar should be 1/2 to account for the sleeping
        clock_scalar = 0.5;
      } else {
        // Otherwise, account for the fact that there are more than 2x more threads than CPUs
        clock_scalar = num_online_cpus / (double)num_threads;
      }
    } else {
      // If there's enough CPUs for all the threads to be working
      if (num_threads <= num_online_cpus) {
        // Then the scalar should be 1 because there's no sleeping
        clock_scalar = 1;
      } else {
        // Otherwise, acount for the fact that there are more threads than CPUs
        clock_scalar = num_online_cpus / (double)num_threads;
      }
    }
    // For each of the threads
    for (tnum=0; tnum<num_threads; ++tnum) {
      // Increment the number of samples in the statistics
      ++num_samples;
      // Update the statistics with this measurement
      update_stats(&stats_clock, times[tnum]->clock * clock_scalar, num_samples, tinfo.num_iterations, 1);
      update_stats(&stats_user, times[tnum]->user, num_samples, tinfo.num_iterations, clocks_to_usec);
      update_stats(&stats_sys, times[tnum]->sys, num_samples, tinfo.num_iterations, clocks_to_usec);
      // And clean it up
      free(times[tnum]);
    }
  }

  // Clean up the thread creation attributes
  s = pthread_attr_destroy(&attr);
  if (s != 0) {
    printf("Error cleaning up thread attributes\n");
    return -5;
  }

  // Finish the calculation of the standard deviation
  stats_clock.stddev = sqrtf(stats_clock.stddev / (num_samples - 1));
  stats_user.stddev = sqrtf(stats_user.stddev / (num_samples - 1));
  stats_sys.stddev = sqrtf(stats_sys.stddev / (num_samples - 1));

  // Print out the statistics of the times
  print_stats("gettimeofday_per_iteration", &stats_clock);
  print_stats("utime_per_iteration", &stats_user);
  print_stats("stime_per_iteration", &stats_sys);

  // Clean up the allocated threads and times
  free(threads);
  free(times);

  return 0;
}
