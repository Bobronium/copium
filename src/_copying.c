/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * copium
 * - Fast, native deepcopy with reduce protocol + keepalive memo
 * - Pin integration via _pinning.c (Pin/PinsProxy + APIs)
 *
 * Public API:
 *   py_deepcopy(x, memo=None) -> any
 *   py_copy(x) -> any
 *   py_replace(x, **replace=None) -> any
 *   py_replicate(x, n, /) -> any
 *
 * Python 3.10–3.14 compatible.
 */


#include <stddef.h>  // ptrdiff_t
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include "Python.h"
#include "pycore_object.h"  // _PyNone_Type, _PyNotImplemented_Type
// _PyDict_NewPresized/_PyDict_*KnownHas
#if PY_VERSION_HEX < PY_VERSION_3_11_HEX
#include "dictobject.h"
#else
#include "pycore_dict.h"
#endif
// _PySet_NextEntry()

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
#include "setobject.h"
#else
#include "pycore_setobject.h"
#endif

/* -----------------------------------------------------------------------------
 * Inlining policy
 *
 * Goal:
 *   - Make ALWAYS_INLINE truly "always inline" (force) everywhere it appears.
 *   - Avoid forced inlining on recursive dispatchers to prevent GCC/Clang errors.
 *   - Provide MAYBE_INLINE as a strong hint so PGO can choose profitable expansion.
 *
 * Policy:
 *   - MSVC: ALWAYS_INLINE = __forceinline; MAYBE_INLINE = __inline.
 *   - GCC/Clang: ALWAYS_INLINE = inline + always_inline (+hot); MAYBE_INLINE = inline (+hot).
 * ---------------------------------------------------------------------------*/
#ifdef ALWAYS_INLINE
#undef ALWAYS_INLINE
#endif
#ifdef MAYBE_INLINE
#undef MAYBE_INLINE
#endif

/* Feature-gated attributes for portability */
#if defined(__has_attribute)
  #if __has_attribute(hot)
    #define COPIUM_ATTR_HOT __attribute__((hot))
  #else
    #define COPIUM_ATTR_HOT
  #endif
  #if __has_attribute(gnu_inline)
    #define COPIUM_ATTR_GNU_INLINE __attribute__((gnu_inline))
  #else
    #define COPIUM_ATTR_GNU_INLINE
  #endif
#else
  #if defined(__GNUC__) || defined(__clang__)
    #define COPIUM_ATTR_HOT __attribute__((hot))
    #define COPIUM_ATTR_GNU_INLINE __attribute__((gnu_inline))
  #else
    #define COPIUM_ATTR_HOT
    #define COPIUM_ATTR_GNU_INLINE
  #endif
#endif

#if defined(_MSC_VER)
  #define ALWAYS_INLINE __forceinline
  #define MAYBE_INLINE __inline
#elif defined(__GNUC__) || defined(__clang__)
  #define ALWAYS_INLINE inline __attribute__((always_inline)) COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
  #define MAYBE_INLINE inline COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
#else
  #define ALWAYS_INLINE inline
  #define MAYBE_INLINE inline
#endif

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
static Py_tss_t g_dictiter_tss = Py_tss_NEEDS_INIT;
static int g_dict_watcher_id = -1;
static int g_dict_watcher_registered = 0;
#endif


/* ------------------------------ Small helpers ------------------------------ */

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
/* Use internal non-raising lookup that does not clobber unrelated exceptions. */
static ALWAYS_INLINE int get_optional_attr(PyObject* obj, PyObject* name, PyObject** out) {
    return _PyObject_LookupAttr(obj, name, out);
}
#define PyObject_GetOptionalAttr(obj, name, out) get_optional_attr((obj), (name), (out))
#endif

/* -------- Dict iteration with mutation check (fast path) -------- */
/* Abstraction:
   DictIterGuard di;
   dict_iter_init(&di, dict);
   while ((ret = dict_iter_next(&di, &key, &value)) > 0) { ... }
   if (ret < 0) -> error set (mutation detected)
*/
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
/* Python 3.14+: use public dict watcher API to detect mutation without touching private fields. */

typedef struct DictIterGuard {
    PyObject* dict;
    Py_ssize_t pos;
    Py_ssize_t size0; /* initial size snapshot (detect transient size changes like pop→add) */
    int watching;
    int mutated;
    int size_changed; /* set if size ever differed from size0; else keys-only changes */
    int last_event;   /* PyDict_WatchEvent (for debugging/future use) */
    struct DictIterGuard* prev;
} DictIterGuard;

/* Walk the TLS stack and flag the top-most guard matching `dict`. */
static int _copium_dict_watcher_cb(
    PyDict_WatchEvent event, PyObject* dict, PyObject* key, PyObject* new_value
) {
    (void)key;
    (void)new_value;
    DictIterGuard* g = (DictIterGuard*)PyThread_tss_get(&g_dictiter_tss);
    for (; g; g = g->prev) {
        if (g->dict == dict) {
            g->mutated = 1;
            g->last_event = (int)event;

            /* Treat as "changed size" if:
               - the event is an add/delete/clear, OR
               - the dict's current size differs from the initial snapshot (transient del→add). */
            if (event == PyDict_EVENT_ADDED || event == PyDict_EVENT_DELETED ||
                event == PyDict_EVENT_CLEARED || event == PyDict_EVENT_CLONED) {
                g->size_changed = 1;
            } else {
                Py_ssize_t cur = PyDict_Size(dict);
                if (cur >= 0 && cur != g->size0) {
                    g->size_changed = 1;
                }
            }
            /* Other events (CLONED, MODIFIED, etc.) fall back to "keys changed". */
            /* DO NOT break: mark all active guards for this dict (handles nested iterations). */
        }
    }
    return 0; /* never raise; per docs this would become unraisable */
}

static ALWAYS_INLINE void dict_iter_init(DictIterGuard* di, PyObject* dict) {
    di->dict = dict;
    di->pos = 0;
    di->size0 = PyDict_Size(dict); /* snapshot initial size (errors unlikely; -1 harmless) */
    di->watching = 0;
    di->mutated = 0;
    di->size_changed = 0;
    di->last_event = 0;
    di->prev = (DictIterGuard*)PyThread_tss_get(&g_dictiter_tss);

    /* Push onto TLS stack first so any same-thread mutation after Watch sees this guard. */
    PyThread_tss_set(&g_dictiter_tss, di);

    if (g_dict_watcher_registered && PyDict_Watch(g_dict_watcher_id, dict) == 0) {
        di->watching = 1;
    }
}

static ALWAYS_INLINE void dict_iter_cleanup(DictIterGuard* di) {
    /* Pop from TLS stack (defensive unlink in case of nested/early exits). */
    DictIterGuard* top = (DictIterGuard*)PyThread_tss_get(&g_dictiter_tss);
    if (top == di) {
        PyThread_tss_set(&g_dictiter_tss, di->prev);
    } else {
        DictIterGuard* cur = top;
        while (cur && cur->prev != di)
            cur = cur->prev;
        if (cur && cur->prev == di)
            cur->prev = di->prev;
    }
    if (di->watching) {
        PyDict_Unwatch(g_dict_watcher_id, di->dict);
        di->watching = 0;
    }
}

static ALWAYS_INLINE int dict_iter_next(DictIterGuard* di, PyObject** key, PyObject** value) {
    if (PyDict_Next(di->dict, &di->pos, key, value)) {
        if (UNLIKELY(di->mutated)) {
            /* Decide message based on net size delta from start of iteration. */
            int size_changed_now = 0;
            Py_ssize_t cur = PyDict_Size(di->dict);
            if (cur >= 0) {
                size_changed_now = (cur != di->size0);
            } else {
                /* Fallback if PyDict_Size errored: use watcher heuristic. */
                size_changed_now = di->size_changed;
            }
            PyErr_SetString(
                PyExc_RuntimeError,
                size_changed_now ? "dictionary changed size during iteration"
                                 : "dictionary keys changed during iteration"
            );
            dict_iter_cleanup(di);
            return -1;
        }
        return 1;
    }
    /* End of iteration. If a mutation happened at any point, surface it now. */
    if (UNLIKELY(di->mutated)) {
        int size_changed_now = 0;
        Py_ssize_t cur = PyDict_Size(di->dict);
        if (cur >= 0) {
            size_changed_now = (cur != di->size0);
        } else {
            size_changed_now = di->size_changed;
        }
        PyErr_SetString(
            PyExc_RuntimeError,
            size_changed_now ? "dictionary changed size during iteration"
                             : "dictionary keys changed during iteration"
        );
        dict_iter_cleanup(di);
        return -1;
    }
    dict_iter_cleanup(di);
    return 0;
}

#else /* < 3.14: keep version-tag based guard (uses private fields, but gated) */

typedef struct {
    PyObject* dict;
    Py_ssize_t pos;
    uint64_t ver0;
    Py_ssize_t used0;
} DictIterGuard;

static ALWAYS_INLINE void dict_iter_init(DictIterGuard* di, PyObject* dict) {
    di->dict = dict;
    di->pos = 0;
    di->ver0 = ((PyDictObject*)dict)->ma_version_tag;
    di->used0 = ((PyDictObject*)dict)->ma_used;
}

static ALWAYS_INLINE int dict_iter_next(DictIterGuard* di, PyObject** key, PyObject** value) {
    if (PyDict_Next(di->dict, &di->pos, key, value)) {
        uint64_t ver_now = ((PyDictObject*)di->dict)->ma_version_tag;
        if (UNLIKELY(ver_now != di->ver0)) {
            Py_ssize_t used_now = ((PyDictObject*)di->dict)->ma_used;
            if (used_now != di->used0) {
                PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
            } else {
                PyErr_SetString(PyExc_RuntimeError, "dictionary keys changed during iteration");
            }
            return -1;
        }
        return 1;
    }
    return 0;
}
#endif

/* ------------------------------ Module state --------------------------------
 * Cache frequently used objects/types. Pin-specific caches live in _pinning.c.
 */
typedef struct {
    // interned strings
    PyObject* str_reduce_ex;
    PyObject* str_reduce;
    PyObject* str_deepcopy;
    PyObject* str_setstate;
    PyObject* str_dict;
    PyObject* str_append;
    PyObject* str_update;
    PyObject* str_new;
    PyObject* str_get;

    // cached types
    PyObject* sentinel;
    PyTypeObject* BuiltinFunctionType;
    PyTypeObject* MethodType;
    PyTypeObject* CodeType;
    PyTypeObject* range_type;
    PyTypeObject* property_type;
    PyTypeObject* weakref_ref_type;
    PyTypeObject* re_Pattern_type;
    PyTypeObject* Decimal_type;
    PyTypeObject* Fraction_type;

    // stdlib refs
    PyObject* copyreg_dispatch;                  // dict
    PyObject* copy_Error;                        // exception class
    PyObject* copyreg_newobj;                    // copyreg.__newobj__ (or sentinel)
    PyObject* copyreg_newobj_ex;                 // copyreg.__newobj_ex__ (or sentinel)
    PyObject* create_precompiler_reconstructor;  // duper.snapshots.create_precompiler_reconstructor

    // recursion depth guard (thread-local counter for safe deepcopy)
    Py_tss_t recdepth_tss;

    // thread-local memo for implicit use
    Py_tss_t memo_tss;

} ModuleState;

static ModuleState module_state = {0};

/* ------------------------------ Atomic checks ------------------------------
 */

static ALWAYS_INLINE int is_literal_immutable(PyTypeObject* tp) {
    // first tier: the most popular literal immutables
    unsigned long r = (tp == &_PyNone_Type) | (tp == &PyLong_Type) | (tp == &PyUnicode_Type) |
        (tp == &PyBool_Type) | (tp == &PyFloat_Type) | (tp == &PyBytes_Type);
    return (int)r;
}

