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

#ifndef LIBMILL_H_INCLUDED
#define LIBMILL_H_INCLUDED

#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>

/******************************************************************************/
/*  ABI versioning support                                                    */
/******************************************************************************/

/*  Don't change this unless you know exactly what you're doing and have      */
/*  read and understand the following documents:                              */
/*  www.gnu.org/software/libtool/manual/html_node/Libtool-versioning.html     */
/*  www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html  */

/*  The current interface version. */
#define MILL_VERSION_CURRENT 14

/*  The latest revision of the current interface. */
#define MILL_VERSION_REVISION 0

/*  How many past interface versions are still supported. */
#define MILL_VERSION_AGE 2

/******************************************************************************/
/*  Symbol visibility                                                         */
/******************************************************************************/

/* N.B.: MILL_THREAD hack is to allow use of libmill with tinycc (tcc) compiler.
 * tcc should not be used to compile libmill itself. */

#if defined MILL_NO_EXPORTS
#   define MILL_EXPORT
#else
#   if defined _WIN32
#      if defined MILL_EXPORTS
#          define MILL_EXPORT __declspec(dllexport)
#      else
#          define MILL_EXPORT __declspec(dllimport)
#      endif
#   else
#      if defined __SUNPRO_C
#          define MILL_EXPORT __global
#      elif (defined __GNUC__ && __GNUC__ >= 4) || \
             defined __INTEL_COMPILER || defined __clang__
#          define MILL_EXPORT __attribute__ ((visibility("default")))
#          define MILL_THREAD __thread
#      else
#          define MILL_EXPORT
#          define MILL_THREAD
#      endif
#   endif
#endif

MILL_EXPORT extern void *(*mill_realloc_func)(void *ptr, size_t size);

MILL_EXPORT void *mill_init(int stacksize, int nworkers);
MILL_EXPORT void mill_fini(void);
MILL_EXPORT void mill_sethook(void *data,
        void (*resume_hook)(void *),
        void (*suspend_hook)(void *, int));
MILL_EXPORT int iscrmain(void);

MILL_EXPORT int gocount(void);
MILL_EXPORT int taskcount(void);

/******************************************************************************/
/*  Helpers                                                                   */
/******************************************************************************/

MILL_EXPORT int64_t now(void);

/******************************************************************************/
/*  Coroutines                                                                */
/******************************************************************************/

MILL_EXPORT extern volatile MILL_THREAD int mill_unoptimisable1;
MILL_EXPORT extern volatile MILL_THREAD void *mill_unoptimisable2;

/* Allocates new stack. Returns pointer to the *top* of the stack.
   For now we assume that the stack grows downwards. */
MILL_EXPORT void *mill_allocstack(void);

MILL_EXPORT sigjmp_buf *mill_getctx(void);
MILL_EXPORT void *mill_go_prologue(void *stackmem);
MILL_EXPORT void mill_go_epilogue(void);

#define mill_string2(x) #x
#define mill_string(x) mill_string2(x)

#if defined __GNUC__ || defined __clang__
#define coroutine __attribute__((noinline))
#else
#define coroutine
#endif

#define mill_go(fn, mem) \
    do {\
        void *mill_sp;\
        if(!sigsetjmp(*mill_getctx(), 0)) {\
            mill_sp = mill_go_prologue(mem);\
            int mill_anchor[mill_unoptimisable1];\
            mill_unoptimisable2 = &mill_anchor;\
            char mill_filler[(char*)&mill_anchor - (char*)(mill_sp)];\
            mill_unoptimisable2 = &mill_filler;\
            fn;\
            mill_go_epilogue();\
        }\
    } while(0)

#define go(fn) mill_go(fn, NULL)

#define yield() mill_yield()

MILL_EXPORT void mill_yield(void);

MILL_EXPORT void mill_sleep(int64_t deadline);

/******************************************************************************/
/*  Channels                                                                  */
/******************************************************************************/

typedef struct mill_chan *chan;

MILL_EXPORT void mill_panic(const char *text);

#define MILL_CLAUSELEN (sizeof(struct{void *f1; void *f2; void *f3; void *f4; \
    void *f5; void *f6; int f7; int f8; int f9;}))

