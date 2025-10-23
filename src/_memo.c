/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * copyc._memo (compiled into copyc._copying extension)
 * - Memo type (internal hash table for deepcopy memo)
 * - Implements MutableMapping protocol with views
 * - C hooks for _copying.c: Memo_New, memo_lookup, memo_store
 */
#define PY_SSIZE_T_CLEAN

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "Python.h"

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* ------------------------------ Memo table -------------------------------- */

typedef struct {
  void* key;      /* object address; NULL = empty; (void*)-1 = tombstone */
  PyObject* value; /* owned reference */
} MemoEntry;

typedef struct {
  MemoEntry* slots;
  Py_ssize_t size;   /* power of two */
  Py_ssize_t used;   /* non-empty (excludes tombstones) */
  Py_ssize_t filled; /* non-empty + tombstones */
} MemoTable;

#define MEMO_TOMBSTONE ((void*)(uintptr_t)(-1))

static inline Py_ssize_t hash_pointer(void* ptr) {
  uintptr_t h = (uintptr_t)ptr;
  h ^= h >> 33;
  h *= (uintptr_t)0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return (Py_ssize_t)h;
}

/* Exported so _copying.c can compute the exact same hash once per object */
Py_ssize_t memo_hash_pointer(void* ptr) {
  return hash_pointer(ptr);
}

void memo_table_free(MemoTable* table) {
  if (!table)
    return;
  for (Py_ssize_t i = 0; i < table->size; i++) {
    if (table->slots[i].key && table->slots[i].key != MEMO_TOMBSTONE) {
      Py_XDECREF(table->slots[i].value);
    }
  }
  free(table->slots);
  free(table);
}

static int memo_table_resize(MemoTable** table_ptr,
                             Py_ssize_t min_capacity_needed) {
  MemoTable* old = *table_ptr;
  Py_ssize_t new_size = 8;
  while (new_size < (min_capacity_needed * 2)) {
    new_size <<= 1;
    if (new_size <= 0) {
      new_size = (Py_ssize_t)1 << (sizeof(void*) * 8 - 2);
      break;
    }
  }
  MemoEntry* new_slots = (MemoEntry*)calloc((size_t)new_size, sizeof(MemoEntry));
  if (!new_slots)
    return -1;

  MemoTable* nt = (MemoTable*)malloc(sizeof(MemoTable));
  if (!nt) {
    free(new_slots);
    return -1;
  }
  nt->slots = new_slots;
  nt->size = new_size;
  nt->used = 0;
  nt->filled = 0;

  if (old) {
    for (Py_ssize_t i = 0; i < old->size; i++) {
      void* key = old->slots[i].key;
      if (key && key != MEMO_TOMBSTONE) {
        PyObject* value = old->slots[i].value; /* transfer */
        Py_ssize_t mask = nt->size - 1;
        Py_ssize_t idx = hash_pointer(key) & mask;
        while (nt->slots[idx].key)
          idx = (idx + 1) & mask;
        nt->slots[idx].key = key;
        nt->slots[idx].value = value;
        nt->used++;
        nt->filled++;
      }
    }
    free(old->slots);
    free(old);
  }
  *table_ptr = nt;
  return 0;
}

static inline int memo_table_ensure(MemoTable** table_ptr) {
  if (*table_ptr)
    return 0;
  return memo_table_resize(table_ptr, 1);
}

/* Existing non-hash-parameterized APIs (kept for compatibility) */
PyObject* memo_table_lookup(MemoTable* table, void* key) {
  if (!table)
    return NULL;
  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx = hash_pointer(key) & mask;
  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key)
      return NULL;
    if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
      return table->slots[idx].value; /* borrowed */
    }
    idx = (idx + 1) & mask;
  }
}

int memo_table_insert(MemoTable** table_ptr, void* key, PyObject* value) {
  if (memo_table_ensure(table_ptr) < 0)
    return -1;
  MemoTable* table = *table_ptr;

  if ((table->filled * 10) >= (table->size * 7)) {
    if (memo_table_resize(table_ptr, table->used + 1) < 0)
      return -1;
    table = *table_ptr;
  }

  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx = hash_pointer(key) & mask;
  Py_ssize_t first_tomb = -1;

  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key) {
      Py_ssize_t insert_at = (first_tomb >= 0) ? first_tomb : idx;
      table->slots[insert_at].key = key;
      Py_INCREF(value);
      table->slots[insert_at].value = value;
      table->used++;
      table->filled++;
      return 0;
    }
    if (slot_key == MEMO_TOMBSTONE) {
      if (first_tomb < 0)
        first_tomb = idx;
    } else if (slot_key == key) {
      PyObject* old_value = table->slots[idx].value;
      Py_INCREF(value);
      table->slots[idx].value = value;
      Py_XDECREF(old_value);
      return 0;
    }
    idx = (idx + 1) & mask;
  }
}