static ALWAYS_INLINE int is_builtin_immutable(PyTypeObject* tp) {
    /* second tier: less common than builtin containers */
    unsigned long r = (tp == &PyRange_Type) | (tp == &PyFunction_Type) | (tp == &PyCFunction_Type) |
        (tp == &PyProperty_Type) | (tp == &_PyWeakref_RefType) | (tp == &PyCode_Type) |
        (tp == &PyModule_Type) | (tp == &_PyNotImplemented_Type) | (tp == &PyEllipsis_Type) |
        (tp == &PyComplex_Type);
    return (int)r;
}

static ALWAYS_INLINE int is_stdlib_immutable(PyTypeObject* tp) {
    /* third tier: stdlib immutables cached at runtime */
    unsigned long r = (tp == module_state.re_Pattern_type) | (tp == module_state.Decimal_type) |
        (tp == module_state.Fraction_type);
    return (int)r;
}

static ALWAYS_INLINE int is_class(PyTypeObject* tp) {
    /* type objects themselves and type-subclasses are immutable */
    return PyType_HasFeature(tp, Py_TPFLAGS_TYPE_SUBCLASS);
}

static ALWAYS_INLINE int is_atomic_immutable(PyTypeObject* tp) {
    /* consolidated type-based predicate (no object needed) */
    unsigned long r = (unsigned long)is_literal_immutable(tp) |
        (unsigned long)is_builtin_immutable(tp) | (unsigned long)is_class(tp) |
        (unsigned long)is_stdlib_immutable(tp);
    return (int)r;
}

/* ------------------------ TLS memo & recursion guard ------------------------
 */

static PyObject* get_thread_local_memo(void) {
    void* val = PyThread_tss_get(&module_state.memo_tss);
    if (val == NULL || Py_REFCNT((PyObject*)val) > 1) {
        PyObject* memo = Memo_New();
        if (memo == NULL)
            return NULL;
        if (PyThread_tss_set(&module_state.memo_tss, (void*)memo) != 0) {
            Py_DECREF(memo);
            return NULL;
        }
        return memo;
    }
    return (PyObject*)val;
}

/* ------------------------- Recursion depth guard (stack cap) ----------------
 * (unchanged: we only sample when entering fallback/reconstructor)
 */

#ifndef COPIUM_STACKCHECK_STRIDE
#define COPIUM_STACKCHECK_STRIDE 32u
#endif

#ifndef COPIUM_STACK_SAFETY_MARGIN
#define COPIUM_STACK_SAFETY_MARGIN (256u * 1024u)  // 256 KiB
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static _Thread_local unsigned int _copium_tls_depth = 0;
static _Thread_local int _copium_tls_stack_inited = 0;
static _Thread_local char* _copium_tls_stack_low = NULL;
static _Thread_local char* _copium_tls_stack_high = NULL;
#endif

static ALWAYS_INLINE void _copium_stack_init_if_needed(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    if (_copium_tls_stack_inited)
        return;
    _copium_tls_stack_inited = 1;

#if defined(__APPLE__)
    pthread_t t = pthread_self();
    size_t sz = pthread_get_stacksize_np(t);
    void* base = pthread_get_stackaddr_np(t);

    char* high = (char*)base;
    char* low = high - (ptrdiff_t)sz;

    if ((size_t)sz > COPIUM_STACK_SAFETY_MARGIN)
        low += COPIUM_STACK_SAFETY_MARGIN;

    _copium_tls_stack_low = low;
    _copium_tls_stack_high = high;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = NULL;
        size_t sz = 0;
        if (pthread_attr_getstack(&attr, &addr, &sz) == 0 && addr && sz) {
            char* low = (char*)addr;
            char* high = low + (ptrdiff_t)sz;
            if (sz > COPIUM_STACK_SAFETY_MARGIN)
                low += COPIUM_STACK_SAFETY_MARGIN;
            _copium_tls_stack_low = low;
            _copium_tls_stack_high = high;
        }
        pthread_attr_destroy(&attr);
    }
#else
    _copium_tls_stack_low = NULL;
    _copium_tls_stack_high = NULL;
#endif
#endif
}

static ALWAYS_INLINE int _copium_recdepth_enter(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    /* Fast warmup: for shallow recursion, avoid any stack/limit checks. */
    unsigned int d = ++_copium_tls_depth;
    if (LIKELY(d < 16u)) {
        return 0;
    }
    /* Existing logic for deeper recursion (sampled by stride). */
    if (UNLIKELY((d & (COPIUM_STACKCHECK_STRIDE - 1u)) == 0u)) {
        _copium_stack_init_if_needed();
        if (_copium_tls_stack_low) {
            char sp_probe;
            char* sp = (char*)&sp_probe;
            if (UNLIKELY(sp <= _copium_tls_stack_low)) {
                _copium_tls_depth--;
                PyErr_SetString(
                    PyExc_RecursionError,
                    "maximum recursion depth exceeded in copium.deepcopy (stack safety cap)"
                );
                return -1;
            }
        } else {
            uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
            uintptr_t next = depth_u + 1;
            int limit = Py_GetRecursionLimit();
            if (limit > 10000)
                limit = 10000;
            if (UNLIKELY((int)next > limit)) {
                _copium_tls_depth--;
                PyErr_SetString(
                    PyExc_RecursionError, "maximum recursion depth exceeded in copium.deepcopy"
                );
                return -1;
            }
            (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)next);
        }
    }
    return 0;
#else
    uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
    uintptr_t next = depth_u + 1;

    /* Fast warmup for shallow recursion on non-C11 builds as well. */
    if (LIKELY(next < 16u)) {
        (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)next);
        return 0;
    }

#ifdef _WIN32
    if (UNLIKELY((next & (COPIUM_STACKCHECK_STRIDE - 1u)) == 0u)) {
        typedef VOID(WINAPI * GetStackLimitsFn)(PULONG_PTR, PULONG_PTR);
        static GetStackLimitsFn _copium_pGetCurrentThreadStackLimits = NULL;
        static int _copium_stacklimits_resolved = 0;
        if (!_copium_stacklimits_resolved) {
            HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
            if (hKernel32) {
                _copium_pGetCurrentThreadStackLimits =
                    (GetStackLimitsFn)GetProcAddress(hKernel32, "GetCurrentThreadStackLimits");
            }
            _copium_stacklimits_resolved = 1;
        }
        if (_copium_pGetCurrentThreadStackLimits) {
            ULONG_PTR low = 0, high = 0;
            _copium_pGetCurrentThreadStackLimits(&low, &high);
            char sp_probe;
            char* sp = (char*)&sp_probe;
            char* lowc = (char*)low;
            size_t sz = (size_t)(high - low);
            if (sz > COPIUM_STACK_SAFETY_MARGIN)
                lowc += COPIUM_STACK_SAFETY_MARGIN;
            if (UNLIKELY(sp <= lowc)) {
                PyErr_SetString(
                    PyExc_RecursionError,
                    "maximum recursion depth exceeded in copium.deepcopy (stack safety cap)"
                );
                return -1;
            }
        }
    }
#endif

    int limit = Py_GetRecursionLimit();
    if (limit > 10000)
        limit = 10000;
    if (UNLIKELY((int)next > limit)) {
        PyErr_SetString(
            PyExc_RecursionError, "maximum recursion depth exceeded in copium.deepcopy"
        );
        return -1;
    }
    (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)next);
    return 0;
#endif
}

static ALWAYS_INLINE void _copium_recdepth_leave(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    if (_copium_tls_depth > 0)
        _copium_tls_depth--;
#else
    uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
    if (depth_u > 0)
        (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)(depth_u - 1));
#endif
}

/* -------------------- Guarded return macros for recursion ------------------- */
#define RETURN_GUARDED(expr)                        \
    do {                                            \
        if (UNLIKELY(_copium_recdepth_enter() < 0)) \
            return NULL;                            \
        PyObject* _ret = (expr);                    \
        _copium_recdepth_leave();                   \
        return _ret;                                \
    } while (0)

#define RETURN_GUARDED_PY(expr) RETURN_GUARDED(expr)

/* ----------------------- Python-dict memo helpers (inline) ------------------ */

static PyObject* custom_memo_lookup(PyObject* memo, void* key_ptr) {
    PyObject* pykey = PyLong_FromVoidPtr(key_ptr);
    if (UNLIKELY(!pykey))
        return NULL;
    PyObject* res;
    if (PyDict_CheckExact(memo)) {
        res = PyDict_GetItemWithError(memo, pykey);
        Py_DECREF(pykey);
        if (UNLIKELY(res == NULL)) {
            return NULL;
        }
        Py_INCREF(res);
        return res;
    } else {
        /* For custom memos, call memo.get(key, sentinel) using PyObject_CallMethodObjArgs.
           This is more efficient than PyObject_CallMethod because it uses a cached
           interned string object instead of creating a new string each time. */
        res = PyObject_CallMethodObjArgs(memo, module_state.str_get, pykey, module_state.sentinel, NULL);
        Py_DECREF(pykey);
        if (UNLIKELY(!res))
            return NULL;
        if (res == module_state.sentinel) {
            Py_DECREF(res);
            return NULL;
        }
        return res;
    }
}

static int custom_memo_store(PyObject* memo, void* key_ptr, PyObject* value) {
    PyObject* pykey = PyLong_FromVoidPtr(key_ptr);
    if (UNLIKELY(!pykey))
        return -1;
    int rc;
    if (PyDict_CheckExact(memo)) {
        rc = PyDict_SetItem(memo, pykey, value);
    } else {
        rc = PyObject_SetItem(memo, pykey, value);
    }
    Py_DECREF(pykey);
    return rc;
}

static ALWAYS_INLINE int ensure_keep_list_for_pymemo(PyObject* memo, PyObject** keep_list_ptr) {
    if (*keep_list_ptr)
        return 0;
    void* key_ptr = (void*)memo;
    PyObject* existing = custom_memo_lookup(memo, key_ptr);
    if (existing) {
        *keep_list_ptr = existing;
        return 0;
    }
    if (PyErr_Occurred())
        return -1;
    PyObject* new_list = PyList_New(0);
    if (!new_list)
        return -1;
    if (custom_memo_store(memo, key_ptr, new_list) < 0) {
        Py_DECREF(new_list);
        return -1;
    }
    Py_DECREF(new_list);
    Py_INCREF(new_list);
    *keep_list_ptr = new_list;
    return 0;
}

/* ----------------------------- Predecl for c/py paths ---------------------- */

static ALWAYS_INLINE PyObject* deepcopy_c(PyObject* obj, MemoObject* mo);
static ALWAYS_INLINE PyObject* deepcopy_py(PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr);

/* ----------------------------- Type-special helpers ------------------------ */
/* We define two sets of helpers: *_c operate with MemoObject*, *_py with dict. */
/* Each set inlines dispatch and only recurses after type resolution.          */

/* === C-memo specializations ================================================= */

#define MEMO_LOOKUP_C(id, h) memo_table_lookup_h(mo->table, (id), (h))
#define MEMO_STORE_C(id, val, h) memo_table_insert_h(&mo->table, (id), (val), (h))
#define KEEP_APPEND_AFTER_COPY_C(src)                      \
    do {                                                   \
        if ((copy) != (src)) {                             \
            if (keepvector_append(&mo->keep, (src)) < 0) { \
                Py_DECREF(copy);                           \
                return NULL;                               \
            }                                              \
        }                                                  \
    } while (0)
#define MEMO_OBJ_C ((PyObject*)mo)

