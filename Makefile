CFLAGS := -g -Wall -O2 -I/home/kemi/git/lib/include
LIBS := -lpthread -lm -L/home/kemi/git/lib/lib -Wl,-rpath,/home/kemi/git/lib/lib -Wl,--dynamic-linker=/home/kemi/git/lib/lib/ld-linux-x86-64.so.2

target:
	$(CC) $(CFLAGS) -o tst-spin tst-spin.c $(LIBS)

clean:
	rm -f tst-spin 
