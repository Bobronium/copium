#ifndef _COPIUM_MEMO_H
#define _COPIUM_MEMO_H

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

typedef struct {
  PyObject**  items;
  Py_ssize_t  size;
  Py_ssize_t  capacity;
} KeepVector;

struct _MemoObject {
  PyObject_HEAD
  MemoTable* table;
  KeepVector keep; /* internal keepalive vector */
};

void memo_table_free(MemoTable* table);
void keepvector_clear(KeepVector* kv);

#endif