static MAYBE_INLINE PyObject* deepcopy_list_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_tuple_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_dict_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_set_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_frozenset_c(
    PyObject* obj, MemoObject* mo, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_bytearray_c(
    PyObject* obj, MemoObject* mo, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_method_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_via_reduce_c(
    PyObject* obj, PyTypeObject* tp, MemoObject* mo, Py_ssize_t id_hash
);

static ALWAYS_INLINE PyObject* deepcopy_c(PyObject* obj, MemoObject* mo) {
    PyTypeObject* tp = Py_TYPE(obj);
    /* 1) Immortal or literal immutables → fastest return */
    if (LIKELY(is_literal_immutable(tp))) {
        return Py_NewRef(obj);
    }

    /* 2) Memo hit */
    void* id = (void*)obj;
    Py_ssize_t h = memo_hash_pointer(id);
    PyObject* hit = MEMO_LOOKUP_C(id, h);
    if (hit)
        return Py_NewRef(hit);

    /* 3) Popular containers first (specialized, likely hot) */
    if (tp == &PyDict_Type)
        RETURN_GUARDED(deepcopy_dict_c(obj, mo, h));
    if (tp == &PyList_Type)
        RETURN_GUARDED(deepcopy_list_c(obj, mo, h));
    if (tp == &PyTuple_Type)
        RETURN_GUARDED(deepcopy_tuple_c(obj, mo, h));
    if (tp == &PySet_Type)
        RETURN_GUARDED(deepcopy_set_c(obj, mo, h));

    /* 4) Other atomic immutables (builtin/class types) */
    if (is_builtin_immutable(tp) || is_class(tp)) {
        return Py_NewRef(obj);
    }

    if (tp == &PyFrozenSet_Type)
        return deepcopy_frozenset_c(obj, mo, h);

    if (tp == &PyByteArray_Type)
        return deepcopy_bytearray_c(obj, mo, h);

    if (tp == &PyMethod_Type)
        return deepcopy_method_c(obj, mo, h);

    if (is_stdlib_immutable(tp))  // touch non-static types last
        return Py_NewRef(obj);

    /* Robustly detect a user-defined __deepcopy__ via optional lookup (single step, non-raising on miss). */
    {
        PyObject* deepcopy_meth = NULL;
        int has_deepcopy = PyObject_GetOptionalAttr(obj, module_state.str_deepcopy, &deepcopy_meth);
        if (has_deepcopy < 0)
            return NULL;
        if (has_deepcopy) {
            PyObject* res = PyObject_CallOneArg(deepcopy_meth, MEMO_OBJ_C);
            Py_DECREF(deepcopy_meth);
            if (!res)
                return NULL;
            if (res != obj) {
                if (MEMO_STORE_C((void*)obj, res, h) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
                if (keepvector_append(&mo->keep, obj) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
            }
            return res;
        }
    }

    PyObject* res = deepcopy_via_reduce_c(obj, tp, mo, h);
    return res;
}

static MAYBE_INLINE PyObject* deepcopy_list_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash) {
    Py_ssize_t sz = Py_SIZE(obj);
    PyObject* copy = PyList_New(sz);
    if (!copy)
        return NULL;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyList_SET_ITEM(copy, i, Py_NewRef(Py_None));
    }
    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyList_GET_ITEM(obj, i);
        PyObject* copied_item = deepcopy_c(item, mo);  // inlined dispatch with optional recursion
        if (!copied_item) {
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(Py_None);
        PyList_SET_ITEM(copy, i, copied_item);
    }

    /* If the source list grew during the first pass, append the extra tail items. */
    Py_ssize_t i2 = sz;
    while (i2 < Py_SIZE(obj)) {
        PyObject* item2 = PyList_GET_ITEM(obj, i2);
        PyObject* copied2 = deepcopy_c(item2, mo);  // second-pass guard
        if (!copied2) {
            Py_DECREF(copy);
            return NULL;
        }
        if (PyList_Append(copy, copied2) < 0) {
            Py_DECREF(copied2);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(copied2);
        i2++;
    }
    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_tuple_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash) {
    Py_ssize_t sz = Py_SIZE(obj);
    PyObject* copy = PyTuple_New(sz);
    if (!copy)
        return NULL;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyTuple_GET_ITEM(obj, i);
        PyObject* copied = deepcopy_c(item, mo);
        if (!copied) {
            Py_DECREF(copy);
            return NULL;
        }
        if (copied != item)
            all_same = 0;
        PyTuple_SET_ITEM(copy, i, copied);
    }
    if (all_same) {
        Py_DECREF(copy);
        return Py_NewRef(obj);
    }

    /* Handle self-referential tuples: if a recursive path already created a copy,
       prefer that existing copy to maintain identity. */
    PyObject* existing = MEMO_LOOKUP_C((void*)obj, id_hash);
    if (existing) {
        Py_DECREF(copy);
        return Py_NewRef(existing);
    }
    if (PyErr_Occurred()) {
        Py_DECREF(copy);
        return NULL;
    }

    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_dict_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash) {
    PyObject* copy = PyDict_New();
    if (!copy)
        return NULL;
    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }

    DictIterGuard di;
    dict_iter_init(&di, obj);

    PyObject *key, *value;
    int ret;
    while ((ret = dict_iter_next(&di, &key, &value)) > 0) {
        PyObject* ckey = deepcopy_c(key, mo);
        if (!ckey) {
            Py_DECREF(copy);
            return NULL;
        }
        PyObject* cvalue = deepcopy_c(value, mo);
        if (!cvalue) {
            Py_DECREF(ckey);
            Py_DECREF(copy);
            return NULL;
        }
        if (PyDict_SetItem(copy, ckey, cvalue) < 0) {
            Py_DECREF(ckey);
            Py_DECREF(cvalue);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(ckey);
        Py_DECREF(cvalue);
    }
    if (ret < 0) { /* mutation detected -> error already set */
        Py_DECREF(copy);
        return NULL;
    }

    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_set_c(PyObject* obj, MemoObject* mo, Py_ssize_t id_hash) {
    PyObject* copy = PySet_New(NULL);
    if (!copy)
        return NULL;
    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }

    /* Snapshot into a pre-sized tuple without invoking user code. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1) {
        Py_DECREF(copy);
        return NULL;
    }
    PyObject* snap = PyTuple_New(n);
    if (!snap) {
        Py_DECREF(copy);
        return NULL;
    }

    Py_ssize_t pos = 0;
    PyObject* item;
    Py_hash_t hash;
    Py_ssize_t i = 0;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        if (i < n) {
            /* item is borrowed; store owned ref in the tuple */
            Py_INCREF(item);
            PyTuple_SET_ITEM(snap, i, item);
            i++;
        } else {
            /* If the set grows during snapshotting, ignore extras to avoid overflow. */
        }
    }
    if (PyErr_Occurred()) {
        Py_DECREF(snap);
        Py_DECREF(copy);
        return NULL;
    }

    for (Py_ssize_t j = 0; j < i; j++) {
        PyObject* elem = PyTuple_GET_ITEM(snap, j); /* borrowed from snapshot */
        PyObject* citem = deepcopy_c(elem, mo);
        if (!citem) {
            Py_DECREF(snap);
            Py_DECREF(copy);
            return NULL;
        }
        if (PySet_Add(copy, citem) < 0) {
            Py_DECREF(citem);
            Py_DECREF(snap);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(citem);
    }
    Py_DECREF(snap);

    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset_c(
    PyObject* obj, MemoObject* mo, Py_ssize_t id_hash
) {
    /* Pre-size snapshot: frozenset is immutable, so size won't change mid-loop. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        return NULL;

    PyObject* temp = PyTuple_New(n);
    if (!temp)
        return NULL;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        /* item is borrowed; deepcopy_c returns a new reference which tuple will own. */
        PyObject* citem = deepcopy_c(item, mo);
        if (!citem) {
            Py_DECREF(temp);
            return NULL;
        }
        PyTuple_SET_ITEM(temp, i, citem);  // steals reference to citem
        i++;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(temp);
        return NULL;
    }

    PyObject* copy = PyFrozenSet_New(temp);
    Py_DECREF(temp);
    if (!copy)
        return NULL;
    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static ALWAYS_INLINE PyObject* deepcopy_bytearray_c(
    PyObject* obj, MemoObject* mo, Py_ssize_t id_hash
) {
    Py_ssize_t sz = PyByteArray_Size(obj);
    PyObject* copy = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copy)
        return NULL;
    if (sz)
        memcpy(PyByteArray_AS_STRING(copy), PyByteArray_AS_STRING(obj), (size_t)sz);
    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_method_c(
    PyObject* obj, MemoObject* mo, Py_ssize_t id_hash
) {
    PyObject* func = PyMethod_GET_FUNCTION(obj);
    PyObject* self = PyMethod_GET_SELF(obj);
    if (!func || !self)
        return NULL;

    Py_INCREF(func);
    Py_INCREF(self);
    PyObject* cself = deepcopy_c(self, mo);
    Py_DECREF(self);
    if (!cself) {
        Py_DECREF(func);
        return NULL;
    }

    PyObject* copy = PyMethod_New(func, cself);
    Py_DECREF(func);
    Py_DECREF(cself);
    if (!copy)
        return NULL;

    if (MEMO_STORE_C((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_C(obj);
    return copy;
}

static MAYBE_INLINE PyObject* try_reduce_via_registry(PyObject* obj, PyTypeObject* tp) {
    PyObject* reducer = PyDict_GetItemWithError(module_state.copyreg_dispatch, (PyObject*)tp);
    if (!reducer) {
        if (PyErr_Occurred())
            return NULL;
        return NULL;
    }
    if (!PyCallable_Check(reducer)) {
        PyErr_SetString(PyExc_TypeError, "copyreg.dispatch_table value is not callable");
        return NULL;
    }
    return PyObject_CallOneArg(reducer, obj);
}

static MAYBE_INLINE PyObject* call_reduce_method_preferring_ex(PyObject* obj) {
    PyObject* reduce_ex = PyObject_GetAttr(obj, module_state.str_reduce_ex);
    if (reduce_ex) {
        PyObject* res = PyObject_CallFunction(reduce_ex, "i", 4);
        Py_DECREF(reduce_ex);
        if (res)
            return res;
        return NULL;
    }
    PyErr_Clear();
    PyObject* reduce = PyObject_GetAttr(obj, module_state.str_reduce);
    if (reduce) {
        PyObject* res = PyObject_CallNoArgs(reduce);
        Py_DECREF(reduce);
        return res;
    }
    PyErr_Clear();
    PyErr_SetString(
        (PyObject*)module_state.copy_Error, "un(deep)copyable object (no reduce protocol)"
    );
    return NULL;
}

static int unpack_reduce_result_tuple(
    PyObject* reduce_result,
    PyObject** out_constructor,
    PyObject** out_args,
    PyObject** out_state,
    PyObject** out_list_iterator,
    PyObject** out_dict_iterator
) {
    if (!PyTuple_Check(reduce_result)) {
        if (PyUnicode_Check(reduce_result) || PyBytes_Check(reduce_result)) {
            *out_constructor = *out_args = *out_state = *out_list_iterator = *out_dict_iterator =
                NULL;
            return 1;
        }
        PyErr_SetString(PyExc_TypeError, "__reduce__ must return a tuple or str");
        return -1;
    }
    Py_ssize_t sz = PyTuple_GET_SIZE(reduce_result);
    if (sz < 2 || sz > 5) {
        PyErr_SetString(PyExc_TypeError, "__reduce__ tuple length must be in [2,5]");
        return -1;
    }
    PyObject* cons = PyTuple_GET_ITEM(reduce_result, 0);
    PyObject* args = PyTuple_GET_ITEM(reduce_result, 1);
    if (!PyCallable_Check(cons) || !PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError, "__reduce__ first two items must be (callable, tuple)");
        return -1;
    }
    *out_constructor = cons;
    *out_args = args;
    *out_state = (sz >= 3) ? PyTuple_GET_ITEM(reduce_result, 2) : NULL;
    *out_list_iterator = (sz >= 4) ? PyTuple_GET_ITEM(reduce_result, 3) : NULL;
    *out_dict_iterator = (sz == 5) ? PyTuple_GET_ITEM(reduce_result, 4) : NULL;
    return 0;
}

static MAYBE_INLINE PyObject* deepcopy_via_reduce_c(
    PyObject* obj, PyTypeObject* tp, MemoObject* mo, Py_ssize_t id_hash
) {
    PyObject* reduce_res = try_reduce_via_registry(obj, tp);
    if (!reduce_res) {
        if (PyErr_Occurred())
            return NULL;
        reduce_res = call_reduce_method_preferring_ex(obj);
        if (!reduce_res)
            return NULL;
    }

    PyObject *cons = NULL, *args = NULL, *state = NULL, *listit = NULL, *dictit = NULL;
    int unpack = unpack_reduce_result_tuple(reduce_res, &cons, &args, &state, &listit, &dictit);
    if (unpack < 0) {
        Py_DECREF(reduce_res);
        return NULL;
    }
    if (unpack == 1) {
        Py_DECREF(reduce_res);
        return Py_NewRef(obj);
    }

    PyObject* recon = NULL;
    if (cons == module_state.copyreg_newobj) {
        if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) >= 1) {
            PyObject* cls = PyTuple_GET_ITEM(args, 0);
            if (PyType_Check(cls)) {
                Py_ssize_t n = PyTuple_GET_SIZE(args) - 1;
                PyObject* newargs = PyTuple_New(n);
                if (!newargs) {
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                for (Py_ssize_t i = 0; i < n; i++) {
                    PyObject* a = PyTuple_GET_ITEM(args, i + 1);
                    PyObject* ca = deepcopy_c(a, mo);
                    if (!ca) {
                        Py_DECREF(newargs);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                    PyTuple_SET_ITEM(newargs, i, ca);
                }
                recon = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, newargs, NULL);
                Py_DECREF(newargs);
            }
        }
    } else if (cons == module_state.copyreg_newobj_ex) {
        if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 3) {
            PyObject* cls = PyTuple_GET_ITEM(args, 0);
            PyObject* pargs = PyTuple_GET_ITEM(args, 1);
            PyObject* pkwargs = PyTuple_GET_ITEM(args, 2);
            if (PyType_Check(cls) && PyTuple_Check(pargs)) {
                PyObject* dc_pargs = deepcopy_c(pargs, mo);
                if (!dc_pargs) {
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* dc_kwargs = NULL;
                if (pkwargs != Py_None) {
                    dc_kwargs = deepcopy_c(pkwargs, mo);
                    if (!dc_kwargs) {
                        Py_DECREF(dc_pargs);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                }
                recon = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, dc_pargs, dc_kwargs);
                Py_DECREF(dc_pargs);
                Py_XDECREF(dc_kwargs);
            }
        }
    }
    if (!recon) {
        if (PyTuple_GET_SIZE(args) == 0) {
            recon = PyObject_CallNoArgs(cons);
        } else {
            Py_ssize_t n = PyTuple_GET_SIZE(args);
            PyObject* dcargs = PyTuple_New(n);
            if (!dcargs) {
                Py_DECREF(reduce_res);
                return NULL;
            }
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject* a = PyTuple_GET_ITEM(args, i);
                PyObject* ca = deepcopy_c(a, mo);
                if (!ca) {
                    Py_DECREF(dcargs);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyTuple_SET_ITEM(dcargs, i, ca);
            }
            recon = PyObject_CallObject(cons, dcargs);
            Py_DECREF(dcargs);
        }
    }
    if (!recon) {
        Py_DECREF(reduce_res);
        return NULL;
    }

    if (MEMO_STORE_C((void*)obj, recon, id_hash) < 0) {
        Py_DECREF(recon);
        Py_DECREF(reduce_res);
        return NULL;
    }

    if (state && state != Py_None) {
        PyObject* setstate = PyObject_GetAttr(recon, module_state.str_setstate);
        if (setstate) {
            PyObject* cstate = deepcopy_c(state, mo);
            if (!cstate) {
                Py_DECREF(setstate);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            PyObject* r = PyObject_CallOneArg(setstate, cstate);
            Py_DECREF(cstate);
            Py_DECREF(setstate);
            if (!r) {
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            Py_DECREF(r);
        } else {
            PyErr_Clear();
            PyObject* dict_state = NULL;
            PyObject* slot_state = NULL;
            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slot_state = PyTuple_GET_ITEM(state, 1);
                if (slot_state && slot_state != Py_None) {
                    PyObject* it = PyObject_GetIter(slot_state);
                    if (!it) {
                        Py_DECREF(recon);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                    PyObject* key;
                    while ((key = PyIter_Next(it)) != NULL) {
                        PyObject* value = PyObject_GetItem(slot_state, key);
                        if (!value) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            Py_DECREF(recon);
                            Py_DECREF(reduce_res);
                            return NULL;
                        }
                        PyObject* cvalue = deepcopy_c(value, mo);
                        Py_DECREF(value);
                        if (!cvalue) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            Py_DECREF(recon);
                            Py_DECREF(reduce_res);
                            return NULL;
                        }
                        if (PyObject_SetAttr(recon, key, cvalue) < 0) {
                            Py_DECREF(cvalue);
                            Py_DECREF(key);
                            Py_DECREF(it);
                            Py_DECREF(recon);
                            Py_DECREF(reduce_res);
                            return NULL;
                        }
                        Py_DECREF(cvalue);
                        Py_DECREF(key);
                    }
                    Py_DECREF(it);
                    if (PyErr_Occurred()) {
                        Py_DECREF(recon);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                }
            } else {
                dict_state = state;
            }
            if (dict_state && dict_state != Py_None) {
                PyObject* cdict_state = deepcopy_c(dict_state, mo);
                if (!cdict_state) {
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* obj_dict = PyObject_GetAttr(recon, module_state.str_dict);
                if (!obj_dict) {
                    Py_DECREF(cdict_state);
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
                Py_DECREF(obj_dict);
                if (!update) {
                    Py_DECREF(cdict_state);
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* r = PyObject_CallOneArg(update, cdict_state);
                Py_DECREF(update);
                Py_DECREF(cdict_state);
                if (!r) {
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                Py_DECREF(r);
            }
        }
    }

    if (listit && listit != Py_None) {
        PyObject* append = PyObject_GetAttr(recon, module_state.str_append);
        if (!append) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
        PyObject* it = PyObject_GetIter(listit);
        if (!it) {
            Py_DECREF(append);
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
        PyObject* item;
        while ((item = PyIter_Next(it)) != NULL) {
            PyObject* citem = deepcopy_c(item, mo);
            Py_DECREF(item);
            if (!citem) {
                Py_DECREF(it);
                Py_DECREF(append);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            PyObject* r = PyObject_CallOneArg(append, citem);
            Py_DECREF(citem);
            if (!r) {
                Py_DECREF(it);
                Py_DECREF(append);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            Py_DECREF(r);
        }
        Py_DECREF(it);
        Py_DECREF(append);
        if (PyErr_Occurred()) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
    }

    if (dictit && dictit != Py_None) {
        PyObject* it = PyObject_GetIter(dictit);
        if (!it) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
        PyObject* pair;
        while ((pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                PyErr_SetString(PyExc_TypeError, "dictiter must yield (key, value) pairs");
                return NULL;
            }
            PyObject* k = PyTuple_GET_ITEM(pair, 0);
            PyObject* v = PyTuple_GET_ITEM(pair, 1);
            PyObject* ck = deepcopy_c(k, mo);
            if (!ck) {
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            PyObject* cv = deepcopy_c(v, mo);
            if (!cv) {
                Py_DECREF(ck);
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            if (PyObject_SetItem(recon, ck, cv) < 0) {
                Py_DECREF(ck);
                Py_DECREF(cv);
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            Py_DECREF(ck);
            Py_DECREF(cv);
            Py_DECREF(pair);
        }
        Py_DECREF(it);
        if (PyErr_Occurred()) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
    }

    if (recon != obj) {
        if (keepvector_append(&mo->keep, obj) < 0) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
    }
    Py_DECREF(reduce_res);
    return recon;
}

/* === Python-dict memo specializations ====================================== */

#define MEMO_LOOKUP_PY(id, h) custom_memo_lookup(memo_dict, (id))
#define MEMO_STORE_PY(id, val, h) custom_memo_store(memo_dict, (id), (val))
#define KEEP_APPEND_AFTER_COPY_PY(src)                                           \
    do {                                                                         \
        if ((copy) != (src)) {                                                   \
            if (*keep_list_ptr == NULL) {                                        \
                if (ensure_keep_list_for_pymemo(memo_dict, keep_list_ptr) < 0) { \
                    Py_DECREF(copy);                                             \
                    return NULL;                                                 \
                }                                                                \
            }                                                                    \
            if (PyList_Append(*keep_list_ptr, (src)) < 0) {                      \
                Py_DECREF(copy);                                                 \
                return NULL;                                                     \
            }                                                                    \
        }                                                                        \
    } while (0)
#define MEMO_OBJ_PY (memo_dict)

static MAYBE_INLINE PyObject* deepcopy_list_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_tuple_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_dict_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_set_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_frozenset_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static ALWAYS_INLINE PyObject* deepcopy_bytearray_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_method_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_via_reduce_py(
    PyObject* obj, PyTypeObject* tp, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
);

static ALWAYS_INLINE PyObject* deepcopy_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr
) {
    PyTypeObject* tp = Py_TYPE(obj);

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    /* Python 3.14+: Check ALL atomic immutables before memo lookup */
    if (LIKELY(is_literal_immutable(tp) || is_builtin_immutable(tp) || is_class(tp))) {
        return Py_NewRef(obj);
    }
#endif

    /* Memo lookup (Python < 3.14: this happens before immutable check) */
    void* id = (void*)obj;
    Py_ssize_t h = memo_hash_pointer(id);
    PyObject* hit = MEMO_LOOKUP_PY(id, h);
    if (hit)
        return hit;
    if (PyErr_Occurred())
        return NULL;

#if PY_VERSION_HEX < PY_VERSION_3_14_HEX
    /* Python < 3.14: Check immutables after memo lookup */
    if (LIKELY(is_literal_immutable(tp))) {
        return Py_NewRef(obj);
    }
#endif

    /* 3) Popular containers first (specialized, likely hot) */
    if (tp == &PyList_Type)
        RETURN_GUARDED_PY(deepcopy_list_py(obj, memo_dict, keep_list_ptr, h));
    if (tp == &PyTuple_Type)
        RETURN_GUARDED_PY(deepcopy_tuple_py(obj, memo_dict, keep_list_ptr, h));
    if (tp == &PyDict_Type)
        RETURN_GUARDED_PY(deepcopy_dict_py(obj, memo_dict, keep_list_ptr, h));
    if (tp == &PySet_Type)
        RETURN_GUARDED_PY(deepcopy_set_py(obj, memo_dict, keep_list_ptr, h));

    if (is_builtin_immutable(tp) || is_class(tp)) {
        return Py_NewRef(obj);
    }

    if (tp == &PyFrozenSet_Type)
        return deepcopy_frozenset_py(obj, memo_dict, keep_list_ptr, h);
    if (tp == &PyByteArray_Type)
        return deepcopy_bytearray_py(obj, memo_dict, keep_list_ptr, h);
    if (tp == &PyMethod_Type)
        return deepcopy_method_py(obj, memo_dict, keep_list_ptr, h);

    if (is_stdlib_immutable(tp)) {
        return Py_NewRef(obj);
    }

    /* Robustly detect a user-defined __deepcopy__ via optional lookup (single step, non-raising on miss). */
    {
        PyObject* deepcopy_meth = NULL;
        int has_deepcopy = PyObject_GetOptionalAttr(obj, module_state.str_deepcopy, &deepcopy_meth);
        if (has_deepcopy < 0)
            return NULL;
        if (has_deepcopy) {
            PyObject* res = PyObject_CallOneArg(deepcopy_meth, MEMO_OBJ_PY);
            Py_DECREF(deepcopy_meth);
            if (!res)
                return NULL;
            if (res != obj) {
                if (MEMO_STORE_PY((void*)obj, res, id_hash) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
                if (*keep_list_ptr == NULL) {
                    if (ensure_keep_list_for_pymemo(memo_dict, keep_list_ptr) < 0) {
                        Py_DECREF(res);
                        return NULL;
                    }
                }
                if (PyList_Append(*keep_list_ptr, obj) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
            }
            return res;
        }
    }

    PyObject* res = deepcopy_via_reduce_py(obj, tp, memo_dict, keep_list_ptr, h);
    return res;
}

static MAYBE_INLINE PyObject* deepcopy_list_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    Py_ssize_t sz = Py_SIZE(obj);
    PyObject* copy = PyList_New(sz);
    if (!copy)
        return NULL;

    for (Py_ssize_t i = 0; i < sz; ++i)
        PyList_SET_ITEM(copy, i, Py_NewRef(Py_None));
    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyList_GET_ITEM(obj, i);
        PyObject* copied_item = deepcopy_py(
            item, memo_dict, keep_list_ptr
        );  // inlined dispatch with optional recursion
        if (!copied_item) {
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(Py_None);
        PyList_SET_ITEM(copy, i, copied_item);
    }

    // If the source list grew during the first pass, append extra tail items.
    Py_ssize_t i2 = sz;
    while (i2 < Py_SIZE(obj)) {
        PyObject* item2 = PyList_GET_ITEM(obj, i2);
        PyObject* copied2 = deepcopy_py(item2, memo_dict, keep_list_ptr);
        if (!copied2) {
            Py_DECREF(copy);
            return NULL;
        }
        if (PyList_Append(copy, copied2) < 0) {
            Py_DECREF(copied2);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(copied2);
        i2++;
    }
    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_tuple_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    Py_ssize_t sz = Py_SIZE(obj);
    PyObject* copy = PyTuple_New(sz);
    if (!copy)
        return NULL;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyTuple_GET_ITEM(obj, i);
        PyObject* copied = deepcopy_py(item, memo_dict, keep_list_ptr);
        if (!copied) {
            Py_DECREF(copy);
            return NULL;
        }
        if (copied != item)
            all_same = 0;
        PyTuple_SET_ITEM(copy, i, copied);
    }
    if (all_same) {
        Py_DECREF(copy);
        return Py_NewRef(obj);
    }

    /* Handle self-referential tuples: if a recursive path already created a copy,
       prefer that existing copy to maintain identity. */
    PyObject* existing = MEMO_LOOKUP_PY((void*)obj, id_hash);
    if (existing) {
        Py_DECREF(copy);
        return existing;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(copy);
        return NULL;
    }

    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_dict_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    PyObject* copy = PyDict_New();
    if (!copy)
        return NULL;
    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }

    DictIterGuard di;
    dict_iter_init(&di, obj);

    PyObject *key, *value;
    int ret;
    while ((ret = dict_iter_next(&di, &key, &value)) > 0) {
        PyObject* ckey = deepcopy_py(key, memo_dict, keep_list_ptr);
        if (!ckey) {
            Py_DECREF(copy);
            return NULL;
        }
        PyObject* cvalue = deepcopy_py(value, memo_dict, keep_list_ptr);
        if (!cvalue) {
            Py_DECREF(ckey);
            Py_DECREF(copy);
            return NULL;
        }
        if (PyDict_SetItem(copy, ckey, cvalue) < 0) {
            Py_DECREF(ckey);
            Py_DECREF(cvalue);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(ckey);
        Py_DECREF(cvalue);
    }
    if (ret < 0) { /* mutation detected -> error already set */
        Py_DECREF(copy);
        return NULL;
    }

    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_set_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    PyObject* copy = PySet_New(NULL);
    if (!copy)
        return NULL;
    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }

    /* Snapshot into a pre-sized tuple without invoking user code. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1) {
        Py_DECREF(copy);
        return NULL;
    }
    PyObject* snap = PyTuple_New(n);
    if (!snap) {
        Py_DECREF(copy);
        return NULL;
    }

    Py_ssize_t pos = 0;
    PyObject* item;
    Py_hash_t hash;
    Py_ssize_t i = 0;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        if (i < n) {
            /* item is borrowed; store owned ref in the tuple */
            Py_INCREF(item);
            PyTuple_SET_ITEM(snap, i, item);
            i++;
        } else {
            /* If the set grows during snapshotting, ignore extras to avoid overflow. */
        }
    }
    if (PyErr_Occurred()) {
        Py_DECREF(snap);
        Py_DECREF(copy);
        return NULL;
    }

    /* Deepcopy from the stable snapshot prefix. */
    for (Py_ssize_t j = 0; j < i; j++) {
        PyObject* elem = PyTuple_GET_ITEM(snap, j); /* borrowed from snap */
        PyObject* citem = deepcopy_py(elem, memo_dict, keep_list_ptr);
        if (!citem) {
            Py_DECREF(snap);
            Py_DECREF(copy);
            return NULL;
        }
        if (PySet_Add(copy, citem) < 0) {
            Py_DECREF(citem);
            Py_DECREF(snap);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(citem);
    }
    Py_DECREF(snap);

    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    /* Pre-size snapshot: frozenset is immutable, so size won't change mid-loop. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        return NULL;

    PyObject* temp = PyTuple_New(n);
    if (!temp)
        return NULL;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        /* item is borrowed; deepcopy_py returns a new reference which tuple will own. */
        PyObject* citem = deepcopy_py(item, memo_dict, keep_list_ptr);
        if (!citem) {
            Py_DECREF(temp);
            return NULL;
        }
        PyTuple_SET_ITEM(temp, i, citem);  // steals reference to citem
        i++;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(temp);
        return NULL;
    }

    PyObject* copy = PyFrozenSet_New(temp);
    Py_DECREF(temp);
    if (!copy)
        return NULL;
    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_bytearray_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    Py_ssize_t sz = PyByteArray_Size(obj);
    PyObject* copy = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copy)
        return NULL;
    if (sz)
        memcpy(PyByteArray_AS_STRING(copy), PyByteArray_AS_STRING(obj), (size_t)sz);
    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_method_py(
    PyObject* obj, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    PyObject* func = PyMethod_GET_FUNCTION(obj);
    PyObject* self = PyMethod_GET_SELF(obj);
    if (!func || !self)
        return NULL;

    Py_INCREF(func);
    Py_INCREF(self);
    PyObject* cself = deepcopy_py(self, memo_dict, keep_list_ptr);
    Py_DECREF(self);
    if (!cself) {
        Py_DECREF(func);
        return NULL;
    }

    PyObject* copy = PyMethod_New(func, cself);
    Py_DECREF(func);
    Py_DECREF(cself);
    if (!copy)
        return NULL;

    if (MEMO_STORE_PY((void*)obj, copy, id_hash) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    KEEP_APPEND_AFTER_COPY_PY(obj);
    return copy;
}

static MAYBE_INLINE PyObject* deepcopy_via_reduce_py(
    PyObject* obj, PyTypeObject* tp, PyObject* memo_dict, PyObject** keep_list_ptr, Py_ssize_t id_hash
) {
    PyObject* reduce_res = try_reduce_via_registry(obj, tp);
    if (!reduce_res) {
        if (PyErr_Occurred())
            return NULL;
        reduce_res = call_reduce_method_preferring_ex(obj);
        if (!reduce_res)
            return NULL;
    }

    PyObject *cons = NULL, *args = NULL, *state = NULL, *listit = NULL, *dictit = NULL;
    int unpack = unpack_reduce_result_tuple(reduce_res, &cons, &args, &state, &listit, &dictit);
    if (unpack < 0) {
        Py_DECREF(reduce_res);
        return NULL;
    }
    if (unpack == 1) {
        Py_DECREF(reduce_res);
        return Py_NewRef(obj);
    }

    PyObject* recon = NULL;
    if (cons == module_state.copyreg_newobj) {
        if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) >= 1) {
            PyObject* cls = PyTuple_GET_ITEM(args, 0);
            if (PyType_Check(cls)) {
                Py_ssize_t n = PyTuple_GET_SIZE(args) - 1;
                PyObject* newargs = PyTuple_New(n);
                if (!newargs) {
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                for (Py_ssize_t i = 0; i < n; i++) {
                    PyObject* a = PyTuple_GET_ITEM(args, i + 1);
                    PyObject* ca = deepcopy_py(a, memo_dict, keep_list_ptr);
                    if (!ca) {
                        Py_DECREF(newargs);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                    PyTuple_SET_ITEM(newargs, i, ca);
                }
                recon = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, newargs, NULL);
                Py_DECREF(newargs);
            }
        }
    } else if (cons == module_state.copyreg_newobj_ex) {
        if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 3) {
            PyObject* cls = PyTuple_GET_ITEM(args, 0);
            PyObject* pargs = PyTuple_GET_ITEM(args, 1);
            PyObject* pkwargs = PyTuple_GET_ITEM(args, 2);
            if (PyType_Check(cls) && PyTuple_Check(pargs)) {
                PyObject* dc_pargs = deepcopy_py(pargs, memo_dict, keep_list_ptr);
                if (!dc_pargs) {
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* dc_kwargs = NULL;
                if (pkwargs != Py_None) {
                    dc_kwargs = deepcopy_py(pkwargs, memo_dict, keep_list_ptr);
                    if (!dc_kwargs) {
                        Py_DECREF(dc_pargs);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                }
                recon = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, dc_pargs, dc_kwargs);
                Py_DECREF(dc_pargs);
                Py_XDECREF(dc_kwargs);
            }
        }
    }
    if (!recon) {
        if (PyTuple_GET_SIZE(args) == 0) {
            recon = PyObject_CallNoArgs(cons);
        } else {
            Py_ssize_t n = PyTuple_GET_SIZE(args);
            PyObject* dcargs = PyTuple_New(n);
            if (!dcargs) {
                Py_DECREF(reduce_res);
                return NULL;
            }
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject* a = PyTuple_GET_ITEM(args, i);
                PyObject* ca = deepcopy_py(a, memo_dict, keep_list_ptr);
                if (!ca) {
                    Py_DECREF(dcargs);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyTuple_SET_ITEM(dcargs, i, ca);
            }
            recon = PyObject_CallObject(cons, dcargs);
            Py_DECREF(dcargs);
        }
    }
    if (!recon) {
        Py_DECREF(reduce_res);
        return NULL;
    }

    if (MEMO_STORE_PY((void*)obj, recon, id_hash) < 0) {
        Py_DECREF(recon);
        Py_DECREF(reduce_res);
        return NULL;
    }

    if (state && state != Py_None) {
        PyObject* setstate = PyObject_GetAttr(recon, module_state.str_setstate);
        if (setstate) {
            PyObject* cstate = deepcopy_py(state, memo_dict, keep_list_ptr);
            if (!cstate) {
                Py_DECREF(setstate);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            PyObject* r = PyObject_CallOneArg(setstate, cstate);
            Py_DECREF(cstate);
            Py_DECREF(setstate);
            if (!r) {
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            Py_DECREF(r);
        } else {
            PyErr_Clear();
            PyObject* dict_state = NULL;
            PyObject* slot_state = NULL;
            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slot_state = PyTuple_GET_ITEM(state, 1);
                if (slot_state && slot_state != Py_None) {
                    PyObject* it = PyObject_GetIter(slot_state);
                    if (!it) {
                        Py_DECREF(recon);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                    PyObject* key;
                    while ((key = PyIter_Next(it)) != NULL) {
                        PyObject* value = PyObject_GetItem(slot_state, key);
                        if (!value) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            Py_DECREF(recon);
                            Py_DECREF(reduce_res);
                            return NULL;
                        }
                        PyObject* cvalue = deepcopy_py(value, memo_dict, keep_list_ptr);
                        Py_DECREF(value);
                        if (!cvalue) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            Py_DECREF(recon);
                            Py_DECREF(reduce_res);
                            return NULL;
                        }
                        if (PyObject_SetAttr(recon, key, cvalue) < 0) {
                            Py_DECREF(cvalue);
                            Py_DECREF(key);
                            Py_DECREF(it);
                            Py_DECREF(recon);
                            Py_DECREF(reduce_res);
                            return NULL;
                        }
                        Py_DECREF(cvalue);
                        Py_DECREF(key);
                    }
                    Py_DECREF(it);
                    if (PyErr_Occurred()) {
                        Py_DECREF(recon);
                        Py_DECREF(reduce_res);
                        return NULL;
                    }
                }
            } else {
                dict_state = state;
            }
            if (dict_state && dict_state != Py_None) {
                PyObject* cdict_state = deepcopy_py(dict_state, memo_dict, keep_list_ptr);
                if (!cdict_state) {
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* obj_dict = PyObject_GetAttr(recon, module_state.str_dict);
                if (!obj_dict) {
                    Py_DECREF(cdict_state);
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
                Py_DECREF(obj_dict);
                if (!update) {
                    Py_DECREF(cdict_state);
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                PyObject* r = PyObject_CallOneArg(update, cdict_state);
                Py_DECREF(update);
                Py_DECREF(cdict_state);
                if (!r) {
                    Py_DECREF(recon);
                    Py_DECREF(reduce_res);
                    return NULL;
                }
                Py_DECREF(r);
            }
        }
    }

    if (listit && listit != Py_None) {
        PyObject* append = PyObject_GetAttr(recon, module_state.str_append);
        if (!append) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
        PyObject* it = PyObject_GetIter(listit);
        if (!it) {
            Py_DECREF(append);
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
        PyObject* item;
        while ((item = PyIter_Next(it)) != NULL) {
            PyObject* citem = deepcopy_py(item, memo_dict, keep_list_ptr);
            Py_DECREF(item);
            if (!citem) {
                Py_DECREF(it);
                Py_DECREF(append);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            PyObject* r = PyObject_CallOneArg(append, citem);
            Py_DECREF(citem);
            if (!r) {
                Py_DECREF(it);
                Py_DECREF(append);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            Py_DECREF(r);
        }
        Py_DECREF(it);
        Py_DECREF(append);
        if (PyErr_Occurred()) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
    }

    if (dictit && dictit != Py_None) {
        PyObject* it = PyObject_GetIter(dictit);
        if (!it) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
        PyObject* pair;
        while ((pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                PyErr_SetString(PyExc_TypeError, "dictiter must yield (key, value) pairs");
                return NULL;
            }
            PyObject* k = PyTuple_GET_ITEM(pair, 0);
            PyObject* v = PyTuple_GET_ITEM(pair, 1);
            PyObject* ck = deepcopy_py(k, memo_dict, keep_list_ptr);
            if (!ck) {
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            PyObject* cv = deepcopy_py(v, memo_dict, keep_list_ptr);
            if (!cv) {
                Py_DECREF(ck);
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            if (PyObject_SetItem(recon, ck, cv) < 0) {
                Py_DECREF(ck);
                Py_DECREF(cv);
                Py_DECREF(pair);
                Py_DECREF(it);
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
            Py_DECREF(ck);
            Py_DECREF(cv);
            Py_DECREF(pair);
        }
        Py_DECREF(it);
        if (PyErr_Occurred()) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
    }

    if (recon != obj) {
        if (*keep_list_ptr == NULL) {
            if (ensure_keep_list_for_pymemo(memo_dict, keep_list_ptr) < 0) {
                Py_DECREF(recon);
                Py_DECREF(reduce_res);
                return NULL;
            }
        }
        if (PyList_Append(*keep_list_ptr, obj) < 0) {
            Py_DECREF(recon);
            Py_DECREF(reduce_res);
            return NULL;
        }
    }
    Py_DECREF(reduce_res);
    return recon;
}

/* ------------------------------ Public API ---------------------------------
 */

PyObject* py_deepcopy(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    PyObject* obj = NULL;
    PyObject* memo_arg = Py_None;

    if (!kwnames || PyTuple_GET_SIZE(kwnames) == 0) {
        if (UNLIKELY(nargs < 1)) {
            PyErr_Format(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
            return NULL;
        }
        if (UNLIKELY(nargs > 2)) {
            PyErr_Format(
                PyExc_TypeError,
                "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                nargs
            );
            return NULL;
        }
        obj = args[0];
        memo_arg = (nargs == 2) ? args[1] : Py_None;
        goto have_args;
    }

    const Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    if (kwcount == 1) {
        PyObject* kw0 = PyTuple_GET_ITEM(kwnames, 0);
        const int is_memo =
            PyUnicode_Check(kw0) && PyUnicode_CompareWithASCIIString(kw0, "memo") == 0;

        if (is_memo) {
            if (UNLIKELY(nargs < 1)) {
                PyErr_Format(
                    PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'"
                );
                return NULL;
            }
            if (UNLIKELY(nargs > 2)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                    nargs
                );
                return NULL;
            }
            if (UNLIKELY(nargs == 2)) {
                PyErr_SetString(
                    PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'"
                );
                return NULL;
            }
            obj = args[0];
            memo_arg = args[nargs + 0];
            goto have_args;
        }
    }

    {
        Py_ssize_t i;
        int seen_memo_kw = 0;

        if (UNLIKELY(nargs > 2)) {
            PyErr_Format(
                PyExc_TypeError,
                "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                nargs
            );
            return NULL;
        }

        if (nargs >= 1)
            obj = args[0];
        if (nargs == 2)
            memo_arg = args[1];

        const Py_ssize_t kwc = PyTuple_GET_SIZE(kwnames);
        for (i = 0; i < kwc; i++) {
            PyObject* name = PyTuple_GET_ITEM(kwnames, i);
            PyObject* val = args[nargs + i];

            if (!(PyUnicode_Check(name))) {
                PyErr_SetString(PyExc_TypeError, "deepcopy() keywords must be strings");
                return NULL;
            }

            if (PyUnicode_CompareWithASCIIString(name, "x") == 0) {
                if (UNLIKELY(obj != NULL)) {
                    PyErr_SetString(
                        PyExc_TypeError, "deepcopy() got multiple values for argument 'x'"
                    );
                    return NULL;
                }
                obj = val;
                continue;
            }

            if (PyUnicode_CompareWithASCIIString(name, "memo") == 0) {
                if (UNLIKELY(seen_memo_kw || nargs == 2)) {
                    PyErr_SetString(
                        PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'"
                    );
                    return NULL;
                }
                memo_arg = val;
                seen_memo_kw = 1;
                continue;
            }

            PyErr_Format(
                PyExc_TypeError, "deepcopy() got an unexpected keyword argument '%U'", name
            );
            return NULL;
        }

        if (UNLIKELY(obj == NULL)) {
            PyErr_Format(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
            return NULL;
        }
    }

have_args:
    if (memo_arg == Py_None) {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            return Py_NewRef(obj);
        }
        PyObject* memo_local = get_thread_local_memo();
        if (!memo_local)
            return NULL;
        MemoObject* mo = (MemoObject*)memo_local;

        /* Lagged-keep policy: observe keep size at entry. */
        PyObject* result = deepcopy_c(obj, mo);

        if ((int)Py_REFCNT(memo_local) == 1) {
            keepvector_clear(&mo->keep);
            keepvector_shrink_if_large(&mo->keep);
            memo_table_reset(&mo->table);
        }
        /* memo_local is owned by TLS; no DECREF */
        return result;
    }

    PyObject* result = NULL;

    if (Py_TYPE(memo_arg) == &Memo_Type) {
        MemoObject* mo = (MemoObject*)memo_arg;
        Py_INCREF(memo_arg);
        result = deepcopy_c(obj, mo);
        Py_DECREF(memo_arg);
        return result;
    }

    else {
        /* deepcopy_py handles version-specific ordering of immutable check vs memo lookup */
        Py_INCREF(memo_arg);
        PyObject* memo_dict = memo_arg;
        PyObject* keep_list = NULL; /* lazily created on first append */

        result = deepcopy_py(obj, memo_dict, &keep_list);

        Py_XDECREF(keep_list);
        Py_DECREF(memo_dict);
        return result;
    }
}

/* -------------------------------- Utilities -------------------------------- */

static ALWAYS_INLINE PyObject* build_list_by_calling_noargs(PyObject* callable, Py_ssize_t n) {
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 0");
        return NULL;
    }
    PyObject* out = PyList_New(n);
    if (!out)
        return NULL;

    vectorcallfunc vc = PyVectorcall_Function(callable);
    if (LIKELY(vc)) {
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = vc(callable, NULL, PyVectorcall_NARGS(0), NULL);
            if (!item) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, item);
        }
    } else {
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = PyObject_CallNoArgs(callable);
            if (!item) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, item);
        }
    }
    return out;
}

PyObject* py_replicate(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;

    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "replicate(obj, n, /, *, compile_after=20)");
        return NULL;
    }

    PyObject* obj = args[0];

    long n_long = PyLong_AsLong(args[1]);
    if (n_long == -1 && PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 0");
        return NULL;
    }
    Py_ssize_t n = (Py_ssize_t)n_long;

    int duper_available = (module_state.create_precompiler_reconstructor != NULL);

    int compile_after = 20;
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        if (kwcount > 1) {
            PyErr_SetString(PyExc_TypeError, "replicate accepts only 'compile_after' keyword");
            return NULL;
        }
        if (kwcount == 1) {
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, 0);
            int is_compile_after = PyUnicode_Check(kwname) &&
                (PyUnicode_CompareWithASCIIString(kwname, "compile_after") == 0);
            if (!is_compile_after) {
                PyErr_SetString(
                    PyExc_TypeError, "unknown keyword; only 'compile_after' is supported"
                );
                return NULL;
            }
            if (!duper_available) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "replicate(): 'compile_after' requires duper.snapshots; it is not available"
                );
                return NULL;
            }
            PyObject* kwval = args[nargs + 0];
            long ca = PyLong_AsLong(kwval);
            if (ca == -1 && PyErr_Occurred())
                return NULL;
            if (ca < 0) {
                PyErr_SetString(PyExc_ValueError, "compile_after must be >= 0");
                return NULL;
            }
            compile_after = (int)ca;
        }
    }

    if (n == 0)
        return PyList_New(0);

    {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            PyObject* out = PyList_New(n);
            if (!out)
                return NULL;
            for (Py_ssize_t i = 0; i < n; i++) {
                {
                    PyObject* copy_i = Py_NewRef(obj);
                    PyList_SET_ITEM(out, i, copy_i);
                }
            }
            return out;
        }
    }

    {
        PinObject* pin = _duper_lookup_pin_for_object(obj);
        if (pin) {
            PyObject* factory = pin->factory;
            if (UNLIKELY(!factory || !PyCallable_Check(factory))) {
                PyErr_SetString(PyExc_RuntimeError, "pinned object has no valid factory");
                return NULL;
            }
            PyObject* out = build_list_by_calling_noargs(factory, n);
            if (out)
                pin->hits += (uint64_t)n;
            return out;
        }
    }

    if (!duper_available || n <= (Py_ssize_t)compile_after) {
        PyObject* out = PyList_New(n);
        if (!out)
            return NULL;

        PyObject* memo_local = get_thread_local_memo();
        if (!memo_local)
            return NULL;
        MemoObject* mo = (MemoObject*)memo_local;

        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* copy_i = deepcopy_c(obj, mo);

            if ((int)Py_REFCNT(memo_local) == 1) {
                keepvector_clear(&mo->keep);
                keepvector_shrink_if_large(&mo->keep);
                memo_table_reset(&mo->table);
            } else {
                PyObject* memo_local = get_thread_local_memo();
                if (!memo_local)
                    return NULL;
                mo = (MemoObject*)memo_local;
            }

            if (!copy_i) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, copy_i);
        }
        return out;
    }

    {
        PyObject* cpr = module_state.create_precompiler_reconstructor;
        if (UNLIKELY(!cpr || !PyCallable_Check(cpr))) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "duper.snapshots.create_precompiler_reconstructor is not callable"
            );
            return NULL;
        }

        PyObject* reconstructor = PyObject_CallOneArg(cpr, obj);
        if (!reconstructor)
            return NULL;

        if (UNLIKELY(!PyCallable_Check(reconstructor))) {
            Py_DECREF(reconstructor);
            PyErr_SetString(PyExc_TypeError, "reconstructor must be callable (FunctionType)");
            return NULL;
        }

        PyObject* out = build_list_by_calling_noargs(reconstructor, n);
        Py_DECREF(reconstructor);
        return out;
    }
}

PyObject* py_repeatcall(
    PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
) {
    (void)self;
    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "repeatcall(function, size, /)");
        return NULL;
    }
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        PyErr_SetString(PyExc_TypeError, "repeatcall() takes no keyword arguments");
        return NULL;
    }

    PyObject* func = args[0];
    if (UNLIKELY(!PyCallable_Check(func))) {
        PyErr_SetString(PyExc_TypeError, "function must be callable");
        return NULL;
    }

    long n_long = PyLong_AsLong(args[1]);
    if (n_long == -1 && PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 0");
        return NULL;
    }

    Py_ssize_t n = (Py_ssize_t)n_long;
    return build_list_by_calling_noargs(func, n);
}

/* ----------------------------- Shallow reconstruct helper ------------------ */

static PyObject* reconstruct_state(
    PyObject* new_obj, PyObject* state, PyObject* listiter, PyObject* dictiter
) {
    if (UNLIKELY(new_obj == NULL)) {
        PyErr_SetString(PyExc_SystemError, "reconstruct_state: new_obj is NULL");
        return NULL;
    }
    if (!state)
        state = Py_None;
    if (!listiter)
        listiter = Py_None;
    if (!dictiter)
        dictiter = Py_None;

    if (state != Py_None) {
        PyObject* setstate = PyObject_GetAttr(new_obj, module_state.str_setstate);
        if (setstate) {
            PyObject* r = PyObject_CallOneArg(setstate, state);
            Py_DECREF(setstate);
            if (!r)
                return NULL;
            Py_DECREF(r);
        } else {
            PyErr_Clear();
            PyObject* dict_state = NULL;
            PyObject* slot_state = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slot_state = PyTuple_GET_ITEM(state, 1);

                if (slot_state && slot_state != Py_None) {
                    PyObject* it = PyObject_GetIter(slot_state);
                    if (!it)
                        return NULL;
                    PyObject* key;
                    while ((key = PyIter_Next(it)) != NULL) {
                        PyObject* value = PyObject_GetItem(slot_state, key);
                        if (!value) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            return NULL;
                        }
                        if (PyObject_SetAttr(new_obj, key, value) < 0) {
                            Py_DECREF(value);
                            Py_DECREF(key);
                            Py_DECREF(it);
                            return NULL;
                        }
                        Py_DECREF(value);
                        Py_DECREF(key);
                    }
                    Py_DECREF(it);
                    if (PyErr_Occurred())
                        return NULL;
                }
            } else {
                dict_state = state;  // treat as mapping-like
            }

            if (dict_state && dict_state != Py_None) {
                PyObject* obj_dict = PyObject_GetAttr(new_obj, module_state.str_dict);
                if (!obj_dict)
                    return NULL;
                PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
                Py_DECREF(obj_dict);
                if (!update)
                    return NULL;
                PyObject* r = PyObject_CallOneArg(update, dict_state);
                Py_DECREF(update);
                if (!r)
                    return NULL;
                Py_DECREF(r);
            }
        }
    }

    if (listiter != Py_None) {
        PyObject* append = PyObject_GetAttr(new_obj, module_state.str_append);
        if (!append)
            return NULL;
        PyObject* it = PyObject_GetIter(listiter);
        if (!it) {
            Py_DECREF(append);
            return NULL;
        }
        PyObject* item;
        while ((item = PyIter_Next(it)) != NULL) {
            PyObject* r = PyObject_CallOneArg(append, item);
            Py_DECREF(item);
            if (!r) {
                Py_DECREF(it);
                Py_DECREF(append);
                return NULL;
            }
            Py_DECREF(r);
        }
        Py_DECREF(it);
        Py_DECREF(append);
        if (PyErr_Occurred())
            return NULL;
    }

    if (dictiter != Py_None) {
        PyObject* it = PyObject_GetIter(dictiter);
        if (!it)
            return NULL;
        PyObject* pair;
        while ((pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
                Py_DECREF(pair);
                Py_DECREF(it);
                PyErr_SetString(PyExc_TypeError, "dictiter must yield (key, value) pairs");
                return NULL;
            }
            PyObject* k = PyTuple_GET_ITEM(pair, 0);
            PyObject* v = PyTuple_GET_ITEM(pair, 1);
            Py_INCREF(k);
            Py_INCREF(v);
            if (PyObject_SetItem(new_obj, k, v) < 0) {
                Py_DECREF(k);
                Py_DECREF(v);
                Py_DECREF(pair);
                Py_DECREF(it);
                return NULL;
            }
            Py_DECREF(k);
            Py_DECREF(v);
            Py_DECREF(pair);
        }
        Py_DECREF(it);
        if (PyErr_Occurred())
            return NULL;
    }

    return Py_NewRef(new_obj);
}

