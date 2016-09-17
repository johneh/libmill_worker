#ifndef _JS_COMMON_H
#define _JS_COMMON_H

struct js_vm_s;
typedef struct js_vm_s js_vm;

struct js_handle_s;
typedef struct js_handle_s js_handle;

struct js_coro_s;
typedef struct js_coro_s js_coro;
typedef void (*Fncoro)(js_vm *vm, js_coro *, js_handle *);
typedef void (*Fnfree)(void *ptr);

struct cffn_s {
    int pcount;
    js_handle *(*fp)(js_vm *vm, int argc, js_handle *ah[]);
    const char *name;
};

typedef struct cffn_s js_ffn_t;
#endif
