/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * copium
 * - Fast, native deepcopy with reduce protocol + keepalive memo
 * - Dict watcher acceleration (3.12+)
 * - Pin integration via _pinning.c (Pin/PinsProxy + APIs)
 *
 * Public API:
 *   deepcopy(x, memo=None) -> any
 *   pin(obj) -> Pin
 *   unpin(obj, *, strict=False) -> None
 *   pinned(obj) -> Pin | None
 *   clear_pins() -> None
 *   get_pins() -> Mapping[int, Pin] (live)
 *
 * Python 3.10–3.14 compatible. Uses dict watcher when available (3.12+).
 */
#define PY_VERSION_3_11_HEX 0x030B0000
#define PY_VERSION_3_12_HEX 0x030C0000
#define PY_VERSION_3_13_HEX 0x030D0000
#define PY_VERSION_3_14_HEX 0x030E0000

#define PY_SSIZE_T_CLEAN

/* Enable GNU extensions so pthread_getattr_np is declared on Linux */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#define Py_BUILD_CORE
#include <stddef.h>  /* ptrdiff_t */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#include "Python.h"
#include "pycore_object.h"  // _PyNone_Type, _PyNotImplemented_Type
// _PyDict_NewPresized/_PyDict_*KnownHas
#if PY_VERSION_HEX < PY_VERSION_3_11_HEX
#include "dictobject.h"
#else
#include "pycore_dict.h"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* ---------------- Pin integration surface (provided by _pinning.c) ---------
 */
/* Forward declaration must match the layout used in _pinning.c */
typedef struct {
  PyObject_HEAD PyObject* snapshot; /* duper.snapshots.Snapshot */
  PyObject* factory;                /* callable reconstruct() */
  uint64_t hits;                    /* native counter */
} PinObject;

/* Lookup currently pinned object; returns borrowed PinObject* or NULL */
extern PinObject* _duper_lookup_pin_for_object(PyObject* obj);

/* Add Pin and PinsProxy types to the module (called during init) */
extern int _duper_pinning_add_types(PyObject* module);

/* Python-callable functions implemented in _pinning.c */
extern PyObject* py_pin(PyObject* self, PyObject* obj);
extern PyObject* py_unpin(PyObject* self,
                          PyObject* const* args,
                          Py_ssize_t nargs,
                          PyObject* kwnames);
extern PyObject* py_pinned(PyObject* self, PyObject* obj);
extern PyObject* py_clear_pins(PyObject* self, PyObject* noargs);
extern PyObject* py_get_pins(PyObject* self, PyObject* noargs);

/* ---------------- Memo integration surface (provided by _memo.c) -----------
 */

/* Forward declaration for MemoObject (opaque) */
typedef struct _MemoObject MemoObject;

/* Memo type (internal) */
extern PyTypeObject Memo_Type;

/* Create a new Memo object */
extern PyObject* Memo_New(void);

/* Lookup/store value for key (borrowed lookup or NULL) */
extern PyObject* memo_lookup_obj(PyObject* memo, void* key);
extern int       memo_store_obj(PyObject* memo, void* key, PyObject* value);

/* Hash-aware fast-paths for C memo */
extern PyObject* memo_lookup_obj_h(PyObject* memo, void* key, Py_ssize_t khash);
extern int       memo_store_obj_h(PyObject* memo, void* key, PyObject* value, Py_ssize_t khash);

/* Exported pointer hasher (same as C memo table) */
extern Py_ssize_t memo_hash_pointer(void* key);

/* Keepalive unification helpers (new) */
extern int memo_keepalive_ensure(PyObject** memo_ptr, PyObject** keep_proxy_ptr);
extern int memo_keepalive_append(PyObject** memo_ptr, PyObject** keep_proxy_ptr, PyObject* obj);

/* Ready both Memo and _KeepList types */
extern int memo_ready_types(void);

/* ------------------------------ Small helpers ------------------------------
 */

#define XDECREF_CLEAR(op) \
  do {                    \
    Py_XDECREF(op);       \
    (op) = NULL;          \
  } while (0)

/* ------------------------------ Module state --------------------------------
 * Cache frequently used objects/types. Pin-specific caches live in _pinning.c.
 */
typedef struct {
  /* interned strings */
  PyObject* str_reduce_ex;
  PyObject* str_reduce;
  PyObject* str_deepcopy;
  PyObject* str_setstate;
  PyObject* str_dict;
  PyObject* str_append;
  PyObject* str_update;

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
  PyObject* copyreg_dispatch; /* dict */
  PyObject* copy_Error;       /* exception class */
  PyObject*
      create_precompiler_reconstructor; /* duper.snapshots.create_precompiler_reconstructor */

  /* recursion depth guard (thread-local counter for safe deepcopy) */
  Py_tss_t recdepth_tss;

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  int dict_watcher_id;
  Py_tss_t watch_tss;
#endif
} ModuleState;

static ModuleState module_state = {0};

/* ------------------------------ Atomic checks ------------------------------
 */

static inline int is_method_type_exact(PyObject* obj) {
  return Py_TYPE(obj) == module_state.MethodType;
}

// assumes C99+, CPython C API
static inline int is_atomic_immutable(PyObject *obj) {
    PyTypeObject *t = Py_TYPE(obj);

    // Tier 1: very common, branchless
    unsigned long r =
        (obj == Py_None) |
        (t == &PyLong_Type) |
        (t == &PyUnicode_Type) |
        (t == &PyBool_Type) |
        (t == &PyFloat_Type) |
        (t == &PyBytes_Type) |
        (t == &PyComplex_Type);
    if (r) return 1;

    // Tier 2: still cheap, branchless; deduped PyCFunction_Type
    r =
        (t == &PyRange_Type) |
        (t == &PyFunction_Type) |
        (t == &PyCFunction_Type) |
        (t == &PyProperty_Type) |
        (t == &_PyWeakref_RefType) |
        (t == &PyCode_Type) |
        (t == &PyModule_Type) |
        (t == &_PyNotImplemented_Type) |
        (t == &PyEllipsis_Type) |
        (t == module_state.re_Pattern_type) |
        (t == module_state.Decimal_type) |
        (t == module_state.Fraction_type);
    if (r) return 1;

    // Tier 3: "type objects" (rare). Fast exact + rare subtype check.
    if (t == &PyType_Type) return 1;

    // NOTE: This is intentionally last; it can be relatively expensive.
    // If available in your target CPython, a flag check is even cheaper:
    //   if (PyType_HasFeature(t, Py_TPFLAGS_TYPE_SUBCLASS)) return 1;
    // Falling back to the conservative general check:
    return PyType_IsSubtype(t, &PyType_Type);
}

/* ------------------------ Lazy memo/keepalive helpers -----------------------
 */

static inline int ensure_memo_exists(PyObject** memo_ptr) {
  if (*memo_ptr != NULL)
    return 0;
  PyObject* new_memo = Memo_New();
  if (!new_memo)
    return -1;
  *memo_ptr = new_memo;
  return 0;
}

/* Hash-aware variants: use C memo fast path if available */
static inline PyObject* memo_lookup_h(PyObject** memo_ptr, void* key, Py_ssize_t khash) {
  PyObject* memo = *memo_ptr;
  if (memo == NULL)
    return NULL;
  if (Py_TYPE(memo) == &Memo_Type) {
    return memo_lookup_obj_h(memo, key, khash);
  }
  return memo_lookup_obj(memo, key);
}

static inline int memo_store_h(PyObject** memo_ptr, void* key, PyObject* value, Py_ssize_t khash) {
  if (ensure_memo_exists(memo_ptr) < 0)
    return -1;
  if (Py_TYPE(*memo_ptr) == &Memo_Type) {
    return memo_store_obj_h(*memo_ptr, key, value, khash);
  }
  return memo_store_obj(*memo_ptr, key, value);
}

/* Unified keepalive: delegate to _memo.c helpers */
static inline int keepalive_append_if_different(PyObject** memo_ptr,
                                                PyObject** keepalive_list_ptr,
                                                PyObject* original_obj,
                                                PyObject* copied_obj) {
  if (copied_obj == original_obj)
    return 0;
  return memo_keepalive_append(memo_ptr, keepalive_list_ptr, original_obj);
}

static inline int keepalive_append_original(PyObject** memo_ptr,
                                            PyObject** keepalive_list_ptr,
                                            PyObject* original_obj) {
  return memo_keepalive_append(memo_ptr, keepalive_list_ptr, original_obj);
}

/* ------------------------- Recursion depth guard (stack-space cap) ---------- */
/* Goal: prevent SIGSEGV from C stack overflow with *minimal* overhead.
   Strategy: per-thread cached stack bounds (from OS) + stride-sampled check.
   - On macOS: pthread_get_stackaddr_np / pthread_get_stacksize_np
   - On Linux:  pthread_getattr_np / pthread_attr_getstack
   - Elsewhere: fall back to previous TSS-based depth/limit guard.

   Overhead per guarded frame:
     - Always: ++depth (TLS) + one bit-test.
     - Every COPIUM_STACKCHECK_STRIDE frames: 1 pointer compare against cached low-water mark.
*/

#ifndef COPIUM_STACKCHECK_STRIDE
#define COPIUM_STACKCHECK_STRIDE 32u
#endif

/* Safety margin above the OS guard page to allow a few more frames even if
   the stride delays the check. Keep generous but small enough not to bite into
   useful stack. */
