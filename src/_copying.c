/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * copium — deep copy engine (lazy memo, type-once dispatch, immortal-aware fast paths)
 *
 * Public API:
 *   copy(x) -> any
 *   deepcopy(x, memo=None) -> any
 *   replicate(obj, n, /, *, compile_after=20) -> list
 *   repeatcall(function, size, /) -> list
 *   replace(obj, **kwargs)  [3.13+]
 */

#define PY_VERSION_3_11_HEX 0x030B0000
#define PY_VERSION_3_12_HEX 0x030C0000
#define PY_VERSION_3_13_HEX 0x030D0000
#define PY_VERSION_3_14_HEX 0x030E0000

#define PY_SSIZE_T_CLEAN

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#define Py_BUILD_CORE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#include "Python.h"
#include "pycore_object.h"  /* _PyNotImplemented_Type, internals */
// _PyDict_NewPresized / _PyDict_*KnownHas
#if PY_VERSION_HEX < PY_VERSION_3_11_HEX
#  include "dictobject.h"
#else
#  include "pycore_dict.h"
#endif

#include "_memo.h"

/* ---------------- Immortal helper (3.12+) ---------------- */
#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
#  ifndef Py_IsImmortal
     /* _Py_IsImmortal is defined in pycore headers; alias if needed */
#    define Py_IsImmortal(obj) _Py_IsImmortal((PyObject*)(obj))
#  endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define LIKELY(x)   __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define LIKELY(x)   (x)
#  define UNLIKELY(x) (x)
#endif

#define XDECREF_CLEAR(op) do { Py_XDECREF(op); (op) = NULL; } while (0)

/* ---------------- Pin integration surface (provided by _pinning.c) --------- */
typedef struct {
  PyObject_HEAD
  PyObject* snapshot; /* duper.snapshots.Snapshot */
  PyObject* factory;  /* callable reconstruct() */
  uint64_t hits;      /* native counter */
} PinObject;

extern PinObject* _duper_lookup_pin_for_object(PyObject* obj);
extern int        _duper_pinning_add_types(PyObject* module);
extern PyObject*  py_pin(PyObject* self, PyObject* obj);
extern PyObject*  py_unpin(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames);
extern PyObject*  py_pinned(PyObject* self, PyObject* obj);
extern PyObject*  py_clear_pins(PyObject* self, PyObject* noargs);
extern PyObject*  py_get_pins(PyObject* self, PyObject* noargs);

/* ---------------- Memo integration surface (provided by _memo.c) ----------- */
typedef struct _MemoObject MemoObject;

extern PyTypeObject Memo_Type;
extern PyObject*    Memo_New(void);
extern PyObject*    memo_lookup_obj(PyObject* memo, void* key);
extern int          memo_store_obj(PyObject* memo, void* key, PyObject* value);
extern PyObject*    memo_lookup_obj_h(PyObject* memo, void* key, Py_ssize_t khash);
extern int          memo_store_obj_h(PyObject* memo, void* key, PyObject* value, Py_ssize_t khash);
extern Py_ssize_t   memo_hash_pointer(void* key);
extern int          memo_table_reset(MemoTable** table_ptr);
extern void         keepvector_shrink_if_large(KeepVector* kv);
extern void         memo_table_clear(MemoTable* table);
extern int          memo_keepalive_ensure(PyObject** memo_ptr, PyObject** keep_proxy_ptr);
extern int          memo_keepalive_append(PyObject** memo_ptr, PyObject** keep_proxy_ptr, PyObject* obj);
extern int          memo_ready_types(void);

/* ---------------- Module state --------------------------------------------- */
typedef struct {
  /* interned strings */
  PyObject* str_reduce_ex;
  PyObject* str_reduce;
  PyObject* str_deepcopy;
  PyObject* str_setstate;
  PyObject* str_dict;
  PyObject* str_append;
  PyObject* str_update;
  PyObject* str_new;

  /* cached types */
  PyTypeObject* BuiltinFunctionType;
  PyTypeObject* MethodType;
  PyTypeObject* CodeType;
  PyTypeObject* range_type;
  PyTypeObject* property_type;
  PyTypeObject* weakref_ref_type;
  PyTypeObject* re_Pattern_type;
  PyTypeObject* Decimal_type;
  PyTypeObject* Fraction_type;

  /* stdlib refs */
  PyObject* copyreg_dispatch;  /* dict */
  PyObject* copy_Error;        /* exception type */
  PyObject* copyreg_newobj;    /* copyreg.__newobj__ or sentinel */
  PyObject* copyreg_newobj_ex; /* copyreg.__newobj_ex__ or sentinel */
  PyObject* create_precompiler_reconstructor; /* duper.snapshots factory or NULL */

  /* recursion guard fallback */
  Py_tss_t recdepth_tss;

  /* TLS memo holder */
  Py_tss_t memo_tss;

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  int dict_watcher_id;
  Py_tss_t watch_tss;
#endif
} ModuleState;

static ModuleState module_state = {0};

/* ---------------- Context: lazy memo + keepalive --------------------------- */
typedef struct {
  PyObject* memo;          /* NULL until first need */
  PyObject* keepalive;     /* NULL until first non-identical copy */
  int implicit_tls_memo;   /* memo==None at API => acquire TLS memo lazily */
} DCtx;

static inline int memo_is_stolen(PyObject *memo) {
  return Py_REFCNT(memo) > 1;
}

static PyObject* get_thread_local_memo(void) {
  void* val = PyThread_tss_get(&module_state.memo_tss);
  if (val == NULL || memo_is_stolen(val)) {
    PyObject* memo = Memo_New();
    if (!memo) return NULL;
    if (PyThread_tss_set(&module_state.memo_tss, (void*)memo) != 0) {
      Py_DECREF(memo);
      return NULL;
    }
    return memo;
  }
  return (PyObject*)val;
}

static inline int ctx_ensure_tls_memo(DCtx* ctx) {
  if (ctx->memo) return 0;
  PyObject* memo = get_thread_local_memo();
  if (!memo) return -1;
  /* Clear for fresh run, reuse allocation */
  MemoObject* mo = (MemoObject*)memo;
  if (mo->table) memo_table_clear(mo->table);
  keepvector_clear(&mo->keep);
  ctx->memo = memo;  /* borrowed from TSS; we keep our own borrowed pointer */
  return 0;
}

/* Ensure any memo exists (TLS if implicit, else allocate a fresh Memo) */
static inline int ctx_ensure_memo(DCtx* ctx) {
  if (ctx->memo) return 0;
  if (ctx->implicit_tls_memo) {
    return ctx_ensure_tls_memo(ctx);
  }
  /* Explicit, on-demand Memo for edge cases (rare: explicit memo==None was not used) */
  PyObject* memo = Memo_New();
  if (!memo) return -1;
  ctx->memo = memo; /* owned reference */
  return 0;
}

static inline PyObject* ctx_memo_lookup_h(DCtx* ctx, void* key, Py_ssize_t khash) {
  if (!ctx->memo) return NULL;
  if (Py_TYPE(ctx->memo) == &Memo_Type) {
    return memo_lookup_obj_h(ctx->memo, key, khash);
  }
  return memo_lookup_obj(ctx->memo, key);
}

static inline int ctx_memo_store_h(DCtx* ctx, void* key, PyObject* value, Py_ssize_t khash) {
  if (ctx_ensure_memo(ctx) < 0) return -1;
  if (Py_TYPE(ctx->memo) == &Memo_Type) {
    return memo_store_obj_h(ctx->memo, key, value, khash);
  }
  return memo_store_obj(ctx->memo, key, value);
}

static inline int ctx_keepalive_append_if_different(DCtx* ctx, PyObject* original, PyObject* copied) {
  if (copied == original) return 0;
  return memo_keepalive_append(&ctx->memo, &ctx->keepalive, original);
}

static inline int ctx_keepalive_append_original(DCtx* ctx, PyObject* original) {
  return memo_keepalive_append(&ctx->memo, &ctx->keepalive, original);
}

/* ---------------- Stack safety (same design, tiny tweaks) ------------------ */
#ifndef COPIUM_STACKCHECK_STRIDE
#  define COPIUM_STACKCHECK_STRIDE 32u
#endif
#ifndef COPIUM_STACK_SAFETY_MARGIN
#  define COPIUM_STACK_SAFETY_MARGIN (256u * 1024u)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static _Thread_local unsigned int _copium_tls_depth = 0;
static _Thread_local int          _copium_tls_stack_inited = 0;
static _Thread_local char*        _copium_tls_stack_low = NULL;
static _Thread_local char*        _copium_tls_stack_high = NULL;
#endif

static inline void _copium_stack_init_if_needed(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  if (_copium_tls_stack_inited) return;
  _copium_tls_stack_inited = 1;

# if defined(__APPLE__)
  pthread_t t = pthread_self();
  size_t sz = pthread_get_stacksize_np(t);
  void* base = pthread_get_stackaddr_np(t);
  char* high = (char*)base;
  char* low  = high - (ptrdiff_t)sz;
  if ((size_t)sz > COPIUM_STACK_SAFETY_MARGIN) low += COPIUM_STACK_SAFETY_MARGIN;
  _copium_tls_stack_low  = low;
  _copium_tls_stack_high = high;
# elif defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* addr = NULL;
    size_t sz = 0;
    if (pthread_attr_getstack(&attr, &addr, &sz) == 0 && addr && sz) {
      char* low  = (char*)addr;
      char* high = low + (ptrdiff_t)sz;
      if (sz > COPIUM_STACK_SAFETY_MARGIN) low += COPIUM_STACK_SAFETY_MARGIN;
      _copium_tls_stack_low  = low;
      _copium_tls_stack_high = high;
    }
    pthread_attr_destroy(&attr);
  }
