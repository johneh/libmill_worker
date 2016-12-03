#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include "libpill.h"
#include "fd.h"
#include "utils.h"

/*
 * Linux eventfd based mutex alternative. Advantage over pthread-mutex
 * is that it does not block an entire thread.
*/

struct mill_mutex_s {
    int fd[2];
    int lock;
    int refcnt;
};

struct mill_mutex_s *mill_mutex_make(void) {
    int fd[2];
    struct mill_mutex_s *mu;
    int flag, errcode = 0;

    if (pipe(fd) == -1)
        return NULL;
    mu = mill_malloc(sizeof(struct mill_mutex_s));
    if (! mu) {
        errcode = ENOMEM;
        goto er;
    }
    memset(mu, '\0', sizeof(struct mill_mutex_s));
    int val = 1;
    while (1) {
        int rc = (int) write(fd[1], &val, sizeof(val));
        if (rc == sizeof(val))
            break;
        mill_assert(rc == -1);
        if (errno != EINTR) {
            errcode = errno;
            goto er;
        }
    }
 
    flag = fcntl(fd[0], F_GETFL);
    if (flag == -1)
        flag = 0;
    if (-1 == fcntl(fd[0], F_SETFL, flag|O_NONBLOCK)) {
        errcode = errno;
        goto er;
    }
#if 0
    flag = fcntl(fd[1], F_GETFL);
    if (flag == -1)
        flag = 0;
    if (-1 == fcntl(fd[1], F_SETFL, flag|O_NONBLOCK)) {
        errcode = errno;
        goto er;
    }
#endif
    mu->fd[0] = fd[0];
    mu->fd[1] = fd[1];
    mu->refcnt = 1;
    return mu;
er:
    close(fd[0]);
    close(fd[1]);
    if (mu)
        mill_free(mu);
    errno = errcode;
    return NULL;
}

static int trylock(struct mill_mutex_s *mu) {
    return mill_atomic_set(&mu->lock, 0, 1);
}

static void unlock(struct mill_mutex_s *mu) {
    int ret = mill_atomic_set(&mu->lock, 1, 0);
    mill_assert(ret);
}

void mill_mutex_lock(struct mill_mutex_s *mu) {
    unsigned size = sizeof(int);
    int n, val, total = 0;
    
    while (1) {
        if (trylock(mu)) {
again:
            n = (int) read(mu->fd[0], &val, size - total);
            mill_assert(n != 0);
            if (n > 0) {
                total += n;
                if (mill_fast(total == size))
                    return;
                /* reading from pipe may not be atomic */
                goto again;
            }
            /* n == -1 */
            if (errno == EINTR)
                goto again;
            if (errno == EAGAIN) {
                mill_fdevent(mu->fd[0], FDW_IN, -1);
                goto again;
            }
            unlock(mu);
            mill_panic(strerror(errno));
            break;
        }

        mill_fdevent(mu->fd[0], FDW_IN, -1);
        /* Multiple threads may receive notification. Race for the lock. */
    }
}

void mill_mutex_unlock(struct mill_mutex_s *mu) {
    int val = 1;
    unlock(mu);    /* N.B.: both trylock and unlock act as full memory barrier. */
    while (1) {
        int n = (int) write(mu->fd[1], &val, sizeof(int));
        if (n == sizeof(int))
            break;
        mill_assert(n < 0 && errno == EINTR);
    }
}

struct mill_mutex_s *mill_mutex_ref(struct mill_mutex_s *mu) {
    mill_atomic_add(&mu->refcnt, 1);
    return mu;
}

void mill_mutex_unref(struct mill_mutex_s *mu) {
    int ref = mill_atomic_sub(&mu->refcnt, 1);
    if (ref <= 0) {
        if (mill_atomic_add(&mu->lock, 0) != 0)
            mill_panic("destroying a locked mutex");
        (void) close(mu->fd[1]);    /* FIXME: check return value and errno == EINTR */
        (void) close(mu->fd[0]);    /* Ditto */
        mill_free(mu);
    }
}

