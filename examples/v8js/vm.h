#ifndef _VM_H
#define _VM_H

#define MAXARGS 10

// V8 state (Isolate)

struct js_vm_s {
    mill_worker w;  // must be the first
    v8::Isolate *isolate;
    char *errstr;

    /* Remote coroutines */
    mill_pipe inq;
    mill_pipe outq;

    chan ch;
    int ncoro;

    struct js_handle_s *global_handle;
    struct js_handle_s *null_handle;
    struct js_handle_s *undef_handle;
    struct js_handle_s *nullptr_handle;

    v8::Persistent<v8::Context> context;
    v8::Persistent<v8::ObjectTemplate> extptr_template;
    v8::Persistent<v8::ObjectTemplate> extfunc_template;

    v8::Persistent<v8::Value> ctype_proto;

    js_handle *args[MAXARGS];
};

struct js_handle_s {
    enum js_code type;
    int flags;
#define PERM_HANDLE (1 << 0)
#define DBL_HANDLE  (1 << 1)
#define STR_HANDLE  (1 << 2)
#define INT32_HANDLE    (1 << 3)
#define VALUE_MASK (DBL_HANDLE|STR_HANDLE|INT32_HANDLE)
#define ARG_HANDLE  (1 << 4)

    union {
        double d;
        char *stp;
        int32_t i;
        void *ptr;
    };
    js_vm *vm;
    Fnfree free_func;
    v8::Persistent<v8::Value> handle;
};

typedef struct js_vm_s js_vm;

#define LOCK_SCOPE(isolate) \
Locker locker(isolate); \
Isolate::Scope isolate_scope(isolate); \
HandleScope handle_scope(isolate);

#define ThrowTypeError(isolate, m_) \
do {\
isolate->ThrowException(Exception::TypeError(\
    String::NewFromUtf8(isolate, m_))); \
return; } while(0)

#define ThrowNotEnoughArgs(isolate, t_) \
do {\
if (t_) {                      \
isolate->ThrowException(Exception::Error(\
    String::NewFromUtf8(isolate, "too few arguments"))); \
return; }} while(0)

#define ThrowError(isolate, m) \
do {\
isolate->ThrowException(Exception::Error(\
    String::NewFromUtf8(isolate, (m))));\
return; } while(0)
#endif
