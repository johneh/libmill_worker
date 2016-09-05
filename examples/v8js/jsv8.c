#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

// XXX: include libmill before v8_binding.h (need choose macro)
#define MILL_CHOOSE 1
#include "libmill.h"

#include "v8binding.h"
#include "jsv8.h"
#include "util.h"

#define js8_worker(vm)  *((mill_worker *) (vm))

js_vm *js_vmopen(mill_worker w) {
    if (!w)
        mill_panic("js_vmopen: mill_worker expected");
    js_vm *vm = js8_vmnew(w);   /* This runs in the main thread. */
    assert(*((mill_worker *) vm) == js8_worker(vm));
    /* Run js8_vminit() in the V8 thread. */
    int rc = task_run(w, (void *) js8_vminit, vm, -1);
    assert(rc == 0);
    return vm;
}

void js_vmclose(js_vm *vm) {
    mill_waitall(-1);
    js8_vmclose(vm);
}

js_handle *js_eval(js_vm *vm, const char *src) {
    struct js8_arg_s args;
    args.type = V8COMPILERUN;
    args.vm = vm;
    args.source = (char *) src;
    (void) task_run(js8_worker(vm), (void *) js8_do, &args, -1);
    return args.h;  /* NULL if error */
}

int js_run(js_vm *vm, const char *src) {
    js_handle *h = js_eval(vm, src);
    if (!h)
        return 0;
    js_dispose(h);
    return 1;
}

//
// js_call(vm, func, NULL, (jsargs_t){0})
// -> this === Global and 0 args
//
js_handle *js_call(js_vm *vm, js_handle *hfunc,
        js_handle *hself, jsargs_t hargs) {

    struct js8_arg_s args;
    args.type = V8CALL;
    if (!js_isfunction(hfunc)) {
        js_set_errstr(vm, "js_call: argument #2: function expected");
        return NULL;
    }
    args.vm = vm;
    args.h1 = hfunc;
    int i, nargs = 0;
    for(i = 0; i < 4 && hargs[i]; i++) {
        nargs++;
        args.a[i] = hargs[i];
    }
    args.nargs = nargs;
    args.h = hself;
    (void) task_run(js8_worker(vm), (void *) js8_do, &args, -1);
    return args.h; /* NULL in case of error */
}

// source is a function expression
js_handle *js_callstr(js_vm *vm, const char *source,
        js_handle *hself, jsargs_t hargs) {

    struct js8_arg_s args;
    args.type = V8CALLSTR;
    assert(source);
    args.vm = vm;
    args.source = (char *)source;
    int i, nargs = 0;
    for(i = 0; i < 4 && hargs[i]; i++) {
        nargs++;
        args.a[i] = hargs[i];
    }
    args.nargs = nargs;
    args.h = hself;
    (void) task_run(js8_worker(vm), (void *) js8_do, &args, -1);
    return args.h; /* NULL in case of error */
}

void js_gc(js_vm *vm) {
    struct js8_arg_s args;
    args.type = V8GC;
    args.vm = vm;
    int rc = task_run(js8_worker(vm), (void *) js8_do, &args, -1);
    assert(rc);
}

js_coro *choose_coro(chan ch, int64_t ddline) {
    js_coro *t = NULL;
    mill_sleep(ddline);
    choose {
    in(ch, js_coro *, t1):
        t = t1;
    deadline(ddline):
        t = NULL;
    end
    }
    return t;
}