# else
  _copium_tls_stack_low  = NULL;
  _copium_tls_stack_high = NULL;
# endif

#endif
}

static inline int _copium_recdepth_enter(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  unsigned int d = ++_copium_tls_depth;
  if (UNLIKELY((d & (COPIUM_STACKCHECK_STRIDE - 1u)) == 0u)) {
    _copium_stack_init_if_needed();
    if (_copium_tls_stack_low) {
      char sp_probe;
      char* sp = (char*)&sp_probe;
      if (UNLIKELY(sp <= _copium_tls_stack_low)) {
        _copium_tls_depth--;
        PyErr_SetString(PyExc_RecursionError,
                        "maximum recursion depth exceeded in copium.deepcopy (stack safety cap)");
        return -1;
      }
    } else {
      uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
      uintptr_t next = depth_u + 1;
      int limit = Py_GetRecursionLimit();
      if (limit > 10000) limit = 10000;
      if (UNLIKELY((int)next > limit)) {
        _copium_tls_depth--;
        PyErr_SetString(PyExc_RecursionError,
                        "maximum recursion depth exceeded in copium.deepcopy");
        return -1;
      }
      (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)next);
    }
  }
  return 0;
#else
  uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
  uintptr_t next = depth_u + 1;
  int limit = Py_GetRecursionLimit();
  if (limit > 10000) limit = 10000;
  if (UNLIKELY((int)next > limit)) {
    PyErr_SetString(PyExc_RecursionError,
                    "maximum recursion depth exceeded in copium.deepcopy");
    return -1;
  }
  (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)next);
  return 0;
#endif
}

static inline void _copium_recdepth_leave(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  if (_copium_tls_depth > 0) _copium_tls_depth--;
#else
  uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
  if (depth_u > 0) (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)(depth_u - 1));
#endif
}

/* ---------------- Dict watcher (3.12+) ------------------------------------- */
#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
typedef struct WatchContext {
  PyObject* dict;    /* borrowed */
  int* mutated_flag; /* points to an int owned by the copier frame */
  struct WatchContext* prev;
} WatchContext;

static inline WatchContext* watch_stack_get_current(void) {
  return (WatchContext*)PyThread_tss_get(&module_state.watch_tss);
}
static inline void watch_stack_set_current(WatchContext* ctx) {
  (void)PyThread_tss_set(&module_state.watch_tss, (void*)ctx);
}
static inline void watch_context_push(WatchContext* ctx) {
  ctx->prev = watch_stack_get_current();
  watch_stack_set_current(ctx);
}
static inline void watch_context_pop(void) {
  WatchContext* top = watch_stack_get_current();
  if (top) watch_stack_set_current(top->prev);
}

static int dict_watch_callback(PyDict_WatchEvent event, PyObject* dict, PyObject* key, PyObject* new_value) {
  (void)key; (void)new_value;
  switch (event) {
    case PyDict_EVENT_ADDED:
    case PyDict_EVENT_MODIFIED:
    case PyDict_EVENT_DELETED:
    case PyDict_EVENT_CLONED:
    case PyDict_EVENT_DEALLOCATED: {
      for (WatchContext* ctx = watch_stack_get_current(); ctx; ctx = ctx->prev) {
        if (ctx->dict == dict) {
          *(ctx->mutated_flag) = 1;
          break;
        }
      }
      break;
    }
    default: break;
  }
  return 0;
}
#endif

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
PyAPI_FUNC(int) _PyDict_Next(PyObject* mp, Py_ssize_t* ppos, PyObject** pkey, PyObject** pvalue, Py_hash_t* phash);
#endif

static inline int dict_iterate_with_hash(PyObject* dict_obj,
                                         Py_ssize_t* position_ptr,
                                         PyObject** key_ptr,
                                         PyObject** value_ptr,
                                         Py_hash_t* hash_ptr) {
#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
  return _PyDict_Next(dict_obj, position_ptr, key_ptr, value_ptr, hash_ptr);
#else
  int has_next = PyDict_Next(dict_obj, position_ptr, key_ptr, value_ptr);
  if (!has_next) return 0;
  if (hash_ptr) {
    Py_hash_t computed_hash = PyObject_Hash(*key_ptr);
    if (computed_hash == -1 && PyErr_Occurred()) return -1;
    *hash_ptr = computed_hash;
  }
  return 1;
#endif
}

/* ---------------- Atomic / type helpers ------------------------------------ */

static inline int is_method_type_exact(PyTypeObject* tp) {
  return tp == module_state.MethodType;
}

/* Immortal-aware newref */
static inline PyObject* newref_maybe_immortal(PyObject* obj) {
#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  if (Py_IsImmortal(obj)) {
    /* Do NOT INCREF immortals */
    return obj;
  }
#endif
  Py_INCREF(obj);
  return obj;
}

/* Atomic immutable predicate by *type* (obj only for None) */
static inline int is_atomic_immutable_by_type(PyObject* obj, PyTypeObject* tp) {
  /* Tier 1: very common */
  unsigned long r =
      (obj == Py_None) |
      (tp == &PyLong_Type) |
      (tp == &PyUnicode_Type) |
      (tp == &PyBool_Type) |
      (tp == &PyFloat_Type) |
      (tp == &PyBytes_Type) |
      (tp == &PyComplex_Type);
  if (r) return 1;

  /* Tier 2 */
  r =
      (tp == &PyRange_Type) |
      (tp == &PyFunction_Type) |
      (tp == &PyCFunction_Type) |
      (tp == &PyProperty_Type) |
      (tp == &_PyWeakref_RefType) |
      (tp == &PyCode_Type) |
      (tp == &PyModule_Type) |
      (tp == &_PyNotImplemented_Type) |
      (tp == &PyEllipsis_Type) |
      (tp == module_state.re_Pattern_type) |
      (tp == module_state.Decimal_type) |
      (tp == module_state.Fraction_type);
  if (r) return 1;

  if (tp == &PyType_Type) return 1;

  return PyType_HasFeature(tp, Py_TPFLAGS_TYPE_SUBCLASS);
}

/* ---------------- Small utilities ----------------------------------------- */

#define RETURN_IF_EMPTY(make_cond, make_expr) do { \
  if ((make_cond)) { return (make_expr); }         \
} while (0)

/* ---------------- Forward decls (type-once dispatch) ----------------------- */
static PyObject* dc_dispatch(PyObject* src, PyTypeObject* tp, DCtx* ctx);
static PyObject* dc_dispatch_skip_atomic(PyObject* src, PyTypeObject* tp, DCtx* ctx);

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
static PyObject* dc_dict_with_watcher(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                      void* oid, Py_ssize_t ohash);
#endif
static PyObject* dc_dict_legacy(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                void* oid, Py_ssize_t ohash);

static PyObject* dc_copy_list(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                              void* oid, Py_ssize_t ohash);
static PyObject* dc_copy_tuple(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                               void* oid, Py_ssize_t ohash);
static PyObject* dc_copy_set(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                             void* oid, Py_ssize_t ohash);
static PyObject* dc_copy_frozenset(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                   void* oid, Py_ssize_t ohash);
static PyObject* dc_copy_bytearray(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                   void* oid, Py_ssize_t ohash);

static PyObject* dc_reduce_reconstruct(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                       void* oid, Py_ssize_t ohash);

/* ---------------- reduce helpers / protocol ------------------------------- */

static PyObject* try_reduce_via_registry(PyObject* obj, PyTypeObject* tp) {
  (void)tp;
  PyObject* f = PyDict_GetItemWithError(module_state.copyreg_dispatch, (PyObject*)Py_TYPE(obj));
  if (!f) {
    if (PyErr_Occurred()) return NULL;
    return NULL;
  }
  if (!PyCallable_Check(f)) {
    PyErr_SetString(PyExc_TypeError, "copyreg.dispatch_table value is not callable");
    return NULL;
  }
  return PyObject_CallOneArg(f, obj);
}

static PyObject* call_reduce_method_preferring_ex(PyObject* obj) {
  PyObject* ex = PyObject_GetAttr(obj, module_state.str_reduce_ex);
  if (ex) {
    PyObject* r = PyObject_CallFunction(ex, "i", 4);
    Py_DECREF(ex);
    if (r) return r;
    return NULL;
  }
  PyErr_Clear();
  PyObject* m = PyObject_GetAttr(obj, module_state.str_reduce);
  if (m) {
    PyObject* r = PyObject_CallNoArgs(m);
    Py_DECREF(m);
    return r;
  }
  PyErr_Clear();
  PyErr_SetString((PyObject*)module_state.copy_Error,
                  "un(deep)copyable object (no reduce protocol)");
  return NULL;
}

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
static int get_optional_attr(PyObject* obj, PyObject* name, PyObject** out) {
  *out = PyObject_GetAttr(obj, name);
  if (*out) return 1;
  if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
    PyErr_Clear();
    return 0;
  }
  return -1;
}
#  define PyObject_GetOptionalAttr(obj, name, out) get_optional_attr((obj), (name), (out))
#endif

