//! Raw FFI bindings to CPython C API

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

// Re-export PyO3 FFI types
pub use pyo3::ffi::*;

use std::ptr::addr_of_mut;

// Additional type objects not in PyO3 stable ABI
extern "C" {
    #[link_name = "PyCode_Type"]
    pub static mut PyCode_Type: PyTypeObject;

    #[link_name = "PyFunction_Type"]
    pub static mut PyFunction_Type: PyTypeObject;

    #[link_name = "PyProperty_Type"]
    pub static mut PyProperty_Type: PyTypeObject;

    #[link_name = "PyCFunction_Type"]
    pub static mut PyCFunction_Type: PyTypeObject;
}

/// Compute pointer hash using SplitMix64
#[inline(always)]
pub fn hash_pointer(ptr: *const std::os::raw::c_void) -> Py_hash_t {
    let mut h = ptr as u64;
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51afd7ed558ccd_u64);
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ceb9fe1a85ec53_u64);
    h ^= h >> 33;
    h as Py_hash_t
}

/// Check if object is immutable and should be returned as-is
#[inline(always)]
pub unsafe fn is_immutable_literal(obj: *mut PyObject) -> bool {
    if obj.is_null() {
        return false;
    }

    // Check for None
    if obj == Py_None() {
        return true;
    }

    // Check for True/False
    if obj == Py_True() || obj == Py_False() {
        return true;
    }

    let tp = Py_TYPE(obj);

    // Immutable types that can be returned as-is
    tp == addr_of_mut!(PyLong_Type)
        || tp == addr_of_mut!(PyFloat_Type)
        || tp == addr_of_mut!(PyUnicode_Type)
        || tp == addr_of_mut!(PyBytes_Type)
        || tp == addr_of_mut!(PyBool_Type)
        || tp == addr_of_mut!(PyComplex_Type)
        || tp == addr_of_mut!(PyFrozenSet_Type)
        || tp == addr_of_mut!(PyCode_Type)
        || tp == addr_of_mut!(PyFunction_Type)
        || tp == addr_of_mut!(PyProperty_Type)
        || tp == addr_of_mut!(PyCFunction_Type)
        || tp == addr_of_mut!(PyType_Type)
        || tp == addr_of_mut!(PyRange_Type)
}

/// Call a callable with one argument (PyO3 0.22 doesn't have PyObject_CallOneArg)
#[inline]
pub unsafe fn call_one_arg(callable: *mut PyObject, arg: *mut PyObject) -> *mut PyObject {
    let args = PyTuple_New(1);
    if args.is_null() {
        return std::ptr::null_mut();
    }
    Py_INCREF(arg);
    PyTuple_SetItem(args, 0, arg);
    let result = PyObject_CallObject(callable, args);
    Py_DECREF(args);
    result
}

/// Call a callable with no arguments
#[inline]
pub unsafe fn call_no_args(callable: *mut PyObject) -> *mut PyObject {
    let args = PyTuple_New(0);
    if args.is_null() {
        return std::ptr::null_mut();
    }
    let result = PyObject_CallObject(callable, args);
    Py_DECREF(args);
    result
}
