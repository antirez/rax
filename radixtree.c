#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "radixtree.h"

/* Turn debugging messages on/off. */
#if 1
#include <stdio.h>
#define debugf(...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf(__VA_ARGS__);                                                   \
    } while (0);
#else
#define debugf(...)
#endif

/* This is a special pointer that is guaranteed to never have the same value
 * of a radix tree node. It's used in order to report "not found" error without
 * requiring the function to have multiple return values. */
void *trieNotFound = (void*)"trie-not-found-pointer";

/* ------------------------- trieStack functions ------------------------------
 * The trieStack is a simple stack of pointers that is capable of switching
 * from using a stack-allocated array to dynamic heap once a given number of
 * items are reached. It is used in order to retain the list of parent nodes
 * while walking the radix tree in order to implement certain operations that
 * need to navigate the tree upward.
 * ------------------------------------------------------------------------- */

#define TRIESTACK_STACK_ITEMS 32
typedef struct trieStack {
    void **stack;
    size_t items, maxitems;
    void *static_items[TRIESTACK_STACK_ITEMS];
} trieStack;

/* Initialize the stack. */
static inline void trieStackInit(trieStack *ts) {
    ts->stack = ts->static_items;
    ts->items = 0;
    ts->maxitems = TRIESTACK_STACK_ITEMS;
}

/* Push an item into the stack, returns 1 on success, 0 on out of memory. */
static inline int trieStackPush(trieStack *ts, void *ptr) {
    if (ts->items == ts->maxitems) {
        if (ts->stack == ts->static_items) {
            ts->stack = malloc(sizeof(void*)*ts->maxitems*2);
            memcpy(ts->stack,ts->static_items,sizeof(void*)*ts->maxitems);
        } else {
            ts->stack = realloc(ts->stack,sizeof(void*)*ts->maxitems*2);
        }
        if (ts->stack == NULL) return 0;
        ts->maxitems *= 2;
    }
    ts->stack[ts->items] = ptr;
    ts->items++;
    return 1;
}

/* Pop an item from the stack, the function returns NULL if there are no
 * items to pop. */
static inline void *trieStackPop(trieStack *ts) {
    if (ts->items == 0) return NULL;
    ts->items--;
    return ts->stack[ts->items];
}

/* Free the stack in case we used heap allocation. */
static inline void trieStackFree(trieStack *ts) {
    if (ts->stack != ts->static_items) free(ts->stack);
}

/* Allocate a new non compressed node with the specified number of children. */
trieNode *trieNewNode(size_t children) {
    size_t nodesize = sizeof(trieNode)+children+sizeof(trieNode*)*children;
    trieNode *node = malloc(nodesize);
    node->iskey = 0;
    node->isnull = 0;
    node->iscompr = 0;
    node->size = children;
    return node;
}

/* ----------------------------------------------------------------------------
 * Radis tree implementation
 * --------------------------------------------------------------------------*/

/* Allocate a new trie. */
trie *trieNew(void) {
    trie *trie = malloc(sizeof(*trie));
    trie->numele = 0;
    trie->numnodes = 1;
    trie->head = trieNewNode(0);
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
    trieNode *child = trieNewNode(0);
    n->data[n->size] = c;
    memcpy(n->data+n->size+1+sizeof(trieNode*)*n->size,
           &child,sizeof(void*));
    n->size++;
    *childptr = child;
    return n;
}

/* Return the pointer to the last child pointer in a node. For the compressed
 * nodes this is the only child pointer. */
trieNode **trieNodeLastChildPtr(trieNode *n) {
    char *p = (char*)n;
    p += trieNodeCurrentLength(n) - sizeof(trieNode*);
    if (n->iskey && !n->isnull) p -= sizeof(void*);
    return (trieNode**)p;
}

/* Turn the node 'n', that must be a node without any children, into a
 * compressed node representing a set of nodes linked one after the other
 * and having exactly one child each. The node can be a key or not: this
 * property and the associated value if any will be preserved.
 *
 * The function also returns a child node, since the last node of the
 * compressed chain cannot be part of the chain: it has zero children while
 * we can only compress inner nodes with exactly one child each. */
