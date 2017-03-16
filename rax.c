#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "rax.h"

/* This is a special pointer that is guaranteed to never have the same value
 * of a radix tree node. It's used in order to report "not found" error without
 * requiring the function to have multiple return values. */
void *raxNotFound = (void*)"rax-not-found-pointer";

/* -------------------------------- Debugging ------------------------------ */

void raxDebugShowNode(const char *msg, raxNode *n);

/* Turn debugging messages on/off. */
#if 0
#define debugf(...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf(__VA_ARGS__);                                                   \
        fflush(stdout);                                                        \
    } while (0);

#define debugnode(msg,n) raxDebugShowNode(msg,n)
#else
#define debugf(...)
#define debugnode(msg,n)
#endif

/* ------------------------- raxStack functions --------------------------
 * The raxStack is a simple stack of pointers that is capable of switching
 * from using a stack-allocated array to dynamic heap once a given number of
 * items are reached. It is used in order to retain the list of parent nodes
 * while walking the radix tree in order to implement certain operations that
 * need to navigate the tree upward.
 * ------------------------------------------------------------------------- */

#define RAXSTACK_STACK_ITEMS 32
typedef struct raxStack {
    void **stack;
    size_t items, maxitems;
    void *static_items[RAXSTACK_STACK_ITEMS];
} raxStack;

/* Initialize the stack. */
static inline void raxStackInit(raxStack *ts) {
    ts->stack = ts->static_items;
    ts->items = 0;
    ts->maxitems = RAXSTACK_STACK_ITEMS;
}

/* Push an item into the stack, returns 1 on success, 0 on out of memory. */
static inline int raxStackPush(raxStack *ts, void *ptr) {
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
static inline void *raxStackPop(raxStack *ts) {
    if (ts->items == 0) return NULL;
    ts->items--;
    return ts->stack[ts->items];
}

/* Return the stack item at the top of the stack without actually consuming
 * it. */
static inline void *raxStackPeek(raxStack *ts) {
    if (ts->items == 0) return NULL;
    return ts->stack[ts->items-1];
}

/* Free the stack in case we used heap allocation. */
static inline void raxStackFree(raxStack *ts) {
    if (ts->stack != ts->static_items) free(ts->stack);
}

/* ----------------------------------------------------------------------------
 * Radis tree implementation
 * --------------------------------------------------------------------------*/

/* Allocate a new non compressed node with the specified number of children. */
raxNode *raxNewNode(size_t children) {
    size_t nodesize = sizeof(raxNode)+children+
                      sizeof(raxNode*)*children;
    raxNode *node = malloc(nodesize);
    node->iskey = 0;
    node->isnull = 0;
    node->iscompr = 0;
    node->size = children;
    return node;
}

/* Allocate a new rax. */
rax *raxNew(void) {
    rax *rax = malloc(sizeof(*rax));
    rax->numele = 0;
    rax->numnodes = 1;
    rax->head = raxNewNode(0);
    return rax;
}

/* Return the current total size of the node. */
#define raxNodeCurrentLength(n) ( \
    sizeof(raxNode)+(n)->size+ \
    ((n)->iscompr ? sizeof(raxNode*) : sizeof(raxNode*)*(n)->size)+ \
    (((n)->iskey && !(n)->isnull)*sizeof(void*)) \
)

/* Realloc the node to make room for auxiliary data in order
 * to store an item in that node. */
raxNode *raxReallocForData(raxNode *n, void *data) {
    if (data == NULL) return n; /* No reallocation needed, setting isnull=1 */
    size_t curlen = raxNodeCurrentLength(n);
    return realloc(n,curlen+sizeof(void*));
}

/* Set the node auxiliary data to the specified pointer. */
void raxSetData(raxNode *n, void *data) {
    n->iskey = 1;
    if (data != NULL) {
        void **ndata = (void**)
            ((char*)n+raxNodeCurrentLength(n)-sizeof(void*));
        memcpy(ndata,&data,sizeof(data));
        n->isnull = 0;
    } else {
        n->isnull = 1;
    }
}

/* Get the node auxiliary data. */
void *raxGetData(raxNode *n) {
    if (n->isnull) return NULL;
    void **ndata =(void**)((char*)n+raxNodeCurrentLength(n)-sizeof(void*));
    void *data;
    memcpy(&data,ndata,sizeof(data));
    return data;
}

/* Add a new child to the node 'n' representing the character 'c' and return
 * its new pointer, as well as the child pointer by reference. Additionally
 * '***parentlink' is populated with the raxNode pointer-to-pointer of where
 * the new child was stored, which is useful for the caller to replace the
 * child pointer if it gets reallocated. */
