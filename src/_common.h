/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef Py_BUILD_CORE_MODULE
#define Py_BUILD_CORE_MODULE
#endif
#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif

#ifndef COPIUM_COMMON_H
#define COPIUM_COMMON_H

#define PY_VERSION_3_11_HEX 0x030B0000
#define PY_VERSION_3_12_HEX 0x030C0000
#define PY_VERSION_3_13_HEX 0x030D0000
#define PY_VERSION_3_14_HEX 0x030E0000
#define PY_VERSION_3_15_HEX 0x030F0000

#ifndef LIKELY
    #if defined(__GNUC__) || defined(__clang__)
        #define LIKELY(x) __builtin_expect(!!(x), 1)
        #define UNLIKELY(x) __builtin_expect(!!(x), 0)
    #else
        #define LIKELY(x) (x)
        #define UNLIKELY(x) (x)
    #endif
#endif

#ifndef UNLIKELY
    #define UNLIKELY(x) (x)
#endif

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
    #define ALWAYS_INLINE \
        inline __attribute__((always_inline)) COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
    #define MAYBE_INLINE inline COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
#else
    #define ALWAYS_INLINE inline
    #define MAYBE_INLINE inline
#endif

#ifdef Py_GIL_DISABLED
    #define COPIUM_Py_BEGIN_CRITICAL_SECTION(op) Py_BEGIN_CRITICAL_SECTION(op)
    #define COPIUM_Py_END_CRITICAL_SECTION() Py_END_CRITICAL_SECTION()
#else
    #define COPIUM_Py_BEGIN_CRITICAL_SECTION(op)
    #define COPIUM_Py_END_CRITICAL_SECTION()
#endif

#include <Python.h>

#include "pycore_gc.h"
#include "pycore_object.h"
#include "pycore_list.h"

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
    #define PyObject_GetOptionalAttr(obj, name, out) _PyObject_LookupAttr((obj), (name), (out))
#endif

#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
    #include "pycore_dict.h"
    #define COPIUM_PyDict_SetItem_Take2(op, key, value) _PyDict_SetItem_Take2(op, key, value)
#else
static inline int COPIUM_PyDict_SetItem_Take2(PyDictObject* op, PyObject* key, PyObject* value) {
    int res = PyDict_SetItem((PyObject*)op, key, value);
    Py_DECREF(key);
    Py_DECREF(value);
    return res;
}
#endif

static inline int valid_index(Py_ssize_t i, Py_ssize_t limit) {
    /* The cast to size_t lets us use just a single comparison
       to check whether i is in the range: 0 <= i < limit.

       See:  Section 14.2 "Bounds Checking" in the Agner Fog
       optimization manual found at:
       https://www.agner.org/optimize/optimizing_cpp.pdf
    */
    return (size_t)i < (size_t)limit;
}

#ifdef Py_GIL_DISABLED
    #if PY_VERSION_HEX >= PY_VERSION_3_15_HEX
        #define COPIUM_PyList_GET_ITEM_REF(op, i) PyList_GetItemRef((op), (i))
    #else  // code below is a copy-paste from CPython's listobject.c

typedef struct {
    Py_ssize_t allocated;
    PyObject* ob_item[];
} _PyListArray;

static Py_ssize_t list_capacity(PyObject** items) {
    _PyListArray* array = _Py_CONTAINER_OF(items, _PyListArray, ob_item);
    return array->allocated;
}

static PyObject* list_item_impl(PyListObject* self, Py_ssize_t idx) {
    PyObject* item = NULL;
    Py_BEGIN_CRITICAL_SECTION(self);
    if (!_PyObject_GC_IS_SHARED(self)) {
        _PyObject_GC_SET_SHARED(self);
    }
    Py_ssize_t size = Py_SIZE(self);
    if (UNLIKELY(!valid_index(idx, size))) {
        goto exit;
    }
    item = _Py_NewRefWithLock(self->ob_item[idx]);
exit:
    Py_END_CRITICAL_SECTION();
    return item;
}

static inline PyObject* list_get_item_ref(PyListObject* op, Py_ssize_t i) {
    if (!_Py_IsOwnedByCurrentThread((PyObject*)op) && !_PyObject_GC_IS_SHARED(op)) {
        return list_item_impl(op, i);
    }
    // Need atomic operation for the getting size.
    Py_ssize_t size = PyList_GET_SIZE(op);
    if (UNLIKELY(!valid_index(i, size))) {
        return NULL;
    }
    PyObject** ob_item = _Py_atomic_load_ptr(&op->ob_item);
    if (UNLIKELY(ob_item == NULL)) {
        return NULL;
    }
    Py_ssize_t cap = list_capacity(ob_item);
    assert(cap != -1);
    if (UNLIKELY(!valid_index(i, cap))) {
        return NULL;
    }
    PyObject* item = _Py_TryXGetRef(&ob_item[i]);
    if (UNLIKELY(item == NULL)) {
        return list_item_impl(op, i);
    }
    return item;
}
        #define COPIUM_PyList_GET_ITEM_REF(op, i) list_get_item_ref(((PyListObject*)op), (i))
    #endif
#else
static inline PyObject* list_get_item_ref(PyListObject* op, Py_ssize_t i) {
    if (!valid_index(i, Py_SIZE(op))) {
        return NULL;
    }
    return Py_NewRef(PyList_GET_ITEM(op, i));
}
    #define COPIUM_PyList_GET_ITEM_REF(op, i) list_get_item_ref(((PyListObject*)op), (i))
#endif

#endif /* COPIUM_COMMON_H */