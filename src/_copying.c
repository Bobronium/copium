/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * copyc
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

#define Py_BUILD_CORE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Python.h"
#include "pycore_object.h"  // _PyNone_Type, _PyNotImplemented_Type
// _PyDict_NewPresized/_PyDict_*KnownHas
#if PY_VERSION_HEX < PY_VERSION_3_11_HEX
#include "dictobject.h"
#else
#include "pycore_dict.h"
#endif

extern PyTypeObject Pattern_Type;
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
      create_precompiler_reconstructor; /* duper.snapshots.create_precompiler_reconstructor
                                         */

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  int dict_watcher_id;
  Py_tss_t watch_tss;
#endif
} ModuleState;

static ModuleState module_state = {0};

/* ------------------------------ Atomic checks ------------------------------
 */

static inline PyObject* create_id_from_pointer(PyObject* obj) {
  return PyLong_FromVoidPtr((void*)obj);
}

static inline int is_method_type_exact(PyObject* obj) {
  return Py_TYPE(obj) == module_state.MethodType;
}

static inline int is_atomic_immutable(PyObject* obj) {
  if (obj == Py_None)
    return 1;
  PyTypeObject* obj_type = Py_TYPE(obj);
  if (LIKELY(obj_type == &PyLong_Type))
    return 1;
  if (LIKELY(obj_type == &PyUnicode_Type))
    return 1;
  if (obj_type == &PyBool_Type)
    return 1;
  if (obj_type == &PyFloat_Type)
    return 1;
  if (obj_type == &PyType_Type)
    return 1;
  if (obj_type == &PyBytes_Type)
    return 1;
  if (obj_type == &PyFunction_Type)
    return 1;
  if (obj_type == &PyCFunction_Type)
    return 1;
  if (obj_type == &PyComplex_Type)
    return 1;
  if (obj_type == &PyModule_Type)
    return 1;
  if (obj_type == &PyRange_Type)
    return 1;
  if (obj_type == &PyCFunction_Type)
    return 1;
  if (obj_type == &PyProperty_Type)
    return 1;
  if (obj_type == &_PyWeakref_RefType)
    return 1;
  if (obj_type == &PyCode_Type)
    return 1;
  if (PyType_IsSubtype(obj_type, &PyType_Type))
    return 1;

  return (obj_type == module_state.re_Pattern_type) |
         (obj_type == module_state.Decimal_type) |
         (obj_type == module_state.Fraction_type) |
         (obj_type == &_PyNotImplemented_Type) | (obj_type == &PyEllipsis_Type);
}

/* ------------------------ Lazy memo/keepalive helpers -----------------------
 */

static inline int ensure_memo_dict_exists(PyObject** memo_dict_ptr) {
  if (*memo_dict_ptr != NULL)
    return 0;
  PyObject* new_memo = PyDict_New();
  if (!new_memo)
    return -1;
  *memo_dict_ptr = new_memo;
  return 0;
}

static inline PyObject* memo_lookup(PyObject** memo_dict_ptr,
                                    PyObject* object_id) {
  if (*memo_dict_ptr == NULL)
    return NULL;
  return PyDict_GetItemWithError(*memo_dict_ptr, object_id);
}

static inline int memo_store(PyObject** memo_dict_ptr,
                             PyObject* object_id,
                             PyObject* copied_obj) {
  if (ensure_memo_dict_exists(memo_dict_ptr) < 0)
    return -1;
  return PyDict_SetItem(*memo_dict_ptr, object_id, copied_obj);
}

static inline int ensure_keepalive_list_exists(PyObject** memo_dict_ptr,
                                               PyObject** keepalive_list_ptr) {
  if (*keepalive_list_ptr)
    return 0;
  if (ensure_memo_dict_exists(memo_dict_ptr) < 0)
    return -1;

  PyObject* memo_id = create_id_from_pointer(*memo_dict_ptr);
  if (!memo_id)
    return -1;

  PyObject* existing_list = PyDict_GetItemWithError(*memo_dict_ptr, memo_id);
  if (existing_list) {
    if (!PyList_Check(existing_list)) {
      Py_DECREF(memo_id);
      PyErr_SetString(PyExc_TypeError, "memo[id(memo)] must be a list");
      return -1;
    }
    Py_INCREF(existing_list);
    *keepalive_list_ptr = existing_list;
    Py_DECREF(memo_id);
    return 0;
  }
  if (PyErr_Occurred()) {
    Py_DECREF(memo_id);
    return -1;
  }

  PyObject* new_list = PyList_New(0);
  if (!new_list) {
    Py_DECREF(memo_id);
    return -1;
  }
  if (PyDict_SetItem(*memo_dict_ptr, memo_id, new_list) < 0) {
    Py_DECREF(memo_id);
    Py_DECREF(new_list);
    return -1;
  }
  Py_DECREF(memo_id);
  *keepalive_list_ptr = new_list;
  return 0;
}

static inline int keepalive_append_if_different(PyObject** memo_dict_ptr,
                                                PyObject** keepalive_list_ptr,
                                                PyObject* original_obj,
                                                PyObject* copied_obj) {
  if (copied_obj == original_obj)
    return 0;
  if (ensure_keepalive_list_exists(memo_dict_ptr, keepalive_list_ptr) < 0)
    return -1;
  return PyList_Append(*keepalive_list_ptr, original_obj);
}

