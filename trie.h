#ifndef TRIE_H
#define TRIE_H

#include <stdint.h>

typedef struct trieNode {
    uint32_t iskey:1;
    uint32_t children:31; /* Number of children, or string len if leaf. */
    /* If it's a leaf, data layout is: leaf tail string + void ptr to store
     * data associated with the key.
     *
     * If it's an inner node data layout is: N bytes (one for each children)
     * + N trieNode pointers. */
    unsigned char data[];
} trieNode;

typedef struct trie {
    trieNode *head;
    uint64_t numele;
} trie;

extern void *trieNotFound;

#endif
