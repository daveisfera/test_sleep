#include <float.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>


// Uncomment to use select inside the loop of each of the threads
//#define SLEEP_TYPE 1
// Uncomment to use poll inside the loop of each of the threads
//#define SLEEP_TYPE 2
// Uncomment to use usleep inside the loop of each of the threads
//#define SLEEP_TYPE 3


struct thread_info {
  int sleep_time;
  int num_iterations;
  int work_size;
};

void *do_test(void *arg)
{
  const struct thread_info *tinfo = (struct thread_info *)arg;

  const int sleep_time = tinfo->sleep_time;
  const int num_iterations = tinfo->num_iterations;
  const int work_size = tinfo->work_size;

#if SLEEP_TYPE == 1
  // Data for calling select
  struct timeval ts;
#elif SLEEP_TYPE == 2
  // Data for calling poll
  struct pollfd pfd;
#endif
  // Data for doing work
  int *buf;
  int pseed;
  int inum, bnum;
  // Data for tracking the time
  struct timeval before, after;
  long long *diff;

  buf = calloc(work_size, sizeof(int));
  diff = malloc(sizeof(unsigned long long));

#if SLEEP_TYPE == 2
  // Initialize the poll data
  pfd.fd = 0;
  pfd.events = 0;
#endif

  // Get the time before starting the processing
  gettimeofday(&before, NULL);

  // Do the requested number of iterations
  for (inum=0; inum<num_iterations; ++inum) {
#if SLEEP_TYPE == 1
    ts.tv_sec = 0;
    ts.tv_usec = sleep_time;
    select(0, 0, 0, 0, &ts);
#elif SLEEP_TYPE == 2
    poll(&pfd, 1, sleep_time / 1000);
#elif SLEEP_TYPE == 3
    usleep(sleep_time);
#else
    // Get rid of warning about unused variable
    (void)sleep_time;
#endif

    // Fill in a buffer with random numbers (taken from latt.c by Jens Axboe <jens.axboe@oracle.com>)
    pseed = 1;
    for (bnum=0; bnum<work_size; ++bnum) {
      pseed = pseed * 1103515245 + 12345;
      buf[bnum] = (pseed / 65536) % 32768;
    }
  }

  // Get the time after starting the processing
  gettimeofday(&after, NULL);

  // Calculate the delta time
  *diff = 1000000LL * (after.tv_sec - before.tv_sec);
  *diff += after.tv_usec - before.tv_usec;

  // Clean up the data
  free(buf);

  return diff;
}

int main(int argc, char **argv)
{
  if (argc < 5) {
    printf("Usage: %s <sleep_time> <outer_iterations> <inner_iterations> <work_size> <num_threads>\n", argv[0]);
    return -1;
  }

  struct thread_info tinfo;
  int outer_iterations;
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