raxNode *raxAddChild(raxNode *n, char c, raxNode **childptr, raxNode ***parentlink) {
    assert(n->iscompr == 0);
    size_t curlen = sizeof(raxNode)+
                    n->size+
                    sizeof(raxNode*)*n->size;
    size_t newlen;
    if (n->iskey) curlen += sizeof(void*);
    newlen = curlen+sizeof(raxNode*)+1; /* Add 1 char and 1 pointer. */
    n = realloc(n,newlen);

    /* After the reallocation, we have 5/9 (depending on the system
     * pointer size) bytes at the end, that is, the additional char
     * in the 'data' section, plus one pointer to the new child:
     *
     * [numc][abx][ap][bp][xp]|auxp|.....
     *
     * Let's find where to insert the new child in order to make sure
     * it is inserted in-place lexicographically. */
    int pos;
    for (pos = 0; pos < n->size; pos++) {
        if (n->data[pos] > c) break;
    }

    /* Now, if present, move auxiliary data pointer at the end
     * so that we can mess with the other data without overwriting it.
     * We will obtain something like that:
     *
     * [numc][abx][ap][bp][xp].....|auxp| */
    unsigned char *src;
    if (n->iskey && !n->isnull) {
        src = n->data+n->size+sizeof(raxNode*)*n->size;
        memmove(src+1+sizeof(raxNode*),src,sizeof(void*));
    }

    /* Now imagine we are adding a node with edge 'c'. The insertion
     * point is between 'b' and 'x', so the 'pos' variable value is
     * To start, move all the child pointers after the insertion point
     * of 1+sizeof(pointer) bytes on the right, to obtain:
     *
     * [numc][abx][ap][bp].....[xp]|auxp| */
    src = n->data+n->size+sizeof(raxNode*)*pos;
    memmove(src+1+sizeof(raxNode*),src,sizeof(raxNode*)*(n->size-pos));

    /* Now make the space for the additional char in the data section,
     * but also move the pointers before the insertion point in the right
     * by 1 byte, in order to obtain the following:
     *
     * [numc][ab.x][ap][bp]....[xp]|auxp| */
    src = n->data+pos;
    memmove(src+1,src,n->size-pos+sizeof(raxNode*)*pos);

    /* We can now set the character and its child node pointer to get:
     *
     * [numc][abcx][ap][bp][cp]....|auxp|
     * [numc][abcx][ap][bp][cp][xp]|auxp| */
    raxNode *child = raxNewNode(0);
    n->data[pos] = c;
    n->size++;
    raxNode **childfield = (raxNode**)(n->data+n->size+sizeof(raxNode*)*pos);
    memcpy(childfield,&child,sizeof(child));
    *childptr = child;
    *parentlink = childfield;
    return n;
}

/* Return the pointer to the last child pointer in a node. For the compressed
 * nodes this is the only child pointer. */
#define raxNodeLastChildPtr(n) ((raxNode**) ( \
    ((char*)(n)) + \
    raxNodeCurrentLength(n) - \
    sizeof(raxNode*) - \
    (((n)->iskey && !(n)->isnull) ? sizeof(void*) : 0) \
))

/* Return the pointer to the first child pointer. */
#define raxNodeFirstChildPtr(n) ((raxNode**)((n)->data+(n)->size))

/* Turn the node 'n', that must be a node without any children, into a
 * compressed node representing a set of nodes linked one after the other
 * and having exactly one child each. The node can be a key or not: this
 * property and the associated value if any will be preserved.
 *
 * The function also returns a child node, since the last node of the
 * compressed chain cannot be part of the chain: it has zero children while
 * we can only compress inner nodes with exactly one child each. */
