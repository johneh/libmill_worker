#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include "libpill.h"

int count = 0;
double d = 0.0;
int pcount = 1000;
int noex = 0;

/* thread start routine */
void *produce(void *q) {
    mill_mutex mu = q;
    int i;
    mill_init(-1, 0);
    for (i = 1; i <= pcount; i++) {
        mill_sleep(now() + (random() % 2));
        if (noex) {
            count++;
            d++;
        } else {
            mill_mutex_lock(mu);
            count++;
            d++;
            mill_mutex_unlock(mu);
        }
    }
    mill_mutex_unref(mu);
    mill_fini();
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t thrd[3];
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        /* increment counter without any mutual exclusion. */
        noex = 1;
    }
    mill_init(-1, -1);
    mill_mutex mu = mill_mutex_make();
    int j;
    for (j = 0; j < 3; j++) {
        pthread_create(&thrd[j], NULL, produce, mill_mutex_ref(mu));
    }

    for (j = 0; j < 3; j++) {
        pthread_join(thrd[j], NULL);
    }
    mill_mutex_unref(mu);
    printf("count = %d, d = %g\n", count, d);
    assert(noex || (count == 3*pcount));

    mill_fini();
    return 0;
}
