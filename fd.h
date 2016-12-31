#ifndef MILL_IO_INCLUDED
#define MILL_IO_INCLUDED
#include "libpill.h"
#include "list.h"

enum mill_fdflags {
    MILL_SOCK = 1,
#define MILL_SOCK 1
    MILL_TCPSOCK = ((1 << 1)|MILL_SOCK),
    MILL_UDPSOCK = ((1 << 2)|MILL_SOCK),
};

struct mill_fd_s {
    int fd;
    enum mill_fdflags flags;
    void *data;

    union {
        uint32_t currevs;   /* epoll */
        int index;  /* poll */
    };
    struct mill_cr *in;
    struct mill_cr *out;
    struct mill_list_item item; /* epoll */
};

#endif
