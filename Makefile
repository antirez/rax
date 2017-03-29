DEBUG=-g -ggdb
OPT=-O2

all: rax-test rax-oom-test rax-bench

rax-test: rax.c rax.h
	$(CC) $(DEBUG) $(OPT) -Wall -W --std=c99 -o rax-test rax.c -DTEST_MAIN -g -ggdb

rax-oom-test: rax.c rax.h rax-oom-test.c rax_oom_malloc.h
	$(CC) $(DEBUG) $(OPT) -Wall -W --std=c99 -o rax-oom-test rax.c rax-oom-test.c -DRAX_MALLOC_INCLUDE='"rax_oom_malloc.h"' -g -ggdb

rax-bench: rax.c rax.h
	$(CC) $(DEBUG) $(OPT) -Wall -W --std=c99 -o rax-bench rax.c -DBENCHMARK_MAIN -g -ggdb

clean:
	rm -f rax-test rax-bench
