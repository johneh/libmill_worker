#ifdef __cplusplus
extern "C" {
#endif
#include "libmill.h"
#include "jsdef.h"

struct js_coro_s {
    /* C from JS */
    void *coro;
    js_handle *inval;

    /* JS from C */
    js_handle *outval;
    js_handle *callback; // Exceptions thrown are swallowed.
    int err;    // errstr in outval

    struct js_vm_s *vm;
};


enum js_code {
    V8UNKNOWN = 0,
    V8EXTPTR,
    V8EXTFUNC,
    V8UNDEFINED,
    V8NULL,
    V8NUMBER,
    V8STRING,
    V8OBJECT,
    V8ARRAY,
    V8FUNC,
    V8ERROR,
    V8COMPILERUN,
    V8CALL,
    V8CALLSTR,
    V8GC,   /* Request garbage collection */
};

struct js8_arg_s {
    enum js_code type;
    int nargs;   /* js_call */
    js_vm *vm;

    union {
        js_handle *h1;
        char *source;
    };
    js_handle *a[4];
    /* input + output */
    volatile union {
        js_handle *h;
    };
};

extern js_vm *js8_vmnew(mill_worker w);
extern int js8_vminit(js_vm *vm);
extern int js8_do(struct js8_arg_s *args);
extern void js8_vmclose(js_vm *vm);
#ifdef __cplusplus
}
#endif
