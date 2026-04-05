use pyo3_ffi::*;
use std::ffi::CStr;
use std::hint::unlikely;
use std::os::raw::c_int;
use std::ptr;

use super::{ffi, object::PyObjectPtr, PyTypeInfo};

#[inline(always)]
unsafe fn is_valid_index(index: Py_ssize_t, limit: Py_ssize_t) -> bool {
    (index as usize) < (limit as usize)
}

pub unsafe trait PySeqPtr: Sized {
    unsafe fn length(&self) -> Py_ssize_t;
    unsafe fn get_borrowed_unchecked(self, index: Py_ssize_t) -> *mut PyObject;
    unsafe fn steal_item_unchecked<T: PyTypeInfo>(self, index: Py_ssize_t, value: *mut T);

    #[inline(always)]
    unsafe fn steal_item<T: PyTypeInfo>(self, index: Py_ssize_t, value: *mut T) -> c_int {
        if unlikely(!is_valid_index(index, self.length())) {
            return -1;
        }
        self.steal_item_unchecked(index, value);
        0
    }

    #[inline(always)]
    unsafe fn get_owned_check_bounds(self, index: Py_ssize_t) -> *mut PyObject {
        if unlikely(!is_valid_index(index, self.length())) {
            return ptr::null_mut();
        }
        self.get_borrowed_unchecked(index).newref()
    }
}

unsafe impl PySeqPtr for *mut PyListObject {
    #[inline(always)]
    unsafe fn length(&self) -> Py_ssize_t {
        pyo3_ffi::PyList_GET_SIZE(*self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn get_borrowed_unchecked(self, index: Py_ssize_t) -> *mut PyObject {
        pyo3_ffi::PyList_GET_ITEM(self as *mut PyObject, index)
    }

    #[inline(always)]
    unsafe fn steal_item_unchecked<T: PyTypeInfo>(self, index: Py_ssize_t, value: *mut T) {
        pyo3_ffi::PyList_SET_ITEM(self as *mut PyObject, index, value as *mut PyObject)
    }
}

unsafe impl PySeqPtr for *mut PyTupleObject {
    #[inline(always)]
    unsafe fn length(&self) -> Py_ssize_t {
        pyo3_ffi::PyTuple_GET_SIZE(*self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn get_borrowed_unchecked(self, index: Py_ssize_t) -> *mut PyObject {
        pyo3_ffi::PyTuple_GET_ITEM(self as *mut PyObject, index)
    }

    #[inline(always)]
    unsafe fn steal_item_unchecked<T: PyTypeInfo>(self, index: Py_ssize_t, value: *mut T) {
        pyo3_ffi::PyTuple_SET_ITEM(self as *mut PyObject, index, value as *mut PyObject)
    }
}
