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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ip.h"
#include "libmill.h"
#include "utils.h"
#include "slist.h"
#include "fd.h"

#ifdef MSG_NOSIGNAL
#define MILL_NOSIGPIPE MSG_NOSIGNAL
#else
#define MILL_NOSIGPIPE 0
#endif

static int mill_tcptune(int s) {
    int opt = 1;
    (void) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
    /* If possible, prevent SIGPIPE signal when writing to the connection
        already closed by the peer. */
#ifdef SO_NOSIGPIPE
    opt = 1;
    (void) setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof (opt));
#endif
    return 0;
}

struct mill_fd_s *tcpconnect(ipaddr *addr, int64_t deadline) {
    int s = socket(ipfamily(addr), SOCK_STREAM, 0);
    if(s == -1)
        return NULL;
    struct mill_fd_s *mfd = mill_open(s);
    if (!mfd)
        return NULL;
    mfd->flags = MILL_TCPSOCK;
    if(mill_tcptune(s) == -1)
        goto er;
    int rc = connect(s, (struct sockaddr *) addr, iplen(addr));
    if(rc != 0) {
        mill_assert(rc == -1);
        if(errno != EINPROGRESS)
            goto er;
        rc = mill_fdwait(mfd, FDW_OUT, deadline);
        if(rc == 0) {
            errno = ETIMEDOUT;
            goto er;
        }
        int err;
        socklen_t errsz = sizeof(err);
        rc = getsockopt(s, SOL_SOCKET, SO_ERROR, (void*)&err, &errsz);
        if (rc != 0)
            goto er;
        if (err != 0) {
            errno = err;
            goto er;
        }
    }
    return mfd;
er:
    {
        int save_errno = errno;
        mill_close(mfd, 1);
        errno = save_errno;
    }
    return NULL;
}

struct mill_fd_s *tcplisten(ipaddr *addr, int backlog, int reuseport) {
    int s = socket(ipfamily(addr), SOCK_STREAM, 0);
    if(s == -1)
        return NULL;
    if(mill_tcptune(s) == -1)
        goto er;
    if(reuseport) {
        int opt = 1;
        (void) setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof (opt));
    }
    int rc = bind(s, (struct sockaddr *) addr, iplen(addr));
    if(rc != 0)
        goto er;
    rc = listen(s, backlog);
    if(rc == 0) {
        struct mill_fd_s *mfd = mill_open(s);
        if (!mfd)
            goto er;
        mfd->flags = MILL_TCPSOCK;
        return mfd;
    }
er:
    {
        int save_errno = errno;
        close(s);
        errno = save_errno;
    }
    return NULL;
}

struct mill_fd_s *tcpaccept(struct mill_fd_s *lsock, int64_t deadline) {
    socklen_t addrlen;
    ipaddr addr;
    while (1) {
        /* Try to get new connection (non-blocking). */
        addrlen = sizeof(addr);
        int as = accept(mill_getfd(lsock), (struct sockaddr *)&addr, &addrlen);
        if (as >= 0) {
            if (mill_tcptune(as) != -1) {
                struct mill_fd_s *mfd = mill_open(as);
                if (mfd) {
                    mfd->flags = MILL_TCPSOCK;
                    return mfd;
                }
            }
            int save_errno = errno;
            close(as);
            errno = save_errno;
            return NULL;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return NULL;
        /* Wait till new connection is available. */
        int rc = mill_fdwait(lsock, FDW_IN, deadline);
        if (rc == 0) {
            errno = ETIMEDOUT;
            return NULL;
        }
        mill_assert(rc == FDW_IN);
    }
}

static void mill_fdinit(struct mill_fd_s *mfd, int fd) {
    memset(mfd, '\0', sizeof(*mfd));
    mfd->fd = fd;
    mill_list_set_detached(&mfd->item);
}

struct mill_fd_s *mill_open(int fd) {
    /* Make the file descriptor non-blocking. */
    int opt = fcntl(fd, F_GETFL, 0);
    if (opt == -1)
        opt = 0;
    if (-1 == fcntl(fd, F_SETFL, opt | O_NONBLOCK))
        return NULL;
    struct mill_fd_s *mfd = mill_malloc(sizeof (struct mill_fd_s));
    if (!mfd) {
        errno = ENOMEM;
        return NULL;
    }
    mill_fdinit(mfd, fd);
    return mfd;
}

int mill_close(struct mill_fd_s *mfd, int doclosefd) {
    int fd;
    if (!mfd) {
        errno = EINVAL;
        return -1;
    }
    fd = mfd->fd;
    if (fd >= 0)
        mill_fdclean(mfd);
    mill_free(mfd);
    if (doclosefd)
        close(fd);
    return 0;
}

int mill_getfd(struct mill_fd_s *mfd) {
    if (!mfd) {
        errno = EINVAL;
        return -1;
    }
    return mfd->fd;
}

void mill_setdata(struct mill_fd_s *mfd, void *data) {
    mfd->data = data;
}

void *mill_getdata(struct mill_fd_s *mfd) {
    return mfd->data;
}

int mill_write(struct mill_fd_s *mfd, const void *buf, int len,
        int64_t deadline) {
    int rc;
    if (!mfd || len < 0) {
        errno = EINVAL;
        return -1;
    }
    if (mfd->fd < 0) {
        errno = EBADF;
        return -1;
    }
    do {
        if ((mfd->flags & MILL_TCPSOCK) == MILL_TCPSOCK)
            rc = (int) send(mfd->fd, buf, len, MILL_NOSIGPIPE);
        else
            rc = (int) write(mfd->fd, buf, len);
        if (rc >= 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN)
            return -1;
        rc = mill_fdwait(mfd, FDW_OUT, deadline);
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    } while (1);
    return rc;
}

int mill_read(struct mill_fd_s *mfd, void *buf, int len,
        int64_t deadline) {
    int rc;
    if (!mfd || len < 0) {
        errno = EINVAL;
        return -1;
    }
    if (mfd->fd < 0) {
        errno = EBADF;
        return -1;
    }
    do {
        rc = (int) read(mfd->fd, buf, len);
        if (rc >= 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN)
            return -1;
        rc = mill_fdwait(mfd, FDW_IN, deadline);
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    } while (1);
    return rc;
}

int mill_fdevent(int fd, int events, int64_t deadline) {
    struct mill_fd_s mfd;
    mill_fdinit(&mfd, fd);
    int rc = mill_fdwait(&mfd, events, deadline);
    mill_fdclean(&mfd);
    return rc;
}