/* New hash-parameterized hot-path APIs (avoid recomputing hash) */
PyObject* memo_table_lookup_h(MemoTable* table, void* key, Py_ssize_t hash) {
  if (!table)
    return NULL;
  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx = hash & mask;
  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key)
      return NULL;
    if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
      return table->slots[idx].value; /* borrowed */
    }
    idx = (idx + 1) & mask;
  }
}

int memo_table_insert_h(MemoTable** table_ptr, void* key, PyObject* value, Py_ssize_t hash) {
  if (memo_table_ensure(table_ptr) < 0)
    return -1;
  MemoTable* table = *table_ptr;

  if ((table->filled * 10) >= (table->size * 7)) {
    if (memo_table_resize(table_ptr, table->used + 1) < 0)
      return -1;
    table = *table_ptr;
  }

  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx = hash & mask;
  Py_ssize_t first_tomb = -1;

  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key) {
      Py_ssize_t insert_at = (first_tomb >= 0) ? first_tomb : idx;
      table->slots[insert_at].key = key;
      Py_INCREF(value);
      table->slots[insert_at].value = value;
      table->used++;
      table->filled++;
      return 0;
    }
    if (slot_key == MEMO_TOMBSTONE) {
      if (first_tomb < 0)
        first_tomb = idx;
    } else if (slot_key == key) {
      PyObject* old_value = table->slots[idx].value;
      Py_INCREF(value);
      table->slots[idx].value = value;
      Py_XDECREF(old_value);
      return 0;
    }
    idx = (idx + 1) & mask;
  }
}

int memo_table_remove(MemoTable* table, void* key) {
  if (!table)
    return -1;
  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx = hash_pointer(key) & mask;
  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key)
      return -1; /* not found */
    if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
      table->slots[idx].key = MEMO_TOMBSTONE;
      Py_XDECREF(table->slots[idx].value);
      table->slots[idx].value = NULL;
      table->used--;
      return 0;
    }
    idx = (idx + 1) & mask;
  }
}

/* ------------------------- Memo type & views -------------------------------
 */

typedef struct _MemoObject MemoObject;
struct _MemoObject {
  PyObject_HEAD
  MemoTable* table;
};

PyTypeObject Memo_Type;

static void Memo_dealloc(MemoObject* self) {
  memo_table_free(self->table);
  Py_TYPE(self)->tp_free((PyObject*)self);
}

PyObject* Memo_New(void) {
  MemoObject* self = PyObject_New(MemoObject, &Memo_Type);
  if (!self) return NULL;
  self->table = NULL;
  return (PyObject*)self;
}

static Py_ssize_t Memo_len(MemoObject* self) {
  return self->table ? self->table->used : 0;
}

static PyObject* Memo_subscript(MemoObject* self, PyObject* pykey) {
  if (!PyLong_Check(pykey)) {
    PyErr_SetString(PyExc_KeyError, "keys must be integers");
    return NULL;
  }
  void* key = PyLong_AsVoidPtr(pykey);
  if (key == NULL && PyErr_Occurred()) return NULL;

  PyObject* value = memo_table_lookup(self->table, key);
  if (!value) {
    PyErr_SetObject(PyExc_KeyError, pykey);
    return NULL;
  }
  Py_INCREF(value);
  return value;
}

static int Memo_ass_subscript(MemoObject* self, PyObject* pykey, PyObject* value) {
  if (!PyLong_Check(pykey)) {
    PyErr_SetString(PyExc_KeyError, "keys must be integers");
    return -1;
  }
  void* key = PyLong_AsVoidPtr(pykey);
  if (key == NULL && PyErr_Occurred()) return -1;

  if (value == NULL) {
    int res = memo_table_remove(self->table, key);
    if (res < 0) {
      PyErr_SetObject(PyExc_KeyError, pykey);
    }
    return res;
  } else {
    return memo_table_insert(&(self->table), key, value);
  }
}

static PyObject* Memo_iter(MemoObject* self) {
  PyObject* keys_list = PyList_New(0);
  if (!keys_list) return NULL;
  if (self->table) {
    for (Py_ssize_t i = 0; i < self->table->size; i++) {
      void* key = self->table->slots[i].key;
      if (key && key != MEMO_TOMBSTONE) {
        PyObject* py_key = PyLong_FromVoidPtr(key);
        if (!py_key || PyList_Append(keys_list, py_key) < 0) {
          Py_XDECREF(py_key);
          Py_DECREF(keys_list);
          return NULL;
        }
        Py_DECREF(py_key);
      }
    }
  }
  PyObject* it = PyObject_GetIter(keys_list);
  Py_DECREF(keys_list);
  return it;
}

