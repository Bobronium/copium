use pyo3_ffi::*;
use std::os::raw::c_int;

use super::PyTypeInfo;

#[inline(always)]
pub unsafe fn new(length: Py_ssize_t) -> *mut PyTupleObject {
    pyo3_ffi::PyTuple_New(length) as *mut PyTupleObject
}

#[inline(always)]
pub unsafe fn size<T: PyTypeInfo>(tuple: *mut T) -> Py_ssize_t {
    pyo3_ffi::PyTuple_GET_SIZE(tuple as *mut PyObject)
}

#[inline(always)]
pub unsafe fn get_item<T: PyTypeInfo>(tuple: *mut T, index: Py_ssize_t) -> *mut PyObject {
    pyo3_ffi::PyTuple_GET_ITEM(tuple as *mut PyObject, index)
}

#[inline(always)]
pub unsafe fn set_item<T: PyTypeInfo>(tuple: *mut T, index: Py_ssize_t, item: *mut PyObject) -> c_int {
    pyo3_ffi::PyTuple_SetItem(tuple as *mut PyObject, index, item)
}

#[inline(always)]
pub unsafe fn check<T: PyTypeInfo>(tuple: *mut T) -> bool {
    pyo3_ffi::PyTuple_Check(tuple as *mut PyObject) != 0
}
