#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <limits.h>
#include <sys/param.h>
#include <errno.h>

#define memory_barrier() __asm ("" ::: "memory")
#define pause() __asm  ("rep ; nop" ::: "memory")

#define CACHELINE_SIZE	64
#define CACHE_ALIGNED	__attribute__((aligned(CACHELINE_SIZE)))
#define array_size(array) sizeof(array)/sizeof(array[0])

#define constant_time 5
#define iterations INT_MAX
int sys_numthreads;
int nums;
int workset[4] = {1, 5, 10, 20};

unsigned long g_val CACHE_ALIGNED;
unsigned long g_val2 CACHE_ALIGNED;
unsigned long g_val3 CACHE_ALIGNED;
unsigned long cmplock CACHE_ALIGNED;

struct count *gcount;

#define PTHREAD_SPINLOCK 1
#define PTHREAD_ADAPTIVE_MUTEX 2
#define PTHREAD_QUEUESPINNER_MUTEX 3

#ifndef LOCK_TYPE
#define LOCK_TYPE PTHREAD_SPINLOCK
#endif

#if LOCK_TYPE == PTHREAD_SPINLOCK
struct {
	pthread_spinlock_t testlock;
	char pad[64];
} test CACHE_ALIGNED;
#define lock() pthread_spin_lock(&test.testlock)
#define unlock() pthread_spin_unlock(&test.testlock)

static void __attribute__((constructor)) init_spin(void)
{
	pthread_spin_init(&test.testlock, 0);
}
#endif

#if LOCK_TYPE == PTHREAD_ADAPTIVE_MUTEX
struct {
	pthread_mutex_t testlock;
	char pad[64];
} test CACHE_ALIGNED = {
	.testlock = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
};

#define lock() pthread_mutex_lock(&test.testlock)
#define unlock() pthread_mutex_unlock(&test.testlock)
#endif

#if LOCK_TYPE == PTHREAD_QUEUESPINNER_MUTEX
struct {
	pthread_mutex_t testlock;
	char pad[64];
} test CACHE_ALIGNED = {
	.testlock = PTHREAD_QUEUESPINNER_MUTEX_INITIALIZER_NP
};

#define lock() pthread_mutex_lock(&test.testlock)
#define unlock() pthread_mutex_unlock(&test.testlock)
#endif

struct ops
{
  void *(*test) (void *arg);
  void (*aggregate) (void **, int);
} *ops;

struct stats_result
{
  unsigned long num;
};

void *work_thread (void *arg);
void aggregate (void **r, int num);

void test_threads (int numthreads);

static volatile int start_thread;
static volatile int stop_flag;

/* Delay some fixed time */
static inline void 
delay_tsc (unsigned long n)
{
  unsigned long long start, current, diff;
  unsigned int aux;
  start = __builtin_ia32_rdtscp (&aux);
  while (1)
    {
      current = __builtin_ia32_rdtscp (&aux);
      diff = current - start;
      if (diff < n)
	pause ();
      else
	break;
    }
}

static inline void
wait_a_bit (int delay_time)
{
  if (delay_time > 0)
    delay_tsc (delay_time);
}

static inline void wait_random_time ()
{
	wait_a_bit ((rand() % 100) * 20 * nums);
}

void *
work_thread (void *arg)
{
  long i, j;
  unsigned long ret;
  struct stats_result *res;

  if (posix_memalign ((void **)&res, CACHELINE_SIZE,
		      roundup (sizeof (*res), CACHELINE_SIZE)) != 0)
    {
      printf ("memory allocation failure, %s\n", strerror (errno));
      exit (errno);
    }
  long num = 0;

  while (!start_thread)
    pause ();

  for (i = 0; i < iterations && !stop_flag; i++)
    {
      lock();
	  for (j = 0; j < nums; j++)
	  {
      	g_val = g_val + 1;
      	g_val2 = g_val2 + 1;
      	ret = __sync_val_compare_and_swap (&cmplock, 0, 1);
      	g_val3 = g_val3 + 1 + ret;
	  }
      unlock ();
      wait_random_time ();
      num++;
    }
  res->num = num;

  return res;
}

void
aggregate (void **r, int num)
{
  struct stats_result **res = (struct stats_result **)r;
  int i;
  unsigned long nlocks = 0;

  for (i = 0; i < num; i++)
    nlocks += res[i]->num;
  printf("%d threads, total iterations: %lu\n", num, nlocks);
}

void
init_global_data(void)
{
  g_val = 0;
  g_val2 = 0;
  g_val3 = 0;
  cmplock = 0;
}

void
test_threads (int numthreads)
{
  start_thread = 0;
  stop_flag = 0;

  memory_barrier ();

  pthread_t thr[numthreads];
  void *res[numthreads];
  int i;

  init_global_data ();
  for (i = 0; i < numthreads; i++)
    {
      pthread_attr_t attr;
      pthread_attr_init (&attr);
      cpu_set_t set;
      CPU_ZERO (&set);
      (void)CPU_SET (i % sys_numthreads, &set);
      pthread_attr_setaffinity_np (&attr, sizeof(cpu_set_t), &set);
      pthread_create (&thr[i], &attr, ops->test, (void *)(long)i);
    }

  memory_barrier ();
  start_thread = 1;
  memory_barrier ();
  sched_yield ();

  struct timespec ts =
  {
    ts.tv_sec = constant_time,
    ts.tv_nsec = 0
  };
  clock_nanosleep (CLOCK_MONOTONIC, 0, &ts, NULL);
  memory_barrier ();
  stop_flag = 1;

  for (i = 0; i < numthreads; i++)
    pthread_join (thr[i], (void **)&res[i]);

  ops->aggregate (res, numthreads);
}

struct ops cmpxchg_ops =
{
  .test = work_thread,
  .aggregate = aggregate,
};

struct ops *ops;

int
main (void)
{
  int i, j;
  sys_numthreads = sysconf (_SC_NPROCESSORS_ONLN);
  ops = &cmpxchg_ops;

  for (j = 0; j < array_size(workset); j++)
  {
  	printf("Run cmpxchg in %d seconds with %dx worksize, print statistics \
below:\n", constant_time, workset[j]);
  	nums = workset[j];
  for (i = 1; i < sys_numthreads;)
  {
  	test_threads(i);
	if (i > 6)
   	  i += i;
	else
	  i++;
  }

  test_threads(sys_numthreads);
  test_threads(4 * sys_numthreads);
  }
  return 0;
}
