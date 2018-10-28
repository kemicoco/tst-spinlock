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
#include <math.h>
#include <stdarg.h>
#include <ctype.h>

#define u32 unsigned 
#define u64 unsigned long long
#define force_inline __attribute__((always_inline))

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
static int delay_time_unlocked = 250;

struct SHA1_CTX
{
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
};

typedef struct SHA1_CTX SHA1_CTX;

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
} *ops;

#define HASHBUF 20
u64 hashtime;

struct hashwork_result {
	unsigned long hashes;
};

/* Calibrate single threaded hash work */
void hashwork_init(void);
void *hashwork(void *arg);
void hashwork_aggregate(void **r, int num);

void test_threads(int numthreads, unsigned long time);

#define iterations INT_MAX

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

/* Hashwork */
/*
SHA-1 in C
By Steve Reid <sreid@sea-to-sky.net>
100% Public Domain

-----------------
Modified 7/98
By James H. Brown <jbrown@burgoyne.com>
Still 100% Public Domain

Corrected a problem which generated improper hash values on 16 bit machines
Routine SHA1Update changed from
  void SHA1Update(SHA1_CTX* context, unsigned char* data, unsigned int
len)
to
  void SHA1Update(SHA1_CTX* context, unsigned char* data, unsigned
long len)

The 'len' parameter was declared an int which works fine on 32 bit machines.
However, on 16 bit machines an int is too small for the shifts being done
against
it.  This caused the hash function to generate incorrect values if len was
greater than 8191 (8K - 1) due to the 'len << 3' on line 3 of SHA1Update().

Since the file IO in main() reads 16K at a time, any file 8K or larger would
be guaranteed to generate the wrong hash (e.g. Test Vector #3, a million
"a"s).

I also changed the declaration of variables i & j in SHA1Update to
unsigned long from unsigned int for the same reason.

These changes should make no difference to any 32 bit implementations since
an
int and a long are the same size in those environments.

--
I also corrected a few compiler warnings generated by Borland C.
1. Added #include <process.h> for exit() prototype
2. Removed unused variable 'j' in SHA1Final
3. Changed exit(0) to return(0) at end of main.

ALL changes I made can be located by searching for comments containing 'JHB'
-----------------
Modified 8/98
By Steve Reid <sreid@sea-to-sky.net>
Still 100% public domain

1- Removed #include <process.h> and used return() instead of exit()
2- Fixed overwriting of finalcount in SHA1Final() (discovered by Chris Hall)
3- Changed email address from steve@edmweb.com to sreid@sea-to-sky.net

-----------------
Modified 4/01
By Saul Kravitz <Saul.Kravitz@celera.com>
Still 100% PD
Modified to run on Compaq Alpha hardware.

-----------------
Modified 07/2002
By Ralph Giles <giles@ghostscript.com>
Still 100% public domain
modified for use with stdint types, autoconf
code cleanup, removed attribution comments
switched SHA1Final() argument order for consistency
use SHA1_ prefix for public api
move public api to sha1.h
*/

/*
Test Vectors (from FIPS PUB 180-1)
"abc"
  A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
A million repetitions of "a"
  34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
*/

/* public api for steve reid's public domain SHA-1 implementation */
/* this file is in the public domain */

#define rol rotl32
static inline __attribute__((always_inline)) uint32_t rotl32 ( uint32_t x, int8_t r )
{
	  return (x << r) | (x >> (32 - r));
}
/* blk0() and blk() perform the initial expand. */
/* I got the idea of expanding during the round function from SSLeay */
/* FIXME: can we do this in an endian-proof way? */

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

void *hashwork(void *arg)
{
	int i;
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
		int j;
		for (j = 0; j < NUM_HASHES; j++) {
			SHA1_Update(&ctx, (uint8_t *)buf, HASHBUF);
			res->hashes++;
		}
		unlock();
		wait_a_bit(delay_time_unlocked);
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
	print_field(num, "threads,");
	print_field(hashes, "total hashes,");
	print_field(hashes/num, "hashes per thread");
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
}

struct ops hashwork_ops = {
	.test = hashwork,
	.aggregate = hashwork_aggregate,
};

struct ops *ops;

int main(int ac, char **av)
{
	int i;
	int numthreads = sysconf(_SC_NPROCESSORS_ONLN);
	ops = &hashwork_ops;

	init_delay();
	
	printf("Run hashwork in %d seconds, print statistics below:\n",	constant_time);
	
	for (i = 1; i < numthreads;)
	{
		test_threads(i, constant_time);
		if (i > 6)
			i += i;
		else
			i++;
	}

	test_threads(numthreads, constant_time);
	return 0;
}