#ifndef COPIUM_STACK_SAFETY_MARGIN
#define COPIUM_STACK_SAFETY_MARGIN (256u * 1024u)  /* 256 KiB */
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static _Thread_local unsigned int _copium_tls_depth = 0;
static _Thread_local int          _copium_tls_stack_inited = 0;
static _Thread_local char*        _copium_tls_stack_low = NULL;   /* lowest usable addr + safety */
static _Thread_local char*        _copium_tls_stack_high = NULL;  /* highest addr (mostly informational) */
#endif

static inline void _copium_stack_init_if_needed(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  if (_copium_tls_stack_inited) return;
  _copium_tls_stack_inited = 1;

#if defined(__APPLE__)
  /* macOS: base is the *high* address; stack grows downward. */
  pthread_t t = pthread_self();
  size_t sz = pthread_get_stacksize_np(t);
  void* base = pthread_get_stackaddr_np(t);

  char* high = (char*)base;
  char* low  = high - (ptrdiff_t)sz;

  /* Apply a safety margin above the OS guard page */
  if ((size_t)sz > COPIUM_STACK_SAFETY_MARGIN) {
    low += COPIUM_STACK_SAFETY_MARGIN;
  }

  _copium_tls_stack_low  = low;
  _copium_tls_stack_high = high;
#elif defined(__linux__)
  /* Linux: attr stackaddr is the *low* address of the reserved stack region. */
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* addr = NULL;
    size_t sz = 0;
    if (pthread_attr_getstack(&attr, &addr, &sz) == 0 && addr && sz) {
      char* low  = (char*)addr;
      char* high = low + (ptrdiff_t)sz;

      /* Apply safety margin above guard page (at the low end) */
      if (sz > COPIUM_STACK_SAFETY_MARGIN) {
        low += COPIUM_STACK_SAFETY_MARGIN;
      }

      _copium_tls_stack_low  = low;
      _copium_tls_stack_high = high;
    }
    pthread_attr_destroy(&attr);
  }
#else
  /* Other platforms: leave pointers NULL -> fall back path in enter(). */
  _copium_tls_stack_low = NULL;
  _copium_tls_stack_high = NULL;
#endif

  /* Nothing else to do; if we failed to obtain bounds, low remains NULL. */
#else
  /* No C11 TLS: nothing to initialize here; we will use TSS fallback. */
#endif
}

static inline int _copium_recdepth_enter(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  unsigned int d = ++_copium_tls_depth;

  /* Sample only every N frames to keep cost negligible. */
  if (UNLIKELY((d & (COPIUM_STACKCHECK_STRIDE - 1u)) == 0u)) {
    _copium_stack_init_if_needed();

    if (_copium_tls_stack_low) {
      /* Compare current stack pointer to low-water mark. */
      char sp_probe;
      char* sp = (char*)&sp_probe;

      /* Most platforms grow downward; if sp <= low, we're in the danger zone. */
      if (UNLIKELY(sp <= _copium_tls_stack_low)) {
        _copium_tls_depth--;
        PyErr_SetString(PyExc_RecursionError,
                        "maximum recursion depth exceeded in copium.deepcopy (stack safety cap)");
        return -1;
      }
    } else {
      /* No OS stack bounds available: TSS-based limit fallback (rare path). */
      uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
      uintptr_t next = depth_u + 1;

      int limit = Py_GetRecursionLimit();
      if (limit > 10000) { limit = 10000; }

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
  /* No C11 TLS: fall back entirely to the existing TSS/limit accounting. */
  uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
  uintptr_t next = depth_u + 1;

  int limit = Py_GetRecursionLimit();
  if (limit > 10000) { limit = 10000; }

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
  if (_copium_tls_depth > 0) {
    _copium_tls_depth--;
  }
#else
  uintptr_t depth_u = (uintptr_t)PyThread_tss_get(&module_state.recdepth_tss);
  if (depth_u > 0) {
    (void)PyThread_tss_set(&module_state.recdepth_tss, (void*)(depth_u - 1));
  }
#endif
}

/* ------------------------- Dict watcher (3.12+) ----------------------------
 */

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
  if (top)
    watch_stack_set_current(top->prev);
}

static int dict_watch_callback(PyDict_WatchEvent event,
                               PyObject* dict,
                               PyObject* key,
                               PyObject* new_value) {
  (void)key;
  (void)new_value;
  switch (event) {
    case PyDict_EVENT_ADDED:
    case PyDict_EVENT_MODIFIED:
    case PyDict_EVENT_DELETED:
    case PyDict_EVENT_CLONED:
    case PyDict_EVENT_DEALLOCATED: {
      for (WatchContext* ctx = watch_stack_get_current(); ctx;
           ctx = ctx->prev) {
        if (ctx->dict == dict) {
          *(ctx->mutated_flag) = 1;
          break;
        }
      }
      break;
    }
    default:
      break;
  }
  return 0;
}
#endif

// On < 3.13, _PyDict_Next exists and never errors.
// On >= 3.13, we synthesize the hash via PyObject_Hash and can error (-1).

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX /* 3.13.0 */
PyAPI_FUNC(int) _PyDict_Next(PyObject* mp,
                             Py_ssize_t* ppos,
                             PyObject** pkey,
                             PyObject** pvalue,
                             Py_hash_t* phash);
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
  if (!has_next)
    return 0;
  if (hash_ptr) {
    Py_hash_t computed_hash = PyObject_Hash(*key_ptr);
    if (computed_hash == -1 && PyErr_Occurred())
      return -1;
    *hash_ptr = computed_hash;
  }
  return 1;
#endif
}

/* ---------------------------- Deepcopy internals ---------------------------
 */

#define RETURN_IF_EMPTY(check_condition, make_empty_expr)          \
  do {                                                             \
    if ((check_condition)) {                                       \
      /* Pin-accelerated empty branch: consult _pinning.c first */ \
      PinObject* __pin = _duper_lookup_pin_for_object(source_obj); \
      if (__pin) {                                                 \
        PyObject* __res = PyObject_CallNoArgs(__pin->factory);     \
        if (__res) {                                               \
          __pin->hits += 1;                                        \
        }                                                          \
        return __res;                                              \
      }                                                            \
      return (make_empty_expr);                                    \
    }                                                              \
  } while (0)

static PyObject* deepcopy_recursive_impl(PyObject* source_obj,
                                         PyObject** memo_ptr,
                                         PyObject** keepalive_list_ptr,
                                         int skip_atomic_check);
static PyObject* deepcopy_recursive_skip_atomic(PyObject* source_obj,
                                                PyObject** memo_ptr,
                                                PyObject** keepalive_list_ptr);

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
static PyObject* deepcopy_dict_with_watcher(PyObject* source_obj,
                                            PyObject** memo_ptr,
                                            PyObject** keepalive_list_ptr,
                                            void* object_id,
                                            Py_ssize_t object_id_hash);
#endif

static PyObject* deepcopy_dict_legacy(PyObject* source_obj,
                                      PyObject** memo_ptr,
                                      PyObject** keepalive_list_ptr,
                                      void* object_id,
                                      Py_ssize_t object_id_hash);

static PyObject* deepcopy_dict(PyObject* source_obj,
                               PyObject** memo_ptr,
                               PyObject** keepalive_list_ptr,
                               void* object_id,
                               Py_ssize_t object_id_hash);

static PyObject* deepcopy_list(PyObject* source_obj,
                               PyObject** memo_ptr,
                               PyObject** keepalive_list_ptr,
                               void* object_id,
                               Py_ssize_t object_id_hash);

static PyObject* deepcopy_tuple(PyObject* source_obj,
                                PyObject** memo_ptr,
                                PyObject** keepalive_list_ptr,
                                void* object_id,
                                Py_ssize_t object_id_hash);

static PyObject* deepcopy_set(PyObject* source_obj,
                              PyObject** memo_ptr,
                              PyObject** keepalive_list_ptr,
                              void* object_id,
                              Py_ssize_t object_id_hash);

static PyObject* deepcopy_frozenset(PyObject* source_obj,
                                    PyObject** memo_ptr,
                                    PyObject** keepalive_list_ptr,
                                    void* object_id,
                                    Py_ssize_t object_id_hash);

static PyObject* deepcopy_bytearray(PyObject* source_obj,
                                    PyObject** memo_ptr,
                                    PyObject** keepalive_list_ptr,
                                    void* object_id,
                                    Py_ssize_t object_id_hash);

/* ------------------------ Ergonomic atomic-aware copy helpers --------------- */
/* Expression-form helpers: return a PyObject* so callers can write
     PyObject* dst = deepcopy_likely_atomic(src, memo_ptr, keepalive_ptr);
   They encapsulate the atomic fast-path and recursive slow-path, avoiding
   awkward assignment macros and nested invocations. */

static inline PyObject*
deepcopy_likely_atomic(PyObject* src, PyObject** memo_ptr, PyObject** keepalive_ptr) {
  if (LIKELY(is_atomic_immutable(src))) {
    return Py_NewRef(src);
  }
  return deepcopy_recursive_skip_atomic(src, memo_ptr, keepalive_ptr);
}

static inline PyObject*
deepcopy(PyObject* src, PyObject** memo_ptr, PyObject** keepalive_ptr) {
  if (is_atomic_immutable(src)) {
    return Py_NewRef(src);
  }
  return deepcopy_recursive_skip_atomic(src, memo_ptr, keepalive_ptr);
}


