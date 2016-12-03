/*

  Copyright (c) 2015 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include <stdint.h>
#include <sys/param.h>

#include "cr.h"
#include "libpill.h"
#include "list.h"
#include "poller.h"
#include "timer.h"

/* Forward declarations for the functions implemented by specific poller
   mechanisms (poll, epoll, kqueue). */
static int mill_poller_add(struct mill_fd_s *mfd, int events);
static void mill_poller_rm(struct mill_cr *cr);
static void mill_poller_clean(struct mill_fd_s *mfd);
static int mill_poller_wait(int timeout);

/* Pause current coroutine for a specified time interval. */
void mill_sleep(int64_t deadline) {
    mill_fdwait(NULL, 0, deadline);
}

static void mill_poller_callback(struct mill_timer *timer) {
    struct mill_cr *cr = mill_cont(timer, struct mill_cr, timer);
    mill_resume(cr, 0);
    if (cr->mfd)
        mill_poller_rm(cr);
}

int mill_fdwait(struct mill_fd_s *mfd, int events, int64_t deadline) {
    mill_assert(mill != NULL);
    /* If required, start waiting for the timeout. */
    struct mill_cr *mill_running = mill->running;
    if(deadline >= 0)
        mill_timer_add(&mill_running->timer, deadline, mill_poller_callback);
    /* If required, start waiting for the file descriptor. */
    if(mfd) {
        int rc = mill_poller_add(mfd, events);
#if 0
        if (rc == -1) {
            if (deadline >= 0)
                mill_timer_rm(&mill_running->timer);
            return -1;
        }
#else
        if (rc == -1) {
            mill_assert(errno == EEXIST);
            mill_panic("multiple goroutine waiting for the same fd");
        }
#endif
    }

    /* Do actual waiting. */
    mill_running->state = mfd == NULL ? MILL_MSLEEP : MILL_FDWAIT;
    mill_running->mfd = mfd;
#ifdef MILLDEBUG
    mill_running->events = events;
#endif
    return mill_suspend();
}

void mill_fdclean(struct mill_fd_s *mfd) {
    if(mill_fast(mill))
        mill_poller_clean(mfd);
}

void mill_wait(int block) {
    while(1) {
        /* Compute timeout for the subsequent poll. */
        int timeout = block ? mill_timer_next() : 0;
        /* Wait for events. */
        int fd_fired = mill_poller_wait(timeout);
        /* Fire all expired timers. */
        int timer_fired = mill_timer_fire();
        /* Never retry the poll in non-blocking mode. */
        if(!block || fd_fired || timer_fired)
            break;
        /* If timeout was hit but there were no expired timers do the poll
           again. This should not happen in theory but let's be ready for the
           case when the system timers are not precise. */
    }
}

/* Include the poll-mechanism-specific stuff. */
#if 0
        /* FIXME -- kqueue.inc */
/* User overloads. */
#if defined MILL_EPOLL
#include "epoll.inc"
#elif defined MILL_KQUEUE
#include "kqueue.inc"
#elif defined MILL_POLL
#include "poll.inc"
/* Defaults. */
#elif defined __linux__ && !defined MILL_NO_EPOLL
#include "epoll.inc"
#elif defined BSD && !defined MILL_NO_KQUEUE
#include "kqueue.inc"
#else
#include "poll.inc"
#endif
#endif

#if defined MILL_NO_EPOLL
#include "poll.inc"
#elif defined MILL_EPOLL
#include "epoll.inc"
#else
#include "poll.inc"
#endif

