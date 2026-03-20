use pyo3_ffi::*;
use std::os::raw::c_int;
use std::ptr;

use super::{ffi, PyFrozensetObject, PyTypeInfo};

pub unsafe trait PySetPtr: Sized {
    unsafe fn size(self) -> Py_ssize_t;
    unsafe fn next_entry(
        self,
        position: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        hash: &mut Py_hash_t,
    ) -> bool;

    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        self.size()
    }
}

pub unsafe trait PyMutSetPtr: Sized {
    unsafe fn add<T: PyTypeInfo>(self, item: *mut T) -> c_int;

    #[inline(always)]
    unsafe fn add_item<T: PyTypeInfo>(self, item: *mut T) -> c_int {
        self.add(item)
    }
}

unsafe impl PySetPtr for *mut PySetObject {
    #[inline(always)]
    unsafe fn size(self) -> Py_ssize_t {
        pyo3_ffi::PySet_Size(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn next_entry(
        self,
        position: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        hash: &mut Py_hash_t,
    ) -> bool {
        ffi::_PySet_NextEntry(self as *mut PyObject, position, key, hash) != 0
    }
}

unsafe impl PySetPtr for *mut PyFrozensetObject {
    #[inline(always)]
    unsafe fn size(self) -> Py_ssize_t {
        pyo3_ffi::PySet_Size(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn next_entry(
        self,
        position: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        hash: &mut Py_hash_t,
    ) -> bool {
        ffi::_PySet_NextEntry(self as *mut PyObject, position, key, hash) != 0
    }
}

unsafe impl PyMutSetPtr for *mut PySetObject {
    #[inline(always)]
    unsafe fn add<T: PyTypeInfo>(self, item: *mut T) -> c_int {
        pyo3_ffi::PySet_Add(self as *mut PyObject, item as *mut PyObject)
    }
}

#[inline(always)]
pub unsafe fn new() -> *mut PySetObject {
    pyo3_ffi::PySet_New(ptr::null_mut()) as *mut PySetObject
}

#[inline(always)]
pub unsafe fn from<T: PyTypeInfo>(iterable: *mut T) -> *mut PySetObject {
    pyo3_ffi::PySet_New(iterable as *mut PyObject) as *mut PySetObject
}

#[inline(always)]
pub unsafe fn frozen_from<T: PyTypeInfo>(iterable: *mut T) -> *mut PyFrozensetObject {
    pyo3_ffi::PyFrozenSet_New(iterable as *mut PyObject) as *mut PyFrozensetObject
}