#define chmake(type, bufsz) mill_chmake(sizeof(type), bufsz)

#define chdup(channel) mill_chdup((channel))

#define chs(channel, type, value) \
    do {\
        type val_ = (value);\
        if(-1 == mill_chs((channel), &val_))\
            mill_panic("send to a closed channel");\
    } while(0)

#define chr(channel, type) \
    (*(type*)mill_chr((channel)))

#define chdone(channel, type, value) \
    do {\
        type val_ = (value);\
        if(-1 == mill_chdone((channel), &val_))\
            mill_panic("send to a closed channel");\
    } while(0)

#define chclose(channel) mill_chclose((channel))

MILL_EXPORT chan mill_chmake(size_t sz, size_t bufsz);
MILL_EXPORT chan mill_chdup(chan ch);
MILL_EXPORT int mill_chs(chan ch, void *val);
MILL_EXPORT void *mill_chr(chan ch);
MILL_EXPORT int mill_chdone(chan ch, void *val);
MILL_EXPORT int mill_chclose(chan ch);

/* define MILL_CHOOSE before including libmill.h if using 'choose' */

#ifdef MILL_CHOOSE
#define mill_concat(x,y) x##y

#define choose \
    {\
        mill_choose_init();\
        int mill_idx = -2;\
        while(1) {\
            if(mill_idx != -2) {\
                if(0)

#define mill_in(chan, type, name, idx) \
                    break;\
                }\
                goto mill_concat(mill_label, idx);\
            }\
            char mill_concat(mill_clause, idx)[MILL_CLAUSELEN];\
            mill_choose_in(\
                &mill_concat(mill_clause, idx)[0],\
                (chan),\
                idx);\
            if(0) {\
                type name;\
                mill_concat(mill_label, idx):\
                if(mill_idx == idx) {\
                    name = *(type*)mill_choose_val(sizeof(type));\
                    goto mill_concat(mill_dummylabel, idx);\
                    mill_concat(mill_dummylabel, idx)

#define in(chan, type, name) mill_in((chan), type, name, __COUNTER__)

#define mill_out(chan, type, val, idx) \
                    break;\
                }\
                goto mill_concat(mill_label, idx);\
            }\
            char mill_concat(mill_clause, idx)[MILL_CLAUSELEN];\
            type mill_concat(mill_val, idx) = (val);\
            mill_choose_out(\
                &mill_concat(mill_clause, idx)[0],\
                (chan),\
                &mill_concat(mill_val, idx),\
                idx);\
            if(0) {\
                mill_concat(mill_label, idx):\
                if(mill_idx == idx) {\
                    goto mill_concat(mill_dummylabel, idx);\
                    mill_concat(mill_dummylabel, idx)

#define out(chan, type, val) mill_out((chan), type, (val), __COUNTER__)

#define mill_deadline(ddline, idx) \
                    break;\
                }\
                goto mill_concat(mill_label, idx);\
            }\
            mill_choose_deadline(ddline);\
            if(0) {\
                mill_concat(mill_label, idx):\
                if(mill_idx == -1) {\
                    goto mill_concat(mill_dummylabel, idx);\
                    mill_concat(mill_dummylabel, idx)

#define deadline(ddline) mill_deadline(ddline, __COUNTER__)

#define mill_otherwise(idx) \
                    break;\
                }\
                goto mill_concat(mill_label, idx);\
            }\
            mill_choose_otherwise();\
            if(0) {\
                mill_concat(mill_label, idx):\
                if(mill_idx == -1) {\
                    goto mill_concat(mill_dummylabel, idx);\
                    mill_concat(mill_dummylabel, idx)

#define otherwise mill_otherwise(__COUNTER__)

#define end \
                    break;\
                }\
            }\
            mill_idx = mill_choose_wait();\
        }
#endif

MILL_EXPORT void mill_choose_init(void);
MILL_EXPORT void mill_choose_in(void *clause, chan ch, int idx);
MILL_EXPORT int mill_choose_out(void *clause, chan ch, void *val, int idx);
MILL_EXPORT int mill_choose_deadline(int64_t ddline);
MILL_EXPORT int mill_choose_otherwise(void);
MILL_EXPORT int mill_choose_wait(void);
MILL_EXPORT void *mill_choose_val(size_t sz);

