#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include "libmill.h"
#include "fd.h"
#include "cr.h"
#include "utils.h"

/* Inter-thread communication using pipe */

struct mill_pipe_s {
    int fd[2];
    unsigned sz;
    int lockv;
    int refcnt;
};

struct mill_pipe_s *mill_pipemake(unsigned sz) {
    int fd[2];
    struct mill_pipe_s *mp;
    int flag, errcode = 0;

    mill_assert(sz <= 128);
    if (pipe(fd) == -1)
        return NULL;
    mp = mill_malloc(sizeof(struct mill_pipe_s));
    if (! mp) {
        errcode = ENOMEM;
        goto er;
    }
    memset(mp, '\0', sizeof(struct mill_pipe_s));

    flag = fcntl(fd[0], F_GETFL);
    if (flag == -1)
        flag = 0;
    if (-1 == fcntl(fd[0], F_SETFL, flag|O_NONBLOCK)) {
        errcode = errno;
        goto er;
    }
    flag = fcntl(fd[1], F_GETFL);
    if (flag == -1)
        flag = 0;
    if (-1 == fcntl(fd[1], F_SETFL, flag|O_NONBLOCK)) {
        errcode = errno;
        goto er;
    }
    mp->fd[0] = fd[0];
    mp->fd[1] = fd[1];
    mp->sz = sz;
    mp->refcnt = 1;
    return mp;
er:
    close(fd[0]);
    close(fd[1]);
    if (mp) mill_free(mp);
    errno = errcode;
    return NULL;
}

struct mill_pipe_s *mill_pipedup(struct mill_pipe_s *mp) {
    mill_atomic_add(&mp->refcnt, 1);
    return mp;
}

void mill_pipefree(struct mill_pipe_s *mp) {
    int ref = mill_atomic_sub(&mp->refcnt, 1);
    if (ref <= 0) {
        (void) close(mp->fd[1]); /* maybe already closed */
        (void) close(mp->fd[0]);
        mill_free(mp);
    }
}

static int trylock(struct mill_pipe_s *mp) {
    return mill_atomic_set(&mp->lockv, 0, 1);
}

static void unlock(struct mill_pipe_s *mp) {
    int ret = mill_atomic_set(&mp->lockv, 1, 0);
    mill_assert(ret);
}

static int pipe_read(struct mill_pipe_s *mp, void *ptr) {
    unsigned size = mp->sz;
    int n, total = 0;
    while (1) {
        if (trylock(mp)) {
again:
            n = (int) read(mp->fd[0], (char *) ptr + total, size - total);
            if (mill_slow(n == 0)) {
                /* done */
                mill_assert(total == 0);
                unlock(mp);
                return 0;
            }
            if (n > 0) {
                total += n;
                if (mill_fast(total == size)) {
                    unlock(mp);
                    return total;
                }
                goto again;
            }
            /* n == -1 */
            if (errno == EINTR)
                goto again;
            if (errno == EAGAIN) {
                mill_fdevent(mp->fd[0], FDW_IN, -1);
                goto again;
            }
            unlock(mp);
            break;
        }

        mill_fdevent(mp->fd[0], FDW_IN, -1);
        /* Multiple threads may receive notification. Race for the lock. */
    }
    return -1;
}

static int pipe_write(struct mill_pipe_s *mp, void *ptr) {
    while (1) {
        int n = (int) write(mp->fd[1], ptr, mp->sz);
        if (mill_fast(n == mp->sz))
            break;
        mill_assert(n < 0);
        if (errno == EINTR)
            continue;
        /* EAGAIN -- pipe capacity execeeded ? */
        if (errno != EAGAIN)
            return -1;
        mill_fdevent(mp->fd[1], FDW_OUT, -1);
    }
    return mp->sz;
}

void *mill_piperecv(struct mill_pipe_s *mp, int *done) {
    void *ptr = mill_valbuf(mill->running, mp->sz);
    mill_assert(done);
    int rc = pipe_read(mp, ptr);
    if (mill_slow(rc == -1))
        mill_panic(strerror(errno));    /* Hmm! */
    *done = (rc == 0);
    return ptr;
}

int mill_pipesend(struct mill_pipe_s *mp, void *ptr) {
    int rc = pipe_write(mp, ptr);
    if (mill_slow(rc == -1)) {
        /* mill_panic("attempt to send to a closed pipe"); */
        errno = EPIPE;
        return -1;
    }
    return 0;
}

void mill_pipeclose(struct mill_pipe_s *mp) {
    while (1) {
        if (0 == close(mp->fd[1])) {
            mp->fd[1] = -1;
            return;
        }
        if (errno != EINTR) {
            mill_assert(errno == EBADF);
            break;
        }
    }
    mill_panic("attempt to close previously closed pipe");
}