raxNode *raxCompressNode(raxNode *n, unsigned char *s, size_t len, raxNode **child) {
    assert(n->size == 0 && n->iscompr == 0);
    void *data = NULL; /* Initialized only to avoid warnings. */
    size_t newsize;

    debugf("Compress node: %.*s\n", (int)len,s);

    newsize = sizeof(raxNode)+len+sizeof(raxNode*);
    if (n->iskey) {
        data = raxGetData(n); /* To restore it later. */
        if (!n->isnull) newsize += sizeof(void*);
    }

    n = realloc(n,newsize);
    n->iscompr = 1;
    n->size = len;
    memcpy(n->data,s,len);
    if (n->iskey) raxSetData(n,data);
    raxNode **childfield = raxNodeLastChildPtr(n);
    *child = raxNewNode(0);
    memcpy(childfield,&child,sizeof(child));
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
 * inside the compressed node where the search ended. This is useful to
 * know where to split the node for insertion. */
static inline size_t raxLowWalk(rax *rax, unsigned char *s, size_t len, raxNode **stopnode, raxNode ***plink, int *splitpos, raxStack *ts) {
    raxNode *h = rax->head;
    raxNode **parentlink = &rax->head;

    size_t i = 0; /* Position in the string. */
    size_t j = 0; /* Position in the node children / bytes (if compressed). */
    while(h->size && i < len) {
        debugnode("Lookup current node",h);
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

        if (ts) raxStackPush(ts,h); /* Save stack of parent nodes. */
        raxNode **children = raxNodeFirstChildPtr(h);
        if (h->iscompr) j = 0; /* Compressed node only child is at index 0. */
        memcpy(&h,children+j,sizeof(h));
        parentlink = children+j;
        if (h->iscompr) j = 0; /* If the new node is compressed and we do not
                                  iterate again (since i == l) the split
                                  position is 0. */
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
int raxInsert(rax *rax, unsigned char *s, size_t len, void *data) {
    size_t i;
    int j = 0; /* Split position. If raxLowWalk() stops in a compressed
                  node, the index 'j' represents the char we stopped within the
                  compressed node, that is, the position where to split the
                  node for insertion. */
    raxNode *h, **parentlink;

    debugf("### Insert %.*s with value %p\n", (int)len, s, data);
    i = raxLowWalk(rax,s,len,&h,&parentlink,&j,NULL);

    /* If i == len we walked following the whole string. If we are not
     * in the middle of a compressed node, the string is either already
     * inserted or this middle node is currently not a key, but can represent
     * our key. We have just to reallocate the node and make space for the
     * data pointer. */
    if (i == len && (!h->iscompr || j == 0 /* not in the middle if j is 0 */)) {
        if (h->iskey) {
            raxSetData(h,data);
            return 0; /* Element already exists. */
        }
        h = raxReallocForData(h,data);
        memcpy(parentlink,&h,sizeof(h));
        raxSetData(h,data);
        rax->numele++;
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
     * children, except for the last case which just requires splitting a
     * compressed node.
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
     * 5) Inserting "ANNI"
     *
     *     "ANNI" -> "BALE" -> "SCO" -> []
     *
     * The final algorithm for insertion covering all the above cases is as
     * follows.
     *
     * ============================= ALGO 1 =============================
     *
     * For the above cases 1 to 4, that is, all cases where we stopped in
     * the middle of a compressed node for a character mismatch, do:
     *
     * Let $SPLITPOS be the zero-based index at which, in the
     * compressed node array of characters, we found the mismatching
     * character. For example if the node contains "ANNIBALE" and we add
     * "ANNIENTARE" the $SPLITPOS is 4, that is, the index at which the
     * mismatching character is found.
     *
     * 1. Save the current compressed node $NEXT pointer (the pointer to the
     *    child element, that is always present in compressed nodes).
     *
     * 2. Create "split node" having as child the non common letter
     *    at the compressed node. The other non common letter (at the key)
     *    will be added later as we continue the normal insertion algorithm
     *    at step "6".
     *
     * 3a. IF $SPLITPOS == 0:
     *     Replace the old node with the split node, by copying the auxiliary
     *     data if any. Fix parent's reference. Free old node eventually
     *     (we still need its data for the next steps of the algorithm).
     *
     * 3b. IF $SPLITPOS != 0:
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
     *
     * ============================= ALGO 2 =============================
     *
     * For case 5, that is, if we stopped in the middle of a compressed
     * node but no mismatch was found, do:
     *
     * Let $SPLITPOS be the zero-based index at which, in the
     * compressed node array of characters, we stopped iterating because
     * there were no more keys character to match. So in the example of
     * the node "ANNIBALE", addig the string "ANNI", the $SPLITPOS is 4.
     *
     * 1. Save the current compressed node $NEXT pointer (the pointer to the
     *    child element, that is always present in compressed nodes).
     *
     * 2. Create a "postfix node" containing all the characters from $SPLITPOS
     *    to the end. Use $NEXT as the postfix node child pointer.
     *    If the postfix node length is 1, set iscompr to 0.
     *    Set the node as a key with the associated value of the new
     *    inserted key.
     *
     * 3. Trim the current node to contain the first $SPLITPOS characters.
     *    As usually if the new node length is just 1, set iscompr to 0.
     *    Take the iskey / associated value as it was in the orignal node.
     *    Fix the parent's reference.
     *
     * 4. Set the postfix node as the only child pointer of the trimmed
     *    node created at step 1.
     */

    /* ------------------------- ALGORITHM 1 --------------------------- */
    if (h->iscompr && i != len) {
        debugf("ALGO 1: Stopped at compressed node %.*s (%p)\n",
            h->size, h->data, (void*)h);
        debugf("Still to insert: %.*s\n", (int)(len-i), s+i);
        debugf("Splitting at %d: '%c'\n", j, ((char*)h->data)[j]);
        debugf("Other (key) letter is '%c'\n", s[i]);

        /* 1: Save next pointer. */
        raxNode **childfield = raxNodeLastChildPtr(h);
        raxNode *next;
        memcpy(&next,childfield,sizeof(next));
        debugf("Next is %p\n", (void*)next);
        debugf("iskey %d\n", h->iskey);
        if (h->iskey) {
            debugf("key value is %p\n", raxGetData(h));
        }

        /* 2: Create the split node. */
        raxNode *splitnode = raxNewNode(1);
        splitnode->data[0] = h->data[j];

        if (j == 0) {
            /* 3a: Replace the old node with the split node. */
            if (h->iskey) {
                void *ndata = raxGetData(h);
                splitnode = raxReallocForData(splitnode,ndata);
                raxSetData(splitnode,ndata);
            }
            memcpy(parentlink,&splitnode,sizeof(splitnode));
        } else {
            /* 3b: Trim the compressed node. */
            size_t nodesize = sizeof(raxNode)+j+sizeof(raxNode*);
            if (h->iskey && !h->isnull) nodesize += sizeof(void*);
            raxNode *trimmed = malloc(nodesize);
            trimmed->size = j;
            memcpy(trimmed->data,h->data,j);
            trimmed->iscompr = j > 1 ? 1 : 0;
            trimmed->iskey = h->iskey;
            trimmed->isnull = h->isnull;
            if (h->iskey && !h->isnull) {
                void *ndata = raxGetData(h);
                raxSetData(trimmed,ndata);
            }
            raxNode **cp = raxNodeLastChildPtr(trimmed);
            memcpy(cp,&splitnode,sizeof(splitnode));
            memcpy(parentlink,&trimmed,sizeof(trimmed));
            parentlink = cp; /* Set parentlink to splitnode parent. */
            rax->numnodes++;
        }

        /* 4: Create the postfix node: what remains of the original
         * compressed node after the split. */
        size_t postfixlen = h->size - j - 1;
        raxNode *postfix;
        if (postfixlen) {
            /* 4a: create a postfix node. */
            size_t nodesize = sizeof(raxNode)+postfixlen+
                              sizeof(raxNode*);
            postfix = malloc(nodesize);
            postfix->iskey = 0;
            postfix->isnull = 0;
            postfix->size = postfixlen;
            postfix->iscompr = postfixlen > 1;
            memcpy(postfix->data,h->data+j+1,postfixlen);
            raxNode **cp = raxNodeLastChildPtr(postfix);
            memcpy(cp,&next,sizeof(next));
            rax->numnodes++;
        } else {
            /* 4b: just use next as postfix node. */
            postfix = next;
        }

        /* 5: Set splitnode first child as the postfix node. */
        raxNode **splitchild = raxNodeLastChildPtr(splitnode);
        memcpy(splitchild,&postfix,sizeof(postfix));

        /* 6. Continue insertion: this will cause the splitnode to
         * get a new child (the non common character at the currently
         * inserted key). */
        free(h);
        h = splitnode;
    } else if (h->iscompr && i == len) {
    /* ------------------------- ALGORITHM 2 --------------------------- */
        debugf("ALGO 2: Stopped at compressed node %.*s (%p) j = %d\n",
            h->size, h->data, (void*)h, j);

        /* 1: Save next pointer. */
        raxNode **childfield = raxNodeLastChildPtr(h);
        raxNode *next;
        memcpy(&next,childfield,sizeof(next));

        /* 2: Create the postfix node. */
        size_t postfixlen = h->size - j;
        size_t nodesize = sizeof(raxNode)+postfixlen+sizeof(raxNode*);
        if (data != NULL) nodesize += sizeof(void*);
        raxNode *postfix = malloc(nodesize);
        postfix->size = postfixlen;
        postfix->iscompr = postfixlen > 1;
        postfix->iskey = 1;
        postfix->isnull = 0;
        memcpy(postfix->data,h->data+j,postfixlen);
        raxSetData(postfix,data);
        raxNode **cp = raxNodeLastChildPtr(postfix);
        memcpy(cp,&next,sizeof(next));
        rax->numnodes++;

        /* 3: Trim the compressed node. */
        nodesize = sizeof(raxNode)+j+sizeof(raxNode*);
        if (h->iskey && !h->isnull) nodesize += sizeof(void*);
        raxNode *trimmed = malloc(nodesize);
        trimmed->size = j;
        trimmed->iscompr = j > 1;
        trimmed->iskey = 0;
        trimmed->isnull = 0;
        memcpy(trimmed->data,h->data,j);
        memcpy(parentlink,&trimmed,sizeof(trimmed));
        if (h->iskey) {
            void *aux = raxGetData(h);
            raxSetData(trimmed,aux);
        }

        /* Fix the trimmed node child pointer to point to
         * the postfix node. */
        cp = raxNodeLastChildPtr(trimmed);
        memcpy(cp,&postfix,sizeof(postfix));

        /* Finish! We don't need to contine with the insertion
         * algorithm for ALGO 2. The key is already inserted. */
        rax->numele++;
        return 1; /* Key inserted. */
    }

    /* We walked the radix tree as far as we could, but still there are left
     * chars in our string. We need to insert the missing nodes.
     * Note: while loop never entered if the node was split by ALGO2,
     * since i == len. */
    while(i < len) {
        raxNode *child;
        rax->numnodes++;

        /* If this node is going to have a single child, and there
         * are other characters, so that that would result in a chain
         * of single-childed nodes, turn it into a compressed node. */
        if (h->size == 0 && len-i > 1) {
            debugf("Inserting compressed node\n");
            size_t comprsize = len-i;
            if (comprsize > RAX_NODE_MAX_SIZE)
                comprsize = RAX_NODE_MAX_SIZE;
            h = raxCompressNode(h,s+i,comprsize,&child);
            memcpy(parentlink,&h,sizeof(h));
            parentlink = raxNodeLastChildPtr(h);
            i += comprsize;
        } else {
            debugf("Inserting normal node\n");
            raxNode **new_parentlink;
            h = raxAddChild(h,s[i],&child,&new_parentlink);
            memcpy(parentlink,&h,sizeof(h));
            parentlink = new_parentlink;
            i++;
        }
        h = child;
    }
    if (!h->iskey) rax->numele++;
    h = raxReallocForData(h,data);
    raxSetData(h,data);
    memcpy(parentlink,&h,sizeof(h));
    return 1; /* Element inserted. */
}

/* Find a key in the rax, returns raxNotFound special void pointer value
 * if the item was not found, otherwise the value associated with the
 * item is returned. */
void *raxFind(rax *rax, unsigned char *s, size_t len) {
    raxNode *h;

    debugf("### Lookup: %.*s\n", (int)len, s);
    size_t i = raxLowWalk(rax,s,len,&h,NULL,NULL,NULL);
    if (i != len) return raxNotFound;
    debugf("Lookup final node: [%p] iskey? %d\n",(void*)h,h->iskey);
    return h->iskey ? raxGetData(h) : raxNotFound;
}

/* Return the memory address where the 'parent' node stores the specified
 * 'child' pointer, so that the caller can update the pointer with another
 * one if needed. The function assumes it will find a match, otherwise the
 * operation is an undefined behavior (it will continue scanning the
 * memory without any bound checking). */
raxNode **raxFindParentLink(raxNode *parent, raxNode *child) {
    raxNode **cp = raxNodeFirstChildPtr(parent);
    raxNode *c;
    while(1) {
        memcpy(&c,cp,sizeof(c));
        if (c == child) break;
        cp++;
    }
    return cp;
}

/* Low level child removal from node. The new node pointer (after the child
 * removal) is returned. Note that this function does not fix the pointer
 * of the node in its parent, so this task is up to the caller. */
raxNode *raxRemoveChild(raxNode *parent, raxNode *child) {
    debugnode("raxRemoveChild before", parent);
    /* If parent is a compressed node (having a single child, as for definition
     * of the data structure), the removal of the child consists into turning
     * it into a normal node without children. */
    if (parent->iscompr) {
        raxNode *newnode = raxNewNode(0);
        if (parent->iskey) {
            void *data = raxGetData(parent);
            newnode = raxReallocForData(newnode,data);
            raxSetData(newnode,data);
        }
        debugnode("raxRemoveChild after", newnode);
        return newnode;
    }

    /* Otherwise we need to scan for the children pointer and memmove()
     * accordingly.
     *
     * 1. To start we seek the first element in both the children
     *    pointers and edge bytes in the node. */
    raxNode **cp = raxNodeLastChildPtr(parent) - (parent->size-1);
    raxNode **c = cp;
    unsigned char *e = parent->data;

    /* 2. Search the child pointer to remove inside the array of children
     *    pointers. */
    while(1) {
        raxNode *aux;
        memcpy(&aux,c,sizeof(aux));
        if (aux == child) break;
        c++;
        e++;
    }

    /* 3. Remove the edge and the pointer by memmoving the remaining children
     *    pointer and edge bytes one position before. */
    int taillen = parent->size - (e - parent->data) - 1;
    debugf("raxRemoveChild tail len: %d\n", taillen);
    memmove(e,e+1,taillen);

    /* Since we have one data byte less, also child pointers start one byte
     * before now. */
    memmove(((char*)cp)-1,cp,(parent->size-taillen-1)*sizeof(raxNode**));

    /* Move the remaining "tail" pointer at the right position as well. */
    memmove(((char*)c)-1,c+1,taillen*sizeof(raxNode**));

    /* 4. Update size. */
    parent->size--;

    /* Realloc the node according to the theoretical memory usage, to free
     * data if we are over-allocating right now. */
    raxNode *newnode = realloc(parent,raxNodeCurrentLength(parent));
    debugnode("raxRemoveChild after", newnode);
    /* Note: if realloc() fails we just return the old address, which
     * is valid. */
    return newnode ? newnode : parent;
}

/* Remove the specified item. Returns 1 if the item was found and
 * deleted, 0 otherwise. */
int raxRemove(rax *rax, unsigned char *s, size_t len) {
    raxNode *h;
    raxStack ts;

    debugf("### Delete: %.*s\n", (int)len, s);
    raxStackInit(&ts);
    size_t i = raxLowWalk(rax,s,len,&h,NULL,NULL,&ts);
    if (i != len || !h->iskey) {
        raxStackFree(&ts);
        return 0;
    }
    h->iskey = 0;
    rax->numele--;

    /* If this node has no children, the deletion needs to reclaim the
     * no longer used nodes. This is an iterative process that needs to
     * walk the three upward, deleting all the nodes with just one child
     * that are not keys, until the head of the rax is reached or the first
     * node with more than one child is found. */

    int trycompress = 0; /* Will be set to 1 if we should try to optimize the
                            tree resulting from the deletion. */

    if (h->size == 0) {
        debugf("Key deleted in node without children. Cleanup needed.\n");
        raxNode *child = NULL;
        while(h != rax->head) {
            child = h;
            debugf("Freeing child %p [%.*s] key:%d\n", (void*)child,
                (int)child->size, (char*)child->data, child->iskey);
            free(child);
            rax->numnodes--;
            h = raxStackPop(&ts);
             /* If this node has more then one child, or actually holds
              * a key, stop here. */
            if (h->iskey || (!h->iscompr && h->size != 1)) break;
        }
        if (child) {
            debugf("Unlinking child %p from parent %p\n",
                (void*)child, (void*)h);
            raxNode *new = raxRemoveChild(h,child);
            if (new != h) {
                raxNode *parent = raxStackPeek(&ts);
                raxNode **parentlink;
                if (parent == NULL) {
                    parentlink = &rax->head;
                } else {
                    parentlink = raxFindParentLink(parent,h);
                }
                memcpy(parentlink,&new,sizeof(new));
            }

            /* If after the removal the node has just a single child
             * and is not a key, we need to try to compress it. */
            if (new->size == 1 && new->iskey == 0) {
                trycompress = 1;
                h = new;
            }
        }
    } else if (h->size == 1) {
        /* If the node had just one child, after the removal of the key
         * further compression with adjacent nodes is pontentially possible. */
        trycompress = 1;
    }

    /* Recompression: if trycompress is true, 'h' points to a radix tree node
     * that changed in a way that could allow to compress nodes in this
     * sub-branch. Compressed nodes represent chains of nodes that are not
     * keys and have a single child, so there are two deletion events that
     * may alter the tree so that further compression is needed:
     *
     * 1) A node with a single child was a key and now no longer is a key.
     * 2) A node with two children now has just one child.
     *
     * We try to navigate upward till there are other nodes that can be
     * compressed, when we reach the upper node which is not a key and has
     * a single child, we scan the chain of children to collect the
     * compressable part of the tree, and replace the current node with the
     * new one, fixing the child pointer to reference the first non
     * compressable node.
     *
     * Example of case "1". A tree stores the keys "FOO" = 1 and
     * "FOOBAR" = 2:
     *
     *
     * "FOO" -> "BAR" -> [] (2)
     *           (1)
     *
     * After the removal of "FOO" the tree can be compressed as:
     *
     * "FOOBAR" -> [] (2)
     *
     *
     * Example of case "2". A tree stores the keys "FOOBAR" = 1 and
     * "FOOTER" = 2:
     *
     *          |B| -> "AR" -> [] (1)
     * "FOO" -> |-|
     *          |T| -> "ER" -> [] (2)
     *
     * After the removal of "FOOTER" the resulting tree is:
     *
     * "FOO" -> |B| -> "AR" -> [] (1)
     *
     * That can be compressed into:
     *
     * "FOOBAR" -> [] (1)
     */
    if (trycompress) {
        debugf("After removing %.*s:\n", (int)len, s);
        debugnode("Compression may be needed",h);
        debugf("Seek start node\n");

        /* Try to reach the upper node that is compressible.
         * At the end of the loop 'h' will point to the first node we
         * can try to compress and 'parent' to its parent. */
        raxNode *parent;
        while(1) {
            parent = raxStackPop(&ts);
            if (!parent || parent->iskey ||
                (!parent->iscompr && parent->size != 1)) break;
            h = parent;
            debugnode("Going up to",h);
        }
        raxNode *start = h; /* Compression starting node. */

        /* Scan chain of nodes we can compress. */
        size_t comprsize = h->size;
        int nodes = 1;
        while(h->size != 0) {
            raxNode **cp = raxNodeLastChildPtr(h);
            memcpy(&h,cp,sizeof(h));
            if (h->iskey || (!h->iscompr && h->size != 1)) break;
            nodes++;
            comprsize += h->size;
        }
        if (nodes > 1) {
            /* If we can compress, create the new node and populate it. */
            size_t nodesize =
                sizeof(raxNode)+comprsize+sizeof(raxNode*);
            raxNode *new = malloc(nodesize);
            new->iskey = 0;
            new->isnull = 0;
            new->iscompr = 1;
            new->size = comprsize;
            rax->numnodes++;

            /* Scan again, this time to populate the new node content and
             * to fix the new node child pointer. At the same time we free
             * all the nodes that we'll no longer use. */
            comprsize = 0;
            h = start;
            while(h->size != 0) {
                memcpy(new->data+comprsize,h->data,h->size);
                comprsize += h->size;
                raxNode **cp = raxNodeLastChildPtr(h);
                raxNode *tofree = h;
                memcpy(&h,cp,sizeof(h));
                free(tofree); rax->numnodes--;
                if (h->iskey || (!h->iscompr && h->size != 1)) break;
            }
            debugnode("New node",new);

            /* Now 'h' points to the first node that we still need to use,
             * so our new node child pointer will point to it. */
            raxNode **cp = raxNodeLastChildPtr(new);
            memcpy(cp,&h,sizeof(h));

            /* Fix parent link. */
            if (parent) {
                raxNode **parentlink = raxFindParentLink(parent,start);
                memcpy(parentlink,&new,sizeof(new));
            } else {
                rax->head = new;
            }

            debugf("Compressed %d nodes, %d total bytes\n",
                nodes, (int)comprsize);
        }
    }
    raxStackFree(&ts);
    return 1;
}

/* This is the core of raxFree(): performs a depth-first scan of the
 * tree and releases all the nodes found. */
void raxRecursiveFree(rax *rax, raxNode *n) {
    int numchildren = n->iscompr ? 1 : n->size;
    raxNode **cp = raxNodeLastChildPtr(n);
    while(numchildren--) {
        raxNode *child;
        memcpy(&child,cp,sizeof(child));
        raxRecursiveFree(rax,child);
        cp--;
    }
    debugnode("free depth-first",n);
    free(n);
    rax->numnodes--;
}

/* Free a whole radix tree. */
void raxFree(rax *rax) {
    raxRecursiveFree(rax,rax->head);
    assert(rax->numnodes == 0);
    free(rax);
}

/* This function is mostly used for debugging and learning purposes.
 * It shows an ASCII representation of a tree on standard output, outling
 * all the nodes and the contained keys.
 *
 * The representation is as follow:
 *
 *  "foobar" (compressed node)
 *  [abc] (normal node with three children)
 *  [abc]=0x12345678 (node is a key, pointing to value 0x12345678)
 *  [] (a normal empty node)
 *
 *  Children are represented in new idented lines, each children prefixed by
 *  the "`-(x)" string, where "x" is the edge byte.
 *
 *  [abc]
 *   `-(a) "ladin"
 *   `-(b) [kj]
 *   `-(c) []
 *
 *  However when a node has a single child the following representation
 *  is used instead:
 *
 *  [abc] -> "ladin" -> []
 */

/* The actual implementation of raxShow(). */
void raxRecursiveShow(int level, int lpad, raxNode *n) {
    char s = n->iscompr ? '"' : '[';
    char e = n->iscompr ? '"' : ']';

    int numchars = printf("%c%.*s%c", s, n->size, n->data, e);
    if (n->iskey) {
        numchars += printf("=%p",raxGetData(n));
    }

    int numchildren = n->iscompr ? 1 : n->size;
    /* Note that 7 and 4 magic constants are the string length
     * of " `-(x) " and " -> " respectively. */
    if (level) {
        lpad += (numchildren > 1) ? 7 : 4;
        if (numchildren == 1) lpad += numchars;
    }
    raxNode **cp = raxNodeFirstChildPtr(n);
    for (int i = 0; i < numchildren; i++) {
        char *branch = " `-(%c) ";
        if (numchildren > 1) {
            printf("\n");
            for (int j = 0; j < lpad; j++) putchar(' ');
            printf(branch,n->data[i]);
        } else {
            printf(" -> ");
        }
        raxNode *child;
        memcpy(&child,cp,sizeof(child));
        raxRecursiveShow(level+1,lpad,child);
        cp++;
    }
}

/* Show a tree, as outlined in the comment above. */
void raxShow(rax *rax) {
    raxRecursiveShow(0,0,rax->head);
    putchar('\n');
}

/* Used by debugnode() macro to show info about a given node. */
void raxDebugShowNode(const char *msg, raxNode *n) {
    printf("%s: %p [%.*s] key:%d size:%d children:",
        msg, (void*)n, (int)n->size, (char*)n->data, n->iskey, n->size);
    int numcld = n->iscompr ? 1 : n->size;
    raxNode **cldptr = raxNodeLastChildPtr(n) - (numcld-1);
    while(numcld--) {
        raxNode *child;
        memcpy(&child,cldptr,sizeof(child));
        cldptr++;
        printf("%p ", (void*)child);
    }
    printf("\n");
    fflush(stdout);
}

#ifdef BENCHMARK_MAIN
#include <stdio.h>
#include <sys/time.h>

/* This is a simple Feistel network in order to turn every possible
 * uint32_t input into another "randomly" looking uint32_t. It is a
 * one to one map so there are no repetitions. */
static uint32_t int2int(uint32_t input) {
    uint16_t l = input & 0xffff;
    uint16_t r = input >> 16;
    for (int i = 0; i < 8; i++) {
        uint16_t nl = r;
        uint16_t F = (((r * 31) + (r >> 5) + 7 * 371) ^ r) & 0xffff;
        r = l ^ F;
        l = nl;
    }
    return (r<<16)|l;
}

/* Turn an uint32_t integer into an alphanumerical key and return its
 * length. This function is used in order to generate keys that have
 * a large charset, so that the radix tree can be testsed with many
 * children per node. */
static size_t int2alphakey(char *s, size_t maxlen, uint32_t i) {
    const char *set = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                      "abcdefghijklmnopqrstuvwxyz"
                      "0123456789";
    const size_t setlen = 62;

    if (maxlen == 0) return 0;
    maxlen--; /* Space for null term char. */
    size_t len = 0;
    while(len < maxlen) {
        s[len++] = set[i%setlen];
        i /= setlen;
        if (i == 0) break;
    }
    s[len] = '\0';
    return len;
}

/* Return the UNIX time in microseconds */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Turn the integer 'i' into a key according to 'mode'.
 * mode = 0, just represents the integer as a string.
 * mode = 1, turn it into a random-looking alphanumerical string
 *           according to the int2alphakey() function. */
static size_t int2key(char *s, size_t maxlen, uint32_t i, int mode) {
    if (mode == 0) {
        return snprintf(s,maxlen,"%lu",(unsigned long)i);
    } else {
        i = int2int(i);
        return int2alphakey(s,maxlen,i);
    }
}

int main(void) {
    for (int mode = 0; mode < 2; mode++) {
        printf("Benchmark with %s keys:\n",
            (mode == 0) ? "integer" : "alphanumerical");
        rax *t = raxNew();
        long long start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf,sizeof(buf),i,mode);
            raxInsert(t,(unsigned char*)buf,len,(void*)(long)i);
        }
        printf("Insert: %f\n", (double)(ustime()-start)/1000000);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf,sizeof(buf),i,mode);
            void *data = raxFind(t,(unsigned char*)buf,len);
            if (data != (void*)(long)i) {
                printf("Issue with %s: %p instead of %p\n", buf,
                    data, (void*)(long)i);
            }
        }
        printf("Lookup: %f\n", (double)(ustime()-start)/1000000);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int r = rand() % 5000000;
            int len = int2key(buf,sizeof(buf),r,mode);
            void *data = raxFind(t,(unsigned char*)buf,len);
            if (data != (void*)(long)r) {
                printf("Issue with %s: %p instead of %p\n", buf,
                    data, (void*)(long)r);
            }
        }
        printf("Random lookup: %f\n", (double)(ustime()-start)/1000000);

        start = ustime();
        int count = 0;
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf,sizeof(buf),i,mode);
            buf[i%len] = '!'; /* "!" is never set into keys. */
            void *data = raxFind(t,(unsigned char*) buf,len);
            if (data != (void*)(long)i) count++;
        }
        printf("Failed lookup: %f\n", (double)(ustime()-start)/1000000);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf,sizeof(buf),i,mode);
            int retval = raxRemove(t,(unsigned char*)buf,len);
            assert(retval == 1);
        }
        printf("Deletion: %f\n", (double)(ustime()-start)/1000000);

        printf("%llu total nodes\n", (unsigned long long)t->numnodes);
        printf("%llu total elements\n", (unsigned long long)t->numele);
        raxFree(t);
    }
    return 0;
}
#endif