/* --------------------------------------------------------------------------- */

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
static PyObject* deepcopy_dict_with_watcher(PyObject* source_obj,
                                            PyObject** memo_ptr,
                                            PyObject** keepalive_list_ptr,
                                            void* object_id,
                                            Py_ssize_t object_id_hash) {
  int dict_was_mutated = 0;
  WatchContext watch_ctx = {
      .dict = source_obj, .mutated_flag = &dict_was_mutated, .prev = NULL};
  watch_context_push(&watch_ctx);
  if (UNLIKELY(module_state.dict_watcher_id < 0 ||
               PyDict_Watch(module_state.dict_watcher_id, source_obj) < 0)) {
    watch_context_pop();
    Py_RETURN_NONE; /* fallback */
  }

  Py_ssize_t expected_size = PyDict_Size(source_obj);

  PyObject* copied_dict = PyDict_New();
  if (!copied_dict) {
    (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
    watch_context_pop();
    return NULL;
  }
  if (memo_store_h(memo_ptr, object_id, copied_dict, object_id_hash) < 0) {
    (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
    watch_context_pop();
    Py_DECREF(copied_dict);
    return NULL;
  }

  Py_ssize_t iteration_pos = 0;
  PyObject *dict_key, *dict_value;
  Py_hash_t key_hash;

  while (dict_iterate_with_hash(source_obj, &iteration_pos, &dict_key,
                                &dict_value, &key_hash)) {
    Py_INCREF(dict_key);
    Py_INCREF(dict_value);

    PyObject* copied_key =
        deepcopy_likely_atomic(dict_key, memo_ptr, keepalive_list_ptr);

    PyObject* copied_value = NULL;
    if (copied_key) {
      copied_value =
          deepcopy(dict_value, memo_ptr, keepalive_list_ptr);
    }

    Py_DECREF(dict_key);
    Py_DECREF(dict_value);

    if (!copied_key || !copied_value) {
      Py_XDECREF(copied_key);
      Py_XDECREF(copied_value);
      (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
      watch_context_pop();
      Py_DECREF(copied_dict);
      return NULL;
    }

    int insert_result;
#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
    if (copied_key == dict_key) {
      insert_result = _PyDict_SetItem_KnownHash(copied_dict, copied_key,
                                                copied_value, key_hash);
    } else {
      /* Key object changed: let dict compute new hash for copied_key. */
      insert_result = PyDict_SetItem(copied_dict, copied_key, copied_value);
    }
#else
    /* _PyDict_SetItem_KnownHash removed in 3.14, always compute hash */
    insert_result = PyDict_SetItem(copied_dict, copied_key, copied_value);
#endif
    if (insert_result < 0) {
      Py_DECREF(copied_key);
      Py_DECREF(copied_value);
      (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
      watch_context_pop();
      Py_DECREF(copied_dict);
      return NULL;
    }

    Py_DECREF(copied_key);
    Py_DECREF(copied_value);

    if (UNLIKELY(dict_was_mutated)) {
      (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
      watch_context_pop();
      Py_DECREF(copied_dict);
      PyErr_SetString(PyExc_RuntimeError,
                      "dictionary changed size during iteration");
      return NULL;
    }
  }

  if (UNLIKELY(dict_was_mutated || PyDict_Size(source_obj) != expected_size)) {
    (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
    watch_context_pop();
    Py_DECREF(copied_dict);
    PyErr_SetString(PyExc_RuntimeError,
                    "dictionary changed size during iteration");
    return NULL;
  }

  (void)PyDict_Unwatch(module_state.dict_watcher_id, source_obj);
  watch_context_pop();

  if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                    source_obj, copied_dict) < 0) {
    Py_DECREF(copied_dict);
    return NULL;
  }
  return copied_dict;
}
#endif

static PyObject* deepcopy_dict_legacy(PyObject* source_obj,
                                      PyObject** memo_ptr,
                                      PyObject** keepalive_list_ptr,
                                      void* object_id,
                                      Py_ssize_t object_id_hash) {
  Py_ssize_t expected_size = PyDict_Size(source_obj);
  PyObject* copied_dict = PyDict_New();
  if (!copied_dict)
    return NULL;
  if (memo_store_h(memo_ptr, object_id, copied_dict, object_id_hash) < 0) {
    Py_DECREF(copied_dict);
    return NULL;
  }

  Py_ssize_t iteration_pos = 0;
  PyObject *dict_key, *dict_value;
  while (PyDict_Next(source_obj, &iteration_pos, &dict_key, &dict_value)) {
    Py_INCREF(dict_key);
    Py_INCREF(dict_value);

    PyObject* copied_key =
        deepcopy_likely_atomic(dict_key, memo_ptr, keepalive_list_ptr);
    if (!copied_key) {
      Py_DECREF(dict_key);
      Py_DECREF(dict_value);
      Py_DECREF(copied_dict);
      return NULL;
    }

    if (UNLIKELY(PyDict_Size(source_obj) != expected_size)) {
      Py_DECREF(copied_key);
      Py_DECREF(dict_key);
      Py_DECREF(dict_value);
      Py_DECREF(copied_dict);
      PyErr_SetString(PyExc_RuntimeError,
                      "dictionary changed size during iteration");
      return NULL;
    }

    PyObject* copied_value = deepcopy(dict_value, memo_ptr, keepalive_list_ptr);
    if (!copied_value) {
      Py_DECREF(copied_key);
      Py_DECREF(dict_key);
      Py_DECREF(dict_value);
      Py_DECREF(copied_dict);
      return NULL;
    }

    if (UNLIKELY(PyDict_Size(source_obj) != expected_size)) {
      Py_DECREF(copied_key);
      Py_DECREF(copied_value);
      Py_DECREF(dict_key);
      Py_DECREF(dict_value);
      Py_DECREF(copied_dict);
      PyErr_SetString(PyExc_RuntimeError,
                      "dictionary changed size during iteration");
      return NULL;
    }

    Py_DECREF(dict_key);
    Py_DECREF(dict_value);
    if (PyDict_SetItem(copied_dict, copied_key, copied_value) < 0) {
      Py_DECREF(copied_key);
      Py_DECREF(copied_value);
      Py_DECREF(copied_dict);
      return NULL;
    }
    Py_DECREF(copied_key);
    Py_DECREF(copied_value);
  }
  if (UNLIKELY(PyDict_Size(source_obj) != expected_size)) {
    Py_DECREF(copied_dict);
    PyErr_SetString(PyExc_RuntimeError,
                    "dictionary changed size during iteration");
    return NULL;
  }
  if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                    source_obj, copied_dict) < 0) {
    Py_DECREF(copied_dict);
    return NULL;
  }
  return copied_dict;
}

static PyObject* deepcopy_dict(PyObject* source_obj,
                               PyObject** memo_ptr,
                               PyObject** keepalive_list_ptr,
                               void* object_id,
                               Py_ssize_t object_id_hash) {
#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  return deepcopy_dict_with_watcher(source_obj, memo_ptr,
                                    keepalive_list_ptr, object_id, object_id_hash);
#endif
  return deepcopy_dict_legacy(source_obj, memo_ptr, keepalive_list_ptr,
                              object_id, object_id_hash);
}

static PyObject* deepcopy_list(PyObject* source_obj,
                               PyObject** memo_ptr,
                               PyObject** keepalive_list_ptr,
                               void* object_id,
                               Py_ssize_t object_id_hash)
{
    /* 1) Allocate a fully-initialized list (no NULL slots visible to user code). */
    Py_ssize_t initial_size = PyList_GET_SIZE(source_obj);
    PyObject* copied_list = PyList_New(initial_size);
    if (copied_list == NULL) {
        return NULL;
    }

    for (Py_ssize_t i = 0; i < initial_size; ++i) {
        #if PY_VERSION_HEX < PY_VERSION_3_12_HEX
          Py_INCREF(Py_None);
        #endif
        PyList_SET_ITEM(copied_list, i, Py_None);
    }

    /* 2) Publish to memo immediately. */
    if (memo_store_h(memo_ptr, object_id, copied_list, object_id_hash) < 0) {
        Py_DECREF(copied_list);
        return NULL;
    }

    /* 3) Deep-copy elements, guarding against concurrent list mutations. */
    for (Py_ssize_t i = 0; i < initial_size; ++i) {
        if (i >= PyList_GET_SIZE(copied_list) || i >= PyList_GET_SIZE(source_obj)) {
            break;
        }

        PyObject* item = PyList_GET_ITEM(source_obj, i);  /* borrowed */
        if (item == NULL) {
            Py_DECREF(copied_list);
            return NULL;
        }

        PyObject* copied_item;
        if (LIKELY(is_atomic_immutable(item))) {
            /* Fast path: avoid function call for atomics */
            copied_item = Py_NewRef(item);
        } else {
            /* Hold a ref while we potentially call back into Python */
            Py_XINCREF(item);
            /* already proven non-atomic above */
            copied_item = deepcopy_recursive_skip_atomic(item, memo_ptr, keepalive_list_ptr);
            Py_DECREF(item);
            if (copied_item == NULL) {
                Py_DECREF(copied_list);
                return NULL;
            }
        }

        if (i >= PyList_GET_SIZE(copied_list)) {
            Py_DECREF(copied_item);
            break;
        }
        // on 3.12 Py_None is immortal
        #if PY_VERSION_HEX < PY_VERSION_3_12_HEX
          Py_DECREF(Py_None);
        #endif
        PyList_SET_ITEM(copied_list, i, copied_item);
    }

    if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                      source_obj, copied_list) < 0) {
        Py_DECREF(copied_list);
        return NULL;
    }

    return copied_list;
}

