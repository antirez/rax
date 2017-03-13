#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "radixtree.h"

/* Turn debugging messages on/off. */
#if 0
#define debugf(...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf(__VA_ARGS__);                                                   \
    } while (0);
#define debugnode(msg,n)                                                       \
    do {                                                                       \
        printf("%s: %p [%.*s] key:%d size:%d children:",                       \
            msg, (void*)n, (int)n->size, (char*)n->data, n->iskey, n->size);   \
        int numcld = n->iscompr ? 1 : n->size;                                 \
        radtreeNode **cldptr = radtreeNodeLastChildPtr(n) - (numcld-1);        \
        while(numcld--) printf("%p ", (void*)*(cldptr++));                     \
        printf("\n");                                                          \
    } while (0);
#else
#define debugf(...)
#define debugnode(msg,n)
#endif

/* This is a special pointer that is guaranteed to never have the same value
 * of a radix tree node. It's used in order to report "not found" error without
 * requiring the function to have multiple return values. */
void *radtreeNotFound = (void*)"radtree-not-found-pointer";

/* ------------------------- radtreeStack functions --------------------------
 * The radtreeStack is a simple stack of pointers that is capable of switching
 * from using a stack-allocated array to dynamic heap once a given number of
 * items are reached. It is used in order to retain the list of parent nodes
 * while walking the radix tree in order to implement certain operations that
 * need to navigate the tree upward.
 * ------------------------------------------------------------------------- */

#define RADTREESTACK_STACK_ITEMS 32
typedef struct radtreeStack {
    void **stack;
    size_t items, maxitems;
    void *static_items[RADTREESTACK_STACK_ITEMS];
} radtreeStack;

/* Initialize the stack. */
static inline void radtreeStackInit(radtreeStack *ts) {
    ts->stack = ts->static_items;
    ts->items = 0;
    ts->maxitems = RADTREESTACK_STACK_ITEMS;
}

/* Push an item into the stack, returns 1 on success, 0 on out of memory. */
static inline int radtreeStackPush(radtreeStack *ts, void *ptr) {
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
static inline void *radtreeStackPop(radtreeStack *ts) {
    if (ts->items == 0) return NULL;
    ts->items--;
    return ts->stack[ts->items];
}

/* Free the stack in case we used heap allocation. */
static inline void radtreeStackFree(radtreeStack *ts) {
    if (ts->stack != ts->static_items) free(ts->stack);
}

/* ----------------------------------------------------------------------------
 * Radis tree implementation
 * --------------------------------------------------------------------------*/

/* Allocate a new non compressed node with the specified number of children. */
radtreeNode *radtreeNewNode(size_t children) {
    size_t nodesize = sizeof(radtreeNode)+children+
                      sizeof(radtreeNode*)*children;
    radtreeNode *node = malloc(nodesize);
    node->iskey = 0;
    node->isnull = 0;
    node->iscompr = 0;
    node->size = children;
    return node;
}

/* Allocate a new radtree. */
radtree *radtreeNew(void) {
    radtree *radtree = malloc(sizeof(*radtree));
    radtree->numele = 0;
    radtree->numnodes = 1;
    radtree->head = radtreeNewNode(0);
    return radtree;
}

/* Return the current total size of the node. */
size_t radtreeNodeCurrentLength(radtreeNode *n) {
    size_t curlen = sizeof(radtreeNode)+n->size;
    curlen += n->iscompr ? sizeof(radtreeNode*) :
                           sizeof(radtreeNode*)*n->size;
    if (n->iskey && !n->isnull) curlen += sizeof(void*);
    return curlen;
}

/* Realloc the node to make room for auxiliary data in order
 * to store an item in that node. */
radtreeNode *radtreeReallocForData(radtreeNode *n, void *data) {
    if (data == NULL) return n; /* No reallocation needed, setting isnull=1 */
    size_t curlen = radtreeNodeCurrentLength(n);
    return realloc(n,curlen+sizeof(void*));
}

/* Set the node auxiliary data to the specified pointer. */
void radtreeSetData(radtreeNode *n, void *data) {
    n->iskey = 1;
    if (data != NULL) {
        void **ndata = (void**)
            ((char*)n+radtreeNodeCurrentLength(n)-sizeof(void*));
        *ndata = data;
        n->isnull = 0;
    } else {
        n->isnull = 1;
    }
}

/* Get the node auxiliary data. */
void *radtreeGetData(radtreeNode *n) {
    if (n->isnull) return NULL;
    void **ndata =(void**)((char*)n+radtreeNodeCurrentLength(n)-sizeof(void*));
    return *ndata;
}

/* Add a new child to the node 'n' representing the character
 * 'c' and return its new pointer, as well as the child pointer
 * by reference. */
radtreeNode *radtreeAddChild(radtreeNode *n, char c, radtreeNode **childptr) {
    assert(n->iscompr == 0);
    size_t curlen = sizeof(radtreeNode)+
                    n->size+
                    sizeof(radtreeNode*)*n->size;
    size_t newlen;
    if (n->iskey) curlen += sizeof(void*);
    newlen = curlen+sizeof(radtreeNode*)+1; /* Add 1 char and 1 pointer. */
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
            curlen-sizeof(radtreeNode)-n->size);

    /* Now, if present, move auxiliary data pointer at the end
     * so that we can store the additional child pointer without
     * overwriting it:
     *
     * [numc][abc].[ap][bp][cp]....|auxp| */
    if (n->iskey) {
        memmove(n->data+newlen-sizeof(radtreeNode)-sizeof(void*),
                n->data+newlen-sizeof(radtreeNode)-sizeof(void*)*2,
                sizeof(void*));
    }

    /* We can now set the character and its child node pointer:
     *
     * [numc][abcd][ap][bp][cp]....|auxp|
     * [numc][abcd][ap][bp][cp][dp]|auxp| */
    radtreeNode *child = radtreeNewNode(0);
    n->data[n->size] = c;
    memcpy(n->data+n->size+1+sizeof(radtreeNode*)*n->size,
           &child,sizeof(void*));
    n->size++;
    *childptr = child;
    return n;
}