/* -------------------------------- copy() ---------------------------------- */

static ALWAYS_INLINE int is_empty_initializable(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyList_Type)
        return Py_SIZE(obj) == 0;
    if (tp == &PyTuple_Type)
        return Py_SIZE(obj) == 0;
    if (tp == &PyDict_Type)
        return PyDict_Size(obj) == 0;
    if (tp == &PySet_Type)
        return PySet_Size(obj) == 0;
    if (tp == &PyFrozenSet_Type)
        return PyObject_Size(obj) == 0;
    if (tp == &PyByteArray_Type)
        return PyByteArray_Size(obj) == 0;
    return 0;
}

static PyObject* make_empty_same_type(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyList_Type)
        return PyList_New(0);
    if (tp == &PyTuple_Type)
        return PyTuple_New(0);
    if (tp == &PyDict_Type)
        return PyDict_New();
    if (tp == &PySet_Type)
        return PySet_New(NULL);
    if (tp == &PyFrozenSet_Type)
        return PyFrozenSet_New(NULL);
    if (tp == &PyByteArray_Type)
        return PyByteArray_FromStringAndSize(NULL, 0);
    Py_RETURN_NONE;  // shouldn't happen; caller guards
}

static PyObject* try_stdlib_mutable_copy(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyDict_Type || tp == &PySet_Type || tp == &PyList_Type || tp == &PyByteArray_Type) {
        PyObject* method = PyObject_GetAttrString(obj, "copy");
        if (!method) {
            PyErr_Clear();
        } else {
            PyObject* out = PyObject_CallNoArgs(method);
            Py_DECREF(method);
            if (out)
                return out;
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

PyObject* py_copy(PyObject* self, PyObject* obj) {
    (void)self;

    {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            return Py_NewRef(obj);
        }
    }

    if (PySlice_Check(obj))
        return Py_NewRef(obj);
    if (PyFrozenSet_CheckExact(obj))
        return Py_NewRef(obj);

    if (PyType_IsSubtype(Py_TYPE(obj), &PyType_Type))
        return Py_NewRef(obj);

    if (is_empty_initializable(obj)) {
        PyObject* fresh = make_empty_same_type(obj);
        if (fresh == Py_None)
            Py_DECREF(fresh);
        else
            return fresh;
    }

    {
        PyObject* maybe = try_stdlib_mutable_copy(obj);
        if (!maybe)
            return NULL;
        if (maybe != Py_None)
            return maybe;
        Py_DECREF(maybe);
    }

    {
        PyObject* cp = PyObject_GetAttrString(obj, "__copy__");
        if (cp) {
            PyObject* out = PyObject_CallNoArgs(cp);
            Py_DECREF(cp);
            return out;
        }
        PyErr_Clear();
    }

    PyTypeObject* obj_type = Py_TYPE(obj);
    PyObject* reduce_result = try_reduce_via_registry(obj, obj_type);
    if (!reduce_result) {
        if (PyErr_Occurred())
            return NULL;
        reduce_result = call_reduce_method_preferring_ex(obj);
        if (!reduce_result)
            return NULL;
    }

    PyObject *constructor = NULL, *args = NULL, *state = NULL, *listiter = NULL, *dictiter = NULL;
    int unpack_result = unpack_reduce_result_tuple(
        reduce_result, &constructor, &args, &state, &listiter, &dictiter
    );
    if (unpack_result < 0) {
        Py_DECREF(reduce_result);
        return NULL;
    }
    if (unpack_result == 1) {
        Py_DECREF(reduce_result);
        return Py_NewRef(obj);
    }

    PyObject* out = NULL;
    if (PyTuple_GET_SIZE(args) == 0) {
        out = PyObject_CallNoArgs(constructor);
    } else {
        out = PyObject_CallObject(constructor, args);
    }
    if (!out) {
        Py_DECREF(reduce_result);
        return NULL;
    }

    if ((state && state != Py_None) || (listiter && listiter != Py_None) ||
        (dictiter && dictiter != Py_None)) {
        PyObject* applied = reconstruct_state(
            out,
            state ? state : Py_None,
            listiter ? listiter : Py_None,
            dictiter ? dictiter : Py_None
        );
        if (!applied) {
            Py_DECREF(out);
            Py_DECREF(reduce_result);
            return NULL;
        }
        Py_DECREF(applied);
    }

    Py_DECREF(reduce_result);
    return out;
}

/* -------------------------------- replace() --------------------------------
 */

#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
PyObject* py_replace(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;
    if (UNLIKELY(nargs == 0)) {
        PyErr_SetString(PyExc_TypeError, "replace() missing 1 required positional argument: 'obj'");
        return NULL;
    }
    if (UNLIKELY(nargs > 1)) {
        PyErr_Format(
            PyExc_TypeError, "replace() takes 1 positional argument but %zd were given", nargs
        );
        return NULL;
    }
    PyObject* obj = args[0];
    PyObject* cls = (PyObject*)Py_TYPE(obj);

    PyObject* func = PyObject_GetAttrString(cls, "__replace__");
    if (!func) {
        PyErr_Clear();
        PyErr_Format(
            PyExc_TypeError, "replace() does not support %.200s objects", Py_TYPE(obj)->tp_name
        );
        return NULL;
    }
    if (!PyCallable_Check(func)) {
        Py_DECREF(func);
        PyErr_SetString(PyExc_TypeError, "__replace__ is not callable");
        return NULL;
    }

    PyObject* posargs = PyTuple_New(1);
    if (!posargs) {
        Py_DECREF(func);
        return NULL;
    }
    Py_INCREF(obj);
    PyTuple_SET_ITEM(posargs, 0, obj);

    PyObject* kwargs = NULL;
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        kwargs = PyDict_New();
        if (!kwargs) {
            Py_DECREF(func);
            Py_DECREF(posargs);
            return NULL;
        }
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            PyObject* key = PyTuple_GET_ITEM(kwnames, i);
            PyObject* val = args[nargs + i];
            if (PyDict_SetItem(kwargs, key, val) < 0) {
                Py_DECREF(func);
                Py_DECREF(posargs);
                Py_DECREF(kwargs);
                return NULL;
            }
        }
    }

    PyObject* out = PyObject_Call(func, posargs, kwargs);
    Py_DECREF(func);
    Py_DECREF(posargs);
    Py_XDECREF(kwargs);
    return out;
}
#endif

