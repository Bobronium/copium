use pyo3_ffi::*;
use std::os::raw::c_int;

use super::PyTypeInfo;

pub unsafe trait PyMutSeqPtr: Sized {
    unsafe fn append<T: PyTypeInfo>(self, item: *mut T) -> c_int;
    unsafe fn insert<T: PyTypeInfo>(self, index: Py_ssize_t, item: *mut T) -> c_int;
    unsafe fn to_tuple(self) -> *mut PyTupleObject;
}

unsafe impl PyMutSeqPtr for *mut PyListObject {
    #[inline(always)]
    unsafe fn append<T: PyTypeInfo>(self, item: *mut T) -> c_int {
        pyo3_ffi::PyList_Append(self as *mut PyObject, item as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn insert<T: PyTypeInfo>(self, index: Py_ssize_t, item: *mut T) -> c_int {
        pyo3_ffi::PyList_Insert(self as *mut PyObject, index, item as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn to_tuple(self) -> *mut PyTupleObject {
        pyo3_ffi::PyList_AsTuple(self as *mut PyObject) as *mut PyTupleObject
    }
}

#[inline(always)]
pub unsafe fn new(length: Py_ssize_t) -> *mut PyListObject {
    pyo3_ffi::PyList_New(length) as *mut PyListObject
}
