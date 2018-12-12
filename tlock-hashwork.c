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
#include <errno.h>
#include <math.h>

#define force_inline __attribute__((always_inline))

#define roundup(x,y) (((x) + (y) - 1)  & ~((typeof(x))(y) - 1))
#define cache_new(ptr) posix_memalign((void **)&(ptr), 64, roundup(sizeof(*(ptr)), 64))
#define array_size(array) sizeof(array)/sizeof(array[0]) 

#define pause() asm volatile("rep ; nop" ::: "memory")
#define mb() asm volatile("":::"memory")

#define CACHE_ALIGNED __attribute__((aligned(64)))
#define constant_time 5

int sys_numthreads;
int nums;
int workset[4] = {1, 5, 10, 20};

struct SHA1_CTX
{
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
};

typedef struct SHA1_CTX SHA1_CTX;

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

struct ops {
 	void *(*test)(void *arg);
 	void (*aggregate)(void **, int);
} *ops;

#define HASHBUF 20
unsigned long long hashtime;

struct hashwork_result {
	unsigned long hashes;
};

/* Calibrate single threaded hash work */
void hashwork_init(void);
void *hashwork(void *arg);
void hashwork_aggregate(void **r, int num);

void test_threads(int numthreads);

#define iterations INT_MAX

static volatile int start_thread;
static volatile int stop_flag;

/* Delay some fixed time.  */
static inline void delay_tsc (unsigned long n)
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

static inline void wait_a_bit(int delay_time)
{
	if (delay_time > 0)
		delay_tsc (delay_time);
}

/* Each iteration of hashwork is about 250 TSCs.  */
static inline void wait_random_time ()
{
	wait_a_bit ((rand() % 100) * 20 * nums);
}

/* Hashwork */
/* SHA-1 in C
   By Steve Reid <sreid@sea-to-sky.net> 100% Public Domain.  */

#define rol rotl32
static inline __attribute__((always_inline)) uint32_t rotl32 ( uint32_t x, int8_t r )
{
	return (x << r) | (x >> (32 - r));
}
#ifdef WORDS_BIGENDIAN
#define blk0(i) block->l[i]
#else
#define blk0(i) (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) | (rol(block->l[i],8)&0x00FF00FF))
#endif
#define blk(i) (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] ^ block->l[(i+2)&15]^block->l[i&15],1))

/* (R0+R1), R2, R3, R4 are the different operations used in SHA1 */
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);

/* Hash a single 512-bit block. This is the core of the algorithm. */
static void SHA1_Transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t a, b, c, d, e;
    typedef union {
        uint8_t c[64];
        uint32_t l[16];
    } CHAR64LONG16;
    CHAR64LONG16* block;

    block = (CHAR64LONG16*)buffer;

    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);

    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;

    /* Wipe variables */
    a = b = c = d = e = 0;
}

/* SHA1Init - Initialize new context */
static void SHA1_Init(SHA1_CTX* context)
{
    /* SHA1 initialization constants */
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = 0;
  	context->count[1] = 0;
}

/* Run your data through this. */
static void SHA1_Update(SHA1_CTX* context, const uint8_t* data, const size_t len)
{
    size_t i, j;

    j = (context->count[0] >> 3) & 63;
    if ((context->count[0] += len << 3) < (len << 3)) context->count[1]++;

    context->count[1] += (len >> 29);

    if ((j + len) > 63) 
  {
        memcpy(&context->buffer[j], data, (i = 64-j));
        SHA1_Transform(context->state, context->buffer);

        for ( ; i + 63 < len; i += 64) 
    {
            SHA1_Transform(context->state, data + i);
        }

        j = 0;
    }
    else i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}

/*void hashwork_init(void)
{
	char buf[HASHBUF];
	SHA1_CTX ctx;

	memset(buf, 0x1, HASHBUF);
	SHA1_Init(&ctx);

	int i;
	unsigned int aux;
	for (i = 0; i < 10; i++)
		SHA1_Update(&ctx, (uint8_t *)buf, HASHBUF);
	unsigned long long a = __builtin_ia32_rdtscp(&aux);
	for (i = 0; i < 10000; i++) {
		SHA1_Update(&ctx, (uint8_t *)buf, HASHBUF);
	}
	unsigned long long b = __builtin_ia32_rdtscp(&aux);
	hashtime = (b-a) / 10000;
	printf("hashtime:%llu TSCs\n", hashtime);
}
*/

void *hashwork(void *arg)
{
	int i, j;
	struct hashwork_result *res;
	if (cache_new(res) != 0) {
		printf("memory allocation failure, %s\n", strerror(errno));
		exit(errno);
	}
	long num = 0;
	char buf[HASHBUF];
	SHA1_CTX ctx;

	res->hashes = 0;

	memset(buf, 0x1, HASHBUF);
	SHA1_Init(&ctx);

	while (!start_thread)
		pause();

	for (i = 0; i < iterations && !stop_flag; i++) {
		lock();
		for (j = 0; j < nums; j++)
			SHA1_Update(&ctx, (uint8_t *)buf, HASHBUF);
		unlock();
		wait_random_time ();
		num++;
	}
	res->hashes = num;

	return res;
}

void hashwork_aggregate(void **r, int num)
{
	struct hashwork_result **res = (struct hashwork_result **)r;
	int i;
	unsigned long hashes = 0;

	for (i = 0; i < num; i++) {
		hashes += res[i]->hashes;
	}
	printf("%d threads, total iterations: %lu\n", num, hashes);
}

void test_threads(int numthreads)
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
		(void)CPU_SET(i % sys_numthreads, &set);
		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &set);
		pthread_create(&thr[i], &attr, 
			       ops->test,
			       (void *)(long)i);
	}

	mb();
	start_thread = 1;
	mb();
	sched_yield();

	struct timespec ts = { 
		ts.tv_sec = constant_time,
		ts.tv_nsec = 0
	};
	clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
	mb();
	stop_flag = 1;
	
	for (i = 0; i < numthreads; i++)
		pthread_join(thr[i], (void **)&res[i]);

	ops->aggregate(res, numthreads);
}

struct ops hashwork_ops = {
	.test = hashwork,
	.aggregate = hashwork_aggregate,
};

struct ops *ops;

int main(int ac, char **av)
{
	int i, j;
	sys_numthreads = sysconf(_SC_NPROCESSORS_ONLN);
	ops = &hashwork_ops;

	for (j = 0; j < array_size(workset); j++)
	{
		printf("Run hashwork in %d seconds with %dx worksize, print total \
iterations below:\n", constant_time, workset[j]);
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
