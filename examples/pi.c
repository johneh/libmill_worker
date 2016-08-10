#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "libmill.h"

double drandom(void) {
    return ((double) random()) / ((double) RAND_MAX + 1.0);
}

struct pi_s {
    int n;
    double pi;
};

/* Monte Carlo estimation of pi */
int calc_pi(void *q) {
    struct pi_s *s = q;
    int hit = 0;
    int i;
    for (i = 0; i < s->n; i++) {
        double x = drandom();
        double y = drandom();
        if (sqrt(x*x+y*y) <= 1.0)
            hit++;
    }
    s->pi = 4.0 * hit / s->n;
    return 0;
}

coroutine void do_pi(int n, int64_t deadline) {
    struct pi_s s;
    s.n = n;
    if (-1 == task_run(NULL, calc_pi, &s, deadline))
        printf("pi (n = %d) Task cancelled (timed out)\n", n);
    else
        printf("pi (n = %d) = %g\n", n, s.pi);
}

/* goroutine launching another goroutine in worker thread */
coroutine void do_pi2(int n, int64_t deadline) {
    struct pi_s s;
    s.n = n;
    if (-1 == task_go(NULL, calc_pi, &s, deadline))
        printf("pi_2(n = %d) Task cancelled (timed out)\n", n);
    else
        printf("pi_2(n = %d) = %g\n", n, s.pi);
}


int main(void) {
    mill_init(-1, 4);
    go(do_pi(100000, -1));
    go(do_pi(10000, -1));
    go(do_pi(1000, -1));
    go(do_pi(100,-1));
    go(do_pi2(100000, -1));
    go(do_pi2(10000, -1));
    go(do_pi(1000, -1));
    go(do_pi(500000, now()+15));
    go(do_pi(1000000, now()+100));
    go(do_pi(1000000, now()+1));
    go(do_pi2(1000000, now()+1));
    go(do_pi2(1000000, -1));

    mill_waitall(-1);
    return 0;
}
