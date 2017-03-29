#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "rax.h"

int oomtest(int cycle) {
    printf("\n=========================\n");
    printf("Cycle %d\n", cycle);
    printf("=========================\n");

    char *toadd[] = {"alligator","alien","baloon","chromodynamic","romane","romanus","romulus","rubens","ruber","rubicon","rubicundus","all","rub","ba",NULL};

    rax *t = raxNew();
    if (t == NULL) return 0; /* Ok... */
    unsigned long items = 0;
    while(toadd[items] != NULL) items++;
    printf("%lu total items\n", items);

    unsigned long failed_insertions = 0;
    for (unsigned long i = 0; i < items; i++) {
        int retval = raxInsert(t,(unsigned char*)toadd[i],strlen(toadd[i]),(void*)i);
        if (retval == 0) {
            if (errno != ENOMEM) {
                printf("Insertion failed but errno != ENOMEM!\n");
                return 1;
            } else {
                printf("Failed to insert %s for OOM\n", toadd[i]);
                failed_insertions++;
            }
        } else {
            printf("Added %s = %p\n", toadd[i], (void*)(long)i);
        }
        if (cycle == 307) raxShow(t);

        if (t->numele != (i+1)-failed_insertions) {
            printf("After insert: %d elements expected, got %d\n",
                (int)((i+1)-failed_insertions), (int)t->numele);
            printf("Offending layout:\n");
            printf("---\n");
            raxShow(t);
            printf("---\n");
            return 1;
        }
    }

    /* Test that the number of items is ok. */
    if (t->numele != items-failed_insertions) {
    }

    raxShow(t);

    raxIterator iter;
    raxStart(&iter,t);

    struct {
        char *str;
        size_t len;
        char *op;
    } seek_places[] = {
        {"rpxxx",5,"<="},
        {"rom",3,">="},
        {"rub",3,">="},
        {"rub",3,">"},
        {"rub",3,"<"},
        {"rom",3,">"},
        {"chro",4,">"},
        {"chro",4,"<"},
        {"chromz",6,"<"},
        {NULL,0,"^"},
        {"zorro",5,"<="},
        {"zorro",5,"<"},
        {NULL,0,"$"},
        {"ro",2,">="}
    };

    int r = rand() % (sizeof(seek_places) / sizeof(seek_places[0]));

    printf("Seeking %s %s\n", seek_places[r].op, seek_places[r].str);
    if (raxSeek(&iter,(unsigned char*)seek_places[r].str,seek_places[r].len,
                                      seek_places[r].op) == 0)
    {
        goto cleanup;
    }

    int check_iterator_count = 1;
    unsigned long steps = 0;

    while(raxNext(&iter,NULL,0,NULL)) {
        printf("NEXT: %.*s, val %p\n", (int)iter.key_len,
                                      (char*)iter.key,
                                      iter.data);
        steps++;
    }
    if (errno == ENOMEM) {
        printf("raxNext() aborted on OOM\n");
        check_iterator_count = 0;
    }

    printf("Seeking %s %s\n", seek_places[r].op, seek_places[r].str);
    if (raxSeek(&iter,(unsigned char*)seek_places[r].str,seek_places[r].len,
                                      seek_places[r].op) == 0)
    {
        goto cleanup;
    }

    while(raxPrev(&iter,NULL,0,NULL)) {
        printf("PREV: %.*s, val %p\n", (int)iter.key_len,
                                      (char*)iter.key,
                                      iter.data);
        steps++;
    }
    if (errno == ENOMEM) {
        printf("raxNext() aborted on OOM\n");
        check_iterator_count = 0;
    }

    /* Check that we walked all the expected elements. */
    if (check_iterator_count) {
        if (steps != t->numele +1) {
            printf("Expected %d total iteration steps, did %d instead\n",
                (int)t->numele + 1, (int)steps);
            return 1;
        }
    }

cleanup:
    raxStop(&iter);
    raxFree(t);
    return 0;
}

int main(void) {
    srand(1234); /* Make the test reproducible. */
    for (int i = 0; i < 10000; i++) {
        if (oomtest(i)) {
            printf("Test failed\n");
            exit(1);
        }
    }
    printf("Test OK!\n");
    return 0;
}