static int unpack_reduce_result_tuple(PyObject* rr,
                                      PyObject** out_ctor,
                                      PyObject** out_args,
                                      PyObject** out_state,
                                      PyObject** out_listiter,
                                      PyObject** out_dictiter) {
  if (!PyTuple_Check(rr)) {
    if (PyUnicode_Check(rr) || PyBytes_Check(rr)) {
      *out_ctor = *out_args = *out_state = *out_listiter = *out_dictiter = NULL;
      return 1;
    }
    PyErr_SetString(PyExc_TypeError, "__reduce__ must return a tuple or str");
    return -1;
  }
  Py_ssize_t n = PyTuple_GET_SIZE(rr);
  if (n < 2 || n > 5) {
    PyErr_SetString(PyExc_TypeError, "__reduce__ tuple length must be in [2,5]");
    return -1;
  }
  PyObject* ctor = PyTuple_GET_ITEM(rr, 0);
  PyObject* args = PyTuple_GET_ITEM(rr, 1);
  if (!PyCallable_Check(ctor) || !PyTuple_Check(args)) {
    PyErr_SetString(PyExc_TypeError, "__reduce__ first two items must be (callable, tuple)");
    return -1;
  }
  *out_ctor = ctor;
  *out_args = args;
  *out_state     = (n >= 3) ? PyTuple_GET_ITEM(rr, 2) : NULL;
  *out_listiter  = (n >= 4) ? PyTuple_GET_ITEM(rr, 3) : NULL;
  *out_dictiter  = (n == 5) ? PyTuple_GET_ITEM(rr, 4) : NULL;
  return 0;
}

/* ---------------- Deepcopy dispatcher (type-once) -------------------------- */

static PyObject* dc_dispatch(PyObject* src, PyTypeObject* tp, DCtx* ctx) {
  if (LIKELY(is_atomic_immutable_by_type(src, tp))) {
    return newref_maybe_immortal(src);
  }
  return dc_dispatch_skip_atomic(src, tp, ctx);
}

static PyObject* dc_dispatch_skip_atomic(PyObject* src, PyTypeObject* tp, DCtx* ctx) {
  if (UNLIKELY(_copium_recdepth_enter() < 0)) return NULL;

  void* oid = (void*)src;
  Py_ssize_t ohash = memo_hash_pointer(oid);

  /* Memo hit? (lazy memo: if no memo yet, lookup returns NULL quickly) */
  {
    PyObject* hit = ctx_memo_lookup_h(ctx, oid, ohash);
    if (hit) {
      Py_INCREF(hit);
      _copium_recdepth_leave();
      return hit;
    }
    if (PyErr_Occurred()) { _copium_recdepth_leave(); return NULL; }
  }

  /* Fast exact containers */
  if (tp == &PyList_Type) {
    PyObject* out = dc_copy_list(src, tp, ctx, oid, ohash);
    _copium_recdepth_leave();
    return out;
  }
  if (tp == &PyTuple_Type) {
    PyObject* out = dc_copy_tuple(src, tp, ctx, oid, ohash);
    _copium_recdepth_leave();
    return out;
  }
  if (tp == &PyDict_Type) {
    PyObject* out;
#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
    out = dc_dict_with_watcher(src, tp, ctx, oid, ohash);
    if (out == Py_None) { Py_DECREF(out); out = dc_dict_legacy(src, tp, ctx, oid, ohash); }
#else
    out = dc_dict_legacy(src, tp, ctx, oid, ohash);
#endif
    _copium_recdepth_leave();
    return out;
  }
  if (tp == &PySet_Type) {
    PyObject* out = dc_copy_set(src, tp, ctx, oid, ohash);
    _copium_recdepth_leave();
    return out;
  }
  if (tp == &PyFrozenSet_Type) {
    PyObject* out = dc_copy_frozenset(src, tp, ctx, oid, ohash);
    _copium_recdepth_leave();
    return out;
  }
  if (tp == &PyByteArray_Type) {
    PyObject* out = dc_copy_bytearray(src, tp, ctx, oid, ohash);
    _copium_recdepth_leave();
    return out;
  }

  /* bound method: copy self, reuse function */
  if (is_method_type_exact(tp)) {
    PyObject* func = PyMethod_GET_FUNCTION(src);
    PyObject* self = PyMethod_GET_SELF(src);
    if (!func || !self) { _copium_recdepth_leave(); return NULL; }
    Py_INCREF(func);
    Py_INCREF(self);
    PyObject* cself = dc_dispatch(self, Py_TYPE(self), ctx);
    Py_DECREF(self);
    if (!cself) { Py_DECREF(func); _copium_recdepth_leave(); return NULL; }
    PyObject* bm = PyMethod_New(func, cself);
    Py_DECREF(func);
    Py_DECREF(cself);
    if (!bm) { _copium_recdepth_leave(); return NULL; }
    if (ctx_memo_store_h(ctx, oid, bm, ohash) < 0) { Py_DECREF(bm); _copium_recdepth_leave(); return NULL; }
    if (ctx_keepalive_append_original(ctx, src) < 0) { Py_DECREF(bm); _copium_recdepth_leave(); return NULL; }
    _copium_recdepth_leave();
    return bm;
  }

  /* Object-level __deepcopy__? (rare) */
  {
    PyObject* dm = NULL;
    int has_dc = PyObject_GetOptionalAttr(src, module_state.str_deepcopy, &dm);
    if (has_dc < 0) { _copium_recdepth_leave(); return NULL; }
    if (has_dc) {
      /* Some implementations expect a "dict-like" memo; our Memo_Type suffices. */
      if (ctx_ensure_memo(ctx) < 0) { Py_DECREF(dm); _copium_recdepth_leave(); return NULL; }
      PyObject* r = PyObject_CallOneArg(dm, ctx->memo);
      Py_DECREF(dm);
      if (!r) { _copium_recdepth_leave(); return NULL; }
      if (r != src) {
        if (ctx_memo_store_h(ctx, oid, r, ohash) < 0) { Py_DECREF(r); _copium_recdepth_leave(); return NULL; }
        if (ctx_keepalive_append_original(ctx, src) < 0) { Py_DECREF(r); _copium_recdepth_leave(); return NULL; }
      }
      _copium_recdepth_leave();
      return r;
    }
  }

  /* copyreg/ __reduce__ protocol */
  {
    PyObject* out = dc_reduce_reconstruct(src, tp, ctx, oid, ohash);
    _copium_recdepth_leave();
    return out;
  }
}

/* ---------------- Container implementations -------------------------------- */

static PyObject* dc_copy_list(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                              void* oid, Py_ssize_t ohash) {
  (void)tp;
  Py_ssize_t n = PyList_GET_SIZE(src);
  /* allocate completely initialized list (no NULL slots ordered) */
  PyObject* out = PyList_New(n);
  if (!out) return NULL;

  /* Pre-publish into memo for cycles */
  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) { Py_DECREF(out); return NULL; }

  for (Py_ssize_t i = 0; i < n; i++) {
    if (i >= PyList_GET_SIZE(src)) break;
    PyObject* it = PyList_GET_ITEM(src, i); /* borrowed */
    if (LIKELY(is_atomic_immutable_by_type(it, Py_TYPE(it)))) {
      PyObject* ci = newref_maybe_immortal(it);
      if (!ci) { Py_DECREF(out); return NULL; }
      PyList_SET_ITEM(out, i, ci);
    } else {
      Py_INCREF(it); /* anchor while re-entering Python */
      PyObject* ci = dc_dispatch_skip_atomic(it, Py_TYPE(it), ctx);
      Py_DECREF(it);
      if (!ci) { Py_DECREF(out); return NULL; }
      PyList_SET_ITEM(out, i, ci);
    }
  }

  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}

static PyObject* dc_copy_tuple(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                               void* oid, Py_ssize_t ohash) {
  (void)tp;
  Py_ssize_t n = PyTuple_GET_SIZE(src);
  if (n == 0) return Py_NewRef(src); /* empty tuple is atomic */

  int identical = 1;
  PyObject* out = PyTuple_New(n);
  if (!out) return NULL;

  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject* e = PyTuple_GET_ITEM(src, i);
    PyObject* ce;
    if (LIKELY(is_atomic_immutable_by_type(e, Py_TYPE(e)))) {
      ce = newref_maybe_immortal(e);
      if (!ce) { Py_DECREF(out); return NULL; }
    } else {
      ce = dc_dispatch_skip_atomic(e, Py_TYPE(e), ctx);
      if (!ce) { Py_DECREF(out); return NULL; }
      if (ce != e) identical = 0;
    }
    PyTuple_SET_ITEM(out, i, ce);
  }

  if (identical) {
    Py_INCREF(src);
    Py_DECREF(out);
    return src;
  }

  PyObject* hit = ctx_memo_lookup_h(ctx, oid, ohash);
  if (hit) { Py_INCREF(hit); Py_DECREF(out); return hit; }
  if (PyErr_Occurred()) { Py_DECREF(out); return NULL; }

  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) { Py_DECREF(out); return NULL; }
  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