trieNode *trieCompressNode(trieNode *n, unsigned char *s, size_t len, trieNode **child){
    assert(n->size == 0 && n->iscompr == 0);
    void *data = NULL; /* Initialized only to avoid warnings. */
    size_t newsize;

    debugf("Compress node: %.*s\n", (int)len,s);

    newsize = sizeof(trieNode)+len+sizeof(trieNode*);
    if (n->iskey) {
        data = trieGetData(n); /* To restore it later. */
        if (!n->isnull) newsize += sizeof(void*);
    }

    n = realloc(n,newsize);
    n->iscompr = 1;
    n->size = len;
    memcpy(n->data,s,len);
    if (n->iskey) trieSetData(n,data);
    trieNode **childfield = trieNodeLastChildPtr(n);
    *child = trieNewNode(0);
    *childfield = *child;
    return n;
}

/* Low level function that walks the tree looking for the string
 * 's' of 'len' bytes. The function returns the number of characters
 * of the key that was possible to process: if the returned integer
 * is the same as 'len', then it means that the node corresponding to the
 * string was found (however it may not be a key, node->iskey == 0).
 * Otherwise there was an early stop.
 *
 * The node where the search ended (because the full string was processed
 * or because there was an early stop) is returned by reference as
 * '*stopnode' if the passed pointer is not NULL. This node link in the
 * parent's node is returned as '*plink' if not NULL. Finally, if the
 * search stopped in a compressed node, '*splitpos' returns the index
 * inside the compressed node where the search ended. This is ussful to
 * know where to split the node for insertion. */
static inline size_t trieLowWalk(trie *trie, unsigned char *s, size_t len, trieNode **stopnode, trieNode ***plink, int *splitpos, trieStack *ts) {
    trieNode *h = trie->head;
    trieNode **parentlink = &trie->head;

    size_t i = 0; /* Position in the string. */
    size_t j = 0; /* Position in the node children / bytes (if compressed). */
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

        if (ts) trieStackPush(ts,h); /* Save stack of parent nodes. */
        trieNode **children = (trieNode**)(h->data+h->size);
        if (h->iscompr) j = 0; /* Compressed node only child is at index 0. */
        h = children[j];
        parentlink = &children[j];
    }
    if (stopnode) *stopnode = h;
    if (plink) *plink = parentlink;
    if (splitpos && h->iscompr) *splitpos = j;
    return i;
}

/* Insert the element 's' of size 'len', setting as auxiliary data
 * the pointer 'data'. If the element is already present, the associated
 * data is updated, and 0 is returned, otherwise the element is inserted
 * and 1 is returned. */
