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

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>

#include "cr.h"
#include "utils.h"
#include "fd.h"


/* TODO: oneshot fdwait() to avoid a fdclean() call. */

#define MILL_EPOLLSETSIZE 128

struct mill_poller {
    struct mill_list fds;
    int efd;
};

void mill_poller_init(void) {
    struct mill_poller *poller = mill_malloc(sizeof(struct mill_poller));
    if(!poller) {
        errno = ENOMEM;
        return;
    }
    poller->efd = epoll_create(1);
    if(mill_slow(poller->efd < 0)) {
        mill_free(poller);
        return;
    }
    mill_list_init(&poller->fds);
    mill->poller = poller;
    errno = 0;
}

void mill_poller_fini(void) {
    struct mill_poller *p = mill->poller;
    (void) close(p->efd);
    mill_free(p);
}

static int mill_poller_add(struct mill_fd_s *mfd, int events) {
    if(events & FDW_IN) {
        if(mfd->in) {
            errno = EEXIST;
            return -1;
        }
        mfd->in = mill->running;
    }
    if(events & FDW_OUT) {
        if(mfd->out) {
            errno = EEXIST;
            return -1;
        }
        mfd->out = mill->running;
    }

    if(mill_list_is_detached(&mfd->item))
        mill_list_insert(&mill->poller->fds, &mfd->item, NULL);
    return 0;
}

/* Mark to remove file descriptor from the pollset */
static void mill_poller_rm(struct mill_cr *cr) {
    struct mill_fd_s *mfd = cr->mfd;
    if(mfd->in == cr) {
        mfd->in = NULL;
        cr->mfd = NULL;
    }
    if(mfd->out == cr) {
        mfd->out = NULL;
        cr->mfd = NULL;
    }
    if(mill_list_is_detached(&mfd->item))
        mill_list_insert(&mill->poller->fds, &mfd->item, NULL);
}

static void mill_poller_clean(struct mill_fd_s *mfd) {
    struct mill_poller *poller = mill->poller;
    mill_assert(!mfd->in);
    mill_assert(!mfd->out);

    /* Remove the file descriptor from the pollset, if it is still present. */
    if(mfd->currevs) {
        struct epoll_event ev;
        ev.data.ptr = mfd;
        ev.events = 0;
        int rc = epoll_ctl(poller->efd, EPOLL_CTL_DEL, mfd->fd, &ev);
        mill_assert(rc == 0 || errno == ENOENT);
    }
    mfd->currevs = 0;
    /* Must remove from waiting list now, mfd may be freed or not be in scope. */
    if(!mill_list_is_detached(&mfd->item))
        mill_list_erase(&poller->fds, &mfd->item);
}

static int mill_poller_wait(int timeout) {
    struct mill_poller *poller = mill->poller;
    while(! mill_slist_empty(&poller->fds)) {
        struct mill_list_item *it = mill_list_begin(&poller->fds);
        struct mill_fd_s *iop = mill_cont(it, struct mill_fd_s, item);
        mill_list_erase(&poller->fds, it);
        struct epoll_event ev;
        ev.data.ptr = iop;
        ev.events = 0;
        if(iop->in)
            ev.events |= EPOLLIN;
        if(iop->out)
            ev.events |= EPOLLOUT;
        if(iop->currevs != ev.events) {
            int op;
            if(!ev.events)
                op = EPOLL_CTL_DEL;
            else if(!iop->currevs)
                op = EPOLL_CTL_ADD;
            else
                op = EPOLL_CTL_MOD;
            iop->currevs = ev.events;
            int rc = epoll_ctl(poller->efd, op, iop->fd, &ev);
            if (rc == -1 && errno == EEXIST)
                mill_panic("multiple goroutine waiting for the same fd");
            mill_assert(rc == 0);
        }
    }

    /* Wait for events. */
    struct epoll_event evs[MILL_EPOLLSETSIZE];
    int numevs;
    while(1) {
        numevs = epoll_wait(poller->efd, evs, MILL_EPOLLSETSIZE, timeout);
        if(numevs < 0 && errno == EINTR)
            continue;
        mill_assert(numevs >= 0);
        break;
    }
    /* Fire file descriptor events. */
    int i;
    for(i = 0; i != numevs; ++i) {
        struct mill_fd_s *iop = evs[i].data.ptr;
        int inevents = 0;
        int outevents = 0;
        struct mill_cr *cr;
        /* Set the result values. */
        if(evs[i].events & EPOLLIN)
            inevents |= FDW_IN;
        if(evs[i].events & EPOLLOUT)
            outevents |= FDW_OUT;
        if(evs[i].events & (EPOLLERR | EPOLLHUP)) {
            inevents |= FDW_ERR;
            outevents |= FDW_ERR;
        }
        /* Resume the blocked coroutines. */
        if(iop->in == iop->out) {
            cr = iop->in;
            mill_resume(cr, inevents | outevents);
            iop->in = iop->out = NULL;
            cr->mfd = NULL;
            if(mill_timer_enabled(&cr->timer))
                mill_timer_rm(&cr->timer);
        }
        else {
            if(iop->in && inevents) {
                cr = iop->in;
                mill_resume(cr, inevents);
                iop->in = NULL;
                cr->mfd = NULL;
                if(mill_timer_enabled(&cr->timer))
                    mill_timer_rm(&cr->timer);
            }
            if(iop->out && outevents) {
                cr = iop->out;
                mill_resume(cr, outevents);
                iop->out = NULL;
                cr->mfd = NULL;
                if(mill_timer_enabled(&cr->timer))
                    mill_timer_rm(&cr->timer);
            }
        }

        if(mill_list_is_detached(&iop->item))
            mill_list_insert(&poller->fds, &iop->item, NULL);
    }
    /* Return 0 in case of time out. 1 if at least one coroutine was resumed. */
    return numevs > 0 ? 1 : 0;
}
