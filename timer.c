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
#include <sys/time.h>
#include <time.h>
#include <string.h>

#if defined __APPLE__
#include <mach/mach_time.h>
static mach_timebase_info_data_t mill_mtid = {0};
#endif

#include "cr.h"
#include "libpill.h"
#include "timer.h"
#include "utils.h"

/* 1 millisecond expressed in CPU ticks. The value is chosen is such a way that
   it works reasonably well for CPU frequencies above 500MHz. On significanly
   slower machines you may wish to reconsider. */
#define MILL_CLOCK_PRECISION 1000000

/* Returns current time by querying the operating system. */
static int64_t mill_now(void) {
#if defined __APPLE__
    if (mill_slow(!mill_mtid.denom))
        mach_timebase_info(&mill_mtid);
    uint64_t ticks = mach_absolute_time();
    return (int64_t)(ticks * mill_mtid.numer / mill_mtid.denom / 1000000);
#elif defined CLOCK_REALTIME
    struct timespec ts;
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    mill_assert (rc == 0);
    return ((int64_t)ts.tv_sec) * 1000 + (((int64_t)ts.tv_nsec) / 1000000);
#else
    struct timeval tv;
    int rc = gettimeofday(&tv, NULL);
    mill_assert(rc == 0);
    return ((int64_t)tv.tv_sec) * 1000 + (((int64_t)tv.tv_usec) / 1000);
#endif
}

int64_t now(void) {
#if 0
#if (defined __GNUC__ || defined __clang__) && \
      (defined __i386__ || defined __x86_64__)
    /* Get the timestamp counter. This is time since startup, expressed in CPU
       cycles. Unlike gettimeofday() or similar function, it's extremely fast -
       it takes only few CPU cycles to evaluate. */
    uint32_t low;
    uint32_t high;
    __asm__ volatile("rdtsc" : "=a" (low), "=d" (high));
    int64_t tsc = (int64_t)((uint64_t)high << 32 | low);
    /* These global variables are used to hold the last seen timestamp counter
       and last seen time measurement. We'll initilise them the first time
       this function is called. */
    static int64_t last_tsc = -1;
    static int64_t last_now = -1;
    if(mill_slow(last_tsc < 0)) {
        last_tsc = tsc;
        last_now = mill_now();
    }   
    /* If TSC haven't jumped back or progressed more than 1/2 ms, we can use
       the cached time value. */
    if(mill_fast(tsc - last_tsc <= (MILL_CLOCK_PRECISION / 2) &&
          tsc >= last_tsc))
        return last_now;
    /* It's more than 1/2 ms since we've last measured the time.
       We'll do a new measurement now. */
    last_tsc = tsc;
    last_now = mill_now();
    return last_now;
#else
    return mill_now();
#endif
#endif
    return mill_now();
}

#define LEFT_CHILD(i) (2 * i + 1)
#define RIGHT_CHILD(i) (2 * i + 2)
#define PARENT(i) (i / 2)

/* Initial size of the array. */
#define MINHEAP_SIZE   1024

#define MILL_TIMER_CACHE_SIZE 256

#define minheap_s mill_timers_s
#define minheap_item_s mill_timer_item

#define ISLT(p1, p2) ((p1)->expiry < (p2)->expiry)

static int
minheap_init(struct minheap_s *hp) {
    hp->size = MINHEAP_SIZE;
    hp->len = 0;
    hp->items = mill_malloc(hp->size * sizeof (void *));
    if (! hp->items)
        goto er;
    hp->cache = mill_malloc(MILL_TIMER_CACHE_SIZE * sizeof (void *));
    if (!hp->cache) {
        mill_free(hp->items);
er:
        errno = ENOMEM;
        return -1;
    }
    hp->items[0] = NULL;
    hp->ncached = 0;
    hp->ncanceled = 0;
    return 0;
}

static void
minheap_clear(struct minheap_s *hp) {
    while (hp->ncached) {
        mill_free(hp->cache[--hp->ncached]);
    }
    mill_free(hp->cache);
    mill_free(hp->items);
}

