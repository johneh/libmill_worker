#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "readdir.h"
#include "libmill.h"

/* synchronous version */
int ReadDir(const char *dirname, FileInfo **info) {
    *info = NULL;
    DIR *dirp = opendir(dirname);
    if (!dirp)
        return -1;
    struct dirent entry;
    struct dirent *dp;
    struct stat sb;
    char *filename;
    int ninfo = 0, sizeinfo = 0;
    FileInfo *finfo = NULL;

    filename = malloc(strlen(dirname)+NAME_MAX+2);
    if(!filename) {
        errno = ENOMEM;
        return -1;
    }

    while (1) {
        int rc = readdir_r(dirp, &entry, &dp);
        if (rc != 0) {
            errno = rc;
            break;
        }
        if (!dp) {
            errno = 0;
            break;
        }

        size_t namelen = strlen(dp->d_name);
        assert(namelen <= NAME_MAX);
        /* Skip . and .. */
        if ((namelen == 1 && dp->d_name[0] == '.')
                || (namelen == 2 && dp->d_name[0] == '.' && dp->d_name[1] == '.')
        )
            continue;
        int64_t filesize = 0;
        int isdir = 0;
        sprintf(filename, "%s/%s", dirname, dp->d_name);
        if (stat(filename, &sb) != -1) {
            filesize = sb.st_size;
            isdir = S_ISDIR(sb.st_mode);
        }
        if (ninfo == sizeinfo) {
            sizeinfo += 16;
            finfo = realloc(finfo, sizeinfo * sizeof(FileInfo));
            if (! finfo) {
                errno = ENOMEM;
                break;
            }
        }
        memcpy(finfo[ninfo].name, dp->d_name, namelen+1);
        finfo[ninfo].size = filesize;
        finfo[ninfo].isdir = isdir;
        ninfo++;
    }
    free(filename);

    int save_errno = errno;
    (void) closedir(dirp);
    errno = save_errno;
    if (errno) {
        free(finfo);
        return -1;
    }
    *info = finfo;
    return finfo ? ninfo : 0;
}

/* async version */

/* Wrapper object for the input(arguments) and output(result) */
struct readdir_s {
    const char *dir;
    FileInfo **finfo;
    int nfiles;
};

/* Wrapper function for the synchronus version */
static int readdir_task(void *q) {
    struct readdir_s *rp = q;
    rp->nfiles = ReadDir(rp->dir, rp->finfo);
    return rp->nfiles < 0 ? -1 : 0;
}

int ReadDir_a(const char *dir, FileInfo **info) {
    struct readdir_s r = {0};
    r.dir = dir;
    r.finfo = info;
    int rc = task_run(NULL, readdir_task, &r, -1);
    if (rc != -1)
        return r.nfiles;
    assert(errno);
    *info = NULL;
    return -1;
}

int FileIsDir(FileInfo *fi) {
    return fi->isdir;
}

int64_t FileSize(FileInfo *fi) {
    return fi->size;
}

const char *FileName(FileInfo *fi) {
    return fi->name;
}

#if 0
/* Test ReadDir_a */

int main(void) {
    int i, n;
    FileInfo *info;
    n = ReadDir_a(".", & info);
    for (i = 0; i < n; i++) {
        printf("%s:%d:%d\n", info[i].name,
                    (int) info[i].size, info[i].isdir);
    }
    free(info);
    return 0;
}
#endif
