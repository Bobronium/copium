/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * copyc._memo (compiled into copyc._copying extension)
 * - Memo type (internal hash table for deepcopy memo)
 * - Implements MutableMapping protocol with views
 * - Keepalive vector with a Python-facing proxy implementing a MutableSequence
 * - C hooks used by _copying.c:
 *     - Memo_New, memo_lookup_obj(_h), memo_store_obj(_h), memo_hash_pointer
 *     - memo_keepalive_ensure, memo_keepalive_append, memo_ready_types
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
  void* key;       /* object address; NULL = empty; (void*)-1 = tombstone */
  PyObject* value; /* owned reference */
} MemoEntry;

typedef struct {
  MemoEntry*  slots;
  Py_ssize_t  size;    /* power of two */
  Py_ssize_t  used;    /* non-empty (excludes tombstones) */
  Py_ssize_t  filled;  /* non-empty + tombstones */
} MemoTable;

/* ------------------------------ Keep vector -------------------------------- */

typedef struct {
  PyObject**  items;
  Py_ssize_t  size;
  Py_ssize_t  capacity;
} KeepVector;

/* Forward decl to refer to Memo_Type in helpers */
typedef struct _MemoObject MemoObject;

#define MEMO_TOMBSTONE ((void*)(uintptr_t)(-1))

/* SplitMix64-style pointer hasher, stable across the process. */
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

/* ------------------------------ Keep vector impl --------------------------- */

static void keepvector_init(KeepVector* kv) {
  kv->items = NULL;
  kv->size = 0;
  kv->capacity = 0;
}

static int keepvector_grow(KeepVector* kv, Py_ssize_t min_capacity) {
  Py_ssize_t new_cap = kv->capacity > 0 ? kv->capacity : 8;
  while (new_cap < min_capacity) {
    /* simple doubling with overflow clamp */
    Py_ssize_t next = new_cap << 1;
    if (next <= 0 || next < new_cap) { /* overflow */
      new_cap = min_capacity;
      break;
    }
    new_cap = next;
  }
  PyObject** ni = (PyObject**)PyMem_Realloc(kv->items, (size_t)new_cap * sizeof(PyObject*));
  if (!ni) return -1;
  kv->items    = ni;
  kv->capacity = new_cap;
  return 0;
}

static int keepvector_append(KeepVector* kv, PyObject* obj) {
  if (kv->size >= kv->capacity) {
    if (keepvector_grow(kv, kv->size + 1) < 0) return -1;
  }
  Py_INCREF(obj);
  kv->items[kv->size++] = obj;
  return 0;
}

static void keepvector_clear(KeepVector* kv) {
  for (Py_ssize_t i = 0; i < kv->size; i++) {
    Py_XDECREF(kv->items[i]);
  }
  kv->size = 0;
}

static void keepvector_free(KeepVector* kv) {
  keepvector_clear(kv);
  PyMem_Free(kv->items);
  kv->items = NULL;
  kv->capacity = 0;
}

/* ------------------------------ Memo table impl ---------------------------- */

void memo_table_free(MemoTable* table) {
  if (!table) return;
  for (Py_ssize_t i = 0; i < table->size; i++) {
    if (table->slots[i].key && table->slots[i].key != MEMO_TOMBSTONE) {
      Py_XDECREF(table->slots[i].value);
    }
  }
  free(table->slots);
  free(table);
}