static PyObject* Memo_clear(MemoObject* self, PyObject* noargs) {
  (void)noargs;
  if (self->table) {
    memo_table_free(self->table);
    self->table = NULL;
  }
  Py_RETURN_NONE;
}

static PyMappingMethods Memo_as_mapping = {
    (lenfunc)Memo_len,
    (binaryfunc)Memo_subscript,
    (objobjargproc)Memo_ass_subscript
};

static PyObject* Memo_get(MemoObject* self, PyObject* const* args, Py_ssize_t nargs) {
  if (nargs < 1 || nargs > 2) {
    PyErr_SetString(PyExc_TypeError, "get expected 1 or 2 arguments");
    return NULL;
  }
  PyObject* pykey = args[0];
  if (!PyLong_Check(pykey)) {
    PyErr_SetString(PyExc_KeyError, "keys must be integers");
    return NULL;
  }
  void* key = PyLong_AsVoidPtr(pykey);
  if (key == NULL && PyErr_Occurred()) return NULL;
  PyObject* value = memo_table_lookup(self->table, key);
  if (value) {
    Py_INCREF(value);
    return value;
  } else {
    if (nargs == 2) {
      Py_INCREF(args[1]);
      return args[1];
    } else {
      PyErr_SetObject(PyExc_KeyError, pykey);
      return NULL;
    }
  }
}

static PyObject* Memo_contains(MemoObject* self, PyObject* pykey) {
  if (!PyLong_Check(pykey)) {
    PyErr_SetString(PyExc_KeyError, "keys must be integers");
    return NULL;
  }
  void* key = PyLong_AsVoidPtr(pykey);
  if (key == NULL && PyErr_Occurred()) return NULL;
  PyObject* value = memo_table_lookup(self->table, key);
  return PyBool_FromLong(value != NULL);
}

static PyMethodDef Memo_methods[] = {
    {"clear", (PyCFunction)Memo_clear, METH_NOARGS, NULL},
    {"get", (PyCFunction)Memo_get, METH_FASTCALL, NULL},
    {"__contains__", (PyCFunction)Memo_contains, METH_O, NULL},
    {NULL, NULL, 0, NULL}
};

PyTypeObject Memo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "copyc._copying._Memo",
    .tp_basicsize = sizeof(MemoObject),
    .tp_dealloc = (destructor)Memo_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_as_mapping = &Memo_as_mapping,
    .tp_iter = (getiterfunc)Memo_iter,
    .tp_methods = Memo_methods,
};

/* --------------------------- C hooks for _copying.c ------------------------ */

/* Old generic APIs (no precomputed hash) */
PyObject* memo_lookup_obj(PyObject* memo, void* key) {
  if (Py_TYPE(memo) == &Memo_Type) {
    MemoObject* mo = (MemoObject*)memo;
    return memo_table_lookup(mo->table, key);
  } else {
    PyObject* pykey = PyLong_FromVoidPtr(key);
    if (!pykey) return NULL;
    PyObject* res = PyDict_GetItemWithError(memo, pykey);
    Py_DECREF(pykey);
    return res;
  }
}

int memo_store_obj(PyObject* memo, void* key, PyObject* value) {
  if (Py_TYPE(memo) == &Memo_Type) {
    MemoObject* mo = (MemoObject*)memo;
    return memo_table_insert(&mo->table, key, value);
  } else {
    PyObject* pykey = PyLong_FromVoidPtr(key);
    if (!pykey) return -1;
    int res = PyDict_SetItem(memo, pykey, value);
    Py_DECREF(pykey);
    return res;
  }
}

/* New hash-aware hot-path APIs (use only for C _Memo; fall back for PyDict) */
PyObject* memo_lookup_obj_h(PyObject* memo, void* key, Py_ssize_t khash) {
  if (Py_TYPE(memo) == &Memo_Type) {
    MemoObject* mo = (MemoObject*)memo;
    return memo_table_lookup_h(mo->table, key, khash);
  } else {
    /* For Python dicts, we can't rely on internal _KnownHash APIs portably. */
    return memo_lookup_obj(memo, key);
  }
}

int memo_store_obj_h(PyObject* memo, void* key, PyObject* value, Py_ssize_t khash) {
  if (Py_TYPE(memo) == &Memo_Type) {
    MemoObject* mo = (MemoObject*)memo;
    return memo_table_insert_h(&mo->table, key, value, khash);
  } else {
    return memo_store_obj(memo, key, value);
  }
}