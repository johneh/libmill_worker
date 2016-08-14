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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>

#include "cr.h"
#include "libmill.h"
#include "ip.h"
#include "poller.h"
#include "stack.h"
#include "utils.h"
#include "worker.h"
#include "waitgroup.h"
#include "timer.h"

void *(*mill_realloc_func)(void *ptr, size_t size) = realloc;

__thread mill_t *mill = NULL;

volatile __thread int mill_unoptimisable1 = 1;
volatile __thread void *mill_unoptimisable2 = NULL;

static void *mill_getvalbuf(struct mill_cr *cr) {
    if(mill_fast(cr != &mill->main)) {
        return (void*)(((char*)cr) - mill->valbuf_size);
    }
    return (void*)mill->main_valbuf;
}

int mill_suspend(void) {
    /* Even if process never gets idle, we have to process external events
       once in a while. The external signal may very well be a deadline or
       a user-issued command that cancels the CPU intensive operation. */
    struct mill_cr *mill_running = mill->running;

    if(mill->counter >= 103) {
        mill_wait(0);
        mill->counter = 0;
    }
    if(mill_running && mill_running->suspend_hook)
        mill_running->suspend_hook(mill_running->cls, 0);
    /* Store the context of the current coroutine, if any. */
    if(mill_running && sigsetjmp(mill_running->ctx, 0))
        return mill_running->result;
    while(1) {
        /* If there's a coroutine ready to be executed go for it. */
        if(!mill_slist_empty(&mill->ready)) {
            ++mill->counter;
            struct mill_slist_item *it = mill_slist_pop(&mill->ready);
            mill->running = mill_running = mill_cont(it, struct mill_cr, ready);
            mill_running->state = 0;
            if(mill_slow(mill_running->resume_hook))
                mill_running->resume_hook(mill_running->cls);
            siglongjmp(mill_running->ctx, 1);
        }
        /* Otherwise, we are going to wait for sleeping coroutines
           and for external events. */
        mill_wait(1);
        /* XXX: not true, See mill_task_timedout() in task.c.
         mill_assert(!mill_slist_empty(&mill->ready)); */
        mill->counter = 0;
    }
}

void mill_resume(struct mill_cr *cr, int result) {
    mill_assert(cr->state != MILL_READY);
    cr->result = result;
    cr->state = MILL_READY;
    mill_slist_push_back(&mill->ready, &cr->ready);
}

void *mill_allocstack(void) {
    if(!mill_slist_empty(&mill->cached_stacks)) {
        --mill->num_cached_stacks;
        return (void*)(mill_slist_pop(&mill->cached_stacks) + 1);
    }
    return mill_allocstackmem();
}

sigjmp_buf *mill_getctx(void) {
    return &mill->running->ctx;
}

/* The intial part of go(). Starts the new coroutine.
   Returns the pointer to the top of its stack. */
void *mill_go_prologue(void *stackmem) {
    mill_assert(mill != NULL);
    /* Allocate and initialise new stack. */
    struct mill_cr *cr = stackmem;
    if(!cr) {
        cr = mill_allocstack();
        if(!cr)
            mill_panic("not enough memory to allocate coroutine stack");
    }
    cr = cr - 1;
    mill_list_insert(&mill->all_crs, &cr->item, NULL);
    cr->cls = NULL;
    cr->resume_hook = NULL;
    cr->suspend_hook = NULL;
    cr->wg = NULL;
    cr->state = 0;
    memset(&cr->timer, '\0', sizeof (struct mill_timer));
    cr->mfd = NULL;
    mill_slist_set_detached(&cr->ready);
    mill_list_set_detached(&cr->wgitem);
    mill->num_cr++;
    /* Suspend the parent coroutine and make the new one running. */
    if(mill_slow(mill->running->suspend_hook))
        mill->running->suspend_hook(mill->running->cls, 0);
    mill_resume(mill->running, 0);
    mill->running = cr;
    /* Return pointer to the top of the stack. There's valbuf interposed
       between the mill_cr structure and the stack itself. */
    return (void*)(((char*)cr) - mill->valbuf_size);
}

/* The final part of go(). Cleans up after the coroutine is finished. */
void mill_go_epilogue(void) {
    struct mill_cr *mill_running = mill->running;
    if(mill_running->suspend_hook)
        mill_running->suspend_hook(mill_running->cls, 1);
    if(mill_slow(mill_running->wg)) {
        mill_wgroup_rm(mill_running);
    }

    mill_list_erase(&mill->all_crs, &mill_running->item);

    /* Make sure coroutine timer isn't in the min-heap */
    mill_timer_cancel(&mill_running->timer);

    mill_freestack(mill_running + 1);
    mill->num_cr--;
    mill->running = NULL;
    if(mill_slow(mill->do_waitall && mill->num_cr == 0)) {
        struct mill_cr *cr = &mill->main;
        mill_assert(mill->num_tasks == 0);
        mill->do_waitall = 0;
        if(mill_timer_enabled(&cr->timer))
            mill_timer_rm(&cr->timer);
        mill_resume(cr, 0);
    }

    /* Given that there's no running coroutine at this point
       this call will never return. */
    mill_suspend();
}

void mill_yield(void) {
    /* This looks fishy, but yes, we can resume the coroutine even before
       suspending it. */
    mill_resume(mill->running, 0);
    mill_suspend();
}

int iscrmain(void) {
    mill_assert(mill != NULL);
    return (mill->running == &mill->main);
}

void *mill_valbuf(struct mill_cr *cr, size_t size) {
    assert(size <= 128);
    return mill_getvalbuf(cr);
}

void mill_sethook(void *data,
        void (*resume_hook)(void *), void (*suspend_hook)(void *, int)) {
    mill->running->cls = data;
    mill->running->resume_hook = resume_hook;
    mill->running->suspend_hook = suspend_hook;
}

/* default stack size */
#define MILL_STACK_SIZE (64 * 1024)

mill_t *mill_init__p(int stacksize) {
    mill = mill_malloc(sizeof (mill_t));
    if(!mill) {
        errno = ENOMEM;
        return NULL;
    }
    memset(mill, '\0', sizeof (mill_t));
    mill->task_fd[0] = mill->task_fd[1] = -1;   /* not used in worker threads */

    if(-1 == mill_timers_init()) {
        mill_free(mill);
        return NULL;
    }
    struct mill_cr *mill_main = & mill->main;
    memset(&mill_main->timer, '\0', sizeof (struct mill_timer));
    mill_main->mfd = NULL;
    mill_main->state = 0;
    mill->valbuf_size = 128;
    mill->all_crs.first = &mill_main->item;
    mill->all_crs.last = &mill_main->item;
    mill->running = mill_main;

    mill_dns_init();
    mill_poller_init();
    if(errno) {
        mill_free(mill);
        mill = NULL;
    }
    if(stacksize < 0)
        stacksize = MILL_STACK_SIZE;
    mill->stack_size = mill_get_stack_size(stacksize);
    return mill;
}

void *mill_init(int stacksize, int nworkers) {
    if(mill_init__p(stacksize))
        init_workers(nworkers);
    return mill;
}

void mill_fini(void) {
    if(mill) {
        mill_waitall(-1);
        close_task_fds();
        mill_poller_fini();
        mill_purgestacks();
        mill_timers_fini();
        mill_free(mill);
        mill = NULL;
    }
}

/* returns number of coroutines excluding main */
int gocount(void) {
    return mill->num_cr;
}

int taskcount(void) {
    return mill->num_tasks;
}

void mill_panic(const char *text) {
    fprintf(stderr, "panic: %s\n", text);
    abort();
}