static inline int keepalive_append_original(PyObject** memo_dict_ptr,
                                            PyObject** keepalive_list_ptr,
                                            PyObject* original_obj) {
  if (ensure_keepalive_list_exists(memo_dict_ptr, keepalive_list_ptr) < 0)
    return -1;
  return PyList_Append(*keepalive_list_ptr, original_obj);
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
    case PyDict_EVENT_CLEARED:
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

// Prototype for the private symbol, only used when it actually exists.
#if PY_VERSION_HEX < PY_VERSION_3_13_HEX /* 3.13.0 */
PyAPI_FUNC(int) _PyDict_Next(PyObject* mp,
                             Py_ssize_t* ppos,
                             PyObject** pkey,
                             PyObject** pvalue,
                             Py_hash_t* phash);
#endif

// Returns: 1 -> yielded (key,value,hash)
//          0 -> end of iteration
//         -1 -> error (only possible on >=3.13 when hashing key)
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

static PyObject* deepcopy_recursive(PyObject* source_obj,
                                    PyObject** memo_dict_ptr,
                                    PyObject** keepalive_list_ptr);
static PyObject* deepcopy_recursive_impl(PyObject* source_obj,
                                         PyObject** memo_dict_ptr,
                                         PyObject** keepalive_list_ptr,
                                         int skip_atomic_check);

#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
static PyObject* deepcopy_dict_with_watcher(PyObject* source_obj,
                                            PyObject** memo_dict_ptr,
                                            PyObject** keepalive_list_ptr,
                                            PyObject* object_id) {
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
  if (memo_store(memo_dict_ptr, object_id, copied_dict) < 0) {
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
        deepcopy_recursive(dict_key, memo_dict_ptr, keepalive_list_ptr);
    PyObject* copied_value =
        copied_key
            ? deepcopy_recursive(dict_value, memo_dict_ptr, keepalive_list_ptr)
            : NULL;

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

  if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
                                    source_obj, copied_dict) < 0) {
    Py_DECREF(copied_dict);
    return NULL;
  }
  return copied_dict;
}
#endif

static PyObject* deepcopy_dict_legacy(PyObject* source_obj,
                                      PyObject** memo_dict_ptr,
                                      PyObject** keepalive_list_ptr,
                                      PyObject* object_id) {
  Py_ssize_t expected_size = PyDict_Size(source_obj);
  PyObject* copied_dict = PyDict_New();
  if (!copied_dict)
    return NULL;
  if (memo_store(memo_dict_ptr, object_id, copied_dict) < 0) {
    Py_DECREF(copied_dict);
    return NULL;
  }

  Py_ssize_t iteration_pos = 0;
  PyObject *dict_key, *dict_value;
  while (PyDict_Next(source_obj, &iteration_pos, &dict_key, &dict_value)) {
    Py_INCREF(dict_key);
    Py_INCREF(dict_value);
    PyObject* copied_key =
        deepcopy_recursive(dict_key, memo_dict_ptr, keepalive_list_ptr);
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
    PyObject* copied_value =
        deepcopy_recursive(dict_value, memo_dict_ptr, keepalive_list_ptr);
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
  if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
                                    source_obj, copied_dict) < 0) {
    Py_DECREF(copied_dict);
    return NULL;
  }
  return copied_dict;
}

static PyObject* deepcopy_dict(PyObject* source_obj,
                               PyObject** memo_dict_ptr,
                               PyObject** keepalive_list_ptr,
                               PyObject* object_id) {
#if PY_VERSION_HEX >= PY_VERSION_3_12_HEX
  return deepcopy_dict_with_watcher(source_obj, memo_dict_ptr,
                                    keepalive_list_ptr, object_id);
#endif
  return deepcopy_dict_legacy(source_obj, memo_dict_ptr, keepalive_list_ptr,
                              object_id);
}

static PyObject* deepcopy_list(PyObject* source_obj,
                               PyObject** memo_dict_ptr,
                               PyObject** keepalive_list_ptr,
                               PyObject* object_id)
{
    /* 1) Allocate a fully-initialized list (no NULL slots visible to user code). */
    Py_ssize_t initial_size = PyList_GET_SIZE(source_obj);
    PyObject* copied_list = PyList_New(initial_size);
    if (copied_list == NULL) {
        return NULL;
    }

    /* Prefill with Py_None. */
    /* assuming None is practically immortal; */
    for (Py_ssize_t i = 0; i < initial_size; ++i) {
        /* SET_ITEM steals; passing borrowed Py_None is fine with immortality. */
        PyList_SET_ITEM(copied_list, i, Py_None);
    }

    /* 2) Publish to memo immediately. */
    if (memo_store(memo_dict_ptr, object_id, copied_list) < 0) {
        Py_DECREF(copied_list);
        return NULL;
    }

    /* 3) Deep-copy elements, guarding against concurrent list mutations. */
    for (Py_ssize_t i = 0; i < initial_size; ++i) {
        /* If user code resized either list during recursion, stop safely. */
        if (i >= PyList_GET_SIZE(copied_list) || i >= PyList_GET_SIZE(source_obj)) {
            break;
        }

        /* Hold a ref to the source element while we call into Python. */
        PyObject* item = PyList_GET_ITEM(source_obj, i);  /* borrowed */
        Py_XINCREF(item);
        if (item == NULL) {
            Py_DECREF(copied_list);
            return NULL;
        }

        PyObject* copied_item =
            deepcopy_recursive(item, memo_dict_ptr, keepalive_list_ptr);

        Py_DECREF(item);  /* release temporary hold */

        if (copied_item == NULL) {
            Py_DECREF(copied_list);
            return NULL;
        }

        /* Destination might have changed size during recursion. */
        if (i >= PyList_GET_SIZE(copied_list)) {
            Py_DECREF(copied_item);  /* PyList_SetItem would not steal on failure */
            break;
        }

        /* 4) Replace slot i with the new element. */
        PyList_SET_ITEM(copied_list, i, copied_item);

    }

    /* 5) Keepalive to avoid premature deallocation in alias-y scenarios. */
    if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
                                      source_obj, copied_list) < 0) {
        Py_DECREF(copied_list);
        return NULL;
    }

    return copied_list;
}