typedef struct mill_pipe_s *mill_pipe;

MILL_EXPORT mill_pipe mill_pipemake(unsigned sz);
MILL_EXPORT mill_pipe mill_pipedup(mill_pipe mp);
MILL_EXPORT void mill_pipefree(mill_pipe mp);
MILL_EXPORT void mill_pipeclose(mill_pipe mp);
MILL_EXPORT void *mill_piperecv(mill_pipe mp, int *done);
MILL_EXPORT int mill_pipesend(mill_pipe mp, void *ptr);
MILL_EXPORT int *mill_pipefds(mill_pipe mp);

/******************************************************************************/
/*  Wait group                                                                */
/******************************************************************************/

typedef struct mill_wgroup_s *mill_wgroup;
MILL_EXPORT mill_wgroup mill_wgmake(void);
MILL_EXPORT int mill_wgadd(mill_wgroup wg);
MILL_EXPORT int mill_wgwait(mill_wgroup wg, int64_t deadline);
MILL_EXPORT int mill_wgcancel(mill_wgroup wg);
MILL_EXPORT void mill_wgfree(mill_wgroup wg);
MILL_EXPORT int mill_waitall(int64_t deadline);

/******************************************************************************/
/*  Worker library                                                            */
/******************************************************************************/
typedef struct mill_worker_s *mill_worker;

MILL_EXPORT int open_a(const char *path, int flags, mode_t mode);
MILL_EXPORT int close_a(int fd);
MILL_EXPORT int stat_a(const char *path, struct stat *buf);
MILL_EXPORT ssize_t pread_a(int fd, void *buf, size_t count, off_t offset);
MILL_EXPORT ssize_t pwrite_a(int fd, const void *buf, size_t count, off_t offset);
MILL_EXPORT int unlink_a(const char *path);
MILL_EXPORT ssize_t readv_a(int fd, const struct iovec *iov, int iovcnt);
MILL_EXPORT ssize_t writev_a(int fd, const struct iovec *iov, int iovcnt);
MILL_EXPORT int fsync_a(int fd);
MILL_EXPORT int fstat_a(int fd, struct stat *buf);

typedef int (*taskfunc)(void *);
MILL_EXPORT int task_run(mill_worker w,
        taskfunc tf, void *data, int64_t deadline);
MILL_EXPORT int task_go(mill_worker w,
        taskfunc tf, void *data, int64_t deadline);

MILL_EXPORT mill_worker mill_worker_create(void);
MILL_EXPORT void mill_worker_delete(mill_worker w);
MILL_EXPORT int mill_worker_await(mill_worker w, int64_t deadline);
MILL_EXPORT int mill_isself(mill_worker w);
/******************************************************************************/
/*  Mutex library                                                               */
/******************************************************************************/

typedef struct mill_mutex_s *mill_mutex;

MILL_EXPORT mill_mutex mill_mutex_make(void);
MILL_EXPORT mill_mutex mill_mutex_ref(mill_mutex mu);
MILL_EXPORT void mill_mutex_unref(mill_mutex mu);
MILL_EXPORT void mill_mutex_lock(mill_mutex mu);
MILL_EXPORT void mill_mutex_unlock(mill_mutex mu);

/******************************************************************************/
/*  IP address library                                                        */
/******************************************************************************/

#define IPADDR_IPV4 1
#define IPADDR_IPV6 2
#define IPADDR_PREF_IPV4 3
#define IPADDR_PREF_IPV6 4
#define IPADDR_MAXSTRLEN 46

typedef struct ipaddr_s {char data[32];} ipaddr;

MILL_EXPORT int ipfamily(const ipaddr *addr);
MILL_EXPORT int iplen(const ipaddr *addr);
MILL_EXPORT int ipport(const ipaddr *addr);

MILL_EXPORT int iplocal(ipaddr *addr, const char *name, int port, int mode);
MILL_EXPORT int ipremote(ipaddr *addr, const char *name, int port, int mode,
    int64_t deadline);
MILL_EXPORT const char *ipaddrstr(const ipaddr *addr, char *ipstr);

typedef struct mill_fd_s *mill_fd;

