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

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

#include "cr.h"
#include "slist.h"
#include "stack.h"
#include "utils.h"


/* Get memory page size. The query is done once only. The value is cached. */
static size_t mill_page_size(void) {
    static long pgsz = 0;
    if(mill_fast(pgsz))
        return (size_t)pgsz;
    pgsz = sysconf(_SC_PAGE_SIZE);
    mill_assert(pgsz > 0);
    return (size_t)pgsz;
}

size_t mill_get_stack_size(size_t req_size) {
#if defined HAVE_POSIX_MEMALIGN && HAVE_MPROTECT
    if(mill_fast(mill->stack_size > 0))
        return mill->stack_size;
    if (req_size < mill_page_size())
        req_size = mill_page_size();
    /* Amount of memory allocated must be multiply of the page size otherwise
       the behaviour of posix_memalign() is undefined. */
    size_t sz = (req_size + mill_page_size() - 1) & ~(mill_page_size() - 1);
    /* Allocate one additional guard page. */
    return (sz + mill_page_size());
#else
    mill_assert(req_size > 0);
    return req_size;
#endif
}

/* Maximum number of unused cached stacks. Keep in mind that we can't
   deallocate the stack you are running on. Thus we need at least one cached
   stack. */
static int mill_max_cached_stacks = 64;

void *mill_allocstackmem(void) {
    void *ptr;

    mill_assert(mill);

#if defined HAVE_POSIX_MEMALIGN && HAVE_MPROTECT
    /* Allocate the stack so that it's memory-page-aligned. */
    int rc = posix_memalign(&ptr, mill_page_size(), mill->stack_size);
    if(mill_slow(rc != 0)) {
        errno = rc;
        return NULL;
    }
    /* The bottom page is used as a stack guard. This way stack overflow will
       cause segfault rather than randomly overwrite the heap. */
    rc = mprotect(ptr, mill_page_size(), PROT_NONE);
    if(mill_slow(rc != 0)) {
        int err = errno;
        free(ptr);
        errno = err;
        return NULL;
    }
#else
    ptr = malloc(mill->stack_size);
    if(mill_slow(!ptr)) {
        errno = ENOMEM;
        return NULL;
    }
#endif
    return (void*)(((char*)ptr) + mill->stack_size);
}

void mill_freestack(void *stack) {
#if 0
    if (mill->num_cached_stacks > 0) {
        struct mill_slist_item *li = mill->cached_stacks.last;
        void *ptr = ((char*)(li + 1)) - mill->stack_size;
        memset(ptr+mill_page_size(), '\0',
                mill->stack_size
                - mill_page_size() /* guard page */
                - sizeof(struct mill_slist_item));
    }
#endif
    /* Put the stack to the list of cached stacks. */
    struct mill_slist_item *item = ((struct mill_slist_item*)stack) - 1;
    mill_slist_push_back(&mill->cached_stacks, item);
    if(mill->num_cached_stacks < mill_max_cached_stacks) {
        ++mill->num_cached_stacks;
        return;
    }
    /* We can't deallocate the stack we are running on at the moment.
       Standard C free() is not required to work when it deallocates its
       own stack from underneath itself. Instead, we'll deallocate one of
       the unused cached stacks. */
    item = mill_slist_pop(&mill->cached_stacks);
    void *ptr = ((char*)(item + 1)) - mill->stack_size;
#if HAVE_POSIX_MEMALIGN && HAVE_MPROTECT
    int rc = mprotect(ptr, mill_page_size(), PROT_READ|PROT_WRITE);
    mill_assert(rc == 0);
#endif
    free(ptr);
}

void mill_purgestacks(void) {
    struct mill_slist *sl = &mill->cached_stacks;
    while (1) {
        struct mill_slist_item *item = mill_slist_pop(sl);
        if (!item)
            break;
        void *ptr = (void *) (((char*)(item + 1)) - mill->stack_size);
#if HAVE_POSIX_MEMALIGN && HAVE_MPROTECT
        int rc = mprotect(ptr, mill_page_size(), PROT_READ|PROT_WRITE);
        mill_assert(rc == 0);
#endif
        free(ptr);
    }
    mill->num_cached_stacks = 0;
}

