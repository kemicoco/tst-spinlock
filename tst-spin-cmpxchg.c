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
#include <limits.h>

#define memory_barrier() __asm ("" ::: "memory")
#define pause() __asm  ("rep ; nop" ::: "memory")

#define CACHELINE_SIZE	64
#define CACHE_ALIGNED	__attribute__((aligned(CACHELINE_SIZE)))

unsigned long g_val CACHE_ALIGNED;
unsigned long g_val2 CACHE_ALIGNED;
unsigned long g_val3 CACHE_ALIGNED;
unsigned long cmplock CACHE_ALIGNED;

struct count
{
	unsigned long long total;
	unsigned long long diff;
} __attribute__((aligned(128)));

struct count *gcount __attribute__((aligned(128)));

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

/* The time consumed by one update is about 200 TSCs.  */
static int delay_time_unlocked = 600;

struct ops
{
	void *(*test) (void *arg);
} *ops;

void *work_thread (void *arg);

void test_threads (int numthreads);

#define iterations LONG_MAX
#define constant_time 3

static volatile int start_thread;
static volatile int stop_flag;

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

struct para {
	unsigned long *v1;
	unsigned long *v2;
	unsigned long *v3;
	unsigned long *v4;
};

void work_td(void *v)
{
	struct para *p = v;	
	unsigned int ret;
	*p->v1 = *p->v1 + 1;
	*p->v2 = *p->v2 + 1;
	ret = __sync_val_compare_and_swap (p->v4, 0, 1);
	*p->v3 = *p->v3 + ret;

}

	void *
work_thread (void *arg)
{
	long i, num = 0;
	unsigned long pid = (unsigned long) arg;
	unsigned long long  start, end;
	struct para p CACHE_ALIGNED;

	p.v1 = &g_val;
	p.v2 = &g_val2;
	p.v3 = &g_val3;
	p.v4 = &cmplock;

	while (!start_thread)
		pause ();

	//unsigned long long spinlock_start, spinlock_end,total = 0;
	unsigned int aux;
	start = __builtin_ia32_rdtscp (&aux);
	for (i = 0; stop_flag != 1 && i < iterations; i++)
	{
		lock();
		work_td(&p);
		unlock();
		wait_a_bit (delay_time_unlocked);
		num++;
	}
	end = __builtin_ia32_rdtscp (&aux);
	gcount[pid].diff = end - start;
	gcount[pid].total = num;

	return NULL;
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

	if (constant_time) {
		struct timespec ts = {
			ts.tv_sec = constant_time,
			ts.tv_nsec = 0
		};
		clock_nanosleep (CLOCK_MONOTONIC, 0, &ts, NULL);
		memory_barrier ();
		stop_flag = 1;
	}

	for (i = 0; i < numthreads; i++)
		pthread_join (thr[i], NULL);
}

struct ops global_ops =
{
	.test = work_thread,
};

struct ops *ops;

	static struct count
total_stats (int numthreads)
{
	int i;
	unsigned long long total = 0;

	memset (gcount, 0, sizeof(gcount[0]) * numthreads);

	unsigned long long start, end, diff;
	unsigned int aux;

	start = __builtin_ia32_rdtscp (&aux);
	test_threads (numthreads);
	end = __builtin_ia32_rdtscp (&aux);
	diff = end - start;

	for (i = 0; i < numthreads; i++)
		total += gcount[i].total;

	struct count cost = { total, diff };
	return cost;
}

static int get_contented_thread_num (int numthreads, struct count *count)
{
	int i;
	struct count res;
	
	res = total_stats (1);
	count->total = res.total;
	count->diff = res.diff;
	for (i = 2; i <= numthreads; i++)
	{
		res = total_stats (i);
		if (res.total > count->total) {
			count->total = res.total;
			count->diff = res.diff;
		}
		else
			break;
	}

	return i;
}

	int
main (void)
{
	int numthreads = sysconf (_SC_NPROCESSORS_ONLN);
	if (numthreads < 8)
		return 1;

	ops = &global_ops;

	if (posix_memalign ((void **)&gcount, 4096,
			sizeof(gcount[0]) * numthreads))
		exit(-1);

	struct count curr, best;
	double diff;
	int i, contention, last = numthreads;

	contention = get_contented_thread_num (numthreads, &best);
	printf("Best result with thread number: %d, total iterations: %lld\n",
			contention - 1, best.total);

	for (i = contention; i <= numthreads; i++)
	{
		last = i;
		curr = total_stats (i);
		diff = best.total - curr.total;
		if (diff < 0)
			continue;
		diff /= best.total;
		diff *= 100;
		printf (" threads number:%4d : %16lld, %0.1f%%\n",
				i, curr.total, diff);
		if ((i* 2)< numthreads)
			i = i * 2;
		else
			i = i + 16;
	}

	if (last != numthreads) {
		i = numthreads;
		curr = total_stats (i);
		diff = best.total - curr.total;
		diff /= best.total;
		diff *= 100;
		printf (" threads number:%4d : %16lld, %0.1f%%\n",
			i, curr.total, diff);
	}
	return 0;
}