static PyObject* deepcopy_tuple(PyObject* source_obj,
                                PyObject** memo_dict_ptr,
                                PyObject** keepalive_list_ptr,
                                PyObject* object_id) {
  Py_ssize_t tuple_length = PyTuple_GET_SIZE(source_obj);
  int all_elements_identical = 1;
  PyObject* copied_tuple = PyTuple_New(tuple_length);
  if (!copied_tuple)
    return NULL;

  for (Py_ssize_t i = 0; i < tuple_length; i++) {
    PyObject* element = PyTuple_GET_ITEM(source_obj, i);
    PyObject* copied_element =
        deepcopy_recursive(element, memo_dict_ptr, keepalive_list_ptr);
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

  PyObject* existing_copy = memo_lookup(memo_dict_ptr, object_id);
  if (existing_copy) {
    Py_INCREF(existing_copy);
    Py_DECREF(copied_tuple);
    return existing_copy;
  }
  if (PyErr_Occurred()) {
    Py_DECREF(copied_tuple);
    return NULL;
  }

  if (memo_store(memo_dict_ptr, object_id, copied_tuple) < 0) {
    Py_DECREF(copied_tuple);
    return NULL;
  }
  if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
                                    source_obj, copied_tuple) < 0) {
    Py_DECREF(copied_tuple);
    return NULL;
  }
  return copied_tuple;
}

static PyObject* deepcopy_set(PyObject* source_obj,
                              PyObject** memo_dict_ptr,
                              PyObject** keepalive_list_ptr,
                              PyObject* object_id) {
  PyObject* copied_set = PySet_New(NULL);
  if (!copied_set)
    return NULL;
  if (memo_store(memo_dict_ptr, object_id, copied_set) < 0) {
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
        deepcopy_recursive(item, memo_dict_ptr, keepalive_list_ptr);
    Py_DECREF(item);
    if (!copied_item) {
      Py_DECREF(iterator);
      Py_DECREF(copied_set);
      return NULL;
    }
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
  if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
                                    source_obj, copied_set) < 0) {
    Py_DECREF(copied_set);
    return NULL;
  }
  return copied_set;
}

static PyObject* deepcopy_frozenset(PyObject* source_obj,
                                    PyObject** memo_dict_ptr,
                                    PyObject** keepalive_list_ptr,
                                    PyObject* object_id) {
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
        deepcopy_recursive(item, memo_dict_ptr, keepalive_list_ptr);
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
  if (memo_store(memo_dict_ptr, object_id, copied_frozenset) < 0) {
    Py_DECREF(copied_frozenset);
    return NULL;
  }
  if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
                                    source_obj, copied_frozenset) < 0) {
    Py_DECREF(copied_frozenset);
    return NULL;
  }
  return copied_frozenset;
}

