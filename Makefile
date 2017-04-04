DEBUG?= -g -ggdb
CFLAGS?= -O2 -Wall -W -std=c99

# CFLAGS+=-fprofile-arcs -ftest-coverage
# LDFLAGS+=-lgcov

PRGNAME = visitors

all: rax-test rax-oom-test

rax.o: rax.h
rax-test.o: rax.h

rax-test: rax-test.o rax.o
	$(CC) -o rax-test $(CFLAGS) $(LDFLAGS) $(DEBUG) rax-test.o rax.o

rax-oom-test: rax-oom-test.o rax.o
	$(CC) -o rax-oom-test $(CFLAGS) $(LDFLAGS) $(DEBUG) rax-oom-test.o rax.o

.c.o:
	$(CC) -c $(CFLAGS) $(DEBUG) $<

clean:
	rm -f rax-test rax-oom-test *.gcda *.gcov *.gcno *.o
