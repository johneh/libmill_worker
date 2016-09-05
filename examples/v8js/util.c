#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "util.h"

void *erealloc(void *ptr, size_t newsize) {
    if (! (ptr = realloc(ptr, newsize))) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return ptr;
}

char *estrdup(const char *str, size_t len) {
    char *s;
    s = erealloc(NULL, len + 1);
    if (len > 0)
        memcpy(s, str, len);
    s[len] = '\0';
    return s;
}

