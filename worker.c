#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

#include "libmill.h"
#include "list.h"
#include "cr.h"
#include "stack.h"
#include "utils.h"
#include "worker.h"
#include "poller.h"

enum task_code {
    tTASK = 1,
    tTASK_CORO,
    tSTAT,
    tOPEN,
    tCLOSE,
    tPREAD,
    tPWRITE,
    tUNLINK,
    tREADV,
    tWRITEV,
    tFSYNC,
    tFSTAT,
    tAWAIT,
};

/* NUM_WORKERS -- # of anonymous permanent workers with a shared task_queue
 *  XXX: use # of cores?
 */

#define NUM_WORKERS 4
#define MAX_WORKERS 64

struct mill_worker_s {
    struct mill_list_item item;
    pthread_t pth;
    mill_pipe task_queue;   /* request */
    int sfd;    /* thread initialization status written to this fd */
};

typedef struct mill_task_s {
    enum task_code code;

    int errcode;    /* errno (and TASK_status < 0) */

#define TASK_QUEUED         -1
#define TASK_TIMEDOUT       -2
#define TASK_INPROGRESS     0

    struct mill_cr *cr;
    void *buf;
    union {
        struct {
            char *path;
            int flags;
            mode_t mode;
        };
        struct {
            int fd;
            size_t count;
            off_t offset;
        };
        taskfunc taskfn;
        void *pf;
    };
    union {
        int ofd;
        ssize_t ssz;
        int64_t ddline;
    };

    int res_fd; /* response */
} task;

/* Global work queue for anonymous (permanent) workers */
static struct mill_pipe_s *mill_task_queue;

static int num_workers;
static pthread_once_t workers_initialized = PTHREAD_ONCE_INIT;
static int init_task_fds(void);
static void init_workers_once(void);

#define in_worker_thread()  (mill->task_fd[0] == -2)

static void mill_task_timedout(struct mill_timer *timer) {
    struct mill_cr *cr = mill_cont(timer, struct mill_cr, timer);
    task *req = cr->tsk;
    if (mill_atomic_set(&req->errcode, TASK_QUEUED, TASK_TIMEDOUT)) {
        mill->num_tasks--;
        mill_resume(cr, 0);
    }
    /* else
        the coroutine is resumed when the task is finished */
}

coroutine static void task_wait(int fd) {
    unsigned size = sizeof (task *);
    task *res;
    char *ptr = (char *) &res;
    int n;
    unsigned count = size;
    struct mill_fd_s *iop;

    iop = mill_open(fd);
    mill_assert(iop);

    /* Adjust counter to exclude this coroutine */
    mill->num_cr--;

    while (1) {
        int n = (int) read(fd, ptr, count);
        if (mill_fast(n > 0)) {
            count -= n;
            ptr += n;
            if (count == 0) {
                mill->num_tasks--;
                if (mill_timer_enabled(&res->cr->timer))
                    mill_timer_rm(&res->cr->timer);
                mill_resume(res->cr, 1);
                ptr = (char *) &res;
                count = size;
            } /* else
                    likely interrupted by signal */
            continue;
        }
        if (n == 0) {
            /* The non-worker thread is exiting; See delete_workers(). */
            mill_close(iop, 1); /* Close the read end of the pipe */
            return;
        }

        if (errno == EINTR)
            continue;
        mill_assert(errno == EAGAIN);
        mill_fdwait(iop, FDW_IN, -1);
    }
}


#define task_alloc(ts) do {\
    if (!(ts = mill_malloc(sizeof(task)))) {\
        errno = ENOMEM;\
        return -1;\
    }\
} while(0)

#define task_free(ptr)    mill_free((void *) ptr)