/* ----------------------------- Module boilerplate --------------------------
 */

extern PyObject* py_copy(PyObject* self, PyObject* obj);
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
extern PyObject* py_replace(
    PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
);
#endif

static void cleanup_on_init_failure(void) {
    Py_XDECREF(module_state.str_reduce_ex);
    Py_XDECREF(module_state.str_reduce);
    Py_XDECREF(module_state.str_deepcopy);
    Py_XDECREF(module_state.str_setstate);
    Py_XDECREF(module_state.str_dict);
    Py_XDECREF(module_state.str_append);
    Py_XDECREF(module_state.str_update);
    Py_XDECREF(module_state.str_new);
    Py_XDECREF(module_state.str_get);

    Py_XDECREF(module_state.BuiltinFunctionType);
    Py_XDECREF(module_state.MethodType);
    Py_XDECREF(module_state.CodeType);
    Py_XDECREF(module_state.range_type);
    Py_XDECREF(module_state.property_type);
    Py_XDECREF(module_state.weakref_ref_type);
    Py_XDECREF(module_state.re_Pattern_type);
    Py_XDECREF(module_state.Decimal_type);
    Py_XDECREF(module_state.Fraction_type);

    Py_XDECREF(module_state.copyreg_dispatch);
    Py_XDECREF(module_state.copy_Error);
    Py_XDECREF(module_state.copyreg_newobj);
    Py_XDECREF(module_state.copyreg_newobj_ex);
    Py_XDECREF(module_state.create_precompiler_reconstructor);
    Py_XDECREF(module_state.sentinel);

    if (PyThread_tss_is_created(&module_state.memo_tss)) {
        PyThread_tss_delete(&module_state.memo_tss);
    }
    if (PyThread_tss_is_created(&module_state.recdepth_tss)) {
        PyThread_tss_delete(&module_state.recdepth_tss);
    }
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    if (PyThread_tss_is_created(&g_dictiter_tss)) {
        PyThread_tss_delete(&g_dictiter_tss);
    }
    if (g_dict_watcher_registered) {
        PyDict_ClearWatcher(g_dict_watcher_id);
        g_dict_watcher_registered = 0;
        g_dict_watcher_id = -1;
    }
#endif
}