static PyObject* dc_dict_with_watcher(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                      void* oid, Py_ssize_t ohash) {
  (void)tp;
  int mutated = 0;
  WatchContext w = {.dict = src, .mutated_flag = &mutated, .prev = NULL};
  watch_context_push(&w);
  if (UNLIKELY(module_state.dict_watcher_id < 0 ||
               PyDict_Watch(module_state.dict_watcher_id, src) < 0)) {
    watch_context_pop();
    Py_RETURN_NONE; /* signal fallback to legacy path */
  }

  Py_ssize_t expect = PyDict_Size(src);
  PyObject* out = PyDict_New();
  if (!out) { (void)PyDict_Unwatch(module_state.dict_watcher_id, src); watch_context_pop(); return NULL; }

  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) {
    Py_DECREF(out);
    (void)PyDict_Unwatch(module_state.dict_watcher_id, src);
    watch_context_pop();
    return NULL;
  }

  Py_ssize_t pos = 0;
  PyObject *k, *v;
  Py_hash_t kh;

  for (;;) {
    int step = dict_iterate_with_hash(src, &pos, &k, &v, &kh);
    if (step == 0) break;         /* end */
    if (step < 0) {               /* error while hashing key */
      (void)PyDict_Unwatch(module_state.dict_watcher_id, src);
      watch_context_pop();
      Py_DECREF(out);
      return NULL;
    }
    Py_INCREF(k);
    Py_INCREF(v);

    PyObject* ck = LIKELY(is_atomic_immutable_by_type(k, Py_TYPE(k)))
                 ? newref_maybe_immortal(k)
                 : dc_dispatch_skip_atomic(k, Py_TYPE(k), ctx);
    PyObject* cv = NULL;
    if (ck) {
      cv = LIKELY(is_atomic_immutable_by_type(v, Py_TYPE(v)))
         ? newref_maybe_immortal(v)
         : dc_dispatch_skip_atomic(v, Py_TYPE(v), ctx);
    }

    Py_DECREF(k); Py_DECREF(v);
    if (!ck || !cv) { Py_XDECREF(ck); Py_XDECREF(cv);
      (void)PyDict_Unwatch(module_state.dict_watcher_id, src); watch_context_pop(); Py_DECREF(out); return NULL; }

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
    int ir;
    if (ck == k) {
      ir = _PyDict_SetItem_KnownHash(out, ck, cv, kh);
    } else {
      ir = PyDict_SetItem(out, ck, cv);
    }
#else
    int ir = PyDict_SetItem(out, ck, cv);
#endif
    Py_DECREF(ck); Py_DECREF(cv);
    if (ir < 0) { (void)PyDict_Unwatch(module_state.dict_watcher_id, src); watch_context_pop(); Py_DECREF(out); return NULL; }

    if (UNLIKELY(mutated)) {
      (void)PyDict_Unwatch(module_state.dict_watcher_id, src);
      watch_context_pop();
      Py_DECREF(out);
      PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
      return NULL;
    }
  }

  if (UNLIKELY(mutated || PyDict_Size(src) != expect)) {
    (void)PyDict_Unwatch(module_state.dict_watcher_id, src);
    watch_context_pop();
    Py_DECREF(out);
    PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
    return NULL;
  }

  (void)PyDict_Unwatch(module_state.dict_watcher_id, src);
  watch_context_pop();

  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}
#endif

static PyObject* dc_dict_legacy(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                void* oid, Py_ssize_t ohash) {
  (void)tp;
  Py_ssize_t expect = PyDict_Size(src);
  PyObject* out = PyDict_New();
  if (!out) return NULL;
  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) { Py_DECREF(out); return NULL; }

  Py_ssize_t pos = 0;
  PyObject *k, *v;
  while (PyDict_Next(src, &pos, &k, &v)) {
    Py_INCREF(k);
    Py_INCREF(v);

    PyObject* ck = LIKELY(is_atomic_immutable_by_type(k, Py_TYPE(k)))
                 ? newref_maybe_immortal(k)
                 : dc_dispatch_skip_atomic(k, Py_TYPE(k), ctx);
    if (!ck) { Py_DECREF(k); Py_DECREF(v); Py_DECREF(out); return NULL; }

    if (UNLIKELY(PyDict_Size(src) != expect)) {
      Py_DECREF(ck); Py_DECREF(k); Py_DECREF(v); Py_DECREF(out);
      PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
      return NULL;
    }

    PyObject* cv = LIKELY(is_atomic_immutable_by_type(v, Py_TYPE(v)))
                 ? newref_maybe_immortal(v)
                 : dc_dispatch_skip_atomic(v, Py_TYPE(v), ctx);
    if (!cv) { Py_DECREF(ck); Py_DECREF(k); Py_DECREF(v); Py_DECREF(out); return NULL; }

    if (UNLIKELY(PyDict_Size(src) != expect)) {
      Py_DECREF(ck); Py_DECREF(cv); Py_DECREF(k); Py_DECREF(v); Py_DECREF(out);
      PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
      return NULL;
    }

    Py_DECREF(k); Py_DECREF(v);
    if (PyDict_SetItem(out, ck, cv) < 0) { Py_DECREF(ck); Py_DECREF(cv); Py_DECREF(out); return NULL; }
    Py_DECREF(ck); Py_DECREF(cv);
  }

  if (UNLIKELY(PyDict_Size(src) != expect)) {
    Py_DECREF(out);
    PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
    return NULL;
  }

  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}

static PyObject* dc_copy_set(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                             void* oid, Py_ssize_t ohash) {
  (void)tp;
  PyObject* out = PySet_New(NULL);
  if (!out) return NULL;
  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) { Py_DECREF(out); return NULL; }

  PyObject* it = PyObject_GetIter(src);
  if (!it) { Py_DECREF(out); return NULL; }
  PyObject* item;
  while ((item = PyIter_Next(it)) != NULL) {
    PyObject* ci = LIKELY(is_atomic_immutable_by_type(item, Py_TYPE(item)))
                 ? newref_maybe_immortal(item)
                 : dc_dispatch_skip_atomic(item, Py_TYPE(item), ctx);
    Py_DECREF(item);
    if (!ci) { Py_DECREF(it); Py_DECREF(out); return NULL; }
    if (PySet_Add(out, ci) < 0) { Py_DECREF(ci); Py_DECREF(it); Py_DECREF(out); return NULL; }
    Py_DECREF(ci);
  }
  Py_DECREF(it);
  if (PyErr_Occurred()) { Py_DECREF(out); return NULL; }

  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}

static PyObject* dc_copy_frozenset(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                   void* oid, Py_ssize_t ohash) {
  (void)tp; (void)ohash; /* store after creation */
  PyObject* it = PyObject_GetIter(src);
  if (!it) return NULL;
  PyObject* tmp = PyList_New(0);
  if (!tmp) { Py_DECREF(it); return NULL; }

  PyObject* item;
  while ((item = PyIter_Next(it)) != NULL) {
    PyObject* ci = LIKELY(is_atomic_immutable_by_type(item, Py_TYPE(item)))
                 ? newref_maybe_immortal(item)
                 : dc_dispatch_skip_atomic(item, Py_TYPE(item), ctx);
    if (!ci) { Py_DECREF(item); Py_DECREF(it); Py_DECREF(tmp); return NULL; }
    if (PyList_Append(tmp, ci) < 0) { Py_DECREF(ci); Py_DECREF(item); Py_DECREF(it); Py_DECREF(tmp); return NULL; }
    Py_DECREF(ci);
    Py_DECREF(item);
  }
  Py_DECREF(it);
  if (PyErr_Occurred()) { Py_DECREF(tmp); return NULL; }

  PyObject* out = PyFrozenSet_New(tmp);
  Py_DECREF(tmp);
  if (!out) return NULL;
  if (ctx_memo_store_h(ctx, (void*)src, out, memo_hash_pointer((void*)src)) < 0) { Py_DECREF(out); return NULL; }
  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}

static PyObject* dc_copy_bytearray(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                   void* oid, Py_ssize_t ohash) {
  (void)tp;
  Py_ssize_t n =
#if defined(PyByteArray_GET_SIZE)
    PyByteArray_GET_SIZE(src);
#else
    PyByteArray_Size(src);
#endif
  PyObject* out = PyByteArray_FromStringAndSize(NULL, n);
  if (!out) return NULL;
  if (n) memcpy(PyByteArray_AS_STRING(out), PyByteArray_AS_STRING(src), (size_t)n);
  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) { Py_DECREF(out); return NULL; }
  if (ctx_keepalive_append_if_different(ctx, src, out) < 0) { Py_DECREF(out); return NULL; }
  return out;
}

/* ---------------- reduce/constructor reconstruction ------------------------ */

static PyObject* dc_reconstruct_state(PyObject* new_obj,
                                      PyObject* state,
                                      PyObject* listiter,
                                      PyObject* dictiter) {
  if (!new_obj) {
    PyErr_SetString(PyExc_SystemError, "reconstruct_state: new_obj is NULL");
    return NULL;
  }
  if (!state) state = Py_None;
  if (!listiter) listiter = Py_None;
  if (!dictiter) dictiter = Py_None;

  if (state != Py_None) {
    PyObject* setstate = PyObject_GetAttr(new_obj, module_state.str_setstate);
    if (setstate) {
      PyObject* r = PyObject_CallOneArg(setstate, state);
      Py_DECREF(setstate);
      if (!r) return NULL;
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
          if (!it) return NULL;
          PyObject* key;
          while ((key = PyIter_Next(it)) != NULL) {
            PyObject* value = PyObject_GetItem(slot_state, key);
            if (!value) { Py_DECREF(key); Py_DECREF(it); return NULL; }
            if (PyObject_SetAttr(new_obj, key, value) < 0) { Py_DECREF(value); Py_DECREF(key); Py_DECREF(it); return NULL; }
            Py_DECREF(value);
            Py_DECREF(key);
          }
          Py_DECREF(it);
          if (PyErr_Occurred()) return NULL;
        }
      } else {
        dict_state = state;
      }

      if (dict_state && dict_state != Py_None) {
        PyObject* obj_dict = PyObject_GetAttr(new_obj, module_state.str_dict);
        if (!obj_dict) return NULL;
        PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
        Py_DECREF(obj_dict);
        if (!update) return NULL;
        PyObject* r = PyObject_CallOneArg(update, dict_state);
        Py_DECREF(update);
        if (!r) return NULL;
        Py_DECREF(r);
      }
    }
  }

  if (listiter != Py_None) {
    PyObject* append = PyObject_GetAttr(new_obj, module_state.str_append);
    if (!append) return NULL;
    PyObject* it = PyObject_GetIter(listiter);
    if (!it) { Py_DECREF(append); return NULL; }
    PyObject* item;
    while ((item = PyIter_Next(it)) != NULL) {
      PyObject* r = PyObject_CallOneArg(append, item);
      Py_DECREF(item);
      if (!r) { Py_DECREF(it); Py_DECREF(append); return NULL; }
      Py_DECREF(r);
    }
    Py_DECREF(it);
    Py_DECREF(append);
    if (PyErr_Occurred()) return NULL;
  }

  if (dictiter != Py_None) {
    PyObject* it = PyObject_GetIter(dictiter);
    if (!it) return NULL;
    PyObject* pair;
    while ((pair = PyIter_Next(it)) != NULL) {
      if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
        Py_DECREF(pair); Py_DECREF(it);
        PyErr_SetString(PyExc_TypeError, "dictiter must yield (key, value) pairs");
        return NULL;
      }
      PyObject* k = PyTuple_GET_ITEM(pair, 0);
      PyObject* v = PyTuple_GET_ITEM(pair, 1);
      Py_INCREF(k); Py_INCREF(v);
      if (PyObject_SetItem(new_obj, k, v) < 0) { Py_DECREF(k); Py_DECREF(v); Py_DECREF(pair); Py_DECREF(it); return NULL; }
      Py_DECREF(k); Py_DECREF(v); Py_DECREF(pair);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return NULL;
  }

  return Py_NewRef(new_obj);
}