#ifdef TEST_MAIN
#include <stdio.h>
#include <time.h>

int main(void) {
    printf("notfound = %p\n", raxNotFound);
    rax *t = raxNew();
    char *toadd[] = {"alligator","alien","baloon","chromodynamic","romane","romanus","romulus","rubens","ruber","rubicon","rubicundus",NULL};

    srand(time(NULL));
    for (int x = 0; x < 10000; x++) rand();

    long items = 0;
    while(toadd[items] != NULL) items++;

    for (long i = 0; i < items; i++)
        raxInsert(t,(unsigned char*)toadd[i],strlen(toadd[i]),(void*)i);
    raxShow(t);

    int rnum = rand();
    int survivor = rnum % items;

#if 1
    printf("Removing everything but %s in random order\n", toadd[survivor]);
    for (long i = 0; i < 1000; i++) {
        int r = rand() % items;
        if (r == survivor) continue;
        raxRemove(t,(unsigned char*)toadd[r],strlen(toadd[r]));
    }
#else
    printf("Removing rubicon\n");
    raxRemove(t,(unsigned char*)"rubicon",7);
#endif

    printf("%llu total nodes\n", (unsigned long long)t->numnodes);
    printf("%llu total elements\n", (unsigned long long)t->numele);

    raxShow(t);
    raxFree(t);
}
#endif
