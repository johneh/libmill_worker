#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#define MILL_CHOOSE 1
#include "libmill.h"

/*
On Linux, closing the write end of a pipe in one thread has no effect
on epoll/poll on other threads. Instead of a single pipe with multiple
writers, provide each writer thread with its own pipe and multiplex
the received values onto a single channel in the receiving thread.
*/

/* thread start routine */
void *produce(void *q) {
    mill_pipe p = q;
    int i;
    mill_init(-1, 0);
    for (i = 1; i <= 10; i++) {
        mill_sleep(now() + (random() % 200));
        mill_pipesend(p, &i);
    }
    mill_pipeclose(p);    /* done */
    mill_pipefree(p);
    mill_fini();
    return NULL;
}

/* receive a value from a pipe and send it to the channel */
coroutine void recv_chs(mill_pipe p, chan ch, mill_wgroup wg) {
    mill_wgadd(wg);
    int done;
    while (1) {
        int k = *((int *)mill_piperecv(p, &done));
        if (done)
            break;
        chs(ch, int, k);
    }
    chclose(ch);
}

coroutine void wait_done(chan ch, mill_wgroup wg) {
    mill_wgwait(wg, -1);
    chdone(ch, int, 0);
}

int main(void) {
    pthread_t thrd[3];
    int done = 0;
    mill_pipe p[3];
    mill_init(-1, -1);
    chan ch = chmake(int, 1);
    mill_wgroup wg = mill_wgmake();
    int j;
    for (j = 0; j < 3; j++) { 
        p[j] = mill_pipemake(sizeof (int));
        go(recv_chs(p[j], chdup(ch), wg));
        pthread_create(&thrd[j], NULL, produce, mill_pipedup(p[j]));
    }

    /* wait for all reader coroutines to finish and then close the channel */
    go(wait_done(ch, wg));

    while (! done) {
    choose {
    in(ch, int, val):
        printf(" %d", val);
        done = ! val;
    otherwise:
        yield();
    end
    }
    }
    printf("\n");

    for (j = 0; j < 3; j++) {
        pthread_join(thrd[j], NULL);
        mill_pipefree(p[j]);
    }
    mill_wgfree(wg);
    chclose(ch);
    mill_fini();
    return 0;
}
