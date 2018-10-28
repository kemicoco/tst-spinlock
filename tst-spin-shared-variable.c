#define _GNU_SOURCE 1
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <limits.h>
#include <getopt.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

#define u32 unsigned 
#define u64 unsigned long long
#define force_inline __attribute__((always_inline))

static unsigned long shared_variable = 0;

#define roundup(x,y) (((x) + (y) - 1)  & ~((typeof(x))(y) - 1))
#define cache_new(ptr) posix_memalign((void **)&(ptr), 64, roundup(sizeof(*(ptr)), 64))

#define pause() asm volatile("rep ; nop" ::: "memory")
#define mb() asm volatile("":::"memory")

#define CACHE_ALIGNED __attribute__((aligned(64)))
#define constant_time 5

#define NUM_HASHES 1

/* The time consumed by one hash update is about 28ns, tested on 2 sockets
 * SKylake platform
 */
static int delay_time_unlocked = 150;

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

struct ops {
 	void *(*test)(void *arg);
 	void (*aggregate)(void **, int);
	void (*print_thread)(void *res, int);
} *ops;

struct stats_result {
	unsigned long num;
};

void summary(void *r, int cpu);
void *work_thread(void *arg);
void aggregate(void **r, int num);

void test_threads(int numthreads, unsigned long time);

#define iterations LONG_MAX

static volatile int start_thread;
static volatile int stop_flag;

/* CPU cycles calculation */
static inline force_inline unsigned long long rdtsc(void)
{
#ifdef __i386__
	u64 s;
	asm volatile("rdtsc" : "=A" (s) :: "memory");
	return s;
#else
	u32 low, high;
	asm volatile("rdtsc" : "=a" (low), "=d" (high) :: "memory");
	return ((u64)high << 32) | low;
#endif
}

static inline force_inline unsigned long long rdtscp(void)
{
#ifdef DISABLE_RDTSCP
	return rdtsc();
#endif
#ifdef __i386__
	u64 s;
	asm volatile("rdtscp" : "=A" (s) :: "ecx", "memory");
	return s;
#else
	u32 low, high;
	asm volatile("rdtscp" : "=a" (low), "=d" (high) :: "ecx", "memory");
	return ((u64)high << 32) | low;
#endif
}

static FILE *account_fh;

static void __attribute__((constructor)) fh_init(void)
{
	account_fh = stdout;
}

static void print_field(unsigned long num, char *field)
{
	fprintf(account_fh, "%lu %s ", num, field);
}

union pad {
	double frequency;
	char pad[64];
};

static union pad freq __attribute__((aligned(64)));
#define frequency freq.frequency

/* Delay some fixed time */
static void delay_ns(unsigned n)
{
	u64 start = rdtsc();
	while (rdtsc() < start + (u64)(n * frequency)) {
		pause();
	}
}

static void init_delay(void)
{
     FILE *f = fopen("/proc/cpuinfo", "r");
     if (!f)
	  goto fallback;

     char *line = NULL;
     size_t linelen = 0;
     frequency = 0;
     while (getline(&line, &linelen, f) > 0) {
	  char unit[10];

	  if (strncmp(line, "model name", sizeof("model name")-1))
	       continue;
	  if (sscanf(line + strcspn(line, "@") + 1, "%lf%10s", 
		     &frequency, unit) == 2) {
	       if (!strcasecmp(unit, "GHz"))
		    ;
	       else if (!strcasecmp(unit, "Mhz"))
		    frequency *= 1000;
	       else {
		    printf("Cannot parse unit %s\n", unit);
		    goto fallback;
	       }
	       break;
	  }
     }     
     free(line);
     fclose(f);
     if (frequency) {
	  return;
     }
    
fallback:
     f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
     int found = 0;
     if (f) {
         found = fscanf(f, "%lf", &frequency);
	 fclose(f);
     }
     if (found == 1) {
         frequency /= 1000000.0;
         return;
     }
     printf("Cannot parse. Using 2Ghz fallback\n");
     frequency = 2.0; /* 2Ghz likely wrong*/
}

static void wait_a_bit(int delay_time)
{
	if (delay_time > 0)
		delay_ns(delay_time);
}

void *work_thread(void *arg)
{
	long i;
	struct stats_result *res;
	if (cache_new(res) != 0) {
		printf("memory allocation failure, %s\n", strerror(errno));
		exit(errno);
	}
	long num = 0;

	while (!start_thread)
		pause();

	for (i = 0; i < iterations && !stop_flag; i++) {
		lock();
		shared_variable++;
		shared_variable++;
		shared_variable--;
		
		shared_variable++;
		shared_variable++;
		shared_variable--;
		
		shared_variable++;
		shared_variable++;
		shared_variable--;
		
		shared_variable++;
		shared_variable++;
		shared_variable--;
		
		shared_variable++;
		shared_variable++;
		shared_variable--;
		unlock();
		wait_a_bit(delay_time_unlocked);
		num++;
	}
	res->num = num;

	return res;
}

void aggregate(void **r, int num)
{
	struct stats_result **res = (struct stats_result **)r;
	int i;
	unsigned long nlocks = 0;

	for (i = 0; i < num; i++)
		nlocks += res[i]->num;
	print_field(num, "threads");
	print_field(nlocks/num, "num");
	puts("");
}

void summary(void *r, int num)
{
	struct stats_result *result = (struct stats_result *)r;

	print_field(num, "thread");
	print_field(result->num, "num");
	puts("");
}

void test_threads(int numthreads, unsigned long time)
{
	start_thread = 0;
	stop_flag = 0;
	
	mb();

	pthread_t thr[numthreads];
	void *res[numthreads];
	int i;
	for (i = 0; i < numthreads; i++) {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		cpu_set_t set;
		CPU_ZERO(&set);
		(void)CPU_SET(i, &set);
		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &set);
		pthread_create(&thr[i], &attr, 
			       ops->test,
			       (void *)(long)i);
	}

	mb();
	start_thread = 1;
	mb();
	sched_yield();

	if (time) {
		struct timespec ts = { 
			ts.tv_sec = time,
			ts.tv_nsec = 0
		};
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
		mb();
		stop_flag = 1;
	}

	for (i = 0; i < numthreads; i++)
		pthread_join(thr[i], (void *)&res[i]);

	ops->aggregate(res, numthreads);

	for (i = 0; i < numthreads; i++)
		ops->print_thread(res[i], i);
}

struct ops hashwork_ops = {
	.test = work_thread,
	.aggregate = aggregate,
	.print_thread = summary,
};

struct ops *ops;

static struct option options[] = {
	{ "thread", 0, NULL, 'n' },
	{ NULL },
};

int main(int ac, char **av)
{
	int opt;
	int numthreads = sysconf(_SC_NPROCESSORS_ONLN);
	ops = &hashwork_ops;

	init_delay();

	while ((opt = getopt_long(ac, av, "n:", options, NULL)) != -1) {
		switch (opt) {
		case 'n':
			if (atoi(optarg) > numthreads) {
				printf("Please use the number of threads less than %d for testing\n", numthreads);
				exit(0);
			}
			else
				numthreads = atoi(optarg);
			break;
		default:
			fprintf(stderr, "tst-spin options ...\n"
				"--thread -n  thread number\n");
			exit(1);
		}
	}

	if (numthreads > 0) 
		test_threads(numthreads, constant_time);

	return 0;
}
