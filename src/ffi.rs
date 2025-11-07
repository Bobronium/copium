//! Low-level FFI bindings for direct PyObject manipulation
//! NO PyO3 overhead - these are used on hot paths

use std::os::raw::{c_char, c_int, c_long, c_void};

#[repr(C)]
pub struct PyObject {
    pub ob_refcnt: isize,
    pub ob_type: *mut PyTypeObject,
}

#[repr(C)]
pub struct PyTypeObject {
    pub ob_base: PyVarObject,
    pub tp_name: *const c_char,
    // ... other fields as needed
}

#[repr(C)]
pub struct PyVarObject {
    pub ob_base: PyObject,
    pub ob_size: isize,
}

// Raw Python C API functions
extern "C" {
    pub fn Py_INCREF(op: *mut PyObject);
    pub fn Py_DECREF(op: *mut PyObject);
    pub fn Py_NewRef(op: *mut PyObject) -> *mut PyObject;
    pub fn Py_XNewRef(op: *mut PyObject) -> *mut PyObject;

    pub fn PyObject_GetAttrString(o: *mut PyObject, attr_name: *const c_char) -> *mut PyObject;
    pub fn PyObject_SetAttrString(o: *mut PyObject, attr_name: *const c_char, v: *mut PyObject) -> c_int;
    pub fn PyObject_Call(callable: *mut PyObject, args: *mut PyObject, kwargs: *mut PyObject) -> *mut PyObject;
    pub fn PyObject_CallOneArg(callable: *mut PyObject, arg: *mut PyObject) -> *mut PyObject;
    pub fn PyObject_Vectorcall(callable: *mut PyObject, args: *const *mut PyObject, nargsf: usize, kwnames: *mut PyObject) -> *mut PyObject;

    pub fn PyDict_New() -> *mut PyObject;
    pub fn PyDict_GetItem(dp: *mut PyObject, key: *mut PyObject) -> *mut PyObject;
    pub fn PyDict_SetItem(dp: *mut PyObject, key: *mut PyObject, item: *mut PyObject) -> c_int;
    pub fn PyDict_Next(dp: *mut PyObject, ppos: *mut isize, pkey: *mut *mut PyObject, pvalue: *mut *mut PyObject) -> c_int;
    pub fn PyDict_Size(dp: *mut PyObject) -> isize;

    pub fn PyList_New(size: isize) -> *mut PyObject;
    pub fn PyList_Append(list: *mut PyObject, item: *mut PyObject) -> c_int;
    pub fn PyList_GET_ITEM(list: *mut PyObject, i: isize) -> *mut PyObject;
    pub fn PyList_SET_ITEM(list: *mut PyObject, i: isize, item: *mut PyObject);

    pub fn PyTuple_New(size: isize) -> *mut PyObject;
    pub fn PyTuple_GET_ITEM(tuple: *mut PyObject, i: isize) -> *mut PyObject;
    pub fn PyTuple_SET_ITEM(tuple: *mut PyObject, i: isize, item: *mut PyObject);

    pub fn PyLong_FromVoidPtr(p: *const c_void) -> *mut PyObject;
    pub fn PyLong_AsVoidPtr(obj: *mut PyObject) -> *mut c_void;
    pub fn PyLong_FromLongLong(v: c_long) -> *mut PyObject;

    pub fn PyErr_Occurred() -> *mut PyObject;
    pub fn PyErr_SetString(exc_type: *mut PyObject, message: *const c_char);
    pub fn PyErr_Clear();

    // Type checks
    pub fn PyType_HasFeature(t: *mut PyTypeObject, feature: c_long) -> c_int;
    pub fn PyType_IsSubtype(a: *mut PyTypeObject, b: *mut PyTypeObject) -> c_int;

    // Reduce protocol
    pub fn PyObject_GetIter(o: *mut PyObject) -> *mut PyObject;
    pub fn PyIter_Next(iter: *mut PyObject) -> *mut PyObject;
}

/// Get type from PyObject* without PyO3 overhead
#[inline(always)]
pub unsafe fn py_type(obj: *mut PyObject) -> *mut PyTypeObject {
    (*obj).ob_type
}

/// Fast identity check
#[inline(always)]
pub unsafe fn py_is(a: *mut PyObject, b: *mut PyObject) -> bool {
    a == b
}

/// Safe increment reference count
#[inline(always)]
pub unsafe fn incref(obj: *mut PyObject) {
    if !obj.is_null() {
        Py_INCREF(obj);
    }
}

/// Safe decrement reference count
#[inline(always)]
pub unsafe fn decref(obj: *mut PyObject) {
    if !obj.is_null() {
        Py_DECREF(obj);
    }
}

/// Compute hash from pointer (SplitMix64)
#[inline(always)]
pub fn hash_pointer(ptr: *const c_void) -> isize {
    let mut h = ptr as usize;
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    h as isize
}
