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
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>

#include "cr.h"
#include "fd.h"
#include "list.h"
#include "utils.h"
  
struct mill_poller {
    /* Pollset used for waiting for file descriptors. */
    int size;
    int capacity;
    struct pollfd *fds;
    struct mill_fd_s **items;
};

/* Find pollset index by fd. If fd is not in pollset, return the index after
   the last item. TODO: This is O(n) operation! */
static int mill_find_pollset(int fd) {
    int i;
    struct mill_poller *poller = mill->poller;
    for(i = 0; i != poller->size; ++i) {
        if(poller->fds[i].fd == fd)
            break;
    }
    return i;
}

void mill_poller_init(void) {
    struct mill_poller *p = mill_malloc(sizeof (struct mill_poller));
    if(!p) {
        errno = ENOMEM;
        return;
    }
    p->size = 0;
    p->capacity = 0;
    p->fds = NULL;
    p->items = NULL;
    mill->poller = p;
    errno = 0;
}

void mill_poller_fini(void) {
    struct mill_poller *p = mill->poller;
    if(p->fds)
        mill_free(p->fds);
    if(p->items)
        mill_free(p->items);
    mill_free(p);
    mill->poller = NULL;
}

static int mill_poller_add(struct mill_fd_s *mfd, int events) {
    struct mill_poller *poller = mill->poller;
    int i = mfd->index;
    mill_assert(events & (FDW_IN|FDW_OUT));
    if(!mfd->in && !mfd->out) {
        i = mill_find_pollset(mfd->fd);
        if(i < poller->size && poller->items[i] != mfd) {
            errno = EEXIST;
            return -1;
        }
    }
    /* Grow the pollset as needed. */
    if(i == poller->size) {
        if(poller->size == poller->capacity) {
            poller->capacity = poller->capacity ?
                poller->capacity * 2 : 64;
            poller->fds = mill_realloc(poller->fds,
                poller->capacity * sizeof(struct pollfd));
            poller->items = mill_realloc(poller->items,
                poller->capacity * sizeof(struct mill_fd_s *));
            if(!poller->fds || !poller->items) {
                errno = ENOMEM;
                return -1;
            }
        }
        ++poller->size;
        poller->fds[i].fd = mfd->fd;
        poller->fds[i].events = 0;
        poller->fds[i].revents = 0;
        poller->items[i] = mfd;
        mfd->index = i;
    }
    /* Register the new file descriptor in the pollset. */
    if(events & FDW_IN) {
        if(mill_slow(mfd->in)) {
            errno = EEXIST;
            return -1;
        }
        poller->fds[i].events |= POLLIN;
        mfd->in = mill->running;
    }
    if(events & FDW_OUT) {
        if(mill_slow(mfd->out)) {
            errno = EEXIST;
            return -1;
        }
        poller->fds[i].events |= POLLOUT;
        mfd->out = mill->running;
    }
    return 0;
}

static void mill_poller_rm(struct mill_cr *cr) {
    struct mill_fd_s *mfd = cr->mfd;
    mill_assert(mfd);
    struct mill_poller *poller = mill->poller;
    int i = mfd->index;
    mill_assert(i >=0 && i < poller->size);
    if(mfd->in == cr) {
        mfd->in = NULL;
        poller->fds[i].events &= ~POLLIN;
        cr->mfd = NULL;
    }
    if(mfd->out == cr) {
        mfd->out = NULL;
        poller->fds[i].events &= ~POLLOUT;
        cr->mfd = NULL;
    }
    if(!poller->fds[i].events) {
        --poller->size;
        if(i < poller->size) {
            poller->items[i] = poller->items[poller->size];
            poller->fds[i] = poller->fds[poller->size];
            poller->items[i]->index = i;
        }
    }
}

static void mill_poller_clean(struct mill_fd_s *mfd) {
}

static int mill_poller_wait(int timeout) {
    /* Wait for events. */
    int numevs;
    struct pollfd *mill_pollset_fds = mill->poller->fds;
    struct mill_fd_s **mill_pollset_items = mill->poller->items;
    int pollset_size = mill->poller->size;

    while(1) {
        numevs = poll(mill_pollset_fds, pollset_size, timeout);
        if(numevs < 0 && errno == EINTR)
            continue;
        mill_assert(numevs >= 0);
        break;  
    }
    if (numevs == 0)
        return 0;   /* timed out */

    /* Fire file descriptor events. */
    int i;
    for(i = 0; i < pollset_size && numevs; ++i) {
        int inevents = 0;
        int outevents = 0;
        if(!mill_pollset_fds[i].revents)
            continue;
        /* Set the result values. */
        if(mill_pollset_fds[i].revents & POLLIN)
            inevents |= FDW_IN;
        if(mill_pollset_fds[i].revents & POLLOUT)
            outevents |= FDW_OUT;
        if(mill_pollset_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            inevents |= FDW_ERR;
            outevents |= FDW_ERR;
        }
        mill_pollset_fds[i].revents = 0;

        /* Resume the blocked coroutines. */
        if(mill_pollset_items[i]->in &&
              mill_pollset_items[i]->in == mill_pollset_items[i]->out) {
            struct mill_cr *cr = mill_pollset_items[i]->in;
            cr->mfd = NULL;
            mill_resume(cr, inevents | outevents);
            mill_pollset_fds[i].events = 0;
            mill_pollset_items[i]->in = NULL;
            mill_pollset_items[i]->out = NULL;
            if(mill_timer_enabled(&cr->timer))
                mill_timer_rm(&cr->timer);
        }
        else {
            if(mill_pollset_items[i]->in && inevents) {
                struct mill_cr *cr = mill_pollset_items[i]->in;
                cr->mfd = NULL;
                mill_resume(cr, inevents);
                mill_pollset_fds[i].events &= ~POLLIN;
                mill_pollset_items[i]->in = NULL;
                if(mill_timer_enabled(&cr->timer))
                    mill_timer_rm(&cr->timer);
            }
            if(mill_pollset_items[i]->out && outevents) {
                struct mill_cr *cr = mill_pollset_items[i]->out;
                cr->mfd = NULL;
                mill_resume(cr, outevents);
                mill_pollset_fds[i].events &= ~POLLOUT;
                mill_pollset_items[i]->out = NULL;
                if(mill_timer_enabled(&cr->timer))
                    mill_timer_rm(&cr->timer);
            }
        }
        /* If nobody is polling for the fd remove it from the pollset. */
        if(!mill_pollset_fds[i].events) {
            mill_assert(!mill_pollset_items[i]->in &&
                !mill_pollset_items[i]->out);
            pollset_size = --mill->poller->size;
            if(i < pollset_size) {
                mill_pollset_fds[i] = mill_pollset_fds[pollset_size];
                mill_pollset_items[i] = mill_pollset_items[pollset_size];
                mill_pollset_items[i]->index = i;
                i--;
            }
        }
        numevs--;
    }
    return 1;
}