/* Return the pointer to the last child pointer in a node. For the compressed
 * nodes this is the only child pointer. */
radtreeNode **radtreeNodeLastChildPtr(radtreeNode *n) {
    char *p = (char*)n;
    p += radtreeNodeCurrentLength(n) - sizeof(radtreeNode*);
    if (n->iskey && !n->isnull) p -= sizeof(void*);
    return (radtreeNode**)p;
}

/* Turn the node 'n', that must be a node without any children, into a
 * compressed node representing a set of nodes linked one after the other
 * and having exactly one child each. The node can be a key or not: this
 * property and the associated value if any will be preserved.
 *
 * The function also returns a child node, since the last node of the
 * compressed chain cannot be part of the chain: it has zero children while
 * we can only compress inner nodes with exactly one child each. */
radtreeNode *radtreeCompressNode(radtreeNode *n, unsigned char *s, size_t len, radtreeNode **child) {
    assert(n->size == 0 && n->iscompr == 0);
    void *data = NULL; /* Initialized only to avoid warnings. */
    size_t newsize;

    debugf("Compress node: %.*s\n", (int)len,s);

    newsize = sizeof(radtreeNode)+len+sizeof(radtreeNode*);
    if (n->iskey) {
        data = radtreeGetData(n); /* To restore it later. */
        if (!n->isnull) newsize += sizeof(void*);
    }

    n = realloc(n,newsize);
    n->iscompr = 1;
    n->size = len;
    memcpy(n->data,s,len);
    if (n->iskey) radtreeSetData(n,data);
    radtreeNode **childfield = radtreeNodeLastChildPtr(n);
    *child = radtreeNewNode(0);
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
static inline size_t radtreeLowWalk(radtree *radtree, unsigned char *s, size_t len, radtreeNode **stopnode, radtreeNode ***plink, int *splitpos, radtreeStack *ts) {
    radtreeNode *h = radtree->head;
    radtreeNode **parentlink = &radtree->head;

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

        if (ts) radtreeStackPush(ts,h); /* Save stack of parent nodes. */
        radtreeNode **children = (radtreeNode**)(h->data+h->size);
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
int radtreeInsert(radtree *radtree, unsigned char *s, size_t len, void *data) {
    size_t i;
    int j = 0; /* Split position. If radtreeLowWalk() stops in a compressed
                  node, the index 'j' represents the char we stopped within the
                  compressed node, that is, the position where to split the
                  node for insertion. */
    radtreeNode *h, **parentlink;

    debugf("### Insert %.*s with value %p\n", (int)len, s, data);
    i = radtreeLowWalk(radtree,s,len,&h,&parentlink,&j,NULL);

    /* If i == len we walked following the whole string. If we are not
     * in the middle of a compressed node, the string is either already
     * inserted or this middle node is currently not a key, but can represent
     * our key. We have just to reallocate the node and make space for the
     * data pointer. */
    if (i == len && (!h->iscompr || j == h->size)) {
        if (h->iskey) {
            radtreeSetData(h,data);
            return 0; /* Element already exists. */
        }
        h = radtreeReallocForData(h,data);
        *parentlink = h;
        radtreeSetData(h,data);
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
     * the node "ANNIBABLE", addig the string "ANNI", the $SPLITPOS is 4.
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
        radtreeNode **childfield = radtreeNodeLastChildPtr(h);
        radtreeNode *next = *childfield;
        debugf("Next is %p\n", (void*)next);
        debugf("iskey %d\n", h->iskey);
        if (h->iskey) {
            debugf("key value is %p\n", radtreeGetData(h));
        }

        /* 2: Create the split node. */
        radtreeNode *splitnode = radtreeNewNode(1);
        splitnode->data[0] = h->data[j];

        if (j == 0) {
            /* 3a: Replace the old node with the split node. */
            if (h->iskey) {
                void *ndata = radtreeGetData(h);
                splitnode = radtreeReallocForData(splitnode,ndata);
                radtreeSetData(splitnode,ndata);
            }
            *parentlink = splitnode;
        } else {
            /* 3b: Trim the compressed node. */
            size_t nodesize = sizeof(radtreeNode)+j+sizeof(radtreeNode*);
            if (h->iskey && !h->isnull) nodesize += sizeof(void*);
            radtreeNode *trimmed = malloc(nodesize);
            trimmed->size = j;
            memcpy(trimmed->data,h->data,j);
            trimmed->iscompr = j > 1 ? 1 : 0;
            trimmed->iskey = h->iskey;
            trimmed->isnull = h->isnull;
            if (h->iskey && !h->isnull) {
                void *ndata = radtreeGetData(h);
                radtreeSetData(trimmed,ndata);
            }
            radtreeNode **cp = radtreeNodeLastChildPtr(trimmed);
            *cp = splitnode;
            *parentlink = trimmed;
            parentlink = cp; /* Set parentlink to splitnode parent. */
            radtree->numnodes++;
        }

        /* 4: Create the postfix node: what remains of the original
         * compressed node after the split. */
        size_t postfixlen = h->size - j - 1;
        radtreeNode *postfix;
        if (postfixlen) {
            /* 4a: create a postfix node. */
            size_t nodesize = sizeof(radtreeNode)+postfixlen+
                              sizeof(radtreeNode*);
            postfix = malloc(nodesize);
            postfix->iskey = 0;
            postfix->isnull = 0;
            postfix->size = postfixlen;
            postfix->iscompr = postfixlen > 1;
            memcpy(postfix->data,h->data+j+1,postfixlen);
            radtreeNode **cp = radtreeNodeLastChildPtr(postfix);
            *cp = next;
            radtree->numnodes++;
        } else {
            /* 4b: just use next as postfix node. */
            postfix = next;
        }

        /* 5: Set splitnode first child as the postfix node. */
        radtreeNode **splitchild = radtreeNodeLastChildPtr(splitnode);
        *splitchild = postfix;

        /* 6. Continue insertion: this will cause the splitnode to
         * get a new child (the non common character at the currently
         * inserted key). */
        free(h);
        h = splitnode;
    } else if (h->iscompr && i == len) {
    /* ------------------------- ALGORITHM 2 --------------------------- */
        debugf("ALGO 2: Stopped at compressed node %.*s (%p)\n",
            h->size, h->data, (void*)h);

        /* 1: Save next pointer. */
        radtreeNode **childfield = radtreeNodeLastChildPtr(h);
        radtreeNode *next = *childfield;

        /* 2: Create the postfix node. */
        size_t postfixlen = h->size - j;
        size_t nodesize = sizeof(radtreeNode)+postfixlen+sizeof(radtreeNode*);
        if (data != NULL) nodesize += sizeof(void*);
        radtreeNode *postfix = malloc(nodesize);
        postfix->size = postfixlen;
        postfix->iscompr = postfixlen > 1;
        memcpy(postfix->data,h->data+j,postfixlen);
        radtreeSetData(postfix,data);
        radtreeNode **cp = radtreeNodeLastChildPtr(postfix);
        *cp = next;
        radtree->numnodes++;

        /* 3: Trim the compressed node. */
        nodesize = sizeof(radtreeNode)+j+sizeof(radtreeNode*);
        if (h->iskey && !h->isnull) nodesize += sizeof(void*);
        radtreeNode *trimmed = malloc(nodesize);
        trimmed->size = j;
        trimmed->iskey = 0;
        trimmed->isnull = 0;
        memcpy(trimmed->data,h->data,j);
        trimmed->iscompr = j > 1 ? 1 : 0;
        *parentlink = trimmed;
        if (h->iskey) {
            void *aux = radtreeGetData(h);
            radtreeSetData(trimmed,aux);
        }
        radtree->numele++;

        /* Fix the trimmed node child pointer to point to
         * the postfix node. */
        cp = radtreeNodeLastChildPtr(trimmed);
        *cp = postfix;

        /* Finish! We don't need to contine with the insertion
         * algorithm for ALGO 2. The key is already inserted. */
        return 1; /* Key inserted. */
    }

    /* We walked the radix tree as far as we could, but still there are left
     * chars in our string. We need to insert the missing nodes.
     * Note: while loop never entered if the node was split by ALGO2,
     * since i == len. */
    while(i < len) {
        radtreeNode *child;
        radtree->numnodes++;

        /* If this node is going to have a single child, and there
         * are other characters, so that that would result in a chain
         * of single-childed nodes, turn it into a compressed node. */
        if (h->size == 0 && len-i > 1) {
            debugf("Inserting compressed node\n");
            size_t comprsize = len-i;
            if (comprsize > RADTREE_NODE_MAX_SIZE)
                comprsize = RADTREE_NODE_MAX_SIZE;
            h = radtreeCompressNode(h,s+i,comprsize,&child);
            *parentlink = h;
            parentlink = radtreeNodeLastChildPtr(h);
            i += comprsize;
        } else {
            debugf("Inserting normal node\n");
            h = radtreeAddChild(h,s[i],&child);
            radtreeNode **children = (radtreeNode**)(h->data+h->size);
            *parentlink = h;
            parentlink = &children[h->size-1];
            i++;
        }
        h = child;
    }
    if (!h->iskey) radtree->numele++;
    h = radtreeReallocForData(h,data);
    radtreeSetData(h,data);
    *parentlink = h;
    return 1; /* Element inserted. */
}

/* Find a key in the radtree, returns radtreeNotFound special void pointer value
 * if the item was not found, otherwise the value associated with the
 * item is returned. */
void *radtreeFind(radtree *radtree, unsigned char *s, size_t len) {
    radtreeNode *h;

    debugf("### Lookup: %.*s\n", (int)len, s);
    size_t i = radtreeLowWalk(radtree,s,len,&h,NULL,NULL,NULL);
    if (i != len) return radtreeNotFound;
    debugf("Lookup final node: [%p] iskey? %d\n",(void*)h,h->iskey);
    return h->iskey ? radtreeGetData(h) : radtreeNotFound;
}

/* Return the memory address where the 'parent' node stores the specified
 * 'child' pointer, so that the caller can update the pointer with another
 * one if needed. The function assumes it will find a match, otherwise the
 * operation is an undefined behavior (it will continue scanning the
 * memory without any bound checking). */
radtreeNode **radtreeFindParentLink(radtreeNode *parent, radtreeNode *child) {
    radtreeNode **cp = radtreeNodeLastChildPtr(parent);
    if (!parent->iscompr) cp -= (parent->size-1);
    while(*cp != child) cp++;
    return cp;
}

/* Low level child removal from node. The new node pointer (after the child
 * removal) is returned. Note that this function does not fix the pointer
 * of the node in its parent, so this task is up to the caller. */
radtreeNode *radtreeRemoveChild(radtreeNode *parent, radtreeNode *child) {
    debugnode("radtreeRemoveChild before", parent);
    /* If parent is a compressed node (having a single child, as for definition
     * of the data structure), the removal of the child consists into turning
     * it into a normal node without children. */
    if (parent->iscompr) {
        radtreeNode *newnode = radtreeNewNode(0);
        if (parent->iskey) {
            void *data = radtreeGetData(parent);
            newnode = radtreeReallocForData(newnode,data);
            radtreeSetData(newnode,data);
        }
        debugnode("radtreeRemoveChild after", newnode);
        return newnode;
    }

    /* Otherwise we need to scan for the children pointer and memmove()
     * accordingly.
     *
     * 1. To start we seek the first element in both the children
     *    pointers and edge bytes in the node. */
    radtreeNode **cp = radtreeNodeLastChildPtr(parent) - (parent->size-1);
    radtreeNode **c = cp;
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
    debugf("radtreeRemoveChild tail len: %d\n", taillen);
    memmove(e,e+1,taillen);

    /* Since we have one data byte less, also child pointers start one byte
     * before now. */
    memmove(((char*)cp)-1,cp,(parent->size-taillen-1)*sizeof(radtreeNode**));

    /* Move the remaining "tail" pointer at the right position as well. */
    memmove(((char*)c)-1,c+1,taillen*sizeof(radtreeNode**));

    /* 4. Update size. */
    parent->size--;

    /* Realloc the node according to the theoretical memory usage, to free
     * data if we are over-allocating right now. */
    radtreeNode *newnode = realloc(parent,radtreeNodeCurrentLength(parent));
    debugnode("radtreeRemoveChild after", newnode);
    /* Note: if realloc() fails we just return the old address, which
     * is valid. */
    return newnode ? newnode : parent;
}

/* Remove the specified item. Returns 1 if the item was found and
 * deleted, 0 otherwise. */
int radtreeRemove(radtree *radtree, unsigned char *s, size_t len) {
    radtreeNode *h;
    radtreeStack ts;

    debugf("### Delete: %.*s\n", (int)len, s);
    radtreeStackInit(&ts);
    size_t i = radtreeLowWalk(radtree,s,len,&h,NULL,NULL,&ts);
    if (i != len || !h->iskey) {
        radtreeStackFree(&ts);
        return 0;
    }
    h->iskey = 0;

    /* If this node has no children, the deletion needs to reclaim the
     * no longer used nodes. This is an iterative process that needs to
     * walk the three upward, deleting all the nodes with just one child
     * that are not keys, until the head of the radtree is reached or the first
     * node with more than one child is found. */

    int trycompress = 0; /* Will be set to 1 if we should try to optimize the
                            tree resulting from the deletion. */

    if (h->size == 0) {
        debugf("Key deleted in node without children. Cleanup needed.\n");
        radtreeNode *child = NULL;
        while(h != radtree->head) {
            child = h;
            debugf("Freeing child %p [%.*s] key:%d\n", (void*)child,
                (int)child->size, (char*)child->data, child->iskey);
            free(child);
            radtree->numnodes--;
            h = radtreeStackPop(&ts);
             /* If this node has more then one child, or actually holds
              * a key, stop here. */
            if (h->iskey || (!h->iscompr && h->size != 1)) break;
        }
        if (child) {
            debugf("Unlinking child %p from parent %p\n",
                (void*)child, (void*)h);
            radtreeNode *new = radtreeRemoveChild(h,child);
            if (new != h) {
                radtreeNode *parent = radtreeStackPop(&ts);
                radtreeNode **parentlink;
                if (parent == NULL) {
                    parentlink = &radtree->head;
                } else {
                    parentlink = radtreeFindParentLink(parent,h);
                }
                *parentlink = new;
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
        debugnode("Attempt compression starting from",h);
    }

    radtreeStackFree(&ts);
    return 1;
}

/* This is the core of radtreeFree(): performs a depth-first scan of the
 * tree and releases all the nodes found. */
void radtreeRecursiveFree(radtree *radtree, radtreeNode *n) {
    int numchildren = n->iscompr ? 1 : n->size;
    radtreeNode **cp = radtreeNodeLastChildPtr(n);
    while(numchildren--) {
        radtreeRecursiveFree(radtree,*cp);
        cp--;
    }
    debugnode("free depth-first",n);
    free(n);
    radtree->numnodes--;
}

/* Free a whole radix tree. */
void radtreeFree(radtree *radtree) {
    radtreeRecursiveFree(radtree,radtree->head);
    assert(radtree->numnodes == 0);
    free(radtree);
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

/* The actual implementation of radtreeShow(). */
void radtreeRecursiveShow(int level, int lpad, radtreeNode *n) {
    char s = n->iscompr ? '"' : '[';
    char e = n->iscompr ? '"' : ']';

    int numchars = printf("%c%.*s%c", s, n->size, n->data, e);
    if (n->iskey) {
        numchars += printf("=%p",radtreeGetData(n));
    }

    int numchildren = n->iscompr ? 1 : n->size;
    /* Note that 7 and 4 magic constants are the string length
     * of " `-(x) " and " -> " respectively. */
    if (level) {
        lpad += (numchildren > 1) ? 7 : 4;
        if (numchildren == 1) lpad += numchars;
    }
    radtreeNode **cp = radtreeNodeLastChildPtr(n);
    cp -= numchildren-1;
    for (int i = 0; i < numchildren; i++) {
        char *branch = " `-(%c) ";
        if (numchildren > 1) {
            printf("\n");
            for (int j = 0; j < lpad; j++) putchar(' ');
            printf(branch,n->data[i]);
        } else {
            printf(" -> ");
        }
        radtreeRecursiveShow(level+1,lpad,*cp);
        cp++;
    }
}

/* Show a tree, as outlined in the comment above. */
void radtreeShow(radtree *radtree) {
    radtreeRecursiveShow(0,0,radtree->head);
    putchar('\n');
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
    radtree *t = radtreeNew();

    long long start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i);
        radtreeInsert(t,(unsigned char*)buf,len,(void*)(long)i);
    }
    printf("Insert: %f\n", (double)(ustime()-start)/1000000);

    start = ustime();
    for (int i = 0; i < 5000000; i++) {
        char buf[64];
        int len = snprintf(buf,sizeof(buf),"%d",i);
        void *data = radtreeFind(t,(unsigned char*)buf,len);
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
        void *data = radtreeFind(t,(unsigned char*)buf,len);
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
        void *data = radtreeFind(t,(unsigned char*) buf,len);
        if (data != (void*)(long)i) count++;
    }
    printf("Failed lookup: %f\n", (double)(ustime()-start)/1000000);

    printf("%llu total nodes\n", (unsigned long long)t->numnodes);
    printf("%llu total elements\n", (unsigned long long)t->numele);

    start = ustime();
    radtreeFree(t);
    printf("Tree release: %f\n", (double)(ustime()-start)/1000000);

    return 0;
}
#endif

#ifdef TEST_MAIN
#include <stdio.h>

int main(void) {
    printf("notfound = %p\n", radtreeNotFound);
    radtree *t = radtreeNew();
    #if 1
    char *toadd[] = {"romane","romanus","romulus","rubens","ruber","rubicon","rubicundus",NULL};
    long i = 0;
    while (toadd[i] != NULL) {
        radtreeInsert(t,(unsigned char*)toadd[i],strlen(toadd[i]),(void*)i);
        i++;
    }
    #else
    radtreeInsert(t,(unsigned char*)"foobar",6,(void*)1);
    radtreeInsert(t,(unsigned char*)"foo",3,(void*)2);
    radtreeInsert(t,(unsigned char*)"foob",4,(void*)3);
    printf("foobar = %p\n", radtreeFind(t,(unsigned char*)"foobar",6));
    printf("foo = %p\n", radtreeFind(t,(unsigned char*)"foo",3));
    printf("foob = %p\n", radtreeFind(t,(unsigned char*)"foob",4));
    radtreeRemove(t,(unsigned char*)"foo",3);
    #endif
    radtreeShow(t);
    radtreeFree(t);
}
#endif