static PyObject* deepcopy_tuple(PyObject* source_obj,
                                PyObject** memo_ptr,
                                PyObject** keepalive_list_ptr,
                                void* object_id,
                                Py_ssize_t object_id_hash) {
  Py_ssize_t tuple_length = PyTuple_GET_SIZE(source_obj);
  int all_elements_identical = 1;
  PyObject* copied_tuple = PyTuple_New(tuple_length);
  if (!copied_tuple)
    return NULL;

  for (Py_ssize_t i = 0; i < tuple_length; i++) {
    PyObject* element = PyTuple_GET_ITEM(source_obj, i);
    PyObject* copied_element =
        deepcopy_likely_atomic(element, memo_ptr, keepalive_list_ptr);
    if (!copied_element) {
      Py_DECREF(copied_tuple);
      return NULL;
    }
    if (copied_element != element)
      all_elements_identical = 0;
    PyTuple_SET_ITEM(copied_tuple, i, copied_element);
  }

  if (all_elements_identical) {
    Py_INCREF(source_obj);
    Py_DECREF(copied_tuple);
    return source_obj;
  }

  PyObject* existing_copy = memo_lookup_h(memo_ptr, object_id, object_id_hash);
  if (existing_copy) {
    Py_INCREF(existing_copy);
    Py_DECREF(copied_tuple);
    return existing_copy;
  }
  if (PyErr_Occurred()) {
    Py_DECREF(copied_tuple);
    return NULL;
  }

  if (memo_store_h(memo_ptr, object_id, copied_tuple, object_id_hash) < 0) {
    Py_DECREF(copied_tuple);
    return NULL;
  }
  if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                    source_obj, copied_tuple) < 0) {
    Py_DECREF(copied_tuple);
    return NULL;
  }
  return copied_tuple;
}

static PyObject* deepcopy_set(PyObject* source_obj,
                              PyObject** memo_ptr,
                              PyObject** keepalive_list_ptr,
                              void* object_id,
                              Py_ssize_t object_id_hash) {
  PyObject* copied_set = PySet_New(NULL);
  if (!copied_set)
    return NULL;
  if (memo_store_h(memo_ptr, object_id, copied_set, object_id_hash) < 0) {
    Py_DECREF(copied_set);
    return NULL;
  }
  PyObject* iterator = PyObject_GetIter(source_obj);
  if (!iterator) {
    Py_DECREF(copied_set);
    return NULL;
  }
  PyObject* item;
  while ((item = PyIter_Next(iterator)) != NULL) {
    PyObject* copied_item =
        deepcopy_likely_atomic(item, memo_ptr, keepalive_list_ptr);
    if (!copied_item) {
      Py_DECREF(item);
      Py_DECREF(iterator);
      Py_DECREF(copied_set);
      return NULL;
    }
    Py_DECREF(item);
    if (PySet_Add(copied_set, copied_item) < 0) {
      Py_DECREF(copied_item);
      Py_DECREF(iterator);
      Py_DECREF(copied_set);
      return NULL;
    }
    Py_DECREF(copied_item);
  }
  Py_DECREF(iterator);
  if (PyErr_Occurred()) {
    Py_DECREF(copied_set);
    return NULL;
  }
  if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                    source_obj, copied_set) < 0) {
    Py_DECREF(copied_set);
    return NULL;
  }
  return copied_set;
}

static PyObject* deepcopy_frozenset(PyObject* source_obj,
                                    PyObject** memo_ptr,
                                    PyObject** keepalive_list_ptr,
                                    void* object_id,
                                    Py_ssize_t object_id_hash) {
  (void)object_id_hash; /* no early store for frozenset (constructed at end) */
  PyObject* iterator = PyObject_GetIter(source_obj);
  if (!iterator)
    return NULL;
  PyObject* temp_list = PyList_New(0);
  if (!temp_list) {
    Py_DECREF(iterator);
    return NULL;
  }
  PyObject* item;
  while ((item = PyIter_Next(iterator)) != NULL) {
    PyObject* copied_item =
        deepcopy_likely_atomic(item, memo_ptr, keepalive_list_ptr);
    if (!copied_item) {
      Py_DECREF(item);
      Py_DECREF(iterator);
      Py_DECREF(temp_list);
      return NULL;
    }
    if (PyList_Append(temp_list, copied_item) < 0) {
      Py_DECREF(copied_item);
      Py_DECREF(item);
      Py_DECREF(iterator);
      Py_DECREF(temp_list);
      return NULL;
    }
    Py_DECREF(copied_item);
    Py_DECREF(item);
  }
  Py_DECREF(iterator);
  if (PyErr_Occurred()) {
    Py_DECREF(temp_list);
    return NULL;
  }

  PyObject* copied_frozenset = PyFrozenSet_New(temp_list);
  Py_DECREF(temp_list);
  if (!copied_frozenset)
    return NULL;
  if (memo_store_h(memo_ptr, object_id, copied_frozenset, object_id_hash) < 0) {
    Py_DECREF(copied_frozenset);
    return NULL;
  }
  if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                    source_obj, copied_frozenset) < 0) {
    Py_DECREF(copied_frozenset);
    return NULL;
  }
  return copied_frozenset;
}

static PyObject* deepcopy_bytearray(PyObject* source_obj,
                                    PyObject** memo_ptr,
                                    PyObject** keepalive_list_ptr,
                                    void* object_id,
                                    Py_ssize_t object_id_hash) {
  Py_ssize_t byte_length = PyByteArray_Size(source_obj);
  PyObject* copied_bytearray = PyByteArray_FromStringAndSize(NULL, byte_length);
  if (!copied_bytearray)
    return NULL;
  if (byte_length)
    memcpy(PyByteArray_AS_STRING(copied_bytearray),
           PyByteArray_AS_STRING(source_obj), (size_t)byte_length);
  if (memo_store_h(memo_ptr, object_id, copied_bytearray, object_id_hash) < 0) {
    Py_DECREF(copied_bytearray);
    return NULL;
  }
  if (keepalive_append_if_different(memo_ptr, keepalive_list_ptr,
                                    source_obj, copied_bytearray) < 0) {
    Py_DECREF(copied_bytearray);
    return NULL;
  }
  return copied_bytearray;
}

/* ----------------------------- reduce protocol -----------------------------
 */

static PyObject* try_reduce_via_registry(PyObject* obj,
                                         PyTypeObject* obj_type) {
  PyObject* reducer_func = PyDict_GetItemWithError(
      module_state.copyreg_dispatch, (PyObject*)obj_type);
  if (!reducer_func) {
    if (PyErr_Occurred())
      return NULL;
    return NULL;
  }
  if (!PyCallable_Check(reducer_func)) {
    PyErr_SetString(PyExc_TypeError,
                    "copyreg.dispatch_table value is not callable");
    return NULL;
  }
  return PyObject_CallOneArg(reducer_func, obj);
}

static PyObject* call_reduce_method_preferring_ex(PyObject* obj) {
  PyObject* reduce_ex_method =
      PyObject_GetAttr(obj, module_state.str_reduce_ex);
  if (reduce_ex_method) {
    PyObject* reduce_result = PyObject_CallFunction(reduce_ex_method, "i", 4);
    Py_DECREF(reduce_ex_method);
    if (reduce_result)
      return reduce_result;
    return NULL;
  }
  PyErr_Clear();
  PyObject* reduce_method = PyObject_GetAttr(obj, module_state.str_reduce);
  if (reduce_method) {
    PyObject* reduce_result = PyObject_CallNoArgs(reduce_method);
    Py_DECREF(reduce_method);
    return reduce_result;
  }
  PyErr_Clear();
  PyErr_SetString((PyObject*)module_state.copy_Error,
                  "un(deep)copyable object (no reduce protocol)");
  return NULL;
}

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
static int get_optional_attr(PyObject* obj, PyObject* name, PyObject** out) {
  *out = PyObject_GetAttr(obj, name);
  if (*out)
    return 1;  // found
  if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
    PyErr_Clear();
    return 0;  // not found (and error cleared)
  }
  return -1;  // real error (still set)
}
#define PyObject_GetOptionalAttr(obj, name, out) \
  get_optional_attr((obj), (name), (out))
#endif

static int unpack_reduce_result_tuple(PyObject* reduce_result,
                                      PyObject** out_constructor,
                                      PyObject** out_args,
                                      PyObject** out_state,
                                      PyObject** out_list_iterator,
                                      PyObject** out_dict_iterator) {
  if (!PyTuple_Check(reduce_result)) {
    if (PyUnicode_Check(reduce_result) || PyBytes_Check(reduce_result)) {
      *out_constructor = *out_args = *out_state = *out_list_iterator =
          *out_dict_iterator = NULL;
      return 1;
    }
    PyErr_SetString(PyExc_TypeError, "__reduce__ must return a tuple or str");
    return -1;
  }
  Py_ssize_t tuple_size = PyTuple_GET_SIZE(reduce_result);
  if (tuple_size < 2 || tuple_size > 5) {
    PyErr_SetString(PyExc_TypeError,
                    "__reduce__ tuple length must be in [2,5]");
    return -1;
  }
  PyObject* constructor = PyTuple_GET_ITEM(reduce_result, 0);
  PyObject* args = PyTuple_GET_ITEM(reduce_result, 1);
  if (!PyCallable_Check(constructor) || !PyTuple_Check(args)) {
    PyErr_SetString(PyExc_TypeError,
                    "__reduce__ first two items must be (callable, tuple)");
    return -1;
  }
  *out_constructor = constructor;
  *out_args = args;
  *out_state = (tuple_size >= 3) ? PyTuple_GET_ITEM(reduce_result, 2) : NULL;
  *out_list_iterator =
      (tuple_size >= 4) ? PyTuple_GET_ITEM(reduce_result, 3) : NULL;
  *out_dict_iterator =
      (tuple_size == 5) ? PyTuple_GET_ITEM(reduce_result, 4) : NULL;
  return 0;
}

