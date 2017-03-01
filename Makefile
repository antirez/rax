DEBUG=-g -ggdb
OPT=-O2

all: test-radixtree

test-radixtree: radixtree.c radixtree.h
	$(CC) $(DEBUG) $(OPT) -Wall -W --std=c99 -o test-radixtree radixtree.c -DTEST_MAIN -g -ggdb

clean:
	rm -f test-radixtree
