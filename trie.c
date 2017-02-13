#include <stdlib.h>
#include <string.h>
#include "trie.h"

#ifdef TEST_MAIN
#include <stdio.h>

void *trieNotFound = (void*)"trie-not-found-pointer";

trieNode *trieNewNode(void) {
    trieNode *node = malloc(sizeof(*node));
    node->iskey = 0;
    node->children = 0;
    return node;
}

trie *trieNew(void) {
    trie *trie = malloc(sizeof(*trie));
    trie->numele = 0;
    trie->head = trieNewNode();
    return trie;
}

/* Realloc the node to make room for auxiliary data in order
 * to store an item in that node. */
trieNode *trieReallocForData(trieNode *n) {
    size_t curlen = sizeof(trieNode)+
                    n->children+
                    sizeof(trieNode*)*n->children;
    return realloc(n,curlen+sizeof(void*));
}

/* Set the node auxiliary data to the specified pointer. */
void trieSetData(trieNode *n, void *data) {
    printf("set data: %p\n", (void*)n);
    void **ndata = (void**)(n->data+n->children+sizeof(trieNode*)*n->children);
    *ndata = data;
    n->iskey = 1;
}

/* Get the node auxiliary data. */
void *trieGetData(trieNode *n) {
    void **ndata = (void**)(n->data+n->children+sizeof(trieNode*)*n->children);
    return *ndata;
}

/* Add a new child to the node 'n' representing the character
 * 'c' and return its new pointer, as well as the child pointer
 * by reference. */
trieNode *trieAddChild(trieNode *n, char c, trieNode **childptr) {
    size_t curlen = sizeof(trieNode)+
                    n->children+
                    sizeof(trieNode*)*n->children;
    size_t newlen;
    if (n->iskey) curlen += sizeof(void*);
    newlen = curlen+sizeof(trieNode*)+1; /* Add 1 char and 1 pointer. */
    n = realloc(n,newlen);

    /* After the reallocation, we have 5/9 (depending on the system
     * pointer size) bytes at the end:
     *
     * [numc][abc][ap][bp][cp]|auxp|.....
     *
     * Move all the tail pointers one byte on the left, to make
     * space for another character in the chars vector:
     *
     * [numc][abc].[ap][bp][cp]|auxp|.... */
    memmove(n->data+n->children+1,
            n->data+n->children,
            curlen-sizeof(trieNode)-n->children);

    /* Now, if present, move auxiliary data pointer at the end
     * so that we can store the additional child pointer without
     * overwriting it:
     *
     * [numc][abc].[ap][bp][cp]....|auxp| */
    if (n->iskey) {
        memmove(n->data+newlen-sizeof(void*)*2,
                n->data+newlen-sizeof(void*),
                sizeof(void*));
    }

    /* We can now set the character and its child node pointer:
     *
     * [numc][abcd][ap][bp][cp]....|auxp|
     * [numc][abcd][ap][bp][cp][dp]|auxp| */
    trieNode *child = trieNewNode();
    n->data[n->children] = c;
    memcpy(n->data+n->children+1+sizeof(trieNode*)*n->children,
           &child,sizeof(void*));
    n->children++;
    *childptr = child;
    return n;
}

/* Insert the element 's' of size 'len', setting as auxiliary data
 * the pointer 'data'. If the element is already present, the associated
 * data is updated, and 0 is returned, otherwise the element is inserted
 * and 1 is returned. */
int trieInsert(trie *trie, char *s, size_t len, void *data) {
    size_t i = 0;
    trieNode **parentlink = NULL;
    trieNode *h = trie->head;

    while(h->children && i < len) {
        char *v = (char*)h->data;
        int j;

        for (j = 0; j < h->children; j++) {
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
        h = trieReallocForData(h);
        if (parentlink) {
            *parentlink = h;
        } else {
            trie->head = h;
        }
        trieSetData(h,data);
        return 1; /* Element inserted. */
    }

    /* We walked the trie as far as we could, but still there are left
     * chars in our string. We need to insert the missing nodes. */
    while(i < len) {
        trieNode *child;
        h = trieAddChild(h,s[i],&child);
        if (parentlink) {
            *parentlink = h;
        } else {
            trie->head = h;
        }
        trieNode **children = (trieNode**)(h->data+h->children);
        parentlink = &children[h->children-1];
        h = child;
        i++;
    }
    h = trieReallocForData(h);
    trieSetData(h,data);
    return 1; /* Element inserted. */
}

/* Find a key in the trie, returns trieNotFound special void pointer value
 * if the item was not found, otherwise the value associated with the
 * item is returned. */
void *trieFind(trie *trie, char *s, size_t len) {
    size_t i = 0;
    trieNode *h = trie->head;

    while(h->children && i < len) {
        char *v = (char*)h->data;
        int j;

        printf("[%p] children: %.*s\n", (void*)h, (int)h->children, v);
        for (j = 0; j < h->children; j++) {
            if (v[j] == s[i]) break;
        }
        if (j == h->children) return trieNotFound;

        trieNode **children = (trieNode**)(h->data+h->children);
        h = children[j];
        i++;
    }
    printf("[%p] iskey?\n",(void*)h);
    return h->iskey ? trieGetData(h) : trieNotFound;
}

int main(void) {
    trie *t = trieNew();
    trieInsert(t,"mystring",8,(void*)0x1);
    trieInsert(t,"mystas",6,(void*)0x2);
    void *data1 = trieFind(t,"mystring",8);
    void *data2 = trieFind(t,"mystas",6);
    return 0;
}
#endif
