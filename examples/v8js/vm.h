#ifndef _VM_H
#define _VM_H

// V8 state (Isolate)

struct js_vm_s {
    mill_worker w;  // must be the first
    v8::Isolate *isolate;
    char *errstr;

    chan ch;
    int ncoro;

    struct js_handle_s *global_handle;
    struct js_handle_s *null_handle;
    struct js_handle_s *undef_handle;

    v8::Persistent<v8::Context> context;
    v8::Persistent<v8::ObjectTemplate> extptr_template;
    v8::Persistent<v8::ObjectTemplate> extfunc_template;
};

#define GetCtypeId(obj) ((obj)->InternalFieldCount() != 2 ? 0 \
    : static_cast<int>(reinterpret_cast<uintptr_t>( \
        (obj)->GetAlignedPointerFromInternalField(0)) >> 2))

#define IsCtypeId(obj, t) ((obj)->InternalFieldCount() == 2 && \
    (t) == static_cast<int>(reinterpret_cast<uintptr_t>( \
        (obj)->GetAlignedPointerFromInternalField(0)) >> 2))

struct js_handle_s {
    enum js_code type;
    v8::Persistent<v8::Value> handle;
    js_vm *vm;
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
