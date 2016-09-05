#ifndef _UTIL_H
#define _UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

extern void *erealloc(void *ptr, size_t newsize);
#define emalloc(l) erealloc(NULL, l)
extern char *estrdup(const char *str, size_t len);

#ifdef __cplusplus
}
#endif

#endif