static PyObject* deepcopy_bytearray(PyObject* source_obj,
                                    PyObject** memo_dict_ptr,
                                    PyObject** keepalive_list_ptr,
                                    PyObject* object_id) {
  Py_ssize_t byte_length = PyByteArray_Size(source_obj);
  PyObject* copied_bytearray = PyByteArray_FromStringAndSize(NULL, byte_length);
  if (!copied_bytearray)
    return NULL;
  if (byte_length)
    memcpy(PyByteArray_AS_STRING(copied_bytearray),
           PyByteArray_AS_STRING(source_obj), (size_t)byte_length);
  if (memo_store(memo_dict_ptr, object_id, copied_bytearray) < 0) {
    Py_DECREF(copied_bytearray);
    return NULL;
  }
  if (keepalive_append_if_different(memo_dict_ptr, keepalive_list_ptr,
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

/* ------------------------ dispatcher core (skip_atomic_check)
 * ---------------------
 */

static PyObject* deepcopy_recursive_impl(PyObject* source_obj,
                                         PyObject** memo_dict_ptr,
                                         PyObject** keepalive_list_ptr,
                                         int skip_atomic_check) {
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

  if (!skip_atomic_check) {
    if (is_atomic_immutable(source_obj))
      return Py_NewRef(source_obj);
  }

  PyObject* object_id = create_id_from_pointer(source_obj);
  if (!object_id)
    return NULL;

  PyObject* memo_hit = memo_lookup(memo_dict_ptr, object_id);
  if (memo_hit) {
    Py_INCREF(memo_hit);
    Py_DECREF(object_id);
    return memo_hit;
  }
  if (PyErr_Occurred()) {
    Py_DECREF(object_id);
    return NULL;
  }

  if (PyList_CheckExact(source_obj)) {
    PyObject* result =
        deepcopy_list(source_obj, memo_dict_ptr, keepalive_list_ptr, object_id);
    Py_DECREF(object_id);
    return result;
  }
  if (PyTuple_CheckExact(source_obj)) {
    PyObject* result = deepcopy_tuple(source_obj, memo_dict_ptr,
                                      keepalive_list_ptr, object_id);
    Py_DECREF(object_id);
    return result;
  }
  if (PyDict_CheckExact(source_obj)) {
    PyObject* result =
        deepcopy_dict(source_obj, memo_dict_ptr, keepalive_list_ptr, object_id);
    Py_DECREF(object_id);
    return result;
  }
  if (PySet_CheckExact(source_obj)) {
    PyObject* result =
        deepcopy_set(source_obj, memo_dict_ptr, keepalive_list_ptr, object_id);
    Py_DECREF(object_id);
    return result;
  }
  if (PyFrozenSet_CheckExact(source_obj)) {
    PyObject* result = deepcopy_frozenset(source_obj, memo_dict_ptr,
                                          keepalive_list_ptr, object_id);
    Py_DECREF(object_id);
    return result;
  }
  if (PyByteArray_CheckExact(source_obj)) {
    PyObject* result = deepcopy_bytearray(source_obj, memo_dict_ptr,
                                          keepalive_list_ptr, object_id);
    Py_DECREF(object_id);
    return result;
  }

  if (PyModule_Check(source_obj)) {
    Py_DECREF(object_id);
    return Py_NewRef(source_obj);
  }

  if (is_method_type_exact(source_obj)) {
    PyObject* method_function = PyMethod_GET_FUNCTION(source_obj);
    PyObject* method_self = PyMethod_GET_SELF(source_obj);
    if (!method_function || !method_self) {
      Py_DECREF(object_id);
      return NULL;
    }
    Py_INCREF(method_function);
    Py_INCREF(method_self);
    PyObject* copied_self =
        deepcopy_recursive(method_self, memo_dict_ptr, keepalive_list_ptr);
    if (!copied_self) {
      Py_DECREF(method_function);
      Py_DECREF(method_self);
      Py_DECREF(object_id);
      return NULL;
    }
    PyObject* bound_method = PyMethod_New(method_function, copied_self);
    Py_DECREF(method_function);
    Py_DECREF(method_self);
    Py_DECREF(copied_self);
    if (!bound_method) {
      Py_DECREF(object_id);
      return NULL;
    }
    if (memo_store(memo_dict_ptr, object_id, bound_method) < 0) {
      Py_DECREF(bound_method);
      Py_DECREF(object_id);
      return NULL;
    }
    if (keepalive_append_original(memo_dict_ptr, keepalive_list_ptr,
                                  source_obj) < 0) {
      Py_DECREF(bound_method);
      Py_DECREF(object_id);
      return NULL;
    }
    Py_DECREF(object_id);
    return bound_method;
  }

  if (Py_TYPE(source_obj) == &PyBaseObject_Type) {
    PyObject* new_baseobject = PyObject_New(PyObject, &PyBaseObject_Type);
    if (!new_baseobject) {
      Py_DECREF(object_id);
      return NULL;
    }
    if (memo_store(memo_dict_ptr, object_id, new_baseobject) < 0) {
      Py_DECREF(new_baseobject);
      Py_DECREF(object_id);
      return NULL;
    }
    if (keepalive_append_original(memo_dict_ptr, keepalive_list_ptr,
                                  source_obj) < 0) {
      Py_DECREF(new_baseobject);
      Py_DECREF(object_id);
      return NULL;
    }
    Py_DECREF(object_id);
    return new_baseobject;
  }

  {
    PyObject* deepcopy_method = NULL;
    int has_deepcopy = PyObject_GetOptionalAttr(
        source_obj, module_state.str_deepcopy, &deepcopy_method);
    if (has_deepcopy < 0) {
      Py_DECREF(object_id);
      return NULL;
    }
    if (has_deepcopy) {
      /* Ensure memo is a dict per copy protocol; many __deepcopy__ expect a
       * dict */
      if (ensure_memo_dict_exists(memo_dict_ptr) < 0) {
        Py_DECREF(deepcopy_method);
        Py_DECREF(object_id);
        return NULL;
      }
      PyObject* result = PyObject_CallOneArg(deepcopy_method, *memo_dict_ptr);
      Py_DECREF(deepcopy_method);
      if (!result) {
        Py_DECREF(object_id);
        return NULL;
      }
      if (result != source_obj) {
        if (memo_store(memo_dict_ptr, object_id, result) < 0) {
          Py_DECREF(result);
          Py_DECREF(object_id);
          return NULL;
        }
        if (keepalive_append_original(memo_dict_ptr, keepalive_list_ptr,
                                      source_obj) < 0) {
          Py_DECREF(result);
          Py_DECREF(object_id);
          return NULL;
        }
      }
      Py_DECREF(object_id);
      return result;
    }
  }

  PyTypeObject* source_type = Py_TYPE(source_obj);
  PyObject* reduce_result = try_reduce_via_registry(source_obj, source_type);
  if (!reduce_result) {
    if (PyErr_Occurred()) {
      Py_DECREF(object_id);
      return NULL;
    }
    reduce_result = call_reduce_method_preferring_ex(source_obj);
    if (!reduce_result) {
      Py_DECREF(object_id);
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
    Py_DECREF(object_id);
    return NULL;
  }
  if (unpack_result == 1) {
    Py_DECREF(reduce_result);
    Py_DECREF(object_id);
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
      Py_DECREF(object_id);
      return NULL;
    }
    for (Py_ssize_t i = 0; i < args_count; i++) {
      PyObject* arg = PyTuple_GET_ITEM(args, i);
      PyObject* copied_arg =
          deepcopy_recursive(arg, memo_dict_ptr, keepalive_list_ptr);
      if (!copied_arg) {
        Py_DECREF(deepcopied_args);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
        return NULL;
      }
      PyTuple_SET_ITEM(deepcopied_args, i, copied_arg);
    }
    reconstructed_obj = PyObject_CallObject(constructor, deepcopied_args);
    Py_DECREF(deepcopied_args);
  }
  if (!reconstructed_obj) {
    Py_DECREF(reduce_result);
    Py_DECREF(object_id);
    return NULL;
  }

  if (memo_store(memo_dict_ptr, object_id, reconstructed_obj) < 0) {
    Py_DECREF(reconstructed_obj);
    Py_DECREF(reduce_result);
    Py_DECREF(object_id);
    return NULL;
  }

  if (state && state != Py_None) {
    PyObject* setstate_method =
        PyObject_GetAttr(reconstructed_obj, module_state.str_setstate);
    if (setstate_method) {
      PyObject* copied_state =
          deepcopy_recursive(state, memo_dict_ptr, keepalive_list_ptr);
      if (!copied_state) {
        Py_DECREF(setstate_method);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
        return NULL;
      }
      PyObject* setstate_result =
          PyObject_CallOneArg(setstate_method, copied_state);
      Py_DECREF(copied_state);
      Py_DECREF(setstate_method);
      if (!setstate_result) {
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
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
            Py_DECREF(object_id);
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
              Py_DECREF(object_id);
              return NULL;
            }
            PyObject* copied_slot_value = deepcopy_recursive(
                slot_value, memo_dict_ptr, keepalive_list_ptr);
            Py_DECREF(slot_value);
            if (!copied_slot_value) {
              Py_DECREF(slot_key);
              Py_DECREF(slot_iterator);
              Py_DECREF(reconstructed_obj);
              Py_DECREF(reduce_result);
              Py_DECREF(object_id);
              return NULL;
            }
            if (PyObject_SetAttr(reconstructed_obj, slot_key,
                                 copied_slot_value) < 0) {
              Py_DECREF(copied_slot_value);
              Py_DECREF(slot_key);
              Py_DECREF(slot_iterator);
              Py_DECREF(reconstructed_obj);
              Py_DECREF(reduce_result);
              Py_DECREF(object_id);
              return NULL;
            }
            Py_DECREF(copied_slot_value);
            Py_DECREF(slot_key);
          }
          Py_DECREF(slot_iterator);
          if (PyErr_Occurred()) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            Py_DECREF(object_id);
            return NULL;
          }
        }

        if (dict_state && dict_state != Py_None) {
          PyObject* copied_dict_state =
              deepcopy_recursive(dict_state, memo_dict_ptr, keepalive_list_ptr);
          if (!copied_dict_state) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            Py_DECREF(object_id);
            return NULL;
          }
          PyObject* obj_dict =
              PyObject_GetAttr(reconstructed_obj, module_state.str_dict);
          if (!obj_dict) {
            Py_DECREF(copied_dict_state);
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            Py_DECREF(object_id);
            return NULL;
          }
          PyObject* update_method =
              PyObject_GetAttr(obj_dict, module_state.str_update);
          Py_DECREF(obj_dict);
          if (!update_method) {
            Py_DECREF(copied_dict_state);
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            Py_DECREF(object_id);
            return NULL;
          }
          PyObject* update_result =
              PyObject_CallOneArg(update_method, copied_dict_state);
          Py_DECREF(update_method);
          Py_DECREF(copied_dict_state);
          if (!update_result) {
            Py_DECREF(reconstructed_obj);
            Py_DECREF(reduce_result);
            Py_DECREF(object_id);
            return NULL;
          }
          Py_DECREF(update_result);
        }
      } else {
        PyObject* copied_dict_state =
            deepcopy_recursive(state, memo_dict_ptr, keepalive_list_ptr);
        if (!copied_dict_state) {
          Py_DECREF(reconstructed_obj);
          Py_DECREF(reduce_result);
          Py_DECREF(object_id);
          return NULL;
        }
        PyObject* obj_dict =
            PyObject_GetAttrString(reconstructed_obj, "__dict__");
        if (!obj_dict) {
          Py_DECREF(copied_dict_state);
          Py_DECREF(reconstructed_obj);
          Py_DECREF(reduce_result);
          Py_DECREF(object_id);
          return NULL;
        }
        PyObject* update_result =
            PyObject_CallMethod(obj_dict, "update", "O", copied_dict_state);
        Py_DECREF(obj_dict);
        Py_DECREF(copied_dict_state);
        if (!update_result) {
          Py_DECREF(reconstructed_obj);
          Py_DECREF(reduce_result);
          Py_DECREF(object_id);
          return NULL;
        }
        Py_DECREF(update_result);
      }
    }
  }

  if (list_iterator && list_iterator != Py_None) {
    PyObject* append_method =
        PyObject_GetAttr(reconstructed_obj, module_state.str_append);
    if (!append_method) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      Py_DECREF(object_id);
      return NULL;
    }
    PyObject* iterator = PyObject_GetIter(list_iterator);
    if (!iterator) {
      Py_DECREF(append_method);
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      Py_DECREF(object_id);
      return NULL;
    }
    PyObject* item;
    while ((item = PyIter_Next(iterator)) != NULL) {
      PyObject* copied_item =
          deepcopy_recursive(item, memo_dict_ptr, keepalive_list_ptr);
      Py_DECREF(item);
      if (!copied_item) {
        Py_DECREF(iterator);
        Py_DECREF(append_method);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
        return NULL;
      }
      PyObject* append_result = PyObject_CallOneArg(append_method, copied_item);
      Py_DECREF(copied_item);
      if (!append_result) {
        Py_DECREF(iterator);
        Py_DECREF(append_method);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
        return NULL;
      }
      Py_DECREF(append_result);
    }
    Py_DECREF(iterator);
    Py_DECREF(append_method);
    if (PyErr_Occurred()) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      Py_DECREF(object_id);
      return NULL;
    }
  }

  if (dict_iterator && dict_iterator != Py_None) {
    PyObject* iterator = PyObject_GetIter(dict_iterator);
    if (!iterator) {
      Py_DECREF(reconstructed_obj);
      Py_DECREF(reduce_result);
      Py_DECREF(object_id);
      return NULL;
    }
    PyObject* pair;
    while ((pair = PyIter_Next(iterator)) != NULL) {
      PyObject* pair_key = PyTuple_GET_ITEM(pair, 0);
      PyObject* pair_value = PyTuple_GET_ITEM(pair, 1);
      Py_INCREF(pair_key);
      Py_INCREF(pair_value);
      PyObject* copied_key =
          deepcopy_recursive(pair_key, memo_dict_ptr, keepalive_list_ptr);
      PyObject* copied_value =
          copied_key ? deepcopy_recursive(pair_value, memo_dict_ptr,
                                          keepalive_list_ptr)
                     : NULL;
      Py_DECREF(pair_key);
      Py_DECREF(pair_value);
      if (!copied_key || !copied_value) {
        Py_XDECREF(copied_key);
        Py_XDECREF(copied_value);
        Py_DECREF(pair);
        Py_DECREF(iterator);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
        return NULL;
      }
      if (PyObject_SetItem(reconstructed_obj, copied_key, copied_value) < 0) {
        Py_DECREF(copied_key);
        Py_DECREF(copied_value);
        Py_DECREF(pair);
        Py_DECREF(iterator);
        Py_DECREF(reconstructed_obj);
        Py_DECREF(reduce_result);
        Py_DECREF(object_id);
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
      Py_DECREF(object_id);
      return NULL;
    }
  }

  if (keepalive_append_original(memo_dict_ptr, keepalive_list_ptr, source_obj) <
      0) {
    Py_DECREF(reconstructed_obj);
    Py_DECREF(reduce_result);
    Py_DECREF(object_id);
    return NULL;
  }
  Py_DECREF(reduce_result);
  Py_DECREF(object_id);
  return reconstructed_obj;
}

