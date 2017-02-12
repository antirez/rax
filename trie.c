#include <stdlib.h>
#include <string.h>
#include "trie.h"

#ifdef TEST_MAIN
#include <stdio.h>

trieNode *trieNew(void) {
    trieNode *node = malloc(sizeof(*node));
    node->iskey = 0;
    node->children = 0;
}

/* Realloc the node to make room for auxiliary data in order
 * to store an item in that node. */
trieNode *trieReallocForData(trieNode *n) {
    size_t curlen = sizeof(trieNode)+
                    h->children+
                    sizeof(trieNode*)*h->children;
    return realloc(curlen+sizeof(void*));
}

/* Set the node auxiliary data to the specified pointer. */
void trieSetData(trieNode *n, void *data) {
    void **ndata = n->data+n->children+sizeof(trieNode*)*n->children;
    *ndata = data;
}

/* Get the node auxiliary data. */
void *trieGetData(trieNode *n) {
    void **ndata = n->data+n->children+sizeof(trieNode*)*n->children;
    return *ndata;
}

/* Add a new child to the node 'n' representing the character
 * 'c' and return its pointer. */
trieNode *trieAddChild(trieNode *n, char c, trieNode **childptr) {
    size_t curlen = sizeof(trieNode)+
                    h->children+
                    sizeof(trieNode*)*h->children;
    if (n->iskey) curlen += sizeof(void*);
    curlen += sizeof(trieNode*)+1; /* Add 1 char and 1 pointer. */
    n = realloc(n,curlen);
    trieNode *child = trieNew();
    /* After the reallocation, we have 5/9 (depending on the system
     * pointer size) bytes at the end:
     *
     * [numc][abc][ap][bp][cp]|auxp|.....
     *
     * Move all the tail pointers one byte on the left, to make
     * space for another character in the chars vector:
     *
     * [numc][abc].[ap][bp][cp]|auxp|....
     *
     * Now, if present, move auxiliary data pointer at the end
     * so that we can store the additional child pointer without
     * overwriting it:
     *
     * [numc][abc].[ap][bp][cp]....|auxp|
     *
     * We can now set the character and its child node pointer:
     *
     * [numc][abcd][ap][bp][cp]....|auxp|
     * [numc][abcd][ap][bp][cp][dp]|auxp| */
    n->children++;
    *childptr = child;
}

/* Insert the element 's' of size 'len', setting as auxiliary data
 * the pointer 'data'. If the element is already present, the associated
 * data is updated, and 0 is returned, otherwise the element is inserted
 * and 1 is returned. */
int trieInsert(trieNode *h, char *s, size_t len, void *data) {
    int i = 0;
    trieNode **parentlink = NULL;
    while(h->children && i < len) {
        char *v = h->data;
        for (int j = 0; j < h->children; j++) {
            if (v[j] == s[i]) break;
        }
        if (j == h->children) break;

        trieNode **children = (trieNode**)(h->data+h->children);
        h = children[j];
        parentlink = &children[j];
        i++;
    }

    /* If i == len we walked following the whole string, so the
     * string is either already inserted or this middle node is
     * currently not a key. We have just to reallocate the node
     * and make space for the data pointer. */
    if (i == len) {
        if (h->iskey) {
            trieSetData(h,data);
            return 0; /* Element already exists. */
        }
        h->iskey = 1;
        h = trieReallocForData(h);
        if (parentlink) *parentlink = h; /* Fix pointer in parent. */
        trieSetData(h,data);
        return 1; /* Element inserted. */
    }

    /* We walked the trie as far as we could, but still there are left
     * chars in our string. We need to insert the missing nodes. */
    while(i < len) {
        trieNode *child;
        h = trieAddChild(h,s[i],&child);
        if (parentlink) *parentlink = h;
        trieNode **children = (trieNode**)(h->data+h->children);
        parentlink = &children[h->children-1];
        h = child;
        i++;
    }
    h = trieReallocForData(h);
    trieSetData(h,data);
}

int main(void) {
    trieNode *head = trieNew();
    trieInsert(head,"mystring",8,NULL);
    return 0;
}
#endif
