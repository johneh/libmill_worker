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

#ifndef MILL_TIMER_INCLUDED
#define MILL_TIMER_INCLUDED

#include <stdint.h>

#include "list.h"

struct mill_timer;

struct mill_timer_item {
    int index;  /* index into the min-heap array */

    /* state == 0 when timer is not on the min-heap */
    int state;
#define MILL_TIMER_ARMED     1
#define MILL_TIMER_DISARMED  2
#define MILL_TIMER_CANCELED  3

    /* The deadline when the timer expires. */
    int64_t expiry;
};

/* min-heap of timers */
struct mill_timers_s {
    /* Size of the "items" array */
    int size;

    /* Total number of timers on the heap */
    int len;

    /* Array of timers */
    struct mill_timer_item **items;

    /* Cache for cancelled timer items */
    struct mill_timer_item **cache;
    int ncached;

    /* Number of canceled timers on the heap */
    int ncanceled;
};

typedef void (*mill_timer_callback)(struct mill_timer *timer);

struct mill_timer {
    struct mill_timer_item item;

    /* Callback invoked when timer expires. Pfui Teufel! */
    mill_timer_callback callback;

    /* Optional data */
    void *data;
};

int mill_timers_init(void);

void mill_timers_fini(void);

#define mill_timer_enabled(tm) \
    (((struct mill_timer_item *) tm)->state == MILL_TIMER_ARMED)

/* Add a timer for the running coroutine. */
int mill_timer_add(struct mill_timer *timer, int64_t deadline,
    mill_timer_callback callback);

/* Disarm the timer associated with the running coroutine. */
void mill_timer_rm(struct mill_timer *timer);

/* Number of milliseconds till the next timer expires.
   If there are no timers returns -1. */
int mill_timer_next(void);

/* Resumes all coroutines whose timers have already expired.
   Returns zero if no coroutine was resumed, 1 otherwise. */
int mill_timer_fire(void);

void mill_timer_cancel(struct mill_timer *timer);

#endif