#define LOAD_TYPE(source_module, type_name, target_field)                              \
    do {                                                                               \
        PyObject* _loaded_type = PyObject_GetAttrString((source_module), (type_name)); \
        if (!_loaded_type || !PyType_Check(_loaded_type)) {                            \
            Py_XDECREF(_loaded_type);                                                  \
            PyErr_Format(                                                              \
                PyExc_ImportError,                                                     \
                "copium: %s.%s missing or not a type",                                 \
                #source_module,                                                        \
                (type_name)                                                            \
            );                                                                         \
            cleanup_on_init_failure();                                                 \
            return -1;                                                                 \
        }                                                                              \
        module_state.target_field = (PyTypeObject*)_loaded_type;                       \
    } while (0)

int _copium_copying_init(PyObject* module) {
    // Intern strings
    module_state.str_reduce_ex = PyUnicode_InternFromString("__reduce_ex__");
    module_state.str_reduce = PyUnicode_InternFromString("__reduce__");
    module_state.str_deepcopy = PyUnicode_InternFromString("__deepcopy__");
    module_state.str_setstate = PyUnicode_InternFromString("__setstate__");
    module_state.str_dict = PyUnicode_InternFromString("__dict__");
    module_state.str_append = PyUnicode_InternFromString("append");
    module_state.str_update = PyUnicode_InternFromString("update");
    module_state.str_new = PyUnicode_InternFromString("__new__");
    module_state.str_get = PyUnicode_InternFromString("get");

    if (!module_state.str_reduce_ex || !module_state.str_reduce || !module_state.str_deepcopy ||
        !module_state.str_setstate || !module_state.str_dict || !module_state.str_append ||
        !module_state.str_update || !module_state.str_new || !module_state.str_get) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to intern required names");
        cleanup_on_init_failure();
        return -1;
    }

    // Load stdlib modules
    PyObject* mod_types = PyImport_ImportModule("types");
    PyObject* mod_builtins = PyImport_ImportModule("builtins");
    PyObject* mod_weakref = PyImport_ImportModule("weakref");
    PyObject* mod_copyreg = PyImport_ImportModule("copyreg");
    PyObject* mod_re = PyImport_ImportModule("re");
    PyObject* mod_decimal = PyImport_ImportModule("decimal");
    PyObject* mod_fractions = PyImport_ImportModule("fractions");

    if (!mod_types || !mod_builtins || !mod_weakref || !mod_copyreg || !mod_re || !mod_decimal ||
        !mod_fractions) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to import required stdlib modules");
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }

    // Cache types
    LOAD_TYPE(mod_types, "BuiltinFunctionType", BuiltinFunctionType);
    LOAD_TYPE(mod_types, "CodeType", CodeType);
    LOAD_TYPE(mod_types, "MethodType", MethodType);
    LOAD_TYPE(mod_builtins, "property", property_type);
    LOAD_TYPE(mod_builtins, "range", range_type);
    LOAD_TYPE(mod_weakref, "ref", weakref_ref_type);
    LOAD_TYPE(mod_re, "Pattern", re_Pattern_type);
    LOAD_TYPE(mod_decimal, "Decimal", Decimal_type);
    LOAD_TYPE(mod_fractions, "Fraction", Fraction_type);

    // copyreg dispatch and copy.Error
    module_state.copyreg_dispatch = PyObject_GetAttrString(mod_copyreg, "dispatch_table");
    if (!module_state.copyreg_dispatch || !PyDict_Check(module_state.copyreg_dispatch)) {
        PyErr_SetString(PyExc_ImportError, "copium: copyreg.dispatch_table missing or not a dict");
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }

    // Cache copyreg special constructors; if absent, use unique sentinels
    module_state.copyreg_newobj = PyObject_GetAttrString(mod_copyreg, "__newobj__");
    if (!module_state.copyreg_newobj) {
        PyErr_Clear();
        module_state.copyreg_newobj = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg_newobj) {
            Py_XDECREF(mod_types);
            Py_XDECREF(mod_builtins);
            Py_XDECREF(mod_weakref);
            Py_XDECREF(mod_copyreg);
            Py_XDECREF(mod_re);
            Py_XDECREF(mod_decimal);
            Py_XDECREF(mod_fractions);
            cleanup_on_init_failure();
            return -1;
        }
    }
    module_state.copyreg_newobj_ex = PyObject_GetAttrString(mod_copyreg, "__newobj_ex__");
    if (!module_state.copyreg_newobj_ex) {
        PyErr_Clear();
        module_state.copyreg_newobj_ex = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg_newobj_ex) {
            Py_XDECREF(mod_types);
            Py_XDECREF(mod_builtins);
            Py_XDECREF(mod_weakref);
            Py_XDECREF(mod_copyreg);
            Py_XDECREF(mod_re);
            Py_XDECREF(mod_decimal);
            Py_XDECREF(mod_fractions);
            cleanup_on_init_failure();
            return -1;
        }
    }

    PyObject* mod_copy = PyImport_ImportModule("copy");
    if (!mod_copy) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to import copy module");
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }
    module_state.copy_Error = PyObject_GetAttrString(mod_copy, "Error");
    if (!module_state.copy_Error || !PyExceptionClass_Check(module_state.copy_Error)) {
        PyErr_SetString(PyExc_ImportError, "copium: copy.Error missing or not an exception");
        Py_XDECREF(mod_copy);
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }
    Py_DECREF(mod_copy);

    module_state.sentinel = PyList_New(0);
    if (!module_state.sentinel) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create sentinel list");
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }

    // Create thread-local recursion depth guard
    if (PyThread_tss_create(&module_state.recdepth_tss) != 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create recursion TSS");
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }

    // Create thread-local memo
    if (PyThread_tss_create(&module_state.memo_tss) != 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create memo TSS");
        Py_XDECREF(mod_types);
        Py_XDECREF(mod_builtins);
        Py_XDECREF(mod_weakref);
        Py_XDECREF(mod_copyreg);
        Py_XDECREF(mod_re);
        Py_XDECREF(mod_decimal);
        Py_XDECREF(mod_fractions);
        cleanup_on_init_failure();
        return -1;
    }

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    // Create TLS for dict-iteration watcher stack
    if (PyThread_tss_create(&g_dictiter_tss) != 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create dict-iteration TSS");
        cleanup_on_init_failure();
        return -1;
    }
    g_dict_watcher_id = PyDict_AddWatcher(_copium_dict_watcher_cb);
    if (g_dict_watcher_id < 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to register dict watcher");
        cleanup_on_init_failure();
        return -1;
    }
    g_dict_watcher_registered = 1;