static PyObject* deepcopy_recursive(PyObject* source_obj,
                                    PyObject** memo_dict_ptr,
                                    PyObject** keepalive_list_ptr) {
  return deepcopy_recursive_impl(source_obj, memo_dict_ptr, keepalive_list_ptr,
                                 /*skip_atomic_check=*/0);
}

/* -------------------------------- Public API --------------------------------
 */

PyObject* py_deepcopy(PyObject* self,
                      PyObject* const* args,
                      Py_ssize_t nargs,
                      PyObject* kwnames) {
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

  PyObject* source_obj = args[0];
  PyObject* memo_arg = (nargs == 2) ? args[1] : Py_None;

  if (kwnames) {
    const Py_ssize_t keyword_count = PyTuple_GET_SIZE(kwnames);
    if (UNLIKELY(nargs == 2 && keyword_count > 0)) {
      PyErr_SetString(PyExc_TypeError,
                      "deepcopy() got multiple values for argument 'memo'");
      return NULL;
    }
    if (UNLIKELY(keyword_count > 1)) {
      PyErr_SetString(PyExc_TypeError,
                      "deepcopy() takes at most 1 keyword argument");
      return NULL;
    }
    if (keyword_count == 1) {
      PyObject* kwname = PyTuple_GET_ITEM(kwnames, 0);
      int is_memo_keyword =
          PyUnicode_Check(kwname) &&
          PyUnicode_CompareWithASCIIString(kwname, "memo") == 0;
      if (UNLIKELY(!is_memo_keyword)) {
        PyErr_Format(PyExc_TypeError,
                     "deepcopy() got an unexpected keyword argument '%U'",
                     kwname);
        return NULL;
      }
      memo_arg = args[nargs + 0];
    }
  }

  if (LIKELY(memo_arg == Py_None)) {
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
    if (LIKELY(is_atomic_immutable(source_obj))) {
      return Py_NewRef(source_obj);
    }
  }

  PyObject* memo_local = NULL;
  PyObject* keepalive_local = NULL;

  if (memo_arg != Py_None) {
    if (!PyDict_Check(memo_arg)) {
      PyErr_Format(PyExc_TypeError,
                   "argument 'memo' must be dict, not %.200s",
                   Py_TYPE(memo_arg)->tp_name);
      return NULL;
    }
    memo_local = memo_arg;
    Py_INCREF(memo_local);
  }

  PyObject* result = deepcopy_recursive_impl(
      source_obj, &memo_local, &keepalive_local, /*skip_atomic_check=*/1);
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
  /* Signature: replicate(obj, /, n: int, compile_after: int = 20)
   *
   * Behavior notes:
   * - If duper.snapshots was NOT available at module import (i.e., the cached
   *   create_precompiler_reconstructor is NULL), the 'compile_after' keyword
   *   is forbidden and the function always performs n×deepcopy (no precompiler).
   * - We do NOT attempt any lazy import here; availability is decided once at init.
   */
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

  /* Determine duper availability once, based on module_state cache set at init */
  int duper_available = (module_state.create_precompiler_reconstructor != NULL);

  /* parse optional keyword: compile_after (only allowed if duper is available) */
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
      /* value is args[nargs + index] */
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

  /* Trivial n==0 fast path */
  if (n == 0) {
    return PyList_New(0);
  }
  if (is_atomic_immutable(obj)) {
      PyObject* out = PyList_New(n);
      if (!out)
        return NULL;

      /* Fast path: atomic, so just copy references quickly */
      for (Py_ssize_t i = 0; i < n; i++) {
          PyObject *copy_i = Py_NewRef(obj);  /* same immutable object */
          PyList_SET_ITEM(out, i, copy_i);     /* steals ref */
      }
      return out;
  }
  /* 1) Pin fast-path: use the pinned factory (reconstructor) */
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
        /* count all hits */
        pin->hits += (uint64_t)n;
      }
      return out;
    }
  }

  /* If duper is unavailable OR n <= compile_after -> small-batch n×deepcopy */
  if (!duper_available || n <= (Py_ssize_t)compile_after) {
    PyObject* out = PyList_New(n);
    if (!out)
      return NULL;

    for (Py_ssize_t i = 0; i < n; i++) {
      PyObject* memo_local = NULL;
      PyObject* keepalive_local = NULL;

      /* Use the internal engine directly to avoid Python-call overhead */
      PyObject* copy_i = deepcopy_recursive_impl(obj, &memo_local, &keepalive_local, /*skip_atomic_check=*/1);

      Py_XDECREF(keepalive_local);
      Py_XDECREF(memo_local);

      if (!copy_i) {
        /* cleanup already-filled slots */
        for (Py_ssize_t j = 0; j < i; j++) {
          PyObject* prev = PyList_GET_ITEM(out, j);
          PyList_SET_ITEM(out, j, NULL);
          Py_XDECREF(prev);
        }
        Py_DECREF(out);
        return NULL;
      }
      PyList_SET_ITEM(out, i, copy_i); /* steals ref */
    }
    return out;
  }

  /* 2) Large-batch with precompiled reconstructor (duper present and n > compile_after) */
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
    return out; /* may be NULL on error */
  }
}

