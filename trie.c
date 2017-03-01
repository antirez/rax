#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "trie.h"

void *trieNotFound = (void*)"trie-not-found-pointer";

/* Allocate a new node. */
trieNode *trieNewNode(void) {
    trieNode *node = malloc(sizeof(*node));
    node->iskey = 0;
    node->isnull = 0;
    node->iscompr = 0;
    node->size = 0;
    return node;
}

/* Allocate a new trie. */
trie *trieNew(void) {
    trie *trie = malloc(sizeof(*trie));
    trie->numele = 0;
    trie->numnodes = 1;
    trie->head = trieNewNode();
    return trie;
}

/* Return the current total size of the node. */
size_t trieNodeCurrentLength(trieNode *n) {
    size_t curlen = sizeof(trieNode)+n->size;
    curlen += n->iscompr ? sizeof(trieNode*) :
                           sizeof(trieNode*)*n->size;
    if (n->iskey && !n->isnull) curlen += sizeof(void*);
    return curlen;
}

/* Realloc the node to make room for auxiliary data in order
 * to store an item in that node. */
trieNode *trieReallocForData(trieNode *n, void *data) {
    if (data == NULL) return n; /* No reallocation needed, setting isnull=1 */
    size_t curlen = trieNodeCurrentLength(n);
    return realloc(n,curlen+sizeof(void*));
}

/* Set the node auxiliary data to the specified pointer. */
void trieSetData(trieNode *n, void *data) {
    n->iskey = 1;
    if (data != NULL) {
        void **ndata =(void**)((char*)n+trieNodeCurrentLength(n)-sizeof(void*));
        *ndata = data;
        n->isnull = 0;
    } else {
        n->isnull = 1;
    }
}

/* Get the node auxiliary data. */
void *trieGetData(trieNode *n) {
    if (n->isnull) return NULL;
    void **ndata =(void**)((char*)n+trieNodeCurrentLength(n)-sizeof(void*));
    return *ndata;
}

/* Add a new child to the node 'n' representing the character
 * 'c' and return its new pointer, as well as the child pointer
 * by reference. */
trieNode *trieAddChild(trieNode *n, char c, trieNode **childptr) {
    assert(n->iscompr == 0);
    size_t curlen = sizeof(trieNode)+
                    n->size+
                    sizeof(trieNode*)*n->size;
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
    memmove(n->data+n->size+1,
            n->data+n->size,
            curlen-sizeof(trieNode)-n->size);

    /* Now, if present, move auxiliary data pointer at the end
     * so that we can store the additional child pointer without
     * overwriting it:
     *
     * [numc][abc].[ap][bp][cp]....|auxp| */
    if (n->iskey) {
        memmove(n->data+newlen-sizeof(trieNode)-sizeof(void*),
                n->data+newlen-sizeof(trieNode)-sizeof(void*)*2,
                sizeof(void*));
    }

    /* We can now set the character and its child node pointer:
     *
     * [numc][abcd][ap][bp][cp]....|auxp|
     * [numc][abcd][ap][bp][cp][dp]|auxp| */
    trieNode *child = trieNewNode();
    n->data[n->size] = c;
    memcpy(n->data+n->size+1+sizeof(trieNode*)*n->size,
           &child,sizeof(void*));
    n->size++;
    *childptr = child;
    return n;
}

/* Turn the node 'n', that must be a node without any children, into a
 * compressed node representing a set of nodes linked one after the other
 * and having exactly one child each. The node can be a key or not: this
 * property and the associated value if any will be preserved.
 *
 * The function also returns a child node, since the last node of the
 * compressed chain cannot be part of the chain: it has zero children while
 * we can only compress inner nodes with exactly one child each. */
trieNode *trieCompressNode(trieNode *n, char *s, size_t len, trieNode **child){
    assert(n->size == 0 && n->iscompr == 0);
    void *data = NULL; /* Initialized only to avoid warnings. */
    size_t newsize, valuelen = 0;

    printf("Compress node: %.*s\n", (int)len,s);

    newsize = sizeof(trieNode)+len+sizeof(trieNode*);
    if (n->iskey) {
        data = trieGetData(n); /* To restore it later. */
        if (!n->isnull) {
            newsize += sizeof(void*);
            valuelen = sizeof(void*);
        }
    }

    n = realloc(n,newsize);
    n->iscompr = 1;
    n->size = len;
    memcpy(n->data,s,len);
    if (n->iskey) trieSetData(n,data);
    trieNode **childfield = (trieNode**)
                            ((char*)n + newsize - valuelen - sizeof(trieNode*));
    *child = trieNewNode();
    *childfield = *child;
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

    printf("### Insert %.*s\n", (int)len, s);
    int j;
    while(h->size && i < len) {
        char *v = (char*)h->data;

        if (h->iscompr) {
            for (j = 0; j < h->size && i < len; j++, i++) {
                if (v[j] != s[i]) break;
            }
            if (j != h->size) break;
        } else {
            for (j = 0; j < h->size; j++) {
                if (v[j] == s[i]) break;
            }
            if (j == h->size) break;
            i++;
        }

        trieNode **children = (trieNode**)(h->data+h->size);
        if (h->iscompr) j = 0; /* Compressed node only child is at index 0. */
        h = children[j];
        parentlink = &children[j];
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
        h = trieReallocForData(h,data);
        if (parentlink) {
            *parentlink = h;
        } else {
            trie->head = h;
        }
        trieSetData(h,data);
        return 1; /* Element inserted. */
    }

    /* If the node we stopped at is a compressed node, we need to
     * split it before to continue. */
    if (h->iscompr) {
        printf("Stopped at compressed node %.*s\n", h->size, h->data);
        printf("Still to insert: %.*s\n", (int)(len-i), s+i);
        printf("Splitting at %d: '%c'\n", j, ((char*)h->data)[j]);
        exit(1);
    }

    /* We walked the trie as far as we could, but still there are left
     * chars in our string. We need to insert the missing nodes. */
    while(i < len) {
        trieNode *child;
        trie->numnodes++;

        /* If this node is going to have a single child, and there
         * are other characters, so that that would result in a chain
         * of single-childed nodes, turn it into a compressed node. */
        if (h->size == 0 && len-i > 1) {
            printf("Inserting compressed node\n");
            size_t comprsize = len-i;
            if (comprsize > TRIE_NODE_MAX_SIZE) comprsize = TRIE_NODE_MAX_SIZE;
            h = trieCompressNode(h,s+i,comprsize,&child);
            i += comprsize;
        } else {
            printf("Inserting normal node\n");
            h = trieAddChild(h,s[i],&child);
            i++;
        }

        /* Fix parent's reference now that we reallocated. */
        if (parentlink) {
            *parentlink = h;
        } else {
            trie->head = h;
        }
        trieNode **children = (trieNode**)(h->data+h->size);
        parentlink = &children[h->size-1];
        h = child;
    }
    h = trieReallocForData(h,data);
    trieSetData(h,data);
    return 1; /* Element inserted. */
}

