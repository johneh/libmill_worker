#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "libmill.h"

/* temporary worker with dedicated task queue */

struct sq_s {
    mill_pipe q1;
    mill_pipe q2;
};

int squarer(void *p) {
    mill_pipe q1 = ((struct sq_s *)p)->q1;
    mill_pipe q2 = ((struct sq_s *)p)->q2;
    while (1) {
        int done1;
        int k = *((int *)mill_piperecv(q1, &done1));
        if (done1)
            break;
        k = k*k;
        mill_pipesend(q2, &k);
    }
    mill_pipeclose(q2);
    mill_pipefree(q1);
    mill_pipefree(q2);
    return 0;
}

coroutine void start_squarer(mill_worker w1, mill_pipe q1, mill_pipe q2) {
    int rc = task_go(w1, squarer,
        & (struct sq_s) {
            .q1 = mill_pipedup(q1),
            .q2 = mill_pipedup(q2)
    }, -1);
    assert(rc == 0);
}

coroutine void do_square(mill_pipe q1, mill_pipe q2) {
    int k, done;
    for (k = 1; k <= 20; k++) {
        mill_pipesend(q1, &k);
        int r = *((int *)mill_piperecv(q2, &done));
        assert(!done);
        printf("%d => %d\n", k, r);
    }
    mill_pipeclose(q1);
}

int main(int argc, char **argv) {
    mill_init(-1, 0);
    mill_worker w1 = mill_worker_create();
    mill_pipe q1 = mill_pipemake(sizeof(int));
    assert(q1);
    mill_pipe q2 = mill_pipemake(sizeof(int));
    assert(q2);
    go(start_squarer(w1, q1, q2));
    go(do_square(q1, q2));

    mill_waitall(-1);
    mill_pipefree(q1);
    mill_pipefree(q2);
    mill_worker_delete(w1);
    return 0;
}

