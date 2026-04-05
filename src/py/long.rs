use pyo3_ffi::*;
use std::ffi::c_void;

use super::PyTypeInfo;

pub unsafe trait PyLongPtr {
    unsafe fn as_i64(self) -> i64;
    unsafe fn as_void_ptr(self) -> *mut c_void;
}

unsafe impl<T: PyTypeInfo> PyLongPtr for *mut T {
    #[inline(always)]
    unsafe fn as_i64(self) -> i64 {
        pyo3_ffi::PyLong_AsLong(self as *mut PyObject) as i64
    }

    #[inline(always)]
    unsafe fn as_void_ptr(self) -> *mut c_void {
        pyo3_ffi::PyLong_AsVoidPtr(self as *mut PyObject)
    }
}

#[inline(always)]
pub unsafe fn from_i64(value: i64) -> *mut PyLongObject {
    pyo3_ffi::PyLong_FromLong(value as libc::c_long) as *mut PyLongObject
}

#[inline(always)]
pub unsafe fn from_ptr<T>(pointer: *mut T) -> *mut PyLongObject {
    pyo3_ffi::PyLong_FromVoidPtr(pointer as *mut c_void) as *mut PyLongObject
}
