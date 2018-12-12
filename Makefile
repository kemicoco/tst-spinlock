CFLAGS := -g -Wall -O2 -I/home/kemi/git/lib/include
LIBS := -lpthread -lm -L/home/kemi/git/lib/lib -Wl,-rpath,/home/kemi/git/lib/lib -Wl,--dynamic-linker=/home/kemi/git/lib/lib/ld-linux-x86-64.so.2

HASHWORK_TARGET := hashwork-pthread-spinlock hashwork-pthread-adaptive hashwork-pthread-queuespinner
CMPXCHG_TARGET := cmpxchg-pthread-spinlock cmpxchg-pthread-adaptive cmpxchg-pthread-queuespinner

all: ${HASHWORK_TARGET} ${CMPXCHG_TARGET}

hashwork-%: tlock-hashwork.c
	$(CC) $(CFLAGS) -DLOCK_TYPE=${TYPE} -o $@ $< $(LIBS)

cmpxchg-%: tlock-cmpxchg.c
	$(CC) $(CFLAGS) -DLOCK_TYPE=${TYPE} -o $@ $< $(LIBS)

hashwork-pthread-spinlock: TYPE=PTHREAD_SPINLOCK
hashwork-pthread-adaptive: TYPE=PTHREAD_ADAPTIVE_MUTEX
hashwork-pthread-queuespinner: TYPE=PTHREAD_QUEUESPINNER_MUTEX

cmpxchg-pthread-spinlock: TYPE=PTHREAD_SPINLOCK
cmpxchg-pthread-adaptive: TYPE=PTHREAD_ADAPTIVE_MUTEX
cmpxchg-pthread-queuespinner: TYPE=PTHREAD_QUEUESPINNER_MUTEX
clean:
	rm -f ${HASHWORK_TARGET} ${CMPXCHG_TARGET}
