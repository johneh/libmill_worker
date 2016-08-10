#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libmill.h"

// Adapted from a test program written by https://github.com/benjolitz:
// Apache bench sends 52 bytes every time per request.
// Let's do a simple counter.
// I expect you will run this server and use:
// ab -c 150 -n 10000  http://localhost:5555/
// To torture it.


void sendok(mill_fd csock, const char *da, int dalen) {
    char *ptr = (char *) da;
    int total = dalen;
    int rc;
again:
    rc = mill_write(csock, ptr, total, -1);
    if (rc > 0) {
        ptr += rc;
        total -= rc;
        if (total > 0)
            goto again;
        return;
    }
    assert(rc < 0 && errno == EAGAIN);
    yield();
    goto again;
}

coroutine void handle_request(mill_fd csock) {
    int msg_length = 0;

    while(1) {
        char buf[2048] = {0};
        int num_bytes_read = mill_read(csock, buf, sizeof(buf), now()+1);
        if (num_bytes_read > 0)
            msg_length += num_bytes_read;
        else if (num_bytes_read == 0 || errno == ECONNRESET)
            break;
        else {
            assert(errno == EAGAIN || errno == ETIMEDOUT);
        }

        if(msg_length >= 52) {
            char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nOk\n";
            sendok(csock, ok, sizeof(ok));
            break;
        }
    }

    mill_close(csock, 1);
}

int main(void) {
    mill_init(-1, 0);
    ipaddr address;
    int rc = iplocal(&address, NULL, 5555, 0);
    assert(rc == 0);
    mill_fd lsock = tcplisten(&address, 300, 0);
    assert(lsock);
    while(1) {
        mill_fd csock = tcpaccept(lsock, -1);
        assert(csock);
        go(handle_request(csock));
    }
}


