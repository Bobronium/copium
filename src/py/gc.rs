use pyo3_ffi::*;
use std::ffi::c_void;
use std::os::raw::c_int;

use super::PyTypeInfo;

#[inline(always)]
pub unsafe fn new<T>(type_object: *mut PyTypeObject) -> *mut T {
    pyo3_ffi::PyObject_GC_New(type_object)
}

#[inline(always)]
pub unsafe fn track(pointer: *mut c_void) {
    pyo3_ffi::PyObject_GC_Track(pointer)
}

#[inline(always)]
pub unsafe fn untrack(pointer: *mut c_void) {
    pyo3_ffi::PyObject_GC_UnTrack(pointer)
}

#[inline(always)]
pub unsafe fn delete(pointer: *mut c_void) {
    pyo3_ffi::PyObject_GC_Del(pointer)
}

#[inline(always)]
pub unsafe fn call_finalizer_from_dealloc<T: PyTypeInfo>(object: *mut T) -> c_int {
    pyo3_ffi::PyObject_CallFinalizerFromDealloc(object as *mut PyObject)
}
