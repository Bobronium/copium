#ifndef _COPIUM_STATE_C
#define _COPIUM_STATE_C
#include "copium_common.h"

typedef struct {
    // Interned strings for attribute lookups
    PyObject* str_reduce_ex;
    PyObject* str_reduce;
    PyObject* str_deepcopy;
    PyObject* str_setstate;
    PyObject* str_dict;
    PyObject* str_append;
    PyObject* str_update;
    PyObject* str_new;
    PyObject* str_get;

    // Used for identity comparison
    PyObject* sentinel;

    // Cached types (runtime-loaded from stdlib)
    PyTypeObject* BuiltinFunctionType;
    PyTypeObject* MethodType;
    PyTypeObject* CodeType;
    PyTypeObject* range_type;
    PyTypeObject* property_type;
    PyTypeObject* weakref_ref_type;
    PyTypeObject* re_Pattern_type;
    PyTypeObject* Decimal_type;
    PyTypeObject* Fraction_type;

    // Stdlib refs
    PyObject* copyreg_dispatch;                  // dict
    PyObject* copy_Error;                        // exception class
    PyObject* copyreg_newobj;                    // copyreg.__newobj__ (or sentinel)
    PyObject* copyreg_newobj_ex;                 // copyreg.__newobj_ex__ (or sentinel)
    PyObject* create_precompiler_reconstructor;  // duper.snapshots.create_precompiler_reconstructor

    // TLS memo allows reuse across deepcopy calls without allocation.
    // Key insight: memo is thread-local, not coroutine-local, which is correct
    // because deepcopy is synchronous and doesn't yield.
    Py_tss_t memo_tss;

} ModuleState;

static ModuleState module_state = {0};
#endif  // _COPIUM_STATE_C