MILL_EXPORT mill_fd mill_open(int fd);
MILL_EXPORT int mill_close(mill_fd mfd, int doclosefd);
MILL_EXPORT void mill_setdata(mill_fd mfd, void *data);
MILL_EXPORT void *mill_getdata(mill_fd mfd);
MILL_EXPORT int mill_getfd(mill_fd mfd);

MILL_EXPORT int mill_read(mill_fd mfd, void *buf, int count,
        int64_t deadline);
MILL_EXPORT int mill_write(mill_fd mfd, const void *buf, int count,
        int64_t deadline);

#define FDW_IN 1
#define FDW_OUT 2
#define FDW_ERR 4

MILL_EXPORT int mill_fdevent(int fd, int events, int64_t deadline);
MILL_EXPORT int mill_fdwait(mill_fd mfd, int events, int64_t deadline);
MILL_EXPORT void mill_fdclean(mill_fd mfd);
MILL_EXPORT void mill_fdclose(mill_fd mfd);

MILL_EXPORT mill_fd tcpconnect(ipaddr *addr, int64_t deadline);
MILL_EXPORT mill_fd tcplisten(ipaddr *addr, int backlog, int reuseport);
MILL_EXPORT mill_fd tcpaccept(mill_fd lsock, int64_t deadline);

#if 0
/******************************************************************************/
/*  TCP library                                                               */
/******************************************************************************/

typedef struct mill_tcpsock *tcpsock;

MILL_EXPORT tcpsock tcplisten(ipaddr addr, int backlog);
MILL_EXPORT int tcpport(tcpsock s);
MILL_EXPORT tcpsock tcpaccept(tcpsock s, int64_t deadline);
MILL_EXPORT ipaddr tcpaddr(tcpsock s);
MILL_EXPORT tcpsock tcpconnect(ipaddr addr, int64_t deadline);
MILL_EXPORT size_t tcpsend(tcpsock s, const void *buf, size_t len,
    int64_t deadline);
MILL_EXPORT void tcpflush(tcpsock s, int64_t deadline);
MILL_EXPORT size_t tcprecv(tcpsock s, void *buf, size_t len, int64_t deadline);
MILL_EXPORT size_t tcprecvuntil(tcpsock s, void *buf, size_t len,
    const char *delims, size_t delimcount, int64_t deadline);
MILL_EXPORT void tcpclose(tcpsock s);
MILL_EXPORT tcpsock tcpattach(int fd, int listening);
MILL_EXPORT int tcpdetach(tcpsock s);

/******************************************************************************/
/*  UDP library                                                               */
/******************************************************************************/

typedef struct mill_udpsock *udpsock;

MILL_EXPORT udpsock udplisten(ipaddr addr);
MILL_EXPORT int udpport(udpsock s);
MILL_EXPORT void udpsend(udpsock s, ipaddr addr, const void *buf, size_t len);
MILL_EXPORT size_t udprecv(udpsock s, ipaddr *addr,
    void *buf, size_t len, int64_t deadline);
MILL_EXPORT void udpclose(udpsock s);
MILL_EXPORT udpsock udpattach(int fd);
MILL_EXPORT int udpdetach(udpsock s);

/******************************************************************************/
/*  UNIX library                                                              */
/******************************************************************************/

typedef struct mill_unixsock *unixsock;

MILL_EXPORT unixsock unixlisten(const char *addr, int backlog);
MILL_EXPORT unixsock unixaccept(unixsock s, int64_t deadline);
MILL_EXPORT unixsock unixconnect(const char *addr);
MILL_EXPORT void unixpair(unixsock *a, unixsock *b);
MILL_EXPORT size_t unixsend(unixsock s, const void *buf, size_t len,
    int64_t deadline);
MILL_EXPORT void unixflush(unixsock s, int64_t deadline);
MILL_EXPORT size_t unixrecv(unixsock s, void *buf, size_t len,
    int64_t deadline);
MILL_EXPORT size_t unixrecvuntil(unixsock s, void *buf, size_t len,
    const char *delims, size_t delimcount, int64_t deadline);
MILL_EXPORT void unixclose(unixsock s);
MILL_EXPORT unixsock unixattach(int fd, int listening);
MILL_EXPORT int unixdetach(unixsock s);
#endif

#endif

