#include <limits.h>
#include <stdint.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

typedef struct {
    int isdir;
    int64_t size;
    char name[NAME_MAX+1];
} FileInfo;

/* synchronous */
int ReadDir(const char *dirname, FileInfo **info);

/* asynchronous */
int ReadDir_a(const char *dirname, FileInfo **info);

int FileIsDir(FileInfo *fi);
int64_t FileSize(FileInfo *fi);
const char *FileName(FileInfo *fi);