/* New public API: repeatcall(function, size, /) -> list[T]
 * Exposes build_list_by_calling_noargs to Python.
 */
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

/* ----------------------------- Shallow reconstruct helper ------------------ */
/* C-only helper (no Python export): apply reduce-state pieces onto new_obj. */
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

  /* Handle state */
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

  /* listiter -> append */
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

  /* dictiter -> __setitem__ */
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
  /* Fast paths for well-known stdlib mutables */
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
      /* if copy() exists but failed, propagate */
      return NULL;
    }
  }
  Py_RETURN_NONE;
}

PyObject* py_copy(PyObject* self, PyObject* obj) {
  (void)self;

  /* 1) Atomic immutables and 'type' subclasses: return as-is */
  if (is_atomic_immutable(obj)) {
    return Py_NewRef(obj);
  }

  /* Additionally treat slice and frozenset as immutable for shallow copy */
  if (PySlice_Check(obj)) {
    return Py_NewRef(obj);
  }
  if (PyFrozenSet_CheckExact(obj)) {
    return Py_NewRef(obj);
  }

  /* types themselves (classes) are treated as immutable */
  if (PyType_IsSubtype(Py_TYPE(obj), &PyType_Type)) {
    return Py_NewRef(obj);
  }

  /* 2) Empty, initializable stdlib collections: return a fresh empty */
  if (is_empty_initializable(obj)) {
    PyObject* fresh = make_empty_same_type(obj);
    if (fresh == Py_None) {
      Py_DECREF(fresh);
      /* fall through; shouldn't reach */
    } else {
      return fresh;
    }
  }

  /* 3) Stdlib mutable fast-paths using .copy() */
  {
    PyObject* maybe = try_stdlib_mutable_copy(obj);
    if (!maybe) return NULL; /* error */
    if (maybe != Py_None) return maybe; /* got a copy */
    Py_DECREF(maybe);
  }

  /* 4) __copy__ if present */
  {
    PyObject* cp = PyObject_GetAttrString(obj, "__copy__");
    if (cp) {
      PyObject* out = PyObject_CallNoArgs(cp);
      Py_DECREF(cp);
      return out;
    }
    PyErr_Clear();
  }

  /* 5) Fallback: reduce protocol (no deep-copy of components) */
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
    out = PyObject_CallObject(constructor, args); /* use args as-is */
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
    Py_DECREF(applied); /* returns a new ref to out */
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
  /* Signature: replace(obj, /, **changes) */
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

  /* Build positional tuple (obj) */
  PyObject* posargs = PyTuple_New(1);
  if (!posargs) { Py_DECREF(func); return NULL; }
  Py_INCREF(obj);
  PyTuple_SET_ITEM(posargs, 0, obj);

  /* Build kwargs dict from our kwnames */
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

/* ----------------------------- Module boilerplate --------------------------
 */

/* Forward declarations for newly added public APIs */
extern PyObject* py_copy(PyObject* self, PyObject* obj);
/* internal-only: reconstruct_state is a static C helper (no Python export) */
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
}