static void
minheap_insert_item(struct minheap_item_s **items,
            struct minheap_item_s *item, int i) {
    while (i) {
        int j = PARENT(i);
        if (! ISLT(item, items[j]))
            break;
        items[i] = items[j];
        items[j]->index = i;
        i = j;
    }
    items[i] = item;
    item->index = i;
}

/*
 * Insert an item into the min-heap. Complexity is O(log n).
 */

static int
minheap_insert(struct minheap_s *hp,
            struct minheap_item_s *item) {
    struct minheap_item_s **items = hp->items;
    if (hp->len == hp->size) {
        hp->size *= 2;
        items = mill_realloc(hp->items,
                    hp->size * sizeof (struct minheap_item_s *));
        if (!items) {
            errno = ENOMEM;
            return -1;
        }
        hp->items = items;
    }
    minheap_insert_item(items, item, hp->len);
    hp->len++;
    return 0;
}

static void
min_heapify(struct minheap_s *hp, unsigned i) {
    int min_child;
    int left = LEFT_CHILD(i), right = RIGHT_CHILD(i);

    min_child = (left < hp->len
                && ISLT(hp->items[left], hp->items[i])) ? left : i;
    if (right < hp->len
            && ISLT(hp->items[right], hp->items[min_child])
    )
        min_child = right;

    if (min_child != i) {
        /* swap i and min */
        struct minheap_item_s *tmp = hp->items[i];
        hp->items[i] = hp->items[min_child];
        hp->items[min_child]->index = i;
        hp->items[min_child] = tmp;
        tmp->index = min_child;
        min_heapify(hp, min_child);
    }
}

/*
 *  Remove the root node, heapify and then return the removed node.
 *  Complexity is O(log n).
 */

static void *
minheap_remove(struct minheap_s *hp) {
    void *item;
    if (hp->len == 0)
        return NULL;
    item = hp->items[0];
    if (--hp->len == 0) {
        hp->items[0] = NULL;
        return item;
    }
    hp->items[0] = hp->items[hp->len];
    hp->items[hp->len]->index = 0;
    if (hp->len > 1)
        min_heapify(hp, 0);
    return item;
}

/* Get a canceled timer */
static struct mill_timer_item *
mill_timer_get(struct minheap_s *hp) {
ll2:
    if (hp->ncached > 0) {
        hp->ncanceled++;
        return hp->cache[--hp->ncached];
    }

    if (hp->ncanceled >= MILL_TIMER_CACHE_SIZE) {
        /* re-heap */
        int k, len = 0;
        struct minheap_item_s **items = mill_malloc(hp->size
                            * sizeof (struct minheap_item_s *));
        if (!items)
            goto ll1;
        for (k = 0; k < hp->len; k++) {
            struct minheap_item_s *item = hp->items[k];
            if (item->state == MILL_TIMER_CANCELED) {
                if (hp->ncached == MILL_TIMER_CACHE_SIZE)
                    mill_free(item);
                else
                    hp->cache[hp->ncached++] = item;
            } else {
                minheap_insert_item(items, item, len);
                len++;
            }
        }
        hp->len = len;
        mill_free(hp->items);
        hp->items = items;
        hp->ncanceled = 0;
        mill_assert(hp->ncached > 0);
        goto ll2;
    }
ll1:;
    void *ptr = mill_malloc(sizeof (struct mill_timer_item));
    if (!ptr) {
        errno = ENOMEM;
        return NULL;
    }
    hp->ncanceled++;
    return ptr;
}

/* Dispose a canceled timer */
static void
mill_timer_dispose(struct minheap_s *hp,
            struct mill_timer_item *item) {
    mill_assert(item);
    if (hp->ncached < MILL_TIMER_CACHE_SIZE)
        hp->cache[hp->ncached++] = item;
    else
        mill_free(item);
    hp->ncanceled--;
}

int
mill_timers_init(void) {
    return minheap_init(&mill->timers);
}

