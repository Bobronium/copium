#![allow(non_snake_case)]
// These shims intentionally mirror CPython C API names.

use std::os::raw::c_int;

use pyo3_ffi::*;

#[cfg(not(Py_3_13))]
use crate::types::PyObjectPtr;

extern "C" {
    pub fn _PyDict_NewPresized(minused: Py_ssize_t) -> *mut PyObject;
    pub fn _PySet_NextEntry(
        set: *mut PyObject,
        pos: *mut Py_ssize_t,
        key: *mut *mut PyObject,
        hash: *mut Py_hash_t,
    ) -> c_int;
}

#[cfg(Py_3_13)]
extern "C" {
    pub fn _PyDict_SetItem_Take2(
        op: *mut PyObject,
        key: *mut PyObject,
        value: *mut PyObject,
    ) -> c_int;
}

#[cfg(not(Py_3_13))]
#[inline]
pub unsafe fn _PyDict_SetItem_Take2(
    op: *mut PyObject,
    key: *mut PyObject,
    value: *mut PyObject,
) -> c_int {
    unsafe {
        let status = PyDict_SetItem(op, key, value);
        key.decref();
        value.decref();
        status
    }
}

#[cfg(any(Py_3_13, Py_3_14))]
extern "C" {
    pub fn PyObject_GetOptionalAttr(
        obj: *mut PyObject,
        name: *mut PyObject,
        result: *mut *mut PyObject,
    ) -> c_int;
}

#[cfg(not(any(Py_3_13, Py_3_14)))]
extern "C" {
    fn _PyObject_LookupAttr(
        obj: *mut PyObject,
        name: *mut PyObject,
        result: *mut *mut PyObject,
    ) -> c_int;
}

#[cfg(not(any(Py_3_13, Py_3_14)))]
#[inline]
pub unsafe fn PyObject_GetOptionalAttr(
    obj: *mut PyObject,
    name: *mut PyObject,
    result: *mut *mut PyObject,
) -> c_int {
    unsafe { _PyObject_LookupAttr(obj, name, result) }
}
