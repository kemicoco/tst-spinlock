#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/param.h>
#include <errno.h>

#define memory_barrier() __asm ("" ::: "memory")
#define pause() __asm  ("rep ; nop" ::: "memory")

#define CACHELINE_SIZE	64
#define CACHE_ALIGNED	__attribute__((aligned(CACHELINE_SIZE)))

#define constant_time 5

unsigned long g_val CACHE_ALIGNED;
unsigned long g_val2 CACHE_ALIGNED;
unsigned long g_val3 CACHE_ALIGNED;
unsigned long cmplock CACHE_ALIGNED;
struct count
{
  unsigned long long total;
  unsigned long long spinlock;
  unsigned long long wall;
} __attribute__((aligned(128)));

struct count *gcount;

/* The time consumed by one update is about 200 TSCs.  */
static int delay_time_unlocked = 200;

struct
{
  pthread_spinlock_t testlock;
  char pad[64];
} test CACHE_ALIGNED;

#define lock() pthread_spin_lock(&test.testlock)
#define unlock() pthread_spin_unlock(&test.testlock)

static void
__attribute__((constructor))
init_spin (void)
{
  pthread_spin_init (&test.testlock, 0);
}

struct ops
{
  void *(*test) (void *arg);
  void (*aggregate) (void **, int);
  void (*print_thread) (void *res, int);
} *ops;

struct stats_result
{
  unsigned long num;
};

void summary (void *r, int cpu);
void *work_thread (void *arg);
void aggregate (void **r, int num);

void test_threads (int numthreads, unsigned long time);

#define iterations (10000 * 5)

static volatile int start_thread;
static volatile int stop_flag;

static FILE *account_fh;

static void
__attribute__((constructor))
fh_init (void)
{
  account_fh = stdout;
}

static void
print_field (unsigned long num, const char *field)
{
  fprintf(account_fh, "%lu %s ", num, field);
}

/* Delay some fixed time */
static void
delay_tsc (unsigned n)
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

static void
wait_a_bit (int delay_time)
{
  if (delay_time > 0)
    delay_tsc (delay_time);
}

void *
work_thread (void *arg)
{
  long i;
  unsigned long pid = (unsigned long) arg;
  unsigned long ret;
  struct stats_result *res;
  unsigned long long diff, start, end;

  if (posix_memalign ((void **)&res, CACHELINE_SIZE,
		      roundup (sizeof (*res), CACHELINE_SIZE)) != 0)
    {
      printf ("memory allocation failure, %s\n", strerror (errno));
      exit (errno);
    }
  long num = 0;

  while (!start_thread)
    pause ();

  unsigned long long spinlock_start, spinlock_end;
  unsigned int aux;
  start = __builtin_ia32_rdtscp (&aux);
  for (i = 0; i < iterations; i++)
    {
      spinlock_start = __builtin_ia32_rdtscp (&aux);
      lock();
      spinlock_end = __builtin_ia32_rdtscp (&aux);

      g_val = g_val + 1;
      g_val2 = g_val2 + 1;
      ret = __sync_val_compare_and_swap (&cmplock, 0, 1);
      g_val3 = g_val3 + 1 + ret;

      unlock ();
      gcount[pid].spinlock += spinlock_end - spinlock_start;
      wait_a_bit (delay_time_unlocked);
      num++;
    }
  end = __builtin_ia32_rdtscp (&aux);
  diff = end - start;
  gcount[pid].total = diff;
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
  print_field (num, "threads");
  print_field (nlocks, "num");
  puts ("");
}

void
summary (void *r, int num)
{
  struct stats_result *result = (struct stats_result *)r;

  print_field (num, "thread");
  print_field (result->num, "num");
  puts ("");
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
test_threads (int numthreads, unsigned long time)
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
      (void)CPU_SET (i, &set);
      pthread_attr_setaffinity_np (&attr, sizeof(cpu_set_t), &set);
      pthread_create (&thr[i], &attr, ops->test, (void *)(long)i);
    }

  memory_barrier ();
  start_thread = 1;
  memory_barrier ();
  sched_yield ();

  if (time)
    {
      struct timespec ts =
	{
	  ts.tv_sec = time,
	  ts.tv_nsec = 0
	};
      clock_nanosleep (CLOCK_MONOTONIC, 0, &ts, NULL);
      memory_barrier ();
      stop_flag = 1;
    }

  for (i = 0; i < numthreads; i++)
    pthread_join (thr[i], (void *)&res[i]);

  ops->aggregate (res, numthreads);
}

struct ops hashwork_ops =
{
  .test = work_thread,
  .aggregate = aggregate,
  .print_thread = summary,
};

struct ops *ops;

static struct count
total_cost (int numthreads)
{
  int i;
  unsigned long long total = 0;
  unsigned long long spinlock = 0;

  memset (gcount, 0, sizeof(gcount[0]) * numthreads);

  unsigned long long start, end, diff;
  unsigned int aux;

  start = __builtin_ia32_rdtscp (&aux);
  test_threads (numthreads, constant_time);
  end = __builtin_ia32_rdtscp (&aux);
  diff = end - start;

  for (i = 0; i < numthreads; i++)
    {
      total += gcount[i].total;
      spinlock += gcount[i].spinlock;
    }

  struct count cost = { total, spinlock, diff };
  return cost;
}

int
main (void)
{
  int numthreads = sysconf (_SC_NPROCESSORS_ONLN);
  if (numthreads < 8)
    return 1;

  ops = &hashwork_ops;

  posix_memalign ((void **)&gcount, 4096,
		  sizeof(gcount[0]) * numthreads);

  struct count cost;
  int i;
  for (i = 1; i <= numthreads; i += i)
    {
      cost = total_cost (i);
      printf ("wall time (%4d threads): %lld\n", i, cost.wall);
    }

  if (i != numthreads)
    {
      cost = total_cost (numthreads);
      printf ("wall time (%4d threads): %lld\n", numthreads, cost.wall);
    }

  return 0;
}