#define LOAD_TYPE(source_module, type_name, target_field)         \
  do {                                                            \
    PyObject* _loaded_type =                                      \
        PyObject_GetAttrString((source_module), (type_name));     \
    if (!_loaded_type || !PyType_Check(_loaded_type)) {           \
      Py_XDECREF(_loaded_type);                                   \
      PyErr_Format(PyExc_ImportError,                             \
                   "copyc: %s.%s missing or not a type", \
                   #source_module, (type_name));                  \
      cleanup_on_init_failure();                                  \
      return -1;                                                  \
    }                                                             \
    module_state.target_field = (PyTypeObject*)_loaded_type;      \
  } while (0)

int _copyc_copying_init(PyObject* module) {
  /* Intern strings */
  module_state.str_reduce_ex = PyUnicode_InternFromString("__reduce_ex__");
  module_state.str_reduce = PyUnicode_InternFromString("__reduce__");
  module_state.str_deepcopy = PyUnicode_InternFromString("__deepcopy__");
  module_state.str_setstate = PyUnicode_InternFromString("__setstate__");
  module_state.str_dict = PyUnicode_InternFromString("__dict__");
  module_state.str_append = PyUnicode_InternFromString("append");
  module_state.str_update = PyUnicode_InternFromString("update");

  if (!module_state.str_reduce_ex || !module_state.str_reduce ||
      !module_state.str_deepcopy || !module_state.str_setstate ||
      !module_state.str_dict || !module_state.str_append ||
      !module_state.str_update) {
    PyErr_SetString(PyExc_ImportError,
                    "copyc: failed to intern required names");
    cleanup_on_init_failure();
    return -1;
  }

  /* Load stdlib modules */
  PyObject* mod_types = PyImport_ImportModule("types");
  PyObject* mod_builtins = PyImport_ImportModule("builtins");
  PyObject* mod_weakref = PyImport_ImportModule("weakref");
  PyObject* mod_copyreg = PyImport_ImportModule("copyreg");
  PyObject* mod_re = PyImport_ImportModule("re");
  PyObject* mod_decimal = PyImport_ImportModule("decimal");
  PyObject* mod_fractions = PyImport_ImportModule("fractions");

  if (!mod_types || !mod_builtins || !mod_weakref || !mod_copyreg || !mod_re ||
      !mod_decimal || !mod_fractions) {
    PyErr_SetString(PyExc_ImportError,
                    "copyc: failed to import required stdlib modules");
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
  LOAD_TYPE(mod_types, "CodeType", CodeType);
  LOAD_TYPE(mod_types, "MethodType", MethodType);
  LOAD_TYPE(mod_builtins, "property", property_type);
  LOAD_TYPE(mod_builtins, "range", range_type);
  LOAD_TYPE(mod_weakref, "ref", weakref_ref_type);
  LOAD_TYPE(mod_re, "Pattern", re_Pattern_type);
  LOAD_TYPE(mod_decimal, "Decimal", Decimal_type);
  LOAD_TYPE(mod_fractions, "Fraction", Fraction_type);

  /* copyreg dispatch and copy.Error */
  module_state.copyreg_dispatch =
      PyObject_GetAttrString(mod_copyreg, "dispatch_table");
  if (!module_state.copyreg_dispatch ||
      !PyDict_Check(module_state.copyreg_dispatch)) {
    PyErr_SetString(
        PyExc_ImportError,
        "copyc: copyreg.dispatch_table missing or not a dict");
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
                    "copyc: failed to import copy module");
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
                    "copyc: copy.Error missing or not an exception");
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
    PyErr_SetString(PyExc_ImportError, "copyc: failed to create TSS");
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
                    "copyc: failed to allocate dict watcher id");
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

  /* Drop temporary module refs; cached types/objects remain */
  Py_DECREF(mod_types);
  Py_DECREF(mod_builtins);
  Py_DECREF(mod_weakref);
  Py_DECREF(mod_copyreg);
  Py_DECREF(mod_re);
  Py_DECREF(mod_decimal);
  Py_DECREF(mod_fractions);

  /* Try duper.snapshots: if available, cache reconstructor factory and expose pin API/types. */
  {
    PyObject* mod_snapshots = PyImport_ImportModule("duper.snapshots");
    if (!mod_snapshots) {
      /* duper not installed: proceed without pin features */
      PyErr_Clear();
      module_state.create_precompiler_reconstructor = NULL;
    } else {
      /* Cache optional reconstructor factory for replicate() fast-paths */
      module_state.create_precompiler_reconstructor =
          PyObject_GetAttrString(mod_snapshots, "create_precompiler_reconstructor");
      if (!module_state.create_precompiler_reconstructor) {
        /* tolerate absence; clear error */
        PyErr_Clear();
      }

      /* Since duper.snapshots is importable, export Pin/PinsProxy types and pin APIs. */
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

int _copyc_copying_duper_available(void) {
  return module_state.create_precompiler_reconstructor != NULL;
}