int trieInsert(trie *trie, unsigned char *s, size_t len, void *data) {
    size_t i;
    int j = 0; /* Split position. If trieLowWalk() stops in a compressed node,
                  the index 'j' represents the char we stopped within the
                  compressed node, that is, the position where to split the
                  node for insertion. */
    trieNode *h, **parentlink;

    debugf("### Insert %.*s with value %p\n", (int)len, s, data);
    i = trieLowWalk(trie,s,len,&h,&parentlink,&j,NULL);

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
        *parentlink = h;
        trieSetData(h,data);
        return 1; /* Element inserted. */
    }

    /* If the node we stopped at is a compressed node, we need to
     * split it before to continue.
     *
     * Splitting a compressed node have a few possibile cases.
     * Imagine that the node 'h' we are currently at is a compressed
     * node contaning the string "ANNIBALE" (it means that it represents
     * nodes A -> N -> N -> I -> B -> A -> L -> E with the only child
     * pointer of this node pointing at the 'E' node, because remember that
     * we have characters at the edges of the graph, not inside the nodes
     * themselves.
     *
     * In order to show a real case imagine our node to also point to
     * another compressed node, that finally points at the node without
     * children, representing 'O':
     *
     *     "ANNIBALE" -> "SCO" -> []
     *
     * When inserting we may face the following cases. Note that all the cases
     * require the insertion of a non compressed node with exactly two
     * children.
     *
     * 1) Inserting "ANNIENTARE"
     *
     *               |B| -> "ALE" -> "SCO" -> []
     *     "ANNI" -> |-|
     *               |E| -> (... continue algo ...) "NTARE" -> []
     *
     * 2) Inserting "ANNIBALI"
     *
     *                  |E| -> "SCO" -> []
     *     "ANNIBAL" -> |-|
     *                  |I| -> (... continue algo ...) []
     *
     * 3) Inserting "AGO" (Like case 1, but set iscompr = 0 into original node)
     *
     *            |N| -> "NIBALE" -> "SCO" -> []
     *     |A| -> |-|
     *            |G| -> (... continue algo ...) |O| -> []
     *
     * 4) Inserting "CIAO"
     *
     *     |A| -> "NNIBALE" -> "SCO" -> []
     *     |-|
     *     |C| -> (... continue algo ...) "IAO" -> []
     *
     * The final algorithm for insertion covering all the above cases is as
     * follows:
     *
     * 1. Save the current compressed node $NEXT pointer (the pointer to the
     *    child element, that is always present in compressed nodes).
     *
     * 2. Create "split node" having as child the non common letter
     *    at the compressed node. The other non common letter (at the key)
     *    will be added later as we continue the normal insertion algorithm
     *    at step "6".
     *
     * 3a. IF split position is 0:
     *     Replace the old node with the split node, by copying the auxiliary
     *     data if any. Fix parent's reference. Free old node eventually
     *     (we still need its data for the next steps of the algorithm).
     *
     * 3b. IF split position != 0:
     *     Trim the compressed node (reallocating it as well) in order to
     *     contain $splitpos characters. Change chilid pointer in order to link
     *     to the split node. If new compressed node len is just 1, set
     *     iscompr to 0 (layout is the same). Fix parent's reference.
     *
     * 4a. IF the postfix len (the length of the remaining string of the
     *     original compressed node after the split character) is non zero,
     *     create a "postfix node". If the postfix node has just one character
     *     set iscompr to 0, otherwise iscompr to 1. Set the postfix node
     *     child pointer to $NEXT.
     *
     * 4b. IF the postfix len is zero, just use $NEXT as postfix pointer.
     *
     * 5. Set child[0] of split node to postfix node.
     *
     * 6. Set the split node as the current node, set current index at child[1]
     *    and continue insertion algorithm as usually.
     */
    if (h->iscompr) {
        debugf("Stopped at compressed node %.*s (%p)\n",
            h->size, h->data, (void*)h);
        debugf("Still to insert: %.*s\n", (int)(len-i), s+i);
        debugf("Splitting at %d: '%c'\n", j, ((char*)h->data)[j]);
        debugf("Other letter is '%c'\n", s[i]);

        /* 1: Save next pointer. */
        trieNode **childfield = trieNodeLastChildPtr(h);
        trieNode *next = *childfield;
        debugf("Next is %p\n", (void*)next);
        debugf("iskey %d\n", h->iskey);
        if (h->iskey) {
            debugf("key value is %p\n", trieGetData(h));
        }

        /* 2: Create the split node. */
        trieNode *splitnode = trieNewNode(1);
        splitnode->data[0] = h->data[j];

        if (j == 0) {
            /* 3a: Replace the old node with the split node. */
            if (h->iskey) {
                void *ndata = trieGetData(h);
                splitnode = trieReallocForData(splitnode,ndata);
                trieSetData(splitnode,ndata);
            }
            *parentlink = splitnode;
        } else {
            /* 3b: Trim the compressed node. */
            size_t nodesize = sizeof(trieNode)+j+sizeof(trieNode*);
            if (h->iskey && !h->isnull) nodesize += sizeof(void*);
            trieNode *trimmed = malloc(nodesize);
            trimmed->size = j;
            memcpy(trimmed->data,h->data,j);
            trimmed->iscompr = j > 1 ? 1 : 0;
            trimmed->iskey = h->iskey;
            trimmed->isnull = h->isnull;
            if (h->iskey && !h->isnull) {
                void *ndata = trieGetData(h);
                trieSetData(trimmed,ndata);
            }
            trieNode **cp = trieNodeLastChildPtr(trimmed);
            *cp = splitnode;
            *parentlink = trimmed;
            parentlink = cp; /* Set parentlink to splitnode parent. */
            trie->numnodes++;
        }

        /* 4: Create the postfix node: what remains of the original
         * compressed node after the split. */
        size_t postfixlen = h->size - j - 1;
        trieNode *postfix;
        if (postfixlen) {
            /* 4a: create a postfix node. */
            size_t nodesize = sizeof(trieNode)+postfixlen+sizeof(trieNode*);
            postfix = malloc(nodesize);
            postfix->iskey = 0;
            postfix->isnull = 0;
            postfix->size = postfixlen;
            postfix->iscompr = postfixlen > 1;
            memcpy(postfix->data,h->data+j+1,postfixlen);
            trieNode **cp = trieNodeLastChildPtr(postfix);
            *cp = next;
            trie->numnodes++;
        } else {
            /* 4b: just use next as postfix node. */
            postfix = next;
        }

        /* 5: Set splitnode first child as the postfix node. */
        trieNode **splitchild = trieNodeLastChildPtr(splitnode);
        *splitchild = postfix;

        /* 6. Continue insertion: this will cause the splitnode to
         * get a new child (the non common character at the currently
         * inserted key). */
        free(h);
        h = splitnode;
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
            debugf("Inserting compressed node\n");
            size_t comprsize = len-i;
            if (comprsize > TRIE_NODE_MAX_SIZE) comprsize = TRIE_NODE_MAX_SIZE;
            h = trieCompressNode(h,s+i,comprsize,&child);
            *parentlink = h;
            parentlink = trieNodeLastChildPtr(h);
            i += comprsize;
        } else {
            debugf("Inserting normal node\n");
            h = trieAddChild(h,s[i],&child);
            trieNode **children = (trieNode**)(h->data+h->size);
            *parentlink = h;
            parentlink = &children[h->size-1];
            i++;
        }
        h = child;
    }
    if (!h->iskey) trie->numele++;
    h = trieReallocForData(h,data);
    trieSetData(h,data);
    *parentlink = h;
    return 1; /* Element inserted. */
}

