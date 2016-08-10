#include "cr.h"
#include "utils.h"
#include "libmill.h"
#include "timer.h"

struct mill_wgroup_s {
    int counter;    /* # of coroutines in the group */

    /* list of coroutines added to this group */
    struct mill_list crs;

    /* the waiting coroutine */
    struct mill_cr *waiter;
};

struct mill_wgroup_s *mill_wgmake(void) {
    struct mill_wgroup_s *wg = mill_malloc(sizeof(struct mill_wgroup_s));
    wg->counter = 0;
    wg->waiter = NULL;
    mill_list_init(&wg->crs);
    return wg;
}

int mill_wgadd(struct mill_wgroup_s *wg) {
    mill_assert(wg);
    struct mill_cr *mill_running = mill->running;
    if (mill_running == &mill->main) {
        /* attempt to add main to a waitgroup */
        errno = EDEADLK;
        return -1;
    }
    if (mill_running->wg) {
        /* coroutine already in a waitgroup */
        errno = EEXIST;
        return -1;
    }
    mill_assert(wg->counter >= 0);
    ++wg->counter;
    mill_list_insert(&wg->crs, &mill_running->wgitem, NULL);
    mill_running->wg = wg;
    return 0;
}

static void wait_timedout(struct mill_timer *timer) {
    struct mill_cr *cr = mill_cont(timer, struct mill_cr, timer);
    struct mill_wgroup_s *wg = timer->data;
    mill_assert(wg);
    wg->waiter = NULL;
    mill_resume(cr, -ETIMEDOUT);
}

int mill_wgwait(struct mill_wgroup_s *wg , int64_t deadline) {
    mill_assert(wg);
    struct mill_cr *cr = mill->running;
    if (wg == cr->wg) {
         /* cannot wait for self */
        errno = EINVAL;
        return -1;
    }
    if (wg->waiter) {
        /* multiple waiters for a waitgroup */
        errno = EEXIST;
        return -1;
    }
    if (wg->counter > 0) {
        if (deadline >= 0) {
            mill_timer_add(&cr->timer, deadline, wait_timedout);
            cr->timer.data = wg;
        }
        wg->waiter = cr;
        int rc = mill_suspend();
        if (rc < 0) {
            errno = -rc;
            return -1;
        }
    }
    return 0;
}

int mill_wgcancel(struct mill_wgroup_s *wg) {
    mill_assert(wg);
    if (wg->counter <= 0)
        return 0;
    while (!mill_list_empty(&wg->crs)) {
        struct mill_cr *cr = mill_cont(
            mill_list_begin(&wg->crs), struct mill_cr, wgitem);
        mill_list_erase(&wg->crs, mill_list_begin(&wg->crs));
        cr->wg = NULL;
    }
    wg->counter = 0;
    if (wg->waiter) {
        struct mill_cr *cr = wg->waiter;
        wg->waiter = NULL;
        if (mill_timer_enabled(&cr->timer))
            mill_timer_rm(&cr->timer);
        mill_resume(cr, -ECANCELED);
    }
    return 0;
}

void mill_wgfree(struct mill_wgroup_s *wg) {
    mill_assert(wg);
    mill_wgcancel(wg);
    mill_free(wg);
}

void mill_wgroup_rm(struct mill_cr *cr) {
    struct mill_wgroup_s *wg = cr->wg;
    mill_assert(wg);
    --wg->counter;
    mill_list_erase(&wg->crs, &cr->wgitem);
    cr->wg = NULL;
    if (wg->counter == 0 && wg->waiter) {
        struct mill_cr *wcr = wg->waiter;
        wg->waiter = NULL;
        if (mill_timer_enabled(&wcr->timer))
            mill_timer_rm(&wcr->timer);
        mill_resume(wcr, 0);
    }
}

static void waitall_timedout(struct mill_timer *timer) {
    struct mill_cr *cr = mill_cont(timer, struct mill_cr, timer);
    mill->do_waitall = 0;
    mill_resume(cr, -ETIMEDOUT);
}

int mill_waitall(int64_t deadline) {
    if (mill->running != &mill->main) {
        /* waitall called in non-main coroutine */
        errno = EDEADLK;
        return -1;
    }
    if (mill->num_cr > 0) {
        if (deadline >= 0)
            mill_timer_add(&mill->running->timer, deadline,
                        waitall_timedout);
        mill->do_waitall = 1;
        int rc = mill_suspend();
        mill_assert(mill->do_waitall == 0);
        if (rc < 0) {
            errno = -rc;
            return -1;
        }
    }
    return 0;
}