/* Deep variant for deepcopy semantics: deep-copy state/list/dict payloads */
static PyObject* dc_reconstruct_state_deep(PyObject* new_obj,
                                           PyObject* state,
                                           PyObject* listiter,
                                           PyObject* dictiter,
                                           DCtx* ctx) {
  if (!new_obj) {
    PyErr_SetString(PyExc_SystemError, "reconstruct_state_deep: new_obj is NULL");
    return NULL;
  }
  if (!state) state = Py_None;
  if (!listiter) listiter = Py_None;
  if (!dictiter) dictiter = Py_None;

  if (state != Py_None) {
    PyObject* setstate = PyObject_GetAttr(new_obj, module_state.str_setstate);
    if (setstate) {
      /* __setstate__(deepcopy(state)) */
      PyObject* dstate = dc_dispatch(state, Py_TYPE(state), ctx);
      if (!dstate) { Py_DECREF(setstate); return NULL; }
      PyObject* r = PyObject_CallOneArg(setstate, dstate);
      Py_DECREF(dstate);
      Py_DECREF(setstate);
      if (!r) return NULL;
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
          if (!it) return NULL;
          PyObject* key;
          while ((key = PyIter_Next(it)) != NULL) {
            PyObject* value = PyObject_GetItem(slot_state, key);
            if (!value) { Py_DECREF(key); Py_DECREF(it); return NULL; }
            PyObject* dval = dc_dispatch(value, Py_TYPE(value), ctx);
            Py_DECREF(value);
            if (!dval) { Py_DECREF(key); Py_DECREF(it); return NULL; }
            if (PyObject_SetAttr(new_obj, key, dval) < 0) {
              Py_DECREF(dval); Py_DECREF(key); Py_DECREF(it);
              return NULL;
            }
            Py_DECREF(dval);
            Py_DECREF(key);
          }
          Py_DECREF(it);
          if (PyErr_Occurred()) return NULL;
        }
      } else {
        dict_state = state; /* treat as mapping-like */
      }

      if (dict_state && dict_state != Py_None) {
        PyObject* dds = dc_dispatch(dict_state, Py_TYPE(dict_state), ctx);
        if (!dds) return NULL;
        PyObject* obj_dict = PyObject_GetAttr(new_obj, module_state.str_dict);
        if (!obj_dict) { Py_DECREF(dds); return NULL; }
        PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
        Py_DECREF(obj_dict);
        if (!update) { Py_DECREF(dds); return NULL; }
        PyObject* r = PyObject_CallOneArg(update, dds);
        Py_DECREF(update);
        Py_DECREF(dds);
        if (!r) return NULL;
        Py_DECREF(r);
      }
    }
  }

  if (listiter != Py_None) {
    PyObject* append = PyObject_GetAttr(new_obj, module_state.str_append);
    if (!append) return NULL;
    PyObject* it = PyObject_GetIter(listiter);
    if (!it) { Py_DECREF(append); return NULL; }
    PyObject* item;
    while ((item = PyIter_Next(it)) != NULL) {
      PyObject* di = dc_dispatch(item, Py_TYPE(item), ctx);
      Py_DECREF(item);
      if (!di) { Py_DECREF(it); Py_DECREF(append); return NULL; }
      PyObject* r = PyObject_CallOneArg(append, di);
      Py_DECREF(di);
      if (!r) { Py_DECREF(it); Py_DECREF(append); return NULL; }
      Py_DECREF(r);
    }
    Py_DECREF(it);
    Py_DECREF(append);
    if (PyErr_Occurred()) return NULL;
  }

  if (dictiter != Py_None) {
    PyObject* it = PyObject_GetIter(dictiter);
    if (!it) return NULL;
    PyObject* pair;
    while ((pair = PyIter_Next(it)) != NULL) {
      if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
        Py_DECREF(pair); Py_DECREF(it);
        PyErr_SetString(PyExc_TypeError, "dictiter must yield (key, value) pairs");
        return NULL;
      }
      PyObject* k = PyTuple_GET_ITEM(pair, 0);
      PyObject* v = PyTuple_GET_ITEM(pair, 1);
      PyObject* dk = dc_dispatch(k, Py_TYPE(k), ctx);
      if (!dk) { Py_DECREF(pair); Py_DECREF(it); return NULL; }
      PyObject* dv = dc_dispatch(v, Py_TYPE(v), ctx);
      if (!dv) { Py_DECREF(dk); Py_DECREF(pair); Py_DECREF(it); return NULL; }
      if (PyObject_SetItem(new_obj, dk, dv) < 0) {
        Py_DECREF(dk); Py_DECREF(dv); Py_DECREF(pair); Py_DECREF(it);
        return NULL;
      }
      Py_DECREF(dk); Py_DECREF(dv);
      Py_DECREF(pair);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return NULL;
  }

  return Py_NewRef(new_obj);
}

static PyObject* dc_reduce_reconstruct(PyObject* src, PyTypeObject* tp, DCtx* ctx,
                                       void* oid, Py_ssize_t ohash) {
  (void)tp;
  PyObject* rr = try_reduce_via_registry(src, Py_TYPE(src));
  if (!rr) {
    if (PyErr_Occurred()) return NULL;
    rr = call_reduce_method_preferring_ex(src);
    if (!rr) return NULL;
  }

  PyObject *ctor=NULL, *args=NULL, *state=NULL, *listiter=NULL, *dictiter=NULL;
  int ur = unpack_reduce_result_tuple(rr, &ctor, &args, &state, &listiter, &dictiter);
  if (ur < 0) { Py_DECREF(rr); return NULL; }
  if (ur == 1) { Py_DECREF(rr); return newref_maybe_immortal(src); }

  PyObject* out = NULL;

  /* copyreg fast-paths */
  if (ctor == module_state.copyreg_newobj) {
    if (LIKELY(PyTuple_Check(args) && PyTuple_GET_SIZE(args) >= 1)) {
      PyObject* cls = PyTuple_GET_ITEM(args, 0);
      if (PyType_Check(cls)) {
        Py_ssize_t npos = PyTuple_GET_SIZE(args) - 1;
        PyObject* newargs = PyTuple_New(npos);
        if (!newargs) { Py_DECREF(rr); return NULL; }
        for (Py_ssize_t i = 0; i < npos; i++) {
          PyObject* a = PyTuple_GET_ITEM(args, i + 1);
          PyObject* ca = dc_dispatch(a, Py_TYPE(a), ctx);
          if (!ca) { Py_DECREF(newargs); Py_DECREF(rr); return NULL; }
          PyTuple_SET_ITEM(newargs, i, ca);
        }
        out = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, newargs, NULL);
        Py_DECREF(newargs);
      }
    }
  } else if (ctor == module_state.copyreg_newobj_ex) {
    if (LIKELY(PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 3)) {
      PyObject* cls = PyTuple_GET_ITEM(args, 0);
      PyObject* pargs = PyTuple_GET_ITEM(args, 1);
      PyObject* pkwargs = PyTuple_GET_ITEM(args, 2);
      if (PyType_Check(cls) && PyTuple_Check(pargs)) {
        PyObject* dpargs = dc_dispatch(pargs, Py_TYPE(pargs), ctx);
        if (!dpargs) { Py_DECREF(rr); return NULL; }
        PyObject* dkwargs = NULL;
        if (pkwargs != Py_None) {
          if (PyDict_Check(pkwargs) && PyDict_Size(pkwargs) == 0) {
            dkwargs = NULL;
          } else {
            dkwargs = dc_dispatch(pkwargs, Py_TYPE(pkwargs), ctx);
            if (!dkwargs) { Py_DECREF(dpargs); Py_DECREF(rr); return NULL; }
          }
        }
        out = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, dpargs, dkwargs);
        Py_DECREF(dpargs);
        Py_XDECREF(dkwargs);
      }
    }
  }

  if (!out) {
    if (PyTuple_GET_SIZE(args) == 0) {
      out = PyObject_CallNoArgs(ctor);
    } else {
      Py_ssize_t n = PyTuple_GET_SIZE(args);
      PyObject* dargs = PyTuple_New(n);
      if (!dargs) { Py_DECREF(rr); return NULL; }
      for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* a = PyTuple_GET_ITEM(args, i);
        PyObject* ca = dc_dispatch(a, Py_TYPE(a), ctx);
        if (!ca) { Py_DECREF(dargs); Py_DECREF(rr); return NULL; }
        PyTuple_SET_ITEM(dargs, i, ca);
      }
      out = PyObject_CallObject(ctor, dargs);
      Py_DECREF(dargs);
    }
  }

  if (!out) { Py_DECREF(rr); return NULL; }

  if (ctx_memo_store_h(ctx, oid, out, ohash) < 0) { Py_DECREF(out); Py_DECREF(rr); return NULL; }

  if ((state && state != Py_None) || (listiter && listiter != Py_None) || (dictiter && dictiter != Py_None)) {
    PyObject* applied = dc_reconstruct_state_deep(out,
                                                  state ? state : Py_None,
                                                  listiter ? listiter : Py_None,
                                                  dictiter ? dictiter : Py_None,
                                                  ctx);
    if (!applied) { Py_DECREF(out); Py_DECREF(rr); return NULL; }
    Py_DECREF(applied);
  }

  if (ctx_keepalive_append_original(ctx, src) < 0) { Py_DECREF(out); Py_DECREF(rr); return NULL; }
  Py_DECREF(rr);
  return out;
}