static ssize_t queue_task(mill_pipe task_queue,
            volatile task *req, int64_t deadline) {
    mill_assert(mill);

    if (mill_slow(mill->task_fd[0] == -1)) {
        int rc = init_task_fds();
        if (rc == -1) {
            task_free(req);
            return -1;
        }
    }

    /* pthread_once(&workers_initialized, init_workers_once);
     *   XXX: Performance killer? Lets create at least one worker thread in init_workers().
     */

    mill_assert(mill_task_queue);
    if (!task_queue)
        task_queue = mill_task_queue;
    req->errcode = TASK_QUEUED;
    req->cr = mill->running;
    /* enqueue task */
    req->res_fd = mill->task_fd[1];
    mill_pipesend(task_queue, (void *) &req);
    mill->num_tasks++;

    if (deadline >= 0) {
        mill_timer_add(&mill->running->timer, deadline, mill_task_timedout);
        req->cr->tsk = (task *) req;
    } else
        req->cr->tsk = NULL;

    if (!mill_suspend()) {
        errno = ETIMEDOUT;
        /* task object will be freed by the worker */
        return -1;
    }
    req->cr = NULL;

    int errcode = req->errcode;
    ssize_t ret = 0;
    switch (req->code) {
    case tOPEN:
        ret = req->ofd;
        break;
    case tPREAD: case tPWRITE: case tREADV: case tWRITEV:
    case tTASK: case tTASK_CORO:
        ret = req->ssz;
        break;
    case tSTAT: case tUNLINK: case tFSYNC: case tFSTAT:
    case tAWAIT:
        /* fall through */
    default:
        if (errcode)
            ret = -1;
    }

    task_free(req);
    errno = errcode;
    return ret;
}

int stat_a(const char *path, struct stat *buf) {
    task *req;
    task_alloc(req);
    req->code = tSTAT;
    req->path = (char *) path;
    req->buf = (void *) buf;
    return queue_task(NULL, req, -1);
}

int open_a(const char *path, int flags, mode_t mode) {
    task *req;
    task_alloc(req);
    req->code = tOPEN;
    req->path = (char *) path;
    req->flags = flags;
    req->mode = mode;
    return queue_task(NULL, req, -1);
}

int close_a(int fd) {
    task *req;
    task_alloc(req);
    req->code = tCLOSE;
    req->fd = fd;
    return queue_task(NULL, req, -1);
}

ssize_t pread_a(int fd, void *buf, size_t count, off_t offset) {
    task *req;
    task_alloc(req);
    req->code = tPREAD;
    req->fd = fd;
    req->count = count;
    req->offset = offset;
    req->buf = (void *) buf;
    return queue_task(NULL, req, -1);
}

ssize_t pwrite_a(int fd, const void *buf, size_t count, off_t offset) {
    task *req;
    task_alloc(req);
    req->code = tPWRITE;
    req->fd = fd;
    req->buf = (void *) buf;
    req->count = count;
    req->offset = offset;
    return queue_task(NULL, req, -1);
}

int unlink_a(const char *path) {
    task *req;
    task_alloc(req);
    req->code = tUNLINK;
    req->path = (char *) path;
    return queue_task(NULL, req, -1);
}

ssize_t readv_a(int fd, const struct iovec *iov, int iovcnt) {
    task *req;
    task_alloc(req);
    req->code = tREADV;
    req->fd = fd;
    req->buf = (void *) iov;
    req->count = iovcnt;
    return queue_task(NULL, req, -1);
}

ssize_t writev_a(int fd, const struct iovec *iov, int iovcnt) {
    task *req;
    task_alloc(req);
    req->code = tWRITEV;
    req->fd = fd;
    req->buf = (void *) iov;
    req->count = iovcnt;
    return queue_task(NULL, req, -1);
}

int fsync_a(int fd) {
    task *req;
    task_alloc(req);
    req->code = tFSYNC;
    req->fd = fd;
    return queue_task(NULL, req, -1);
}

int fstat_a(int fd, struct stat *buf) {
    task *req;
    task_alloc(req);
    req->code = tFSTAT;
    req->fd = fd;
    req->buf = (void *) buf;
    return queue_task(NULL, req, -1);
}

int task_run(struct mill_worker_s *w,
            taskfunc tf, void *da, int64_t deadline) {
    task *req;
    task_alloc(req);
    req->code = tTASK;
    req->taskfn = tf;
    req->buf = da;
    return queue_task(w ? w->task_queue : NULL, req, deadline);
}

