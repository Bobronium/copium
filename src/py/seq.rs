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
    unsafe fn borrow_item_unchecked(self, index: Py_ssize_t) -> *mut PyObject;
    unsafe fn steal_item_unchecked(self, index: Py_ssize_t, value: *mut PyObject);

    #[inline(always)]
    unsafe fn borrow_item(self, index: Py_ssize_t) -> *mut PyObject {
        if unlikely(!is_valid_index(index, self.length())) {
            return ptr::null_mut();
        }
        self.borrow_item_unchecked(index)
    }

    #[inline(always)]
    unsafe fn steal_item(self, index: Py_ssize_t, value: *mut PyObject) -> c_int {
        if unlikely(!is_valid_index(index, self.length())) {
            return -1;
        }
        self.steal_item_unchecked(index, value);
        0
    }

    #[inline(always)]
    unsafe fn own_item(self, index: Py_ssize_t) -> *mut PyObject {
        if unlikely(!is_valid_index(index, self.length())) {
            return ptr::null_mut();
        }
        self.borrow_item_unchecked(index).newref()
    }

    #[inline(always)]
    unsafe fn get_borrowed_unchecked(self, index: Py_ssize_t) -> *mut PyObject {
        self.borrow_item_unchecked(index)
    }

    #[inline(always)]
    unsafe fn set_slot_steal_unchecked(self, index: Py_ssize_t, value: *mut PyObject) {
        self.steal_item_unchecked(index, value)
    }

    #[inline(always)]
    unsafe fn get_owned_check_bounds(self, index: Py_ssize_t) -> *mut PyObject {
        self.own_item(index)
    }
}

unsafe impl PySeqPtr for *mut PyListObject {
    #[inline(always)]
    unsafe fn length(&self) -> Py_ssize_t {
        pyo3_ffi::PyList_GET_SIZE(*self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn borrow_item_unchecked(self, index: Py_ssize_t) -> *mut PyObject {
        pyo3_ffi::PyList_GET_ITEM(self as *mut PyObject, index)
    }

    #[inline(always)]
    unsafe fn steal_item_unchecked(self, index: Py_ssize_t, value: *mut PyObject) {
        pyo3_ffi::PyList_SET_ITEM(self as *mut PyObject, index, value)
    }
}

unsafe impl PySeqPtr for *mut PyTupleObject {
    #[inline(always)]
    unsafe fn length(&self) -> Py_ssize_t {
        pyo3_ffi::PyTuple_GET_SIZE(*self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn borrow_item_unchecked(self, index: Py_ssize_t) -> *mut PyObject {
        pyo3_ffi::PyTuple_GET_ITEM(self as *mut PyObject, index)
    }

    #[inline(always)]
    unsafe fn steal_item_unchecked(self, index: Py_ssize_t, value: *mut PyObject) {
        pyo3_ffi::PyTuple_SET_ITEM(self as *mut PyObject, index, value)
    }
}

#[inline(always)]
pub unsafe fn fast<T: PyTypeInfo>(object: *mut T, message: &CStr) -> *mut PyObject {
    ffi::PySequence_Fast(object as *mut PyObject, message.as_ptr())
}

#[inline(always)]
pub unsafe fn fast_borrow_item_unchecked<T: PyTypeInfo>(
    fast_sequence: *mut T,
    index: Py_ssize_t,
) -> *mut PyObject {
    ffi::PySequence_Fast_GET_ITEM(fast_sequence as *mut PyObject, index)
}

#[inline(always)]
pub unsafe fn fast_length<T: PyTypeInfo>(fast_sequence: *mut T) -> Py_ssize_t {
    ffi::PySequence_Fast_GET_SIZE(fast_sequence as *mut PyObject)
}

#[inline(always)]
pub unsafe fn to_tuple<T: PyTypeInfo>(object: *mut T) -> *mut PyTupleObject {
    pyo3_ffi::PySequence_Tuple(object as *mut PyObject) as *mut PyTupleObject
}

#[inline(always)]
pub unsafe fn to_list<T: PyTypeInfo>(object: *mut T) -> *mut PyListObject {
    pyo3_ffi::PySequence_List(object as *mut PyObject) as *mut PyListObject
}
