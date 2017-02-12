all: test-trie

test-trie: trie.c trie.h
	$(CC) -O2 -Wall -W --std=c99 -o test-trie trie.c -DTEST_MAIN

clean:
	rm -f test-trie