int task_go(struct mill_worker_s *w,
            taskfunc fn, void *da, int64_t deadline) {
    task *req;
    task_alloc(req);
    req->code = tTASK_CORO;
    req->taskfn = fn;
    req->buf = da;
    return queue_task(w ? w->task_queue : NULL, req, deadline);
}

int mill_worker_await(struct mill_worker_s *w, int64_t deadline) {
    task *req;
    if (! w) {
        errno = EINVAL;
        return -1;
    }
    task_alloc(req);
    req->code = tAWAIT;
    req->ddline = deadline;
    return queue_task(w->task_queue, req, deadline);
}

static int task_signal(task *req) {
    int size = sizeof (task *);
    while (1) {
        int n = (int) write(req->res_fd, (void *) & req, size);
        if (n == size)
            break;
        mill_assert(n < 0);
        if (errno == EINTR)
            continue;
        /* EAGAIN -- pipe capacity execeeded ? */
        if (errno != EAGAIN)
            return -1;
        mill_fdevent(req->res_fd, FDW_OUT, -1);
    }
    return 0;
}

static coroutine void do_work(task *req) {
    yield();
    req->ssz = req->taskfn(req->buf);
    if (req->ssz == -1)
        req->errcode = errno;
    if (-1 == task_signal(req))
        task_free(req);
}

static void *worker_func(void *p) {
    struct mill_worker_s *w = p;
    int done = 0;

#define DEQUEUE_TASK(ptr_done)  \
    *((task **) mill_piperecv(w->task_queue, (ptr_done)))

#define WGO(do_fn, rq) do {\
    void *ptr = mill_allocstack(); \
    if (!ptr) { \
        rq->errcode = errno; \
        rq->ssz = -1; \
            break; \
    } \
    mill_go(do_fn(rq), ptr); \
} while(0)

    mill_t *millptr = mill_init__p(64*1024);
    int status = !!millptr;
    int rc = (int) write(w->sfd, &status, sizeof(status));
    mill_assert(rc == sizeof(status));
    if(mill_slow(status == 0))
        return NULL;
    millptr->task_fd[0] = millptr->task_fd[1] = -2; /* Kludge to mark it as a worker thread */

    while (! done) {
        task *req = DEQUEUE_TASK(&done);
        if (done)
            goto x;
        if (! mill_atomic_set(&req->errcode, TASK_QUEUED, TASK_INPROGRESS)) {
            /* Sender timed out. */
            task_free(req);
            goto x;
        }

        mill_assert(req->errcode == 0);
        switch (req->code) {
        case tSTAT:
            if (-1 == stat(req->path, (struct stat *) req->buf))
                req->errcode = errno;
            break;
        case tOPEN:
            req->ofd = open(req->path, req->flags, req->mode);
            if (-1 == req->ofd)
                req->errcode = errno;
            break;
        case tCLOSE:
            if (-1 == close(req->fd))
                req->errcode = errno;
            break;
        case tPREAD:
            req->ssz = pread(req->fd, req->buf, req->count, req->offset);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tPWRITE:
            req->ssz = pwrite(req->fd, req->buf, req->count, req->offset);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tUNLINK:
            if (-1 == unlink(req->path))
                req->errcode = errno;
            break;
        case tREADV:
            req->ssz = readv(req->fd, req->buf, req->count);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tWRITEV:
            req->ssz = writev(req->fd, req->buf, req->count);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tTASK:
            req->ssz = req->taskfn(req->buf);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tFSYNC:
            if (-1 == fsync(req->fd))
                req->errcode = errno;
            break;
        case tFSTAT:
            if (-1 == fstat(req->fd, (struct stat *) req->buf))
                req->errcode = errno;
            break;
        case tTASK_CORO:
            WGO(do_work, req);
            goto x;
        case tAWAIT:
            if (-1 == mill_waitall(req->ddline))
                req->errcode = errno;
            break;
        default:
            mill_panic("libmill: worker_func(): received unexpected code");
        }
        if (-1 == task_signal(req))
            task_free(req);
x:;
    }
    mill_fini();
    return NULL;
#undef DEQUEUE_TASK
#undef WGO
}

