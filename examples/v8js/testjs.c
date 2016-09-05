#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "libmill.h"
#include "jsv8.h"

void js_panic(js_vm *vm) {
    fprintf(stderr, "%s\n", js_errstr(vm));
    exit(1);
}
#define CHECK(rc, vm) if(!rc) js_panic(vm)


void testcall(js_vm *vm) {
    js_handle *list_props = js_eval(vm,
"(function (o){\n\
    let objToInspect;\n\
    let res = [];\n\
    for(objToInspect = o; objToInspect !== null;\n\
        objToInspect = Object.getPrototypeOf(objToInspect)){\n\
            res = res.concat(Object.getOwnPropertyNames(objToInspect));\n\
    }\n\
    res.sort();\n\
    for (let i = 0; i < res.length; i++)\n\
        $print(res[i]);\n\
    }\n\
)");

    CHECK(list_props, vm);
    assert(js_isfunction(list_props));
    js_handle *h1 = js_call(vm, list_props, NULL,
                        (jsargs_t) { JSGLOBAL(vm) });
    CHECK(h1, vm);
    js_dispose(h1);
    js_dispose(list_props);
}

coroutine void do_task1(js_vm *vm, js_coro *cr, js_handle *inh) {
    yield();
    char *s1 = js_tostring(inh);
    fprintf(stderr, "<- %s\n", s1);
    int k = random() % 50;
    mill_sleep(now() + k);
    char tmp[100];
    sprintf(tmp, "%s -> Task done in %d millsecs ...", s1, k);
    js_handle *oh = js_string(vm, tmp, strlen(tmp));
    js_send(cr, oh, 0);  /* oh and inh disposed by V8 */
    free(s1);
}

void testgo(js_vm *vm) {
    js_handle *p1 = js_pointer(vm, (void *) do_task1);
    /* p1 is a js object, can set properties on it */
    int r = js_set_string(p1, "name", "task1");
    assert(r);

    js_handle *f1 = js_callstr(vm, "(function(co) {\
        $print('co name =', co.name); \
        return function(s, callback) {\
            $go(co, s, callback);\
        };\
    });", NULL, (jsargs_t) { p1 } );
    assert(f1);

    /* Global.task1 = f1; */
    int rc = js_set(JSGLOBAL(vm), "task1", f1);
    assert(rc);
    js_dispose(f1);
    js_dispose(p1);

    rc = js_run(vm,
"for(var i=1; i<=10;i++) {\n\
    task1('foo'+i, function (err, data) {\n\
            if (err == null) $print(data);\n\
    });\n\
}\n"
"$print('Waiting for response ...');\n"
"$msleep(35);\n"
"for(var i=11; i<=15;i++) {\n\
    task1('foo'+i, function (err, data) {\n\
        if (err == null) $print(data);\n\
    });\n\
}\n"
    );

    CHECK(rc, vm);
}

int main(int argc, char *argv[]) {
    mill_init(-1, 0);
    mill_worker w = mill_worker_create();
    js_vm *vm = js_vmopen(w);

    testcall(vm);
    testgo(vm);

    js_vmclose(vm);
    mill_worker_delete(w);
    mill_fini();
    return 0;
}
