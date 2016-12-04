/*

Simple file copy program using sendfile(2).

usage: fastcp <source> <destination>

Copyright (C) 2003 Jeff Tranter.


This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/


#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/sendfile.h>
/* #include <sys/stat.h> */
#include "libpill.h"

/* Async sendfile */

/* Wrapper object for the input(arguments) and output(result) */
struct sendfile_s {
    int out_fd;
    int in_fd;
    off_t *offset;
    size_t count;
    ssize_t sz;
};

/* Wrapper function for the synchronous version */
static int sendfile_task(void *q) {
    struct sendfile_s *rp = q;
    rp->sz = sendfile(rp->out_fd, rp->in_fd, rp->offset, rp->count);
    /* return -1 if errno set */
    return rp->sz < 0 ? -1 : 0;
}

ssize_t sendfile_a(int out_fd, int in_fd, off_t *offset, size_t count) {
    struct sendfile_s s = {
            .out_fd = out_fd,
            .in_fd = in_fd,
            .offset = offset,
            .count = count
    };
    (void) task_run(NULL, sendfile_task, &s, -1);
    return s.sz;
}

/* Progress bar */
coroutine static void dot(int *done) {
    char c = '.';
    while (! *done) {
        printf("%c", c);
        mill_sleep(now() + 1);
    }
    printf("\n");
}


int main (int argc, char** argv)
{
  int src;               /* file descriptor for source file */
  int dest;              /* file descriptor for destination file */
  struct stat stat_buf;  /* hold information about input file */
  off_t offset = 0;      /* byte offset used by sendfile */
  ssize_t rc;            /* return code from sendfile */

  mill_init(-1, -1);
  int done = 0;
  go(dot(&done));

  /* check for two command line arguments */
  if (argc != 3) {
    fprintf(stderr, "usage: %s <source> <destination>\n", argv[0]);
    exit(1);
  }

  /* check that source file exists and can be opened */
  src = open_a(argv[1], O_RDONLY, 0);
  if (src == -1) {
    fprintf(stderr, "unable to open '%s': %s\n", argv[1], strerror(errno));
    exit(1);
  }

 /* get size and permissions of the source file */
  fstat_a(src, &stat_buf);

 /* open destination file */
  dest = open_a(argv[2], O_WRONLY|O_CREAT, stat_buf.st_mode);
  if (dest == -1) {
    fprintf(stderr, "unable to open '%s': %s\n", argv[2], strerror(errno));
    exit(1);
  }

 /* copy file using sendfile */
 rc = sendfile_a(dest, src, &offset, stat_buf.st_size);
 if (rc == -1) {
    fprintf(stderr, "error from sendfile: %s\n", strerror(errno));
    exit(1);
 }
 if (rc != stat_buf.st_size) {
   fprintf(stderr, "incomplete transfer from sendfile: %d of %d bytes\n",
           (int) rc,
           (int)stat_buf.st_size);
   exit(1);
 }

 /* clean up and exit */
 close_a(dest);
 close_a(src);

 done = 1;
 mill_waitall(-1);
 return 0;
}