#endif
    Py_DECREF(mod_types);
    Py_DECREF(mod_builtins);
    Py_DECREF(mod_weakref);
    Py_DECREF(mod_copyreg);
    Py_DECREF(mod_re);
    Py_DECREF(mod_decimal);
    Py_DECREF(mod_fractions);

    // Ready memo + keep proxy types
    if (memo_ready_types() < 0) {
        cleanup_on_init_failure();
        return -1;
    }

    // Try duper.snapshots: if available, cache reconstructor factory and expose pin API/types.
    {
        PyObject* mod_snapshots = PyImport_ImportModule("duper.snapshots");
        if (!mod_snapshots) {
            PyErr_Clear();
            module_state.create_precompiler_reconstructor = NULL;
        } else {
            module_state.create_precompiler_reconstructor =
                PyObject_GetAttrString(mod_snapshots, "create_precompiler_reconstructor");
            if (!module_state.create_precompiler_reconstructor) {
                PyErr_Clear();
            }

            if (_duper_pinning_add_types(module) < 0) {
                Py_DECREF(mod_snapshots);
                cleanup_on_init_failure();
                return -1;
            }
            Py_DECREF(mod_snapshots);
        }
    }

    if (PyObject_SetAttrString(module, "Error", module_state.copy_Error) < 0) {
        cleanup_on_init_failure();
        return -1;
    }
    return 0;
}

int _copium_copying_duper_available(void) {
    return module_state.create_precompiler_reconstructor != NULL;
}