void
mill_timers_fini(void) {
#if 1
    /* Should be only canceled timers ? */
    mill_assert(mill->num_cr == 0);

    while (mill->timers.len > 0) {
        struct mill_timer_item *tm = minheap_remove(&mill->timers);
        if (tm->state == MILL_TIMER_CANCELED)
            mill_timer_dispose(&mill->timers, tm);
    }
#endif
    minheap_clear(&mill->timers);
}

#define timer_first() \
    ((struct mill_timer_item *) mill->timers.items[0])

int
mill_timer_next(void) {
    struct mill_timer_item *tm;
    int timeout;
    while ((tm = timer_first()) && (tm->state != MILL_TIMER_ARMED)) {
        (void) minheap_remove(&mill->timers);
        if (tm->state == MILL_TIMER_CANCELED)
            mill_timer_dispose(&mill->timers, tm);
        tm->state = 0;
    }
    if (! tm) {
        mill_assert(mill->timers.len == 0);
        return -1;
    }
    timeout = tm->expiry - now();
    return timeout <= 0 ? 0 : timeout;
}

int
mill_timer_fire(void) {
    /* Avoid getting current time if there are no timers anyway. */
    if (mill->timers.len == 0)
        return 0;
    struct mill_timer_item *tm;
    int64_t nw = now();
    int fired = 0;
    while ((tm = timer_first()) && nw >= tm->expiry) {
        (void) minheap_remove(&mill->timers);
        int state = tm->state;
        tm->state = 0;
        if (state == MILL_TIMER_CANCELED) {
            mill_timer_dispose(&mill->timers, tm);
            continue;
        }
        if (state == MILL_TIMER_ARMED) {
            if (((struct mill_timer *)tm)->callback) {
                struct mill_timer *timer = (struct mill_timer *) tm;
                timer->callback(timer);
            }
            fired = 1;
        }
    }
    return fired;
}

/* Replace the coroutine timer with a canceled timer.
 * Unlike the coroutine timer, this canceled timer can outlive the coroutine.  */
void
mill_timer_cancel(struct mill_timer *timer) {
    int state = ((struct mill_timer_item *)timer)->state;
    if (!state)
        return;
    mill_assert(state == MILL_TIMER_DISARMED);
    struct mill_timer_item *tm = mill_timer_get(&mill->timers);
    if (!tm)
        mill_panic(strerror(errno));
    memcpy(tm, timer, sizeof (*tm));
    tm->state = MILL_TIMER_CANCELED;
    int i = tm->index;
    mill_assert(mill->timers.items[i] == (struct mill_timer_item *) timer);
    mill->timers.items[i] = tm;
    ((struct mill_timer_item *)timer)->state = 0;
}

int
mill_timer_add(struct mill_timer *timer,
            int64_t deadline, mill_timer_callback callback) {
    mill_assert(deadline >= 0);
    struct mill_timer_item *tm = (struct mill_timer_item *) timer;
    if (tm->state == MILL_TIMER_DISARMED) {
        if (tm->expiry == deadline) {
            timer->callback = callback;
            tm->state = MILL_TIMER_ARMED;
            return 0;
        }

        /* Not using a fixed deadline for a session (e.g. resetting deadline
         * everytime in a send/recv loop!)
         */
        mill_timer_cancel(timer);   /* FIXME -- here we can ignore OOM */
    }

    timer->callback = callback;
    tm->expiry = deadline;
    timer->data = NULL;
    int rc = minheap_insert(&mill->timers, (struct mill_timer_item *) tm);
    if (rc == -1) {
        /* Out of memory? Ignore. */
        mill_assert(errno == ENOMEM);
        return 0;
    }
    tm->state = MILL_TIMER_ARMED;
    return 0;

}

void
mill_timer_rm(struct mill_timer *timer) {
    struct mill_timer_item *tm = (struct mill_timer_item *) timer;
    if (tm->state == MILL_TIMER_ARMED)
        tm->state = MILL_TIMER_DISARMED;
}

