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

#ifndef MILL_CR_INCLUDED
#define MILL_CR_INCLUDED

#include <setjmp.h>
#include <stdint.h>
#include <pthread.h>

#include "chan.h"
#include "list.h"
#include "slist.h"
#include "timer.h"
#include "utils.h"
#include "fd.h"

enum mill_state {
    MILL_READY = 1,
    MILL_MSLEEP,
    MILL_FDWAIT,
    MILL_CHR,
    MILL_CHS,
    MILL_CHOOSE,
    MILL_DEAD
};

struct mill_wgroup_s;
struct mill_task_s;

/* The coroutine. The memory layout looks like this:

   +----------------------------------------------------+--------+---------+
   |                                              stack | valbuf | mill_cr |
   +----------------------------------------------------+--------+---------+

   - mill_cr contains generic book-keeping info about the coroutine
   - valbuf is a buffer for temporarily storing values received from channels
   - stack is a standard C stack; it grows downwards (at the moment libmill
     doesn't support microarchitectures where stack grows upwards)

*/
struct mill_cr {
    /* Status of the coroutine. Used for debugging purposes. */
    enum mill_state state;

    /* The coroutine is stored in this list if it is not blocked and it is
       waiting to be executed. */
    struct mill_slist_item ready;

    /* If the coroutine is waiting for a deadline, it uses this timer. */
    struct mill_timer timer;

    /* The file descriptor for which the couroutine is waiting for
       an event in fdwait(). */
    struct mill_fd_s *mfd;

    /* The coroutine is waiting for this task to be scheduled
       in a worker thread. */
    struct mill_task_s *tsk;

    /* This structure is used when the coroutine is executing a choose
       statement. */
    struct mill_choosedata choosedata;

    /* Stored coroutine context while it is not executing. */
    sigjmp_buf ctx;

    struct mill_list_item wgitem;

    struct mill_wgroup_s *wg;

    /* Argument to resume() call being passed to the blocked suspend() call. */
    int result;

    void (*resume_hook)(void *data);
    void (*suspend_hook)(void *data, int exiting);

    /* Coroutine-local storage. */
    void *cls;

    /* List of all coroutines */
    struct mill_list_item item;
};

/* Suspend running coroutine. Move to executing different coroutines. Once
   someone resumes this coroutine using mill_resume function 'result' argument
   of that function will be returned. */
int mill_suspend(void);

void mill_resume(struct mill_cr *cr, int result);

/* Returns pointer to the value buffer. The returned buffer is guaranteed
   to be at least 'size' bytes long. */
void *mill_valbuf(struct mill_cr *cr, size_t size);

typedef struct {
    /* Fake coroutine corresponding to the main thread of execution. */
    struct mill_cr main;

    /* The coroutine that is running at the moment. */
    struct mill_cr *running;

    int num_cr;

    size_t stack_size;

    /* list of all coroutines */
    struct mill_list all_crs;

    int counter;
    int choose_seqnum;

    int do_waitall;

    /* Queue of coroutines scheduled for execution. */
    struct mill_slist ready;

    /* A stack of unused coroutine stacks. This allows for extra-fast allocation
       of a new stack. The FIFO nature of this structure minimises cache misses.
       When the stack is cached its mill_slist_item is placed on its top rather
       then on the bottom. That way we minimise page misses. */
    int num_cached_stacks;
    struct mill_slist cached_stacks;

    /* Global list of all timers. */
    struct mill_timers_s timers;

    /* Poller used to wait for file descriptors. */
    struct mill_poller *poller;

    /* Pipe for notification of finished tasks. */
    int task_fd[2];

    /* Number of pending jobs summitted to the threadpool */
    int num_tasks;

    /* Size of the buffer for temporary storage of values received from channels.
       It should be properly aligned and never change if there are any stacks
       allocated at the moment. */
    size_t valbuf_size;    /* = 128 */

    /* Valbuf for tha main coroutine. */
    char main_valbuf[128];
} mill_t;

extern __thread mill_t *mill;

mill_t *mill_init__p(int stacksize);

#endif