static struct mill_worker_s *worker_create__p(mill_pipe task_queue) {
    struct mill_worker_s *w;
    int fd[2];
    mill_assert(!in_worker_thread());   /* subcontracting isn't allowed */
    w = mill_malloc(sizeof (struct mill_worker_s)); 
    if (! w) {
        errno = ENOMEM;
        return NULL;
    }
    if (-1 == pipe(fd)) {
        mill_free(w);
        return NULL;
    }
    w->task_queue = task_queue;
    w->sfd = fd[1];
    int rc = pthread_create(& w->pth, NULL, worker_func, w);
    if (rc != 0) {
        errno = rc;
        mill_free(w);
        close(fd[0]);
        close(fd[1]);
        return NULL;
    }

    int status = 0;
    rc = (int) read(fd[0], &status, sizeof (int));
    if (rc != sizeof(int) || status <= 0) {
        /* mill_init() failed. */
        (void) pthread_join(w->pth, NULL);
        mill_free(w);
        w = NULL;
        errno = EAGAIN; /* XXX: ?? */
    }
    close(fd[0]);
    close(fd[1]);
    return w;
}

struct mill_worker_s *mill_worker_create(void) {
    mill_assert(mill);
    mill_pipe tq = mill_pipemake(sizeof (task *));
    if (!tq)
        return NULL;
    struct mill_worker_s *w = worker_create__p(tq);
    if (!w)
        mill_pipefree(tq);
    return w;
}

void mill_worker_delete(struct mill_worker_s *w) {
    void *ptr;
    int rc;
    mill_assert(! in_worker_thread());
    mill_pipeclose(w->task_queue);
    rc = pthread_join(w->pth, & ptr);
    mill_assert(rc == 0);
    mill_pipefree(w->task_queue);
    mill_free(w);
}

void close_task_fds(void) {
    if (in_worker_thread())
        return;
    if (mill->task_fd[1] >= 0) {
        /* Avoid leaking stack memory for the task_wait() coroutine */
        close(mill->task_fd[1]);
        mill->num_cr++; /* The counter was decremented in task_wait(). */
        mill_waitall(-1);
    }
}

static int init_task_fds(void) {
    if (mill->task_fd[0] == -1) {
        int fd[2];
        if (-1 == pipe(fd))
            return -1;
        int flag = fcntl(fd[0], F_GETFL);
        if (flag == -1)
            flag = 0;
        if (-1 == fcntl(fd[0], F_SETFL, flag|O_NONBLOCK)) {
err:
            close(fd[0]);
            close(fd[1]);
            return -1;
        }
        flag = fcntl(fd[1], F_GETFL);
        if (flag == -1)
            flag = 0;
        if (-1 == fcntl(fd[1], F_SETFL, flag|O_NONBLOCK))
            goto err;
        void *ptr = mill_allocstack();
        if (!ptr)
            goto err;
        mill->task_fd[0] = fd[0];
        mill->task_fd[1] = fd[1];
        mill_go(task_wait(fd[0]), ptr);
    }
    return 0;
}

void init_workers(int nworkers) {
    if (nworkers < 0) {
        const char *val;
        nworkers = NUM_WORKERS;
        val = getenv("MILL_WORKERS");
        if (val) {
            nworkers = atoi(val);
            if (nworkers < 0)
                nworkers = NUM_WORKERS;
        }
    }
    if (nworkers == 0)
        nworkers = 1;
    else if (nworkers > MAX_WORKERS)
        nworkers = MAX_WORKERS;
    mill_atomic_set(&num_workers, 0, nworkers);
    pthread_once(&workers_initialized, init_workers_once);
}

static void init_workers_once(void) {
    mill_task_queue = mill_pipemake(sizeof (task *));
    if (!mill_task_queue)
        mill_panic("failed to initialize task queue");
    int i, count = 0;
    for (i = 0; i < num_workers; i++) {
        mill_pipe tq = mill_pipedup(mill_task_queue);
        struct mill_worker_s *w = worker_create__p(tq);
        if (!w)
            mill_pipefree(tq);
        else
            count++;
    }
    if (count == 0)
        mill_panic("failed to create any worker thread");
}

int mill_isself(struct mill_worker_s *w) {
    mill_assert(w);
    return pthread_equal(w->pth, pthread_self());
}

