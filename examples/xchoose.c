#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#define MILL_CHOOSE 1
#include "libpill.h"

coroutine void dot(char c, int *done) {
    while (! *done) {
        printf("%c", c);
        mill_sleep(now()+5);
    }
}

/* Send to the main thread */
coroutine void qsend(mill_pipe q, int i) {
    mill_sleep(now() + (random() % 500));
    mill_pipesend(q, &i);
}

/* thread start routine */
static void *produce(void *p) {
    mill_pipe q = p;
    int i;
    mill_init(-1, 0);
    for (i = 1; i <= 10; i++)
        go(qsend(q, i));
    mill_waitall(-1);
    mill_pipeclose(q);    /* done */
    mill_pipefree(q);
    mill_fini();
    return NULL;
}

/* Retrofit a channel to a pipe for 'choose' */
coroutine void qrecv(mill_pipe q, chan ch) {
    int done = 0;
    do {
        int k = *((int *)mill_piperecv(q, & done));
        if (done) {
            chdone(ch, int, 0);
            break;
        }
        chs(ch, int, k);
    } while (1);
}

int main(void) {
    pthread_t thrd1;
    int i = 0;
    int done = 0;

    mill_init(-1, 0);
    mill_pipe q = mill_pipemake(sizeof (int));
    go(dot('.', &done));
    chan ch = chmake(int, 1);
    go(qrecv(q, ch));

    pthread_create(& thrd1, NULL, produce, mill_pipedup(q));

    while (! done) {
    choose {
    in(ch, int, val):
        printf(" %d\n", val);
        done = ! val;
    otherwise:
        i++;
        yield();
    end
    }
    }

    pthread_join(thrd1, NULL);
    mill_pipefree(q);
    mill_fini();
    return 0;
}