/* Find a key in the trie, returns trieNotFound special void pointer value
 * if the item was not found, otherwise the value associated with the
 * item is returned. */
void *trieFind(trie *trie, char *s, size_t len) {
    size_t i = 0;
    trieNode *h = trie->head;

    printf("### Lookup: %.*s\n", (int)len, s);
    while(h->size && i < len) {
        printf("Lookup iteration node %p\n", (void*)h);
        char *v = (char*)h->data;
        int j;

        printf("[%p %s] children: %.*s\n", (void*)h, h->iscompr ? "compr" : "plain", (int)h->size, v);

        if (h->iscompr) {
            for (j = 0; j < h->size && i < len; j++, i++) {
                if (v[j] != s[i]) break;
                printf("%c != %c? \n", v[j], s[i]);
            }
            if (j != h->size) return trieNotFound;
            printf("here %d %d\n", (int)i, (int)len);
            j = 0; /* Our only child is at index 0 for compressed nodes. */
        } else {
            for (j = 0; j < h->size; j++) {
                if (v[j] == s[i]) break;
            }
            if (j == h->size) return trieNotFound;
            i++;
        }
        printf("select next child\n");

        trieNode **children = (trieNode**)(h->data+h->size);
        h = children[j];
    }
    if (i != len) return trieNotFound;
    printf("Lookup final node: [%p] iskey? %d\n",(void*)h,h->iskey);
    return h->iskey ? trieGetData(h) : trieNotFound;
}

#ifdef BENCHMARK_MAIN
#include <stdio.h>
#include <sys/time.h>

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

int main(void) {
    trie *t = trieNew();
    trieInsert(t,"abc",3,(void*)10);
    trieInsert(t,"mystring",8,(void*)0x1);
    trieInsert(t,"mystas",6,(void*)0x2);
    trieInsert(t,"key:123",7,(void*)0x3);
    trieInsert(t,"key:1234",8,(void*)0xff);

    long long start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i);
        trieInsert(t,buf,len,(void*)(long)i);
    }
    printf("Insert: %f\n", (double)(ustime()-start)/1000000);

    start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i);
        void *data = trieFind(t,buf,len);
        if (data != (void*)(long)i) {
            printf("Issue with %s\n", buf);
        }
    }
    printf("Lookup: %f\n", (double)(ustime()-start)/1000000);

    start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int r = rand() % 5000000;
        int len = snprintf(buf,sizeof(buf),"%d",r);
        void *data = trieFind(t,buf,len);
        if (data != (void*)(long)r) {
            printf("Issue with %s\n", buf);
        }
    }
    printf("Random lookup: %f\n", (double)(ustime()-start)/1000000);

    start = ustime();
    int count = 0;
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i+5000000);
        void *data = trieFind(t,buf,len);
        if (data != (void*)(long)i) count++;
    }
    printf("Failed lookup: %f\n", (double)(ustime()-start)/1000000);

    void *data1 = trieFind(t,"mystring",8);
    void *data2 = trieFind(t,"mystas",6);
    void *data3 = trieFind(t,"myzack",6);
    void *data4 = trieFind(t,"key:123",7);
    printf("%p %p %p %p\n", data1, data2, data3, data4);
    printf("%llu total nodes\n", (unsigned long long)t->numnodes);
    return 0;
}
#endif

#ifdef TEST_MAIN
#include <stdio.h>

int main(void) {
    printf("notfound = %p\n", trieNotFound);
    trie *t = trieNew();
    trieInsert(t,"a",1,(void*)1);
    printf("a = %p\n", trieFind(t,"a",1));
    trieInsert(t,"annibale",8,(void*)2);
    printf("a = %p\n", trieFind(t,"a",1));
    printf("annibale = %p\n", trieFind(t,"annibale",8));
    trieInsert(t,"annientare",10,(void*)3);
}
#endif