/* ------------------------ dispatcher core (skip_atomic_check) -------------- */

static PyObject* deepcopy_recursive_impl(PyObject* source_obj,
                                         PyObject** memo_ptr,
                                         PyObject** keepalive_list_ptr,
                                         int skip_atomic_check) {
  if (!skip_atomic_check) {
    if (is_atomic_immutable(source_obj))
      return Py_NewRef(source_obj);
  }

  /* Consult pins via integration hook */
  {
    PinObject* pin = _duper_lookup_pin_for_object(source_obj);
    if (pin) {
      PyObject* result = PyObject_CallNoArgs(pin->factory);
      if (result)
        pin->hits += 1;
      return result;
    }
  }

  void* object_id = (void*)source_obj;
  Py_ssize_t object_id_hash = memo_hash_pointer(object_id);

  PyObject* memo_hit = memo_lookup_h(memo_ptr, object_id, object_id_hash);
  if (memo_hit) {
    Py_INCREF(memo_hit);
    return memo_hit;
  }

  if (PyList_CheckExact(source_obj)) {
    PyObject* result =
        deepcopy_list(source_obj, memo_ptr, keepalive_list_ptr, object_id, object_id_hash);
    return result;
  }
  if (PyTuple_CheckExact(source_obj)) {
    PyObject* result = deepcopy_tuple(source_obj, memo_ptr,
                                      keepalive_list_ptr, object_id, object_id_hash);
    return result;
  }
  if (PyDict_CheckExact(source_obj)) {
    PyObject* result =
        deepcopy_dict(source_obj, memo_ptr, keepalive_list_ptr, object_id, object_id_hash);
    return result;
  }
  if (PySet_CheckExact(source_obj)) {
    PyObject* result =
        deepcopy_set(source_obj, memo_ptr, keepalive_list_ptr, object_id, object_id_hash);
    return result;
  }
  if (PyFrozenSet_CheckExact(source_obj)) {
    PyObject* result = deepcopy_frozenset(source_obj, memo_ptr,
                                          keepalive_list_ptr, object_id, object_id_hash);
    return result;
  }
  if (PyByteArray_CheckExact(source_obj)) {
    PyObject* result = deepcopy_bytearray(source_obj, memo_ptr,
                                          keepalive_list_ptr, object_id, object_id_hash);
    return result;
  }

  if (PyModule_Check(source_obj)) {
    return Py_NewRef(source_obj);
  }

  if (is_method_type_exact(source_obj)) {
    PyObject* method_function = PyMethod_GET_FUNCTION(source_obj);
    PyObject* method_self = PyMethod_GET_SELF(source_obj);
    if (!method_function || !method_self) {
      return NULL;
    }
    Py_INCREF(method_function);
    Py_INCREF(method_self);
    /* safe: types.MethodType implies 'self' is a user instance, not an atomic immutable */
    PyObject* copied_self =
        deepcopy_recursive_skip_atomic(method_self, memo_ptr, keepalive_list_ptr);
    if (!copied_self) {
      Py_DECREF(method_function);
      Py_DECREF(method_self);
      return NULL;
    }
    PyObject* bound_method = PyMethod_New(method_function, copied_self);
    Py_DECREF(method_function);
    Py_DECREF(method_self);
    Py_DECREF(copied_self);
    if (!bound_method) {
      return NULL;
    }
    if (memo_store_h(memo_ptr, object_id, bound_method, object_id_hash) < 0) {
      Py_DECREF(bound_method);
      return NULL;
    }
    if (keepalive_append_original(memo_ptr, keepalive_list_ptr,
                                  source_obj) < 0) {
      Py_DECREF(bound_method);
      return NULL;
    }
    return bound_method;
  }

  if (Py_TYPE(source_obj) == &PyBaseObject_Type) {
    PyObject* new_baseobject = PyObject_New(PyObject, &PyBaseObject_Type);
    if (!new_baseobject) {
      return NULL;
    }
    if (memo_store_h(memo_ptr, object_id, new_baseobject, object_id_hash) < 0) {
      Py_DECREF(new_baseobject);
      return NULL;
    }
    if (keepalive_append_original(memo_ptr, keepalive_list_ptr,
                                  source_obj) < 0) {
      Py_DECREF(new_baseobject);
      return NULL;
    }
    return new_baseobject;
  }

  {
    PyObject* deepcopy_method = NULL;
    int has_deepcopy = PyObject_GetOptionalAttr(
        source_obj, module_state.str_deepcopy, &deepcopy_method);
    if (has_deepcopy < 0) {
      return NULL;
    }
    if (has_deepcopy) {
      /* Ensure memo is a dict per copy protocol; many __deepcopy__ expect a dict */
      if (ensure_memo_exists(memo_ptr) < 0) {
        Py_DECREF(deepcopy_method);
        return NULL;
      }
      PyObject* result = PyObject_CallOneArg(deepcopy_method, *memo_ptr);
      Py_DECREF(deepcopy_method);
      if (!result) {
        return NULL;
      }
      if (result != source_obj) {
        if (memo_store_h(memo_ptr, object_id, result, object_id_hash) < 0) {
          Py_DECREF(result);
          return NULL;
        }
        if (keepalive_append_original(memo_ptr, keepalive_list_ptr,
                                      source_obj) < 0) {
          Py_DECREF(result);
          return NULL;
        }
      }
      return result;
    }
  }

  PyTypeObject* source_type = Py_TYPE(source_obj);
  PyObject* reduce_result = try_reduce_via_registry(source_obj, source_type);
  if (!reduce_result) {
    if (PyErr_Occurred()) {
      return NULL;
    }
    reduce_result = call_reduce_method_preferring_ex(source_obj);
    if (!reduce_result) {
      return NULL;
    }
  }

  PyObject *constructor = NULL, *args = NULL, *state = NULL,
           *list_iterator = NULL, *dict_iterator = NULL;
  int unpack_result =
      unpack_reduce_result_tuple(reduce_result, &constructor, &args, &state,
                                 &list_iterator, &dict_iterator);
  if (unpack_result < 0) {
    Py_DECREF(reduce_result);
    return NULL;
  }
  if (unpack_result == 1) {
    Py_DECREF(reduce_result);
    return Py_NewRef(source_obj);
  }

  PyObject* reconstructed_obj = NULL;
  if (PyTuple_GET_SIZE(args) == 0) {
    reconstructed_obj = PyObject_CallNoArgs(constructor);
  } else {
    Py_ssize_t args_count = PyTuple_GET_SIZE(args);
    PyObject* deepcopied_args = PyTuple_New(args_count);
    if (!deepcopied_args) {
      Py_DECREF(reduce_result);
      return NULL;
    }
    for (Py_ssize_t i = 0; i < args_count; i++) {
      PyObject* arg = PyTuple_GET_ITEM(args, i);
      PyObject* copied_arg =
          deepcopy(arg, memo_ptr, keepalive_list_ptr);
      if (!copied_arg) {
        Py_DECREF(deepcopied_args);
        Py_DECREF(reduce_result);
        return NULL;
      }
      PyTuple_SET_ITEM(deepcopied_args, i, copied_arg);
    }
    reconstructed_obj = PyObject_CallObject(constructor, deepcopied_args);
    Py_DECREF(deepcopied_args);
  }
  if (!reconstructed_obj) {
    Py_DECREF(reduce_result);
    return NULL;
  }

  if (memo_store_h(memo_ptr, object_id, reconstructed_obj, object_id_hash) < 0) {
    Py_DECREF(reconstructed_obj);
    Py_DECREF(reduce_result);
    return NULL;
  }

  if (state && state != Py_None) {
    PyObject* setstate_method =
        PyObject_GetAttr(reconstructed_obj, module_state.str_setstate);
    if (setstate_method) {
      PyObject* copied_state = deepcopy(state, memo_ptr, keepalive_list_ptr);
      if (!copied_state) {
        Py_DECREF(setstate_method);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        return NULL;
      }
      PyObject* setstate_result =
          PyObject_CallOneArg(setstate_method, copied_state);
      Py_DECREF(copied_state);
      Py_DECREF(setstate_method);
      if (!setstate_result) {
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        return NULL;
      }
      Py_DECREF(setstate_result);
    } else {
      PyErr_Clear();
      if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
        PyObject* dict_state = PyTuple_GET_ITEM(state, 0);
        PyObject* slot_state = PyTuple_GET_ITEM(state, 1);

        if (slot_state && slot_state != Py_None) {
          PyObject* slot_iterator = PyObject_GetIter(slot_state);
          if (!slot_iterator) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            return NULL;
          }
          PyObject* slot_key;
          while ((slot_key = PyIter_Next(slot_iterator)) != NULL) {
            PyObject* slot_value = PyObject_GetItem(slot_state, slot_key);
            if (!slot_value) {
              Py_DECREF(slot_key);
              Py_DECREF(slot_iterator);
              Py_DECREF(reconstructed_obj);
              Py_DECREF(reduce_result);
              return NULL;
            }
            PyObject* copied_slot_value = deepcopy(slot_value, memo_ptr, keepalive_list_ptr);
            Py_DECREF(slot_value);
            if (!copied_slot_value) {
              Py_DECREF(slot_key);
              Py_DECREF(slot_iterator);
              Py_DECREF(reconstructed_obj);
              Py_DECREF(reduce_result);
              return NULL;
            }
            if (PyObject_SetAttr(reconstructed_obj, slot_key,
                                 copied_slot_value) < 0) {
              Py_DECREF(copied_slot_value);
              Py_DECREF(slot_key);
              Py_DECREF(slot_iterator);
              Py_DECREF(reconstructed_obj);
              Py_DECREF(reduce_result);
              return NULL;
            }
            Py_DECREF(copied_slot_value);
            Py_DECREF(slot_key);
          }
          Py_DECREF(slot_iterator);
          if (PyErr_Occurred()) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            return NULL;
          }
        }

        if (dict_state && dict_state != Py_None) {
          PyObject* copied_dict_state = deepcopy(dict_state, memo_ptr, keepalive_list_ptr);
          if (!copied_dict_state) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            return NULL;
          }
          PyObject* obj_dict =
              PyObject_GetAttr(reconstructed_obj, module_state.str_dict);
          if (!obj_dict) {
            Py_DECREF(copied_dict_state);
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            return NULL;
          }
          PyObject* update_method =
              PyObject_GetAttr(obj_dict, module_state.str_update);
          Py_DECREF(obj_dict);
          if (!update_method) {
            Py_DECREF(copied_dict_state);
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            return NULL;
          }
          PyObject* update_result =
              PyObject_CallOneArg(update_method, copied_dict_state);
          Py_DECREF(update_method);
          Py_DECREF(copied_dict_state);
          if (!update_result) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            return NULL;
          }
          Py_DECREF(update_result);
        }
      } else {
        PyObject* copied_dict_state = deepcopy(state, memo_ptr, keepalive_list_ptr);
        if (!copied_dict_state) {
          Py_DECREF(reconstructed_obj);
          Py_DECREF(reduce_result);
          return NULL;
        }
        PyObject* obj_dict =
            PyObject_GetAttrString(reconstructed_obj, "__dict__");
        if (!obj_dict) {
          Py_DECREF(copied_dict_state);
          Py_DECREF(reconstructed_obj);
          Py_DECREF(reduce_result);
          return NULL;
        }
        PyObject* update_result =
            PyObject_CallMethod(obj_dict, "update", "O", copied_dict_state);
        Py_DECREF(obj_dict);
        Py_DECREF(copied_dict_state);
        if (!update_result) {
          Py_DECREF(reconstructed_obj);
          Py_DECREF(reduce_result);
          return NULL;
        }
        Py_DECREF(update_result);
      }
    }
  }

  if (list_iterator && list_iterator != Py_None) {
    PyObject* append_method = PyObject_GetAttr(reconstructed_obj, module_state.str_append);
    if (!append_method) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      return NULL;
    }
    PyObject* iterator = PyObject_GetIter(list_iterator);
    if (!iterator) {
      Py_DECREF(append_method);
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      return NULL;
    }
    PyObject* item;
    while ((item = PyIter_Next(iterator)) != NULL) {
      PyObject* copied_item = deepcopy_likely_atomic(item, memo_ptr, keepalive_list_ptr);
      Py_DECREF(item);
      if (!copied_item) {
        Py_DECREF(iterator);
        Py_DECREF(append_method);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        return NULL;
      }
      PyObject* append_result = PyObject_CallOneArg(append_method, copied_item);
      Py_DECREF(copied_item);
      if (!append_result) {
        Py_DECREF(iterator);
        Py_DECREF(append_method);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        return NULL;
      }
      Py_DECREF(append_result);
    }
    Py_DECREF(iterator);
    Py_DECREF(append_method);
    if (PyErr_Occurred()) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      return NULL;
    }
  }

  if (dict_iterator && dict_iterator != Py_None) {
    PyObject* iterator = PyObject_GetIter(dict_iterator);
    if (!iterator) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      return NULL;
    }
    PyObject* pair;
    while ((pair = PyIter_Next(iterator)) != NULL) {
      PyObject* pair_key = PyTuple_GET_ITEM(pair, 0);
      PyObject* pair_value = PyTuple_GET_ITEM(pair, 1);
      Py_INCREF(pair_key);
      Py_INCREF(pair_value);

      PyObject* copied_key =
          deepcopy_likely_atomic(pair_key, memo_ptr, keepalive_list_ptr);

      PyObject* copied_value = NULL;
      if (copied_key) {
        copied_value =
            deepcopy_likely_atomic(pair_value, memo_ptr, keepalive_list_ptr);
      }

      Py_DECREF(pair_key);
      Py_DECREF(pair_value);

      if (!copied_key || !copied_value) {
        Py_XDECREF(copied_key);
        Py_XDECREF(copied_value);
        Py_DECREF(pair);
        Py_DECREF(iterator);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        return NULL;
      }
      if (PyObject_SetItem(reconstructed_obj, copied_key, copied_value) < 0) {
        Py_DECREF(copied_key);
        Py_DECREF(copied_value);
        Py_DECREF(pair);
        Py_DECREF(iterator);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        return NULL;
      }
      Py_DECREF(copied_key);
      Py_DECREF(copied_value);
      Py_DECREF(pair);
    }
    Py_DECREF(iterator);
    if (PyErr_Occurred()) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      return NULL;
    }
  }

  if (keepalive_append_original(memo_ptr, keepalive_list_ptr, source_obj) <
      0) {
    Py_DECREF(reconstructed_obj);
    Py_DECREF(reduce_result);
    return NULL;
  }
  Py_DECREF(reduce_result);
  return reconstructed_obj;
}