/* ---------------- Public API: deepcopy() ----------------------------------- */

PyObject* py_deepcopy(PyObject* self,
                      PyObject* const* args,
                      Py_ssize_t nargs,
                      PyObject* kwnames) {
  (void)self;
  PyObject* src = NULL;
  PyObject* memo_arg = Py_None;

  /* Parse: fast paths for keyword-less and single "memo" kw */
  if (!kwnames || PyTuple_GET_SIZE(kwnames) == 0) {
    if (UNLIKELY(nargs < 1)) {
      PyErr_SetString(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
      return NULL;
    }
    if (UNLIKELY(nargs > 2)) {
      PyErr_Format(PyExc_TypeError, "deepcopy() takes from 1 to 2 positional arguments but %zd were given", nargs);
      return NULL;
    }
    src = args[0];
    memo_arg = (nargs == 2) ? args[1] : Py_None;
  } else {
    const Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    if (kwcount == 1) {
      PyObject* kw0 = PyTuple_GET_ITEM(kwnames, 0);
      int is_memo = PyUnicode_Check(kw0) && PyUnicode_CompareWithASCIIString(kw0, "memo") == 0;
      if (is_memo) {
        if (UNLIKELY(nargs < 1)) {
          PyErr_SetString(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
          return NULL;
        }
        if (UNLIKELY(nargs > 2)) {
          PyErr_Format(PyExc_TypeError, "deepcopy() takes from 1 to 2 positional arguments but %zd were given", nargs);
          return NULL;
        }
        if (UNLIKELY(nargs == 2)) {
          PyErr_SetString(PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'");
          return NULL;
        }
        src = args[0];
        memo_arg = args[nargs + 0];
      } else {
        /* slow keyword path: accept x=..., memo=... */
        if (UNLIKELY(nargs > 2)) {
          PyErr_Format(PyExc_TypeError, "deepcopy() takes from 1 to 2 positional arguments but %zd were given", nargs);
          return NULL;
        }
        if (nargs >= 1) src = args[0];
        if (nargs == 2) memo_arg = args[1];

        Py_ssize_t i;
        int seen_memo_kw = 0;
        for (i = 0; i < kwcount; i++) {
          PyObject* name = PyTuple_GET_ITEM(kwnames, i);
          PyObject* val  = args[nargs + i];
          if (!PyUnicode_Check(name)) {
            PyErr_SetString(PyExc_TypeError, "deepcopy() keywords must be strings");
            return NULL;
          }
          if (PyUnicode_CompareWithASCIIString(name, "x") == 0) {
            if (UNLIKELY(src != NULL)) {
              PyErr_SetString(PyExc_TypeError, "deepcopy() got multiple values for argument 'x'");
              return NULL;
            }
            src = val;
          } else if (PyUnicode_CompareWithASCIIString(name, "memo") == 0) {
            if (UNLIKELY(seen_memo_kw || nargs == 2)) {
              PyErr_SetString(PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'");
              return NULL;
            }
            memo_arg = val;
            seen_memo_kw = 1;
          } else {
            PyErr_Format(PyExc_TypeError, "deepcopy() got an unexpected keyword argument '%U'", name);
            return NULL;
          }
        }
        if (UNLIKELY(src == NULL)) {
          PyErr_SetString(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
          return NULL;
        }
      }
    } else {
      /* slow keyword path: accept x=..., memo=... */
      if (UNLIKELY(nargs > 2)) {
        PyErr_Format(PyExc_TypeError, "deepcopy() takes from 1 to 2 positional arguments but %zd were given", nargs);
        return NULL;
      }
      if (nargs >= 1) src = args[0];
      if (nargs == 2) memo_arg = args[1];

      Py_ssize_t i;
      int seen_memo_kw = 0;
      for (i = 0; i < kwcount; i++) {
        PyObject* name = PyTuple_GET_ITEM(kwnames, i);
        PyObject* val  = args[nargs + i];
        if (!PyUnicode_Check(name)) {
          PyErr_SetString(PyExc_TypeError, "deepcopy() keywords must be strings");
          return NULL;
        }
        if (PyUnicode_CompareWithASCIIString(name, "x") == 0) {
          if (UNLIKELY(src != NULL)) {
            PyErr_SetString(PyExc_TypeError, "deepcopy() got multiple values for argument 'x'");
            return NULL;
          }
          src = val;
        } else if (PyUnicode_CompareWithASCIIString(name, "memo") == 0) {
          if (UNLIKELY(seen_memo_kw || nargs == 2)) {
            PyErr_SetString(PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'");
            return NULL;
          }
          memo_arg = val;
          seen_memo_kw = 1;
        } else {
          PyErr_Format(PyExc_TypeError, "deepcopy() got an unexpected keyword argument '%U'", name);
          return NULL;
        }
      }
      if (UNLIKELY(src == NULL)) {
        PyErr_SetString(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
        return NULL;
      }
    }
  }

  /* Fast atomics / empties before any memo creation. */
  PyTypeObject* tp = Py_TYPE(src);

  /* If an explicit memo was provided, first validate its type, then honor it (parity with stdlib). */
  if (memo_arg != Py_None) {
    if (!PyDict_Check(memo_arg) && Py_TYPE(memo_arg) != &Memo_Type) {
      PyErr_Format(PyExc_TypeError, "argument 'memo' must be dict, not %.200s",
                   Py_TYPE(memo_arg)->tp_name);
      return NULL;
    }
    PyObject* existing = memo_lookup_obj(memo_arg, (void*)src);
    if (existing) { Py_INCREF(existing); return existing; }
    if (PyErr_Occurred()) return NULL;
  }

  if (LIKELY(is_atomic_immutable_by_type(src, tp))) {
    return newref_maybe_immortal(src);
  }

  /* Short-circuit empty mutables only when we won't need to record keepalive/memo. */
  if (memo_arg == Py_None) {
    if (tp == &PyList_Type) {
      RETURN_IF_EMPTY(Py_SIZE(src) == 0, PyList_New(0));
    } else if (tp == &PyTuple_Type) {
      RETURN_IF_EMPTY(Py_SIZE(src) == 0, PyTuple_New(0));
    } else if (tp == &PyDict_Type) {
#if defined(PyDict_GET_SIZE)
      RETURN_IF_EMPTY(PyDict_GET_SIZE(src) == 0, PyDict_New());
#else
      RETURN_IF_EMPTY(PyDict_Size(src) == 0, PyDict_New());
#endif
    } else if (tp == &PySet_Type) {
#if defined(PySet_GET_SIZE)
      RETURN_IF_EMPTY(PySet_GET_SIZE((PySetObject*)src) == 0, PySet_New(NULL));
#else
      RETURN_IF_EMPTY(PySet_Size(src) == 0, PySet_New(NULL));
#endif
    } else if (tp == &PyFrozenSet_Type) {
#if defined(PySet_GET_SIZE)
      RETURN_IF_EMPTY(PySet_GET_SIZE((PySetObject*)src) == 0, PyFrozenSet_New(NULL));
#else
      RETURN_IF_EMPTY(PyObject_Size(src) == 0, PyFrozenSet_New(NULL));
#endif
    } else if (tp == &PyByteArray_Type) {
#if defined(PyByteArray_GET_SIZE)
      RETURN_IF_EMPTY(PyByteArray_GET_SIZE(src) == 0, PyByteArray_FromStringAndSize(NULL, 0));
#else
      RETURN_IF_EMPTY(PyByteArray_Size(src) == 0, PyByteArray_FromStringAndSize(NULL, 0));
#endif
    }
  }

  /* Build the context with lazy memo semantics */
  DCtx ctx = {0};
  if (memo_arg == Py_None) {
    ctx.implicit_tls_memo = 1; /* acquire TLS memo only if/when we actually memoize */
  } else {
    if (!PyDict_Check(memo_arg) && Py_TYPE(memo_arg) != &Memo_Type) {
      PyErr_Format(PyExc_TypeError, "argument 'memo' must be dict, not %.200s", Py_TYPE(memo_arg)->tp_name);
      return NULL;
    }
    ctx.memo = memo_arg;
    Py_INCREF(ctx.memo);
  }

  /* Dispatch */
  PyObject* out = dc_dispatch(src, tp, &ctx);

  /* Cleanup */
  Py_XDECREF(ctx.keepalive);
  if (ctx.implicit_tls_memo) {
    if (ctx.memo) {
      MemoObject* mo = (MemoObject*)ctx.memo;
      if (mo->table) (void)memo_table_reset(&mo->table);
      keepvector_clear(&mo->keep);
      keepvector_shrink_if_large(&mo->keep);
      /* The memo object stays in TLS cache; no DECREF here. */
    }
  } else {
    Py_XDECREF(ctx.memo);
  }

  return out;
}

/* ---------------- Public API: copy() --------------------------------------- */

static inline int is_empty_initializable(PyObject* obj, PyTypeObject* tp) {
  if (tp == &PyList_Type) {
    return Py_SIZE(obj) == 0;
  } else if (tp == &PyTuple_Type) {
    return Py_SIZE(obj) == 0;
  } else if (tp == &PyDict_Type) {
#if defined(PyDict_GET_SIZE)
    return PyDict_GET_SIZE(obj) == 0;
#else
    return PyDict_Size(obj) == 0;
#endif
  } else if (tp == &PySet_Type) {
#if defined(PySet_GET_SIZE)
    return PySet_GET_SIZE((PySetObject*)obj) == 0;
#else
    return PySet_Size(obj) == 0;
#endif
  } else if (tp == &PyFrozenSet_Type) {
#if defined(PySet_GET_SIZE)
    return PySet_GET_SIZE((PySetObject*)obj) == 0;
#else
    return PyObject_Size(obj) == 0;
#endif
  } else if (tp == &PyByteArray_Type) {
#if defined(PyByteArray_GET_SIZE)
    return PyByteArray_GET_SIZE(obj) == 0;
#else
    return PyByteArray_Size(obj) == 0;
#endif
  }
  return 0;
}

static PyObject* make_empty_same_type(PyTypeObject* tp) {
  if (tp == &PyList_Type)      return PyList_New(0);
  if (tp == &PyTuple_Type)     return PyTuple_New(0);
  if (tp == &PyDict_Type)      return PyDict_New();
  if (tp == &PySet_Type)       return PySet_New(NULL);
  if (tp == &PyFrozenSet_Type) return PyFrozenSet_New(NULL);
  if (tp == &PyByteArray_Type) return PyByteArray_FromStringAndSize(NULL, 0);
  Py_RETURN_NONE; /* shouldn’t happen with guard */
}

static PyObject* try_stdlib_mutable_copy(PyObject* obj, PyTypeObject* tp) {
  if (tp == &PyDict_Type || tp == &PySet_Type || tp == &PyList_Type || tp == &PyByteArray_Type) {
    PyObject* m = PyObject_GetAttrString(obj, "copy");
    if (!m) {
      PyErr_Clear();
    } else {
      PyObject* out = PyObject_CallNoArgs(m);
      Py_DECREF(m);
      if (out) return out;
      return NULL;
    }
  }
  Py_RETURN_NONE;
}

PyObject* py_copy(PyObject* self, PyObject* obj) {
  (void)self;
  PyTypeObject* tp = Py_TYPE(obj);

  if (is_atomic_immutable_by_type(obj, tp)) {
    return newref_maybe_immortal(obj);
  }

  if (PySlice_Check(obj))      return newref_maybe_immortal(obj);
  if (PyFrozenSet_CheckExact(obj)) return newref_maybe_immortal(obj);
  if (PyType_IsSubtype(tp, &PyType_Type)) return newref_maybe_immortal(obj);

  if (is_empty_initializable(obj, tp)) {
    PyObject* fresh = make_empty_same_type(tp);
    if (fresh != Py_None) return fresh;
    Py_DECREF(fresh);
  }

  {
    PyObject* maybe = try_stdlib_mutable_copy(obj, tp);
    if (!maybe) return NULL;
    if (maybe != Py_None) return maybe;
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

  /* Fallback to reduce */
  PyObject* rr = try_reduce_via_registry(obj, tp);
  if (!rr) {
    if (PyErr_Occurred()) return NULL;
    rr = call_reduce_method_preferring_ex(obj);
    if (!rr) return NULL;
  }

  PyObject *ctor=NULL, *args=NULL, *state=NULL, *listiter=NULL, *dictiter=NULL;
  int ur = unpack_reduce_result_tuple(rr, &ctor, &args, &state, &listiter, &dictiter);
  if (ur < 0) { Py_DECREF(rr); return NULL; }
  if (ur == 1) { Py_DECREF(rr); return newref_maybe_immortal(obj); }

  PyObject* out = NULL;
  if (PyTuple_GET_SIZE(args) == 0) {
    out = PyObject_CallNoArgs(ctor);
  } else {
    out = PyObject_CallObject(ctor, args);
  }
  if (!out) { Py_DECREF(rr); return NULL; }

  if ((state && state != Py_None) || (listiter && listiter != Py_None) || (dictiter && dictiter != Py_None)) {
    PyObject* applied = dc_reconstruct_state(out,
                                             state ? state : Py_None,
                                             listiter ? listiter : Py_None,
                                             dictiter ? dictiter : Py_None);
    if (!applied) { Py_DECREF(out); Py_DECREF(rr); return NULL; }
    Py_DECREF(applied);
  }
  Py_DECREF(rr);
  return out;
}

/* ---------------- replicate() / repeatcall() ------------------------------- */

static inline PyObject* build_list_by_calling_noargs(PyObject* callable, Py_ssize_t n) {
  if (n < 0) { PyErr_SetString(PyExc_ValueError, "n must be >= 0"); return NULL; }
  PyObject* out = PyList_New(n);
  if (!out) return NULL;

  vectorcallfunc vc = PyVectorcall_Function(callable);
  if (LIKELY(vc)) {
    for (Py_ssize_t i = 0; i < n; i++) {
      PyObject* item = vc(callable, NULL, PyVectorcall_NARGS(0), NULL);
      if (!item) { Py_DECREF(out); return NULL; }
      PyList_SET_ITEM(out, i, item);
    }
  } else {
    for (Py_ssize_t i = 0; i < n; i++) {
      PyObject* item = PyObject_CallNoArgs(callable);
      if (!item) { Py_DECREF(out); return NULL; }
      PyList_SET_ITEM(out, i, item);
    }
  }
  return out;
}

PyObject* py_replicate(PyObject* self,
                       PyObject* const* args,
                       Py_ssize_t nargs,
                       PyObject* kwnames) {
  (void)self;

  if (UNLIKELY(nargs != 2)) {
    PyErr_SetString(PyExc_TypeError, "replicate(obj, n, /, *, compile_after=20)");
    return NULL;
  }

  PyObject* obj = args[0];
  long n_long = PyLong_AsLong(args[1]);
  if (n_long == -1 && PyErr_Occurred()) return NULL;
  if (n_long < 0) { PyErr_SetString(PyExc_ValueError, "n must be >= 0"); return NULL; }
  Py_ssize_t n = (Py_ssize_t)n_long;

  int duper_available = (module_state.create_precompiler_reconstructor != NULL);

  int compile_after = 20;
  if (kwnames) {
    Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    if (kwcount > 1) { PyErr_SetString(PyExc_TypeError, "replicate accepts only 'compile_after' keyword"); return NULL; }
    if (kwcount == 1) {
      PyObject* kwname = PyTuple_GET_ITEM(kwnames, 0);
      int is_compile_after = PyUnicode_Check(kwname) &&
                             (PyUnicode_CompareWithASCIIString(kwname, "compile_after") == 0);
      if (!is_compile_after) {
        PyErr_SetString(PyExc_TypeError, "unknown keyword; only 'compile_after' is supported");
        return NULL;
      }
      if (!duper_available) {
        PyErr_SetString(PyExc_TypeError,
                        "replicate(): 'compile_after' requires duper.snapshots; it is not available");
        return NULL;
      }
      PyObject* kwval = args[nargs + 0];
      long ca = PyLong_AsLong(kwval);
      if (ca == -1 && PyErr_Occurred()) return NULL;
      if (ca < 0) { PyErr_SetString(PyExc_ValueError, "compile_after must be >= 0"); return NULL; }
      compile_after = (int)ca;
    }
  }

  if (n == 0) return PyList_New(0);

  if (is_atomic_immutable_by_type(obj, Py_TYPE(obj))) {
    PyObject* out = PyList_New(n);
    if (!out) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
      PyObject* ci = newref_maybe_immortal(obj);
      if (!ci) { Py_DECREF(out); return NULL; }
      PyList_SET_ITEM(out, i, ci);
    }
    return out;
  }

  {
    PinObject* pin = _duper_lookup_pin_for_object(obj);
    if (pin) {
      PyObject* factory = pin->factory; /* borrowed */
      if (UNLIKELY(!factory || !PyCallable_Check(factory))) {
        PyErr_SetString(PyExc_RuntimeError, "pinned object has no valid factory");
        return NULL;
      }
      PyObject* out = build_list_by_calling_noargs(factory, n);
      if (out) pin->hits += (uint64_t)n;
      return out;
    }
  }

  if (!duper_available || n <= (Py_ssize_t)compile_after) {
    PyObject* out = PyList_New(n);
    if (!out) return NULL;

    for (Py_ssize_t i = 0; i < n; i++) {
      DCtx ctx = { .implicit_tls_memo = 1 };
      PyObject* copy_i = dc_dispatch_skip_atomic(obj, Py_TYPE(obj), &ctx);
      Py_XDECREF(ctx.keepalive);
      if (ctx.memo) {
        MemoObject* mo = (MemoObject*)ctx.memo;
        if (mo->table) (void)memo_table_reset(&mo->table);
        keepvector_clear(&mo->keep);
        keepvector_shrink_if_large(&mo->keep);
      }
      if (!copy_i) {
        for (Py_ssize_t j = 0; j < i; j++) {
          PyObject* prev = PyList_GET_ITEM(out, j);
          PyList_SET_ITEM(out, j, NULL);
          Py_XDECREF(prev);
        }
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
      PyErr_SetString(PyExc_RuntimeError,
                      "duper.snapshots.create_precompiler_reconstructor is not callable");
      return NULL;
    }
    PyObject* reconstructor = PyObject_CallOneArg(cpr, obj);
    if (!reconstructor) return NULL;
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

PyObject* py_repeatcall(PyObject* self,
                        PyObject* const* args,
                        Py_ssize_t nargs,
                        PyObject* kwnames) {
  (void)self; (void)kwnames;
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
  if (n_long == -1 && PyErr_Occurred()) return NULL;
  if (n_long < 0) { PyErr_SetString(PyExc_ValueError, "size must be >= 0"); return NULL; }
  Py_ssize_t n = (Py_ssize_t)n_long;
  return build_list_by_calling_noargs(func, n);
}

/* ---------------- replace() (3.13+) ---------------------------------------- */
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
PyObject* py_replace(PyObject* self,
                     PyObject* const* args,
                     Py_ssize_t nargs,
                     PyObject* kwnames) {
  (void)self;
  if (UNLIKELY(nargs == 0)) {
    PyErr_SetString(PyExc_TypeError, "replace() missing 1 required positional argument: 'obj'");
    return NULL;
  }
  if (UNLIKELY(nargs > 1)) {
    PyErr_Format(PyExc_TypeError, "replace() takes 1 positional argument but %zd were given", nargs);
    return NULL;
  }
  PyObject* obj = args[0];
  PyObject* cls = (PyObject*)Py_TYPE(obj);

  PyObject* func = PyObject_GetAttrString(cls, "__replace__");
  if (!func) {
    PyErr_Clear();
    PyErr_Format(PyExc_TypeError, "replace() does not support %.200s objects", Py_TYPE(obj)->tp_name);
    return NULL;
  }
  if (!PyCallable_Check(func)) {
    Py_DECREF(func);
    PyErr_SetString(PyExc_TypeError, "__replace__ is not callable");
    return NULL;
  }

  PyObject* posargs = PyTuple_New(1);
  if (!posargs) { Py_DECREF(func); return NULL; }
  Py_INCREF(obj);
  PyTuple_SET_ITEM(posargs, 0, obj);

  PyObject* kwargs = NULL;
  if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
    kwargs = PyDict_New();
    if (!kwargs) { Py_DECREF(func); Py_DECREF(posargs); return NULL; }
    Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    for (Py_ssize_t i = 0; i < kwcount; i++) {
      PyObject* key = PyTuple_GET_ITEM(kwnames, i);
      PyObject* val = args[nargs + i];
      if (PyDict_SetItem(kwargs, key, val) < 0) {
        Py_DECREF(func); Py_DECREF(posargs); Py_DECREF(kwargs);
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

/* ---------------- Module boilerplate --------------------------------------- */
extern PyObject* py_copy(PyObject* self, PyObject* obj);
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
extern PyObject* py_replace(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames);
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

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  if (module_state.dict_watcher_id >= 0) {
    (void)PyDict_ClearWatcher(module_state.dict_watcher_id);
    module_state.dict_watcher_id = -1;
  }
  if (PyThread_tss_is_created(&module_state.watch_tss)) {
    PyThread_tss_delete(&module_state.watch_tss);
  }
#endif
  if (PyThread_tss_is_created(&module_state.memo_tss)) {
    PyThread_tss_delete(&module_state.memo_tss);
  }
  if (PyThread_tss_is_created(&module_state.recdepth_tss)) {
    PyThread_tss_delete(&module_state.recdepth_tss);
  }
}

#define LOAD_TYPE(source_module, type_name, target_field)                   \
  do {                                                                      \
    PyObject* _t = PyObject_GetAttrString((source_module), (type_name));    \
    if (!_t || !PyType_Check(_t)) {                                         \
      Py_XDECREF(_t);                                                       \
      PyErr_Format(PyExc_ImportError, "%s.%s missing or not a type", #source_module, (type_name)); \
      cleanup_on_init_failure();                                            \
      return -1;                                                            \
    }                                                                       \
    module_state.target_field = (PyTypeObject*)_t;                          \
  } while (0)

int _copium_copying_init(PyObject* module) {
  /* interned names */
  module_state.str_reduce_ex = PyUnicode_InternFromString("__reduce_ex__");
  module_state.str_reduce    = PyUnicode_InternFromString("__reduce__");
  module_state.str_deepcopy  = PyUnicode_InternFromString("__deepcopy__");
  module_state.str_setstate  = PyUnicode_InternFromString("__setstate__");
  module_state.str_dict      = PyUnicode_InternFromString("__dict__");
  module_state.str_append    = PyUnicode_InternFromString("append");
  module_state.str_update    = PyUnicode_InternFromString("update");
  module_state.str_new       = PyUnicode_InternFromString("__new__");

  if (!module_state.str_reduce_ex || !module_state.str_reduce ||
      !module_state.str_deepcopy || !module_state.str_setstate ||
      !module_state.str_dict || !module_state.str_append ||
      !module_state.str_update || !module_state.str_new) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to intern required names");
    cleanup_on_init_failure();
    return -1;
  }

  /* stdlib modules */
  PyObject* mod_types     = PyImport_ImportModule("types");
  PyObject* mod_builtins  = PyImport_ImportModule("builtins");
  PyObject* mod_weakref   = PyImport_ImportModule("weakref");
  PyObject* mod_copyreg   = PyImport_ImportModule("copyreg");
  PyObject* mod_re        = PyImport_ImportModule("re");
  PyObject* mod_decimal   = PyImport_ImportModule("decimal");
  PyObject* mod_fractions = PyImport_ImportModule("fractions");

  if (!mod_types || !mod_builtins || !mod_weakref || !mod_copyreg ||
      !mod_re || !mod_decimal || !mod_fractions) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to import required stdlib modules");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }

  /* cache types */
  LOAD_TYPE(mod_types, "BuiltinFunctionType", BuiltinFunctionType);
  LOAD_TYPE(mod_types, "CodeType",             CodeType);
  LOAD_TYPE(mod_types, "MethodType",           MethodType);
  LOAD_TYPE(mod_builtins, "property",          property_type);
  LOAD_TYPE(mod_builtins, "range",             range_type);
  LOAD_TYPE(mod_weakref, "ref",                weakref_ref_type);
  LOAD_TYPE(mod_re, "Pattern",                 re_Pattern_type);
  LOAD_TYPE(mod_decimal, "Decimal",            Decimal_type);
  LOAD_TYPE(mod_fractions, "Fraction",         Fraction_type);

  /* copyreg pieces */
  module_state.copyreg_dispatch = PyObject_GetAttrString(mod_copyreg, "dispatch_table");
  if (!module_state.copyreg_dispatch || !PyDict_Check(module_state.copyreg_dispatch)) {
    PyErr_SetString(PyExc_ImportError, "copium: copyreg.dispatch_table missing or not a dict");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }

  module_state.copyreg_newobj = PyObject_GetAttrString(mod_copyreg, "__newobj__");
  if (!module_state.copyreg_newobj) {
    PyErr_Clear();
    module_state.copyreg_newobj = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
    if (!module_state.copyreg_newobj) {
      Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
      Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
      cleanup_on_init_failure();
      return -1;
    }
  }
  module_state.copyreg_newobj_ex = PyObject_GetAttrString(mod_copyreg, "__newobj_ex__");
  if (!module_state.copyreg_newobj_ex) {
    PyErr_Clear();
    module_state.copyreg_newobj_ex = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
    if (!module_state.copyreg_newobj_ex) {
      Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
      Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
      cleanup_on_init_failure();
      return -1;
    }
  }

  PyObject* mod_copy = PyImport_ImportModule("copy");
  if (!mod_copy) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to import copy module");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }
  module_state.copy_Error = PyObject_GetAttrString(mod_copy, "Error");
  Py_DECREF(mod_copy);
  if (!module_state.copy_Error || !PyExceptionClass_Check(module_state.copy_Error)) {
    PyErr_SetString(PyExc_ImportError, "copium: copy.Error missing or not an exception");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  if (PyThread_tss_create(&module_state.watch_tss) != 0) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to create watch TSS");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }
  module_state.dict_watcher_id = PyDict_AddWatcher(dict_watch_callback);
  if (module_state.dict_watcher_id < 0) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to allocate dict watcher id");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }
#endif

  if (PyThread_tss_create(&module_state.recdepth_tss) != 0) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to create recursion TSS");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }

  if (PyThread_tss_create(&module_state.memo_tss) != 0) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to create memo TSS");
    Py_XDECREF(mod_types); Py_XDECREF(mod_builtins); Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg); Py_XDECREF(mod_re); Py_XDECREF(mod_decimal); Py_XDECREF(mod_fractions);
    cleanup_on_init_failure();
    return -1;
  }

  Py_DECREF(mod_types); Py_DECREF(mod_builtins); Py_DECREF(mod_weakref);
  Py_DECREF(mod_copyreg); Py_DECREF(mod_re); Py_DECREF(mod_decimal); Py_DECREF(mod_fractions);

  if (memo_ready_types() < 0) { cleanup_on_init_failure(); return -1; }

  /* Optional duper.snapshots + pin types */
  {
    PyObject* mod_snapshots = PyImport_ImportModule("duper.snapshots");
    if (!mod_snapshots) {
      PyErr_Clear();
      module_state.create_precompiler_reconstructor = NULL;
      /* still add pin types (they live in _pinning.c) even if snapshots are absent */
      if (_duper_pinning_add_types(module) < 0) {
        cleanup_on_init_failure();
        return -1;
      }
    } else {
      module_state.create_precompiler_reconstructor =
          PyObject_GetAttrString(mod_snapshots, "create_precompiler_reconstructor");
      if (!module_state.create_precompiler_reconstructor) PyErr_Clear();
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