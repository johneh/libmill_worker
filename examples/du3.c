#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define MILL_CHOOSE 1
#include "libpill.h"
#include "readdir.h"

/* N.B.: does not include size of the directory (argument) or any
 * subdirectories */

/* The go version uses a counting semaphore to limit concurrency.
 * In our case, the concurrency-limiting factor is the number of
 * workers in the threadpool. */

// dirents returns the entries of directory dir.
int dirents(const char *dir, FileInfo **entries) {
    int nfiles = ReadDir_a(dir, entries);
    if (nfiles == -1) {
        fprintf(stderr, "du: %s\n", strerror(errno));
        return 0;
    }
    return nfiles;
}

coroutine void walkDir(const char *dirname, mill_wgroup wg, chan fileSizes) {
    mill_wgadd(wg);
    FileInfo *entries;
    int i;

    /* Get our own copy of dirname now!. */
    char *dir = strdup(dirname);
    assert(dir);

    /* Queue the task; Concurrency is via the threadpool. */
    int nfiles = dirents(dir, &entries);

    for (i = 0; i < nfiles; i++) {
        FileInfo *entry = entries + i;
        if (FileIsDir(entry)) {
            char subdir[2*NAME_MAX+2];  // FIXME sizing
            sprintf(subdir, "%s/%s", dir, FileName(entry));
            go(walkDir(subdir, wg, chdup(fileSizes)));
        } else
            chs(fileSizes, int64_t, FileSize(entry));
    }
    free(dir);
    chclose(fileSizes);
}

coroutine void waitClose(mill_wgroup wg, chan fileSizes) {
    mill_wgwait(wg, -1);
    chdone(fileSizes, int64_t, -1);
    chclose(fileSizes);
}

void printDiskUsage(int nfiles, int64_t nbytes) {
	printf("%d files  %g Kb\n", nfiles, nbytes/1024.0);
}

coroutine void ticker(unsigned duration, chan tick) {
    int64_t nw, t2, t1 = now() + duration;
    assert(duration > 0);
    while (1) {
        mill_sleep(t1);
        t2 = t1 + duration;
        /* Drop ticks to make up for a slow receiver. */
        choose {
        out(tick, int64_t, t1):
            ;
        deadline(t2):
            ;   /* Dropped! */
        end
        }
        t1 = t2;
        nw = now();
        if (t1 <= nw)
            t1 = nw + 1; 
    }
}

int main(int argc, char **argv) {
    mill_init(-1, 0);

    int vFlag = 0;

	// Traverse each root of the file tree in parallel.
    chan fileSizes = chmake(int64_t, 0);
    mill_wgroup wg = mill_wgmake();
    assert(wg);
    int i, nroot = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            vFlag = 1;
        else if (*argv[i] != '\0') {
            go(walkDir(argv[i], wg, chdup(fileSizes)));
            nroot++;
        }
    }
    if (!nroot)
        go(walkDir(".", wg, chdup(fileSizes)));

    go(waitClose(wg, chdup(fileSizes)));

    // Print the results periodically.
    chan tick= chmake(int64_t, 1);
    if (vFlag)
        go(ticker(500, chdup(tick)));

    int nfiles = 0;
    int64_t nbytes = 0;
    int done = 0;

    while (! done) {
        choose {
        in(fileSizes, int64_t, size):
            if (size == -1) {
                done = 1; // fileSizes was closed
            } else {
                nfiles++;
                nbytes += size;
            }
        in(tick, int64_t, tval):
            printDiskUsage(nfiles, nbytes);
        end
        }
    }

    printDiskUsage(nfiles, nbytes); // final totals
    chclose(fileSizes);
    mill_wgfree(wg);
    return 0;
}