/* Find a key in the trie, returns trieNotFound special void pointer value
 * if the item was not found, otherwise the value associated with the
 * item is returned. */
void *trieFind(trie *trie, unsigned char *s, size_t len) {
    trieNode *h;

    debugf("### Lookup: %.*s\n", (int)len, s);
    size_t i = trieLowWalk(trie,s,len,&h,NULL,NULL,NULL);
    if (i != len) return trieNotFound;
    debugf("Lookup final node: [%p] iskey? %d\n",(void*)h,h->iskey);
    return h->iskey ? trieGetData(h) : trieNotFound;
}

/* Return the memory address where the 'parent' node stores the specified
 * 'child' pointer, so that the caller can update the pointer with another
 * one if needed. The function assumes it will find a match, otherwise the
 * operation is an undefined behavior (it will continue scanning the
 * memory without any bound checking). */
trieNode **trieFindParentLink(trieNode *parent, trieNode *child) {
    trieNode **cp = trieNodeLastChildPtr(parent) - (parent->size-1);
    while(*cp != child) cp++;
    return cp;
}

/* Low level child removal from node. The new node pointer (after the child
 * removal) is returned. Note that this function does not fix the pointer
 * of the node in its parent, so this task is up to the caller. */
trieNode *trieRemoveChild(trieNode *parent, trieNode *child) {
    /* If parent is a compressed node (having a single child, as for definition
     * of the data structure), the removal of the child consists into turning
     * it into a normal node without children. */
    if (parent->iscompr) {
        trieNode *newnode = trieNewNode(0);
        if (parent->iskey) {
            void *data = trieGetData(parent);
            newnode = trieReallocForData(newnode,data);
            trieSetData(newnode,data);
        }
        return newnode;
    }

    /* Otherwise we need to scan for the children pointer and memmove()
     * accordingly.
     *
     * 1. To start we seek the first element in both the children
     *    pointers and edge bytes in the node. */
    trieNode **cp = trieNodeLastChildPtr(parent) - (parent->size-1);
    trieNode **c = cp;
    unsigned char *e = parent->data;

    /* 2. Search the child pointer to remove inside the array of children
     *    pointers. */
    while(*c != child) {
        c++;
        e++;
    }

    /* 3. Remove the edge and the pointer by memmoving the remaining children
     *    pointer and edge bytes one position before. */
    int taillen = parent->size - (e - parent->data) - 1;
    memmove(e,e+1,taillen);
    memmove(c,c+1,taillen*sizeof(trieNode*));

    /* 4. Update size. */
    parent->size--;

    /* Realloc the node according to the theoretical memory usage, to free
     * data if we are over-allocating right now. */
    trieNode *newnode = realloc(parent,trieNodeCurrentLength(parent));
    /* Note: if realloc() fails we just return the old address, which
     * is valid. */
    return newnode ? newnode : parent;
}