static int memo_table_resize(MemoTable** table_ptr, Py_ssize_t min_capacity_needed) {
  MemoTable* old = *table_ptr;
  Py_ssize_t new_size = 8;
  while (new_size < (min_capacity_needed * 2)) {
    Py_ssize_t next = new_size << 1;
    if (next <= 0 || next < new_size) { /* overflow clamp */
      new_size = (Py_ssize_t)1 << (sizeof(void*) * 8 - 2);
      break;
    }
    new_size = next;
  }

  MemoEntry* new_slots = (MemoEntry*)calloc((size_t)new_size, sizeof(MemoEntry));
  if (!new_slots) return -1;

  MemoTable* nt = (MemoTable*)malloc(sizeof(MemoTable));
  if (!nt) {
    free(new_slots);
    return -1;
  }
  nt->slots = new_slots;
  nt->size  = new_size;
  nt->used  = 0;
  nt->filled= 0;

  if (old) {
    for (Py_ssize_t i = 0; i < old->size; i++) {
      void* key = old->slots[i].key;
      if (key && key != MEMO_TOMBSTONE) {
        PyObject* value = old->slots[i].value; /* transfer */
        Py_ssize_t mask = nt->size - 1;
        Py_ssize_t idx  = hash_pointer(key) & mask;
        while (nt->slots[idx].key) idx = (idx + 1) & mask;
        nt->slots[idx].key   = key;
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
  if (*table_ptr) return 0;
  return memo_table_resize(table_ptr, 1);
}

/* Existing non-hash-parameterized APIs (kept for compatibility) */
PyObject* memo_table_lookup(MemoTable* table, void* key) {
  if (!table) return NULL;
  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx  = hash_pointer(key) & mask;
  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key) return NULL;
    if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
      return table->slots[idx].value; /* borrowed */
    }
    idx = (idx + 1) & mask;
  }
}

int memo_table_insert(MemoTable** table_ptr, void* key, PyObject* value) {
  if (memo_table_ensure(table_ptr) < 0) return -1;
  MemoTable* table = *table_ptr;

  if ((table->filled * 10) >= (table->size * 7)) {
    if (memo_table_resize(table_ptr, table->used + 1) < 0) return -1;
    table = *table_ptr;
  }

  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx  = hash_pointer(key) & mask;
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
      if (first_tomb < 0) first_tomb = idx;
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
  if (!table) return NULL;
  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx  = hash & mask;
  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key) return NULL;
    if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
      return table->slots[idx].value; /* borrowed */
    }
    idx = (idx + 1) & mask;
  }
}

