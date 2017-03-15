DEBUG=-g -ggdb
OPT=-O2

all: rax-test rax-bench

rax-test: rax.c rax.h
	$(CC) $(DEBUG) $(OPT) -Wall -W --std=c99 -o rax-test rax.c -DTEST_MAIN -g -ggdb

rax-bench: rax.c rax.h
	$(CC) $(DEBUG) $(OPT) -Wall -W --std=c99 -o rax-bench rax.c -DBENCHMARK_MAIN -g -ggdb

clean:
	rm -f rax-test rax-bench
