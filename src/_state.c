/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_STATE_C
#define _COPIUM_STATE_C
#include "_common.h"

typedef enum {
    COPIUM_MEMO_NATIVE = 0,
    COPIUM_MEMO_DICT = 1,
} CopiumMemoMode;

typedef enum {
    COPIUM_ON_INCOMPATIBLE_WARN = 0,
    COPIUM_ON_INCOMPATIBLE_RAISE = 1,
    COPIUM_ON_INCOMPATIBLE_SILENT = 2,
} CopiumOnIncompatible;

typedef struct {
    // Interned strings for attribute lookups
    PyObject* s__reduce_ex__;
    PyObject* s__reduce__;
    PyObject* s__deepcopy__;
    PyObject* s__setstate__;
    PyObject* s__dict__;
    PyObject* s_append;
    PyObject* s_update;
    PyObject* s__new__;
    PyObject* s__get__;

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
    PyObject* copyreg___newobj__;                // copyreg.__newobj__ (or sentinel)
    PyObject* copyreg___newobj___ex;             // copyreg.__newobj_ex__ (or sentinel)

    // TLS memo allows reuse across deepcopy calls without allocation.
    // Key insight: memo is thread-local, not coroutine-local, which is correct
    // because deepcopy is synchronous and doesn't yield.
    Py_tss_t memo_tss;

    // Configuration (initialized from env vars, overridable via copium.configure())
    CopiumMemoMode memo_mode;
    CopiumOnIncompatible on_incompatible;
    PyObject* ignored_errors;         // Tuple of error suffixes to suppress warnings for
    PyObject* ignored_errors_joined;  // Pre-joined string for warning message (or NULL if empty)
    PyObject* dict_items_descr;
    vectorcallfunc dict_items_vc;
} ModuleState;

static ModuleState module_state = {0};
#endif  // _COPIUM_STATE_C