/* Remove the specified item. Returns 1 if the item was found and
 * deleted, 0 otherwise. */
int trieRemove(trie *trie, unsigned char *s, size_t len) {
    trieNode *h;
    trieStack ts;

    debugf("### Delete: %.*s\n", (int)len, s);
    trieStackInit(&ts);
    size_t i = trieLowWalk(trie,s,len,&h,NULL,NULL,&ts);
    if (i != len || !h->iskey) {
        trieStackFree(&ts);
        return 0;
    }
    h->iskey = 0;

    /* If this node has no children, the deletion needs to reclaim the
     * no longer used nodes. This is an iterative process that needs to
     * walk the three upward, deleting all the nodes with just one child
     * that are not keys, until the head of the trie is reached or the first
     * node with more than one child is found. */
    if (h->size == 0) {
        debugf("Key deleted in node without children. Cleanup needed.\n");
        trieNode *child = NULL;
        while(h != trie->head) {
            child = h;
            debugf("Freeing child %p\n", (void*)child);
            free(child);
            trie->numnodes--;
            h = trieStackPop(&ts);
             /* If this node has more then one child, or actually holds
              * a key, stop here. */
            if (h->iskey || (!h->iscompr && h->size != 1)) break;
        }
        if (child) {
            debugf("Unlinking child %p from parent %p\n",
                (void*)child, (void*)h);
            trieNode *new = trieRemoveChild(h,child);
            if (new != h) {
                trieNode *parent = trieStackPop(&ts);
                trieNode **parentlink;
                if (parent == NULL) {
                    parentlink = &trie->head;
                } else {
                    parentlink = trieFindParentLink(parent,h);
                }
                *parentlink = new;
            }

            /* XXX: if after the removal the node has just a single child
             * we need to try to compress it. */
            if (new->size == 1) {
                debugf("Re-compression attempt needed.\n");
            }
        }
    }
    trieStackFree(&ts);
    return 1;
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

    long long start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i);
        trieInsert(t,(unsigned char*)buf,len,(void*)(long)i);
    }
    printf("Insert: %f\n", (double)(ustime()-start)/1000000);

    start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i);
        void *data = trieFind(t,(unsigned char*)buf,len);
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
        void *data = trieFind(t,(unsigned char*)buf,len);
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
        void *data = trieFind(t,(unsigned char*) buf,len);
        if (data != (void*)(long)i) count++;
    }
    printf("Failed lookup: %f\n", (double)(ustime()-start)/1000000);

    printf("%llu total nodes\n", (unsigned long long)t->numnodes);
    printf("%llu total elements\n", (unsigned long long)t->numele);
    return 0;
}
#endif

#ifdef TEST_MAIN
#include <stdio.h>

int main(void) {
    printf("notfound = %p\n", trieNotFound);
    trie *t = trieNew();
    trieInsert(t,(unsigned char*)"a",1,(void*)1);
    printf("a = %p\n", trieFind(t,(unsigned char*)"a",1));
    trieInsert(t,(unsigned char*)"annibale",8,(void*)2);
    printf("a = %p\n", trieFind(t,(unsigned char*)"a",1));
    printf("annibale = %p\n", trieFind(t,(unsigned char*)"annibale",8));
    trieInsert(t,(unsigned char*)"annientare",10,(void*)3);
    printf("annientare = %p\n", trieFind(t,(unsigned char*)"annientare",10));
    trieRemove(t,(unsigned char*)"a",1);
    trieRemove(t,(unsigned char*)"annientare",10);
    printf("a = %p\n", trieFind(t,(unsigned char*)"a",1));
    printf("annientare = %p\n", trieFind(t,(unsigned char*)"annientare",10));
    printf("annibale = %p\n", trieFind(t,(unsigned char*)"annibale",8));
}
#endif
