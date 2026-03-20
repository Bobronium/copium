use pyo3_ffi::*;
use std::os::raw::c_int;

use super::PyTypeInfo;

pub unsafe trait PyMutSeqPtr: Sized {
    unsafe fn append<T: PyTypeInfo>(self, item: *mut T) -> c_int;
    unsafe fn insert<T: PyTypeInfo>(self, index: Py_ssize_t, item: *mut T) -> c_int;
    unsafe fn to_tuple(self) -> *mut PyTupleObject;

    #[inline(always)]
    unsafe fn as_tuple(self) -> *mut PyTupleObject {
        self.to_tuple()
    }
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

#[inline(always)]
pub unsafe fn append<L: PyTypeInfo, V: PyTypeInfo>(list: *mut L, item: *mut V) -> c_int {
    pyo3_ffi::PyList_Append(list as *mut PyObject, item as *mut PyObject)
}

#[inline(always)]
pub unsafe fn set_item<L: PyTypeInfo>(list: *mut L, index: Py_ssize_t, item: *mut PyObject) -> c_int {
    pyo3_ffi::PyList_SetItem(list as *mut PyObject, index, item)
}

#[inline(always)]
pub unsafe fn borrow_item<L: PyTypeInfo>(list: *mut L, index: Py_ssize_t) -> *mut PyObject {
    pyo3_ffi::PyList_GET_ITEM(list as *mut PyObject, index)
}

#[inline(always)]
pub unsafe fn size<L: PyTypeInfo>(list: *mut L) -> Py_ssize_t {
    pyo3_ffi::PyList_GET_SIZE(list as *mut PyObject)
}

#[inline(always)]
pub unsafe fn insert<L: PyTypeInfo, V: PyTypeInfo>(list: *mut L, index: Py_ssize_t, item: *mut V) -> c_int {
    pyo3_ffi::PyList_Insert(list as *mut PyObject, index, item as *mut PyObject)
}

#[inline(always)]
pub unsafe fn check<L: PyTypeInfo>(list: *mut L) -> bool {
    pyo3_ffi::PyList_Check(list as *mut PyObject) != 0
}

#[inline(always)]
pub unsafe fn as_tuple<L: PyTypeInfo>(list: *mut L) -> *mut PyTupleObject {
    pyo3_ffi::PyList_AsTuple(list as *mut PyObject) as *mut PyTupleObject
}