/* Variant used when the caller has already excluded atomic immutables.
   Still enforces recursion guard, but skips atomic predicate entirely. */
static PyObject* deepcopy_recursive_skip_atomic(PyObject* source_obj,
                                                PyObject** memo_ptr,
                                                PyObject** keepalive_list_ptr) {
  if (UNLIKELY(_copium_recdepth_enter() < 0)) {
    return NULL;
  }
  PyObject* result =
      deepcopy_recursive_impl(source_obj, memo_ptr, keepalive_list_ptr,
                              /*skip_atomic_check=*/1);
  _copium_recdepth_leave();
  return result;
}

/* -------------------------------- Public API --------------------------------
 */

PyObject* py_deepcopy(PyObject* self,
                      PyObject* const* args,
                      Py_ssize_t nargs,
                      PyObject* kwnames) {
  PyObject* source_obj = NULL;
  PyObject* memo_arg = Py_None;

  // ---------- FAST PATH A: no keywords ----------
  if (!kwnames || PyTuple_GET_SIZE(kwnames) == 0) {
    if (UNLIKELY(nargs < 1)) {
      PyErr_Format(PyExc_TypeError,
                   "deepcopy() missing 1 required positional argument: 'x'");
      return NULL;
    }
    if (UNLIKELY(nargs > 2)) {
      PyErr_Format(PyExc_TypeError,
                   "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                   nargs);
      return NULL;
    }
    source_obj = args[0];
    memo_arg = (nargs == 2) ? args[1] : Py_None;
    goto have_args;
  }

  // ---------- FAST PATH B: one keyword, and it is "memo" ----------
  {
    const Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    if (kwcount == 1) {
      PyObject* kw0 = PyTuple_GET_ITEM(kwnames, 0);
      const int is_memo =
          PyUnicode_Check(kw0) &&
          PyUnicode_CompareWithASCIIString(kw0, "memo") == 0;

      if (is_memo) {
        if (UNLIKELY(nargs < 1)) {
          PyErr_Format(PyExc_TypeError,
                       "deepcopy() missing 1 required positional argument: 'x'");
          return NULL;
        }
        if (UNLIKELY(nargs > 2)) {
          PyErr_Format(PyExc_TypeError,
                       "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                       nargs);
          return NULL;
        }
        // Positional provides x; keyword provides memo (not both!).
        if (UNLIKELY(nargs == 2)) {
          PyErr_SetString(PyExc_TypeError,
                          "deepcopy() got multiple values for argument 'memo'");
          return NULL;
        }
        source_obj = args[0];
        memo_arg = args[nargs + 0];
        goto have_args;
      }
    }

    // ---------- SLOW PATH: anything else with keywords ----------
    // Accept only "x" and "memo". Allow x=..., memo=..., either/both.
    {
      Py_ssize_t i;
      int seen_memo_kw = 0;

      if (UNLIKELY(nargs > 2)) {
        PyErr_Format(PyExc_TypeError,
                     "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                     nargs);
        return NULL;
      }

      // Seed from positionals first.
      if (nargs >= 1) {
        source_obj = args[0];
      }
      if (nargs == 2) {
        memo_arg = args[1];
      }

      const Py_ssize_t kwc = PyTuple_GET_SIZE(kwnames);
      for (i = 0; i < kwc; i++) {
        PyObject* name = PyTuple_GET_ITEM(kwnames, i);
        PyObject* val  = args[nargs + i];

        if (!(PyUnicode_Check(name))) {
          PyErr_SetString(PyExc_TypeError, "deepcopy() keywords must be strings");
          return NULL;
        }

        // name == "x" ?
        if (PyUnicode_CompareWithASCIIString(name, "x") == 0) {
          if (UNLIKELY(source_obj != NULL)) {
            PyErr_SetString(PyExc_TypeError,
                            "deepcopy() got multiple values for argument 'x'");
            return NULL;
          }
          source_obj = val;
          continue;
        }

        if (PyUnicode_CompareWithASCIIString(name, "memo") == 0) {
          if (UNLIKELY(seen_memo_kw || nargs == 2)) {
            PyErr_SetString(PyExc_TypeError,
                            "deepcopy() got multiple values for argument 'memo'");
            return NULL;
          }
          memo_arg = val;
          seen_memo_kw = 1;
          continue;
        }

        // Unknown keyword.
        PyErr_Format(PyExc_TypeError,
                     "deepcopy() got an unexpected keyword argument '%U'",
                     name);
        return NULL;
      }

      if (UNLIKELY(source_obj == NULL)) {
        PyErr_Format(PyExc_TypeError,
                     "deepcopy() missing 1 required positional argument: 'x'");
        return NULL;
      }
    }
  }

have_args:
  // --------- remaining hot logic ----------
  if (LIKELY(memo_arg == Py_None)) {
    if (LIKELY(is_atomic_immutable(source_obj))) {
      return Py_NewRef(source_obj);
    }
    PyTypeObject* source_type = Py_TYPE(source_obj);
    if (source_type == &PyList_Type) {
      RETURN_IF_EMPTY(Py_SIZE(source_obj) == 0, PyList_New(0));
    } else if (source_type == &PyTuple_Type) {
      RETURN_IF_EMPTY(Py_SIZE(source_obj) == 0, PyTuple_New(0));
    } else if (source_type == &PyDict_Type) {
#if defined(PyDict_GET_SIZE)
      RETURN_IF_EMPTY(PyDict_GET_SIZE(source_obj) == 0, PyDict_New());
#else
      RETURN_IF_EMPTY(PyDict_Size(source_obj) == 0, PyDict_New());
#endif
    } else if (source_type == &PySet_Type) {
#if defined(PySet_GET_SIZE)
      RETURN_IF_EMPTY(PySet_GET_SIZE((PySetObject*)source_obj) == 0,
                      PySet_New(NULL));
#else
      RETURN_IF_EMPTY(PySet_Size(source_obj) == 0, PySet_New(NULL));
#endif
    } else if (source_type == &PyFrozenSet_Type) {
#if defined(PySet_GET_SIZE)
      RETURN_IF_EMPTY(PySet_GET_SIZE((PySetObject*)source_obj) == 0,
                      PyFrozenSet_New(NULL));
#else
      RETURN_IF_EMPTY(PyObject_Size(source_obj) == 0, PyFrozenSet_New(NULL));
#endif
    } else if (source_type == &PyByteArray_Type) {
#if defined(PyByteArray_GET_SIZE)
      RETURN_IF_EMPTY(PyByteArray_GET_SIZE(source_obj) == 0,
                      PyByteArray_FromStringAndSize(NULL, 0));
#else
      RETURN_IF_EMPTY(PyByteArray_Size(source_obj) == 0,
                      PyByteArray_FromStringAndSize(NULL, 0));
#endif
    }

  }

  PyObject* memo_local = NULL;
  PyObject* keepalive_local = NULL;

  if (memo_arg != Py_None) {
    if (!PyDict_Check(memo_arg) && Py_TYPE(memo_arg) != &Memo_Type) {
      PyErr_Format(PyExc_TypeError,
                   "argument 'memo' must be dict, not %.200s",
                   Py_TYPE(memo_arg)->tp_name);
      return NULL;
    }
    memo_local = memo_arg;
    Py_INCREF(memo_local);
  }

  /* can't use deepcopy_recursive_skip_atomic because top-level 'x' may be atomic */
  PyObject* result =
      deepcopy(source_obj, &memo_local, &keepalive_local);
  Py_XDECREF(keepalive_local);
  Py_XDECREF(memo_local);
  return result;
}

static inline PyObject* build_list_by_calling_noargs(PyObject* callable,
                                                     Py_ssize_t n) {
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
      PyList_SET_ITEM(out, i, item);  // steals ref
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

PyObject* py_replicate(PyObject* self,
                       PyObject* const* args,
                       Py_ssize_t nargs,
                       PyObject* kwnames) {
  (void)self;

  if (UNLIKELY(nargs != 2)) {
    PyErr_SetString(PyExc_TypeError,
                    "replicate(obj, n, /, *, compile_after=20)");
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
      PyErr_SetString(PyExc_TypeError,
                      "replicate accepts only 'compile_after' keyword");
      return NULL;
    }
    if (kwcount == 1) {
      PyObject* kwname = PyTuple_GET_ITEM(kwnames, 0);
      int is_compile_after =
          PyUnicode_Check(kwname) &&
          (PyUnicode_CompareWithASCIIString(kwname, "compile_after") == 0);
      if (!is_compile_after) {
        PyErr_SetString(PyExc_TypeError,
                        "unknown keyword; only 'compile_after' is supported");
        return NULL;
      }
      if (!duper_available) {
        PyErr_SetString(PyExc_TypeError,
                        "replicate(): 'compile_after' requires duper.snapshots; it is not available");
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

  if (n == 0) {
    return PyList_New(0);
  }
  if (is_atomic_immutable(obj)) {
      PyObject* out = PyList_New(n);
      if (!out)
        return NULL;

      for (Py_ssize_t i = 0; i < n; i++) {
          PyObject *copy_i = Py_NewRef(obj);
          PyList_SET_ITEM(out, i, copy_i);
      }
      return out;
  }
  {
    PinObject* pin = _duper_lookup_pin_for_object(obj);
    if (pin) {
      PyObject* factory = pin->factory; /* borrowed */
      if (UNLIKELY(!factory || !PyCallable_Check(factory))) {
        PyErr_SetString(PyExc_RuntimeError,
                        "pinned object has no valid factory");
        return NULL;
      }
      PyObject* out = build_list_by_calling_noargs(factory, n);
      if (out) {
        pin->hits += (uint64_t)n;
      }
      return out;
    }
  }

  if (!duper_available || n <= (Py_ssize_t)compile_after) {
    PyObject* out = PyList_New(n);
    if (!out)
      return NULL;

    for (Py_ssize_t i = 0; i < n; i++) {
      PyObject* memo_local = NULL;
      PyObject* keepalive_local = NULL;

      /* safe: earlier branch returned for atomics, so obj is non-atomic here */
      PyObject* copy_i =
          deepcopy_recursive_skip_atomic(obj, &memo_local, &keepalive_local);

      Py_XDECREF(keepalive_local);
      Py_XDECREF(memo_local);

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
      PyErr_SetString(
          PyExc_RuntimeError,
          "duper.snapshots.create_precompiler_reconstructor is not callable");
      return NULL;
    }

    PyObject* reconstructor = PyObject_CallOneArg(cpr, obj);
    if (!reconstructor)
      return NULL;

    if (UNLIKELY(!PyCallable_Check(reconstructor))) {
      Py_DECREF(reconstructor);
      PyErr_SetString(PyExc_TypeError,
                      "reconstructor must be callable (FunctionType)");
      return NULL;
    }

    PyObject* out = build_list_by_calling_noargs(reconstructor, n);
    Py_DECREF(reconstructor);
    return out;
  }
}

/* New public API: repeatcall(function, size, /) -> list[T] */
PyObject* py_repeatcall(PyObject* self,
                        PyObject* const* args,
                        Py_ssize_t nargs,
                        PyObject* kwnames) {
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

static PyObject* reconstruct_state(PyObject* new_obj,
                                   PyObject* state,
                                   PyObject* listiter,
                                   PyObject* dictiter) {
  if (UNLIKELY(new_obj == NULL)) {
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
          if (PyErr_Occurred()) return NULL;
        }
      } else {
        dict_state = state; /* treat as mapping-like */
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
    if (PyErr_Occurred()) return NULL;
  }

  if (dictiter != Py_None) {
    PyObject* it = PyObject_GetIter(dictiter);
    if (!it) return NULL;
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
    if (PyErr_Occurred()) return NULL;
  }

  return Py_NewRef(new_obj);
}

/* ---------------------------------- copy() ---------------------------------- */

static inline int is_empty_initializable(PyObject* obj) {
  PyTypeObject* tp = Py_TYPE(obj);
  if (tp == &PyList_Type)     { return Py_SIZE(obj) == 0; }
  if (tp == &PyTuple_Type)    { return Py_SIZE(obj) == 0; }
  if (tp == &PyDict_Type)     {
#if defined(PyDict_GET_SIZE)
    return PyDict_GET_SIZE(obj) == 0;
#else
    return PyDict_Size(obj) == 0;
#endif
  }
  if (tp == &PySet_Type)      {
#if defined(PySet_GET_SIZE)
    return PySet_GET_SIZE((PySetObject*)obj) == 0;
#else
    return PySet_Size(obj) == 0;
#endif
  }
  if (tp == &PyFrozenSet_Type){
#if defined(PySet_GET_SIZE)
    return PySet_GET_SIZE((PySetObject*)obj) == 0;
#else
    return PyObject_Size(obj) == 0;
#endif
  }
  if (tp == &PyByteArray_Type){
#if defined(PyByteArray_GET_SIZE)
    return PyByteArray_GET_SIZE(obj) == 0;
#else
    return PyByteArray_Size(obj) == 0;
#endif
  }
  return 0;
}

static PyObject* make_empty_same_type(PyObject* obj) {
  PyTypeObject* tp = Py_TYPE(obj);
  if (tp == &PyList_Type)      return PyList_New(0);
  if (tp == &PyTuple_Type)     return PyTuple_New(0);
  if (tp == &PyDict_Type)      return PyDict_New();
  if (tp == &PySet_Type)       return PySet_New(NULL);
  if (tp == &PyFrozenSet_Type) return PyFrozenSet_New(NULL);
  if (tp == &PyByteArray_Type) return PyByteArray_FromStringAndSize(NULL, 0);
  Py_RETURN_NONE; /* shouldn't happen; caller guards */
}

static PyObject* try_stdlib_mutable_copy(PyObject* obj) {
  PyTypeObject* tp = Py_TYPE(obj);
  if (tp == &PyDict_Type || tp == &PySet_Type || tp == &PyList_Type
      || tp == &PyByteArray_Type) {
    PyObject* method = PyObject_GetAttrString(obj, "copy");
    if (!method) {
      PyErr_Clear();
    } else {
      PyObject* out = PyObject_CallNoArgs(method);
      Py_DECREF(method);
      if (out) return out;
      return NULL;
    }
  }
  Py_RETURN_NONE;
}

PyObject* py_copy(PyObject* self, PyObject* obj) {
  (void)self;

  if (is_atomic_immutable(obj)) {
    return Py_NewRef(obj);
  }

  if (PySlice_Check(obj)) {
    return Py_NewRef(obj);
  }
  if (PyFrozenSet_CheckExact(obj)) {
    return Py_NewRef(obj);
  }

  if (PyType_IsSubtype(Py_TYPE(obj), &PyType_Type)) {
    return Py_NewRef(obj);
  }

  if (is_empty_initializable(obj)) {
    PyObject* fresh = make_empty_same_type(obj);
    if (fresh == Py_None) {
      Py_DECREF(fresh);
    } else {
      return fresh;
    }
  }

  {
    PyObject* maybe = try_stdlib_mutable_copy(obj);
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

  PyTypeObject* obj_type = Py_TYPE(obj);
  PyObject* reduce_result = try_reduce_via_registry(obj, obj_type);
  if (!reduce_result) {
    if (PyErr_Occurred()) return NULL;
    reduce_result = call_reduce_method_preferring_ex(obj);
    if (!reduce_result) return NULL;
  }

  PyObject *constructor=NULL, *args=NULL, *state=NULL,
           *listiter=NULL, *dictiter=NULL;
  int unpack_result =
      unpack_reduce_result_tuple(reduce_result, &constructor, &args, &state,
                                 &listiter, &dictiter);
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
        dictiter ? dictiter : Py_None);
    if (!applied) { Py_DECREF(out); Py_DECREF(reduce_result); return NULL; }
    Py_DECREF(applied);
  }

  Py_DECREF(reduce_result);
  return out;
}

/* -------------------------------- replace() -------------------------------- */

#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
PyObject* py_replace(PyObject* self,
                     PyObject* const* args,
                     Py_ssize_t nargs,
                     PyObject* kwnames) {
  (void)self;
  if (UNLIKELY(nargs == 0)) {
    PyErr_SetString(PyExc_TypeError,
                    "replace() missing 1 required positional argument: 'obj'");
    return NULL;
  }
  if (UNLIKELY(nargs > 1)) {
    PyErr_Format(PyExc_TypeError,
                 "replace() takes 1 positional argument but %zd were given",
                 nargs);
    return NULL;
  }
  PyObject* obj = args[0];
  PyObject* cls = (PyObject*)Py_TYPE(obj);

  PyObject* func = PyObject_GetAttrString(cls, "__replace__");
  if (!func) {
    PyErr_Clear();
    PyErr_Format(PyExc_TypeError,
                 "replace() does not support %.200s objects",
                 Py_TYPE(obj)->tp_name);
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

/* ----------------------------- Module boilerplate -------------------------- */

extern PyObject* py_copy(PyObject* self, PyObject* obj);
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
extern PyObject* py_replace(PyObject* self,
                            PyObject* const* args,
                            Py_ssize_t nargs,
                            PyObject* kwnames);
#endif

static void cleanup_on_init_failure(void) {
  Py_XDECREF(module_state.str_reduce_ex);
  Py_XDECREF(module_state.str_reduce);
  Py_XDECREF(module_state.str_deepcopy);
  Py_XDECREF(module_state.str_setstate);
  Py_XDECREF(module_state.str_dict);
  Py_XDECREF(module_state.str_append);
  Py_XDECREF(module_state.str_update);

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
  if (PyThread_tss_is_created(&module_state.recdepth_tss)) {
    PyThread_tss_delete(&module_state.recdepth_tss);
  }
}

#define LOAD_TYPE(source_module, type_name, target_field)         \
  do {                                                            \
    PyObject* _loaded_type =                                      \
        PyObject_GetAttrString((source_module), (type_name));     \
    if (!_loaded_type || !PyType_Check(_loaded_type)) {           \
      Py_XDECREF(_loaded_type);                                   \
      PyErr_Format(PyExc_ImportError,                             \
                   "copium: %s.%s missing or not a type",          \
                   #source_module, (type_name));                  \
      cleanup_on_init_failure();                                  \
      return -1;                                                  \
    }                                                             \
    module_state.target_field = (PyTypeObject*)_loaded_type;      \
  } while (0)

int _copium_copying_init(PyObject* module) {
  /* Intern strings */
  module_state.str_reduce_ex = PyUnicode_InternFromString("__reduce_ex__");
  module_state.str_reduce    = PyUnicode_InternFromString("__reduce__");
  module_state.str_deepcopy  = PyUnicode_InternFromString("__deepcopy__");
  module_state.str_setstate  = PyUnicode_InternFromString("__setstate__");
  module_state.str_dict      = PyUnicode_InternFromString("__dict__");
  module_state.str_append    = PyUnicode_InternFromString("append");
  module_state.str_update    = PyUnicode_InternFromString("update");

  if (!module_state.str_reduce_ex || !module_state.str_reduce ||
      !module_state.str_deepcopy || !module_state.str_setstate ||
      !module_state.str_dict || !module_state.str_append ||
      !module_state.str_update) {
    PyErr_SetString(PyExc_ImportError,
                    "copium: failed to intern required names");
    cleanup_on_init_failure();
    return -1;
  }

  /* Load stdlib modules */
  PyObject* mod_types     = PyImport_ImportModule("types");
  PyObject* mod_builtins  = PyImport_ImportModule("builtins");
  PyObject* mod_weakref   = PyImport_ImportModule("weakref");
  PyObject* mod_copyreg   = PyImport_ImportModule("copyreg");
  PyObject* mod_re        = PyImport_ImportModule("re");
  PyObject* mod_decimal   = PyImport_ImportModule("decimal");
  PyObject* mod_fractions = PyImport_ImportModule("fractions");

  if (!mod_types || !mod_builtins || !mod_weakref || !mod_copyreg || !mod_re ||
      !mod_decimal || !mod_fractions) {
    PyErr_SetString(PyExc_ImportError,
                    "copium: failed to import required stdlib modules");
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

  /* Cache types */
  LOAD_TYPE(mod_types, "BuiltinFunctionType", BuiltinFunctionType);
  LOAD_TYPE(mod_types, "CodeType",             CodeType);
  LOAD_TYPE(mod_types, "MethodType",           MethodType);
  LOAD_TYPE(mod_builtins, "property",          property_type);
  LOAD_TYPE(mod_builtins, "range",             range_type);
  LOAD_TYPE(mod_weakref, "ref",                weakref_ref_type);
  LOAD_TYPE(mod_re, "Pattern",                 re_Pattern_type);
  LOAD_TYPE(mod_decimal, "Decimal",            Decimal_type);
  LOAD_TYPE(mod_fractions, "Fraction",         Fraction_type);

  /* copyreg dispatch and copy.Error */
  module_state.copyreg_dispatch =
      PyObject_GetAttrString(mod_copyreg, "dispatch_table");
  if (!module_state.copyreg_dispatch ||
      !PyDict_Check(module_state.copyreg_dispatch)) {
    PyErr_SetString(
        PyExc_ImportError,
        "copium: copyreg.dispatch_table missing or not a dict");
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

  PyObject* mod_copy = PyImport_ImportModule("copy");
  if (!mod_copy) {
    PyErr_SetString(PyExc_ImportError,
                    "copium: failed to import copy module");
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
  if (!module_state.copy_Error ||
      !PyExceptionClass_Check(module_state.copy_Error)) {
    PyErr_SetString(PyExc_ImportError,
                    "copium: copy.Error missing or not an exception");
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

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  if (PyThread_tss_create(&module_state.watch_tss) != 0) {
    PyErr_SetString(PyExc_ImportError, "copium: failed to create TSS");
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
  module_state.dict_watcher_id = PyDict_AddWatcher(dict_watch_callback);
  if (module_state.dict_watcher_id < 0) {
    PyErr_SetString(PyExc_ImportError,
                    "copium: failed to allocate dict watcher id");
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
#endif

  /* Create thread-local recursion depth guard */
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

  Py_DECREF(mod_types);
  Py_DECREF(mod_builtins);
  Py_DECREF(mod_weakref);
  Py_DECREF(mod_copyreg);
  Py_DECREF(mod_re);
  Py_DECREF(mod_decimal);
  Py_DECREF(mod_fractions);

  /* Ready memo + keep proxy types */
  if (memo_ready_types() < 0) {
    cleanup_on_init_failure();
    return -1;
  }

  /* Try duper.snapshots: if available, cache reconstructor factory and expose pin API/types. */
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