int memo_table_insert_h(MemoTable** table_ptr, void* key, PyObject* value, Py_ssize_t hash) {
  if (memo_table_ensure(table_ptr) < 0) return -1;
  MemoTable* table = *table_ptr;

  if ((table->filled * 10) >= (table->size * 7)) {
    if (memo_table_resize(table_ptr, table->used + 1) < 0) return -1;
    table = *table_ptr;
  }

  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx  = hash & mask;
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
      if (first_tomb < 0) first_tomb = idx;
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
  if (!table) return -1;
  Py_ssize_t mask = table->size - 1;
  Py_ssize_t idx  = hash_pointer(key) & mask;
  for (;;) {
    void* slot_key = table->slots[idx].key;
    if (!slot_key) return -1; /* not found */
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

/* ------------------------- Memo type & keep proxy -------------------------- */

struct _MemoObject {
  PyObject_HEAD
  MemoTable* table;
  KeepVector keep; /* internal keepalive vector */
};

PyTypeObject Memo_Type;

/* _KeepList proxy ============================================================
 * A thin Python object that forwards to MemoObject.keep.
 * It is *not* the owner of the storage; it keeps a strong ref to MemoObject.
 */

typedef struct {
  PyObject_HEAD
  MemoObject* owner; /* strong ref to the memo owning the vector */
} KeepListObject;

static PyTypeObject KeepList_Type;

/* Forward decls */
static PyObject* KeepList_New(MemoObject* owner);

static void KeepList_dealloc(KeepListObject* self) {
  Py_XDECREF(self->owner);
  Py_TYPE(self)->tp_free((PyObject*)self);
}

static Py_ssize_t KeepList_len(KeepListObject* self) {
  return self->owner && self->owner->keep.items ? self->owner->keep.size : 0;
}

static PyObject* KeepList_getitem(KeepListObject* self, Py_ssize_t index) {
  if (!self->owner) {
    PyErr_SetString(PyExc_SystemError, "_KeepList has no owner");
    return NULL;
  }
  KeepVector* kv = &self->owner->keep;
  Py_ssize_t n = kv->size;
  if (index < 0) index += n;
  if (index < 0 || index >= n) {
    PyErr_SetString(PyExc_IndexError, "index out of range");
    return NULL;
  }
  PyObject* item = kv->items[index];
  return Py_NewRef(item);
}

static PyObject* KeepList_iter(KeepListObject* self) {
  if (!self->owner) {
    PyErr_SetString(PyExc_SystemError, "_KeepList has no owner");
  }
  KeepVector* kv = &self->owner->keep;
  Py_ssize_t n = kv->size;
  PyObject* list = PyList_New(n);
  if (!list) return NULL;
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject* it = kv->items[i];
    Py_INCREF(it);
    PyList_SET_ITEM(list, i, it);
  }
  PyObject* it = PyObject_GetIter(list);
  Py_DECREF(list);
  return it;
}

static PyObject* KeepList_append(KeepListObject* self, PyObject* arg) {
  if (!self->owner) {
    PyErr_SetString(PyExc_SystemError, "_KeepList has no owner");
    return NULL;
  }
  if (keepvector_append(&self->owner->keep, arg) < 0) return NULL;
  Py_RETURN_NONE;
}

static PyObject* KeepList_clear(KeepListObject* self, PyObject* noargs) {
  (void)noargs;
  if (!self->owner) {
    PyErr_SetString(PyExc_SystemError, "_KeepList has no owner");
    return NULL;
  }
  keepvector_clear(&self->owner->keep);
  Py_RETURN_NONE;
}

static PySequenceMethods KeepList_as_sequence = {
  (lenfunc)KeepList_len,          /* sq_length */
  0,                              /* sq_concat */
  0,                              /* sq_repeat */
  (ssizeargfunc)KeepList_getitem, /* sq_item */
  0,                              /* sq_slice (deprecated) */
  0,                              /* sq_ass_item */
  0,                              /* sq_ass_slice (deprecated) */
  0,                              /* sq_contains */
  0,                              /* sq_inplace_concat */
  0                               /* sq_inplace_repeat */
};

static PyMethodDef KeepList_methods[] = {
  {"append", (PyCFunction)KeepList_append, METH_O, NULL},
  {"clear",  (PyCFunction)KeepList_clear,  METH_NOARGS, NULL},
  {NULL, NULL, 0, NULL}
};

static PyObject* KeepList_New(MemoObject* owner) {
  KeepListObject* self = PyObject_New(KeepListObject, &KeepList_Type);
  if (!self) return NULL;
  Py_INCREF(owner);
  self->owner = owner;
  return (PyObject*)self;
}

static PyTypeObject KeepList_Type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name      = "copyc._copying._KeepList",
  .tp_basicsize = sizeof(KeepListObject),
  .tp_dealloc   = (destructor)KeepList_dealloc,
  .tp_flags     = Py_TPFLAGS_DEFAULT,
  .tp_as_sequence = &KeepList_as_sequence,
  .tp_iter      = (getiterfunc)KeepList_iter,
  .tp_methods   = KeepList_methods,
};

/* --------------------------- Memo object impl ------------------------------ */

static void Memo_dealloc(MemoObject* self) {
  memo_table_free(self->table);
  keepvector_free(&self->keep);
  Py_TYPE(self)->tp_free((PyObject*)self);
}

PyObject* Memo_New(void) {
  MemoObject* self = PyObject_New(MemoObject, &Memo_Type);
  if (!self) return NULL;
  self->table = NULL;
  keepvector_init(&self->keep);
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
  keepvector_clear(&self->keep);
  Py_RETURN_NONE;
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

static PyObject* Memo_keep(MemoObject* self, PyObject* noargs) {
  (void)noargs;
  /* Expose a (fresh) proxy each time; storage lives in self->keep. */
  return KeepList_New(self);
}

static PyMappingMethods Memo_as_mapping = {
    (lenfunc)Memo_len,
    (binaryfunc)Memo_subscript,
    (objobjargproc)Memo_ass_subscript
};

static PyMethodDef Memo_methods[] = {
    {"clear", (PyCFunction)Memo_clear, METH_NOARGS, NULL},
    {"get",   (PyCFunction)NULL,       METH_FASTCALL, NULL}, /* populated below for ABI stability */
    {"__contains__", (PyCFunction)Memo_contains, METH_O, NULL},
    {"keep", (PyCFunction)Memo_keep, METH_NOARGS, NULL}, /* expose keepalive proxy */
    {NULL, NULL, 0, NULL}
};

/* Late-bound 'get' to keep identical signature as before */
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

PyTypeObject Memo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "copyc._copying._Memo",
    .tp_basicsize = sizeof(MemoObject),
    .tp_dealloc   = (destructor)Memo_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_as_mapping= &Memo_as_mapping,
    .tp_iter      = (getiterfunc)Memo_iter,
    .tp_methods   = Memo_methods,
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

/* --------------------------- Keepalive unification ------------------------- */

/* Ensure *keep_proxy_ptr contains a proxy sequence for appending:
 * - For C Memo: returns a new _KeepList bound to memo->keep
 * - For dict memo: returns a Python list under memo[id(memo)]
 */
int memo_keepalive_ensure(PyObject** memo_ptr, PyObject** keep_proxy_ptr) {
  if (*keep_proxy_ptr) return 0;

  if (*memo_ptr == NULL) {
    /* Lazily create a Memo if none was provided yet */
    PyObject* m = Memo_New();
    if (!m) return -1;
    *memo_ptr = m;
  }

  PyObject* memo = *memo_ptr;
  if (Py_TYPE(memo) == &Memo_Type) {
    /* C Memo: compute hash once, ensure memo[id(memo)] is a _KeepList and reuse it */
    MemoObject* mo = (MemoObject*)memo;
    void* key = (void*)memo;
    Py_ssize_t khash = memo_hash_pointer(key);

    PyObject* existing = memo_lookup_obj_h(memo, key, khash); /* borrowed */
    if (existing) {
      Py_INCREF(existing);
      *keep_proxy_ptr = existing;
      return 0;
    }
    if (PyErr_Occurred())
      return -1;

    PyObject* proxy = KeepList_New(mo);
    if (!proxy)
      return -1;

    if (memo_store_obj_h(memo, key, proxy, khash) < 0) {
      Py_DECREF(proxy);
      return -1;
    }
    *keep_proxy_ptr = proxy; /* owned */
    return 0;
  } else {
    /* Python dict path: use memo[id(memo)] list */
    void* memo_id = (void*)memo;
    PyObject* pykey = PyLong_FromVoidPtr(memo_id);
    if (!pykey) return -1;

    PyObject* existing = PyDict_GetItemWithError(memo, pykey); /* borrowed */
    if (!existing) {
      if (PyErr_Occurred()) {
        Py_DECREF(pykey);
        return -1;
      }
      PyObject* new_list = PyList_New(0);
      if (!new_list) {
        Py_DECREF(pykey);
        return -1;
      }
      if (PyDict_SetItem(memo, pykey, new_list) < 0) {
        Py_DECREF(pykey);
        Py_DECREF(new_list);
        return -1;
      }
      existing = new_list; /* still owned below after INCREF */
      Py_DECREF(new_list); /* Dict now owns the reference */
      /* Reborrow below; we INCREF a new handle for caller */
      existing = PyDict_GetItemWithError(memo, pykey);
      if (!existing) { Py_DECREF(pykey); return -1; }
    }
    Py_INCREF(existing);
    *keep_proxy_ptr = existing;
    Py_DECREF(pykey);
    return 0;
  }
}

/* Append obj to keepalive, creating storage if needed. */
int memo_keepalive_append(PyObject** memo_ptr, PyObject** keep_proxy_ptr, PyObject* obj) {
  if (memo_keepalive_ensure(memo_ptr, keep_proxy_ptr) < 0) return -1;
  PyObject* memo = *memo_ptr;

  if (Py_TYPE(memo) == &Memo_Type) {
    MemoObject* mo = (MemoObject*)memo;
    if (keepvector_append(&mo->keep, obj) < 0) return -1;
    return 0;
  } else {
    /* Python list */
    return PyList_Append(*keep_proxy_ptr, obj);
  }
}

/* --------------------------- Type readiness helper ------------------------- */

/* Called from module init in _copying.c */
int memo_ready_types(void) {
  /* Fill in Memo.get at runtime (keeps the .methods table layout stable) */
  for (PyMethodDef* m = Memo_methods; m && m->ml_name; ++m) {
    if (m->ml_meth == NULL && strcmp(m->ml_name, "get") == 0) {
      m->ml_meth = (PyCFunction)Memo_get;
      m->ml_flags = METH_FASTCALL;
      break;
    }
  }

  if (PyType_Ready(&KeepList_Type) < 0) return -1;
  if (PyType_Ready(&Memo_Type) < 0) return -1;
  return 0;
}