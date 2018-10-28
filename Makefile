CFLAGS := -g -Wall -O2 -I/home/kemi/git/lib/include
LIBS := -lpthread -lm -L/home/kemi/git/lib/lib -Wl,-rpath,/home/kemi/git/lib/lib -Wl,--dynamic-linker=/home/kemi/git/lib/lib/ld-linux-x86-64.so.2

target:
	$(CC) $(CFLAGS) -o tst-spin-hashwork tst-spin-hashwork.c $(LIBS)
	$(CC) $(CFLAGS) -o tst-spin-shared-variable tst-spin-shared-variable.c $(LIBS)

clean:
	rm -f tst-spin-hashwork tst-spin-shared-variable 
