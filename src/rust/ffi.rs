//! Raw FFI bindings to CPython C API
//!
//! This module provides direct FFI access to CPython internals for zero-overhead operations.
//! We bypass PyO3 on hot paths for maximum performance.

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

use std::os::raw::{c_char, c_int, c_long, c_void};

// Core Python types
pub type Py_ssize_t = isize;
pub type Py_hash_t = Py_ssize_t;

#[repr(C)]
pub struct PyObject {
    pub ob_refcnt: Py_ssize_t,
    pub ob_type: *mut PyTypeObject,
}

#[repr(C)]
pub struct PyTypeObject {
    pub ob_base: PyVarObject,
    pub tp_name: *const c_char,
    pub tp_basicsize: Py_ssize_t,
    pub tp_itemsize: Py_ssize_t,
    // ... truncated for brevity, add fields as needed
}

#[repr(C)]
pub struct PyVarObject {
    pub ob_base: PyObject,
    pub ob_size: Py_ssize_t,
}

#[repr(C)]
pub struct PyDictObject {
    pub ob_base: PyObject,
    // Internal fields - platform specific
}

#[repr(C)]
pub struct PyListObject {
    pub ob_base: PyVarObject,
    pub ob_item: *mut *mut PyObject,
    pub allocated: Py_ssize_t,
}

#[repr(C)]
pub struct PyTupleObject {
    pub ob_base: PyVarObject,
    pub ob_item: [*mut PyObject; 1], // Flexible array member
}

// FFI function declarations
extern "C" {
    pub fn Py_IncRef(op: *mut PyObject);
    pub fn Py_DecRef(op: *mut PyObject);
    pub fn Py_NewRef(op: *mut PyObject) -> *mut PyObject;
    pub fn Py_XNewRef(op: *mut PyObject) -> *mut PyObject;

    pub fn PyObject_Type(o: *mut PyObject) -> *mut PyTypeObject;
    pub fn PyObject_GetAttr(o: *mut PyObject, attr_name: *mut PyObject) -> *mut PyObject;
    pub fn PyObject_SetAttr(o: *mut PyObject, attr_name: *mut PyObject, v: *mut PyObject) -> c_int;
    pub fn PyObject_CallOneArg(callable: *mut PyObject, arg: *mut PyObject) -> *mut PyObject;
    pub fn PyObject_Call(
        callable: *mut PyObject,
        args: *mut PyObject,
        kwargs: *mut PyObject,
    ) -> *mut PyObject;

    pub fn PyDict_New() -> *mut PyObject;
    pub fn PyDict_GetItem(mp: *mut PyObject, key: *mut PyObject) -> *mut PyObject;
    pub fn PyDict_SetItem(mp: *mut PyObject, key: *mut PyObject, item: *mut PyObject) -> c_int;
    pub fn PyDict_DelItem(mp: *mut PyObject, key: *mut PyObject) -> c_int;
    pub fn PyDict_Size(mp: *mut PyObject) -> Py_ssize_t;
    pub fn PyDict_Next(
        mp: *mut PyObject,
        pos: *mut Py_ssize_t,
        key: *mut *mut PyObject,
        value: *mut *mut PyObject,
    ) -> c_int;

    pub fn PyList_New(size: Py_ssize_t) -> *mut PyObject;
    pub fn PyList_GetItem(list: *mut PyObject, index: Py_ssize_t) -> *mut PyObject;
    pub fn PyList_SetItem(list: *mut PyObject, index: Py_ssize_t, item: *mut PyObject) -> c_int;
    pub fn PyList_Append(list: *mut PyObject, item: *mut PyObject) -> c_int;
    pub fn PyList_Size(list: *mut PyObject) -> Py_ssize_t;

    pub fn PyTuple_New(size: Py_ssize_t) -> *mut PyObject;
    pub fn PyTuple_GetItem(tuple: *mut PyObject, index: Py_ssize_t) -> *mut PyObject;
    pub fn PyTuple_SetItem(tuple: *mut PyObject, index: Py_ssize_t, item: *mut PyObject) -> c_int;
    pub fn PyTuple_Size(tuple: *mut PyObject) -> Py_ssize_t;

    pub fn PySet_New(iterable: *mut PyObject) -> *mut PyObject;
    pub fn PySet_Add(set: *mut PyObject, key: *mut PyObject) -> c_int;
    pub fn PyFrozenSet_New(iterable: *mut PyObject) -> *mut PyObject;

    pub fn PyLong_FromSsize_t(v: Py_ssize_t) -> *mut PyObject;
    pub fn PyLong_FromVoidPtr(p: *mut c_void) -> *mut PyObject;

    pub fn PyErr_Occurred() -> *mut PyObject;
    pub fn PyErr_SetString(exception: *mut PyObject, string: *const c_char);
    pub fn PyErr_Clear();

    // Type objects
    pub static mut PyDict_Type: PyTypeObject;
    pub static mut PyList_Type: PyTypeObject;
    pub static mut PyTuple_Type: PyTypeObject;
    pub static mut PySet_Type: PyTypeObject;
    pub static mut PyFrozenSet_Type: PyTypeObject;
    pub static mut PyLong_Type: PyTypeObject;
    pub static mut PyFloat_Type: PyTypeObject;
    pub static mut PyUnicode_Type: PyTypeObject;
    pub static mut PyBytes_Type: PyTypeObject;
    pub static mut PyBool_Type: PyTypeObject;
    pub static mut PyByteArray_Type: PyTypeObject;
    pub static mut _PyNone_Type: PyTypeObject;
}

/// Helper to get type pointer
#[inline(always)]
pub unsafe fn Py_TYPE(ob: *mut PyObject) -> *mut PyTypeObject {
    (*ob).ob_type
}

/// Check if object is of exact type (not subclass)
#[inline(always)]
pub unsafe fn Py_IS_TYPE(ob: *mut PyObject, tp: *const PyTypeObject) -> bool {
    Py_TYPE(ob) == tp as *mut PyTypeObject
}

/// Compute pointer hash using SplitMix64
#[inline(always)]
pub fn hash_pointer(ptr: *const c_void) -> Py_hash_t {
    let mut h = ptr as u64;
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51afd7ed558ccd_u64);
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ceb9fe1a85ec53_u64);
    h ^= h >> 33;
    h as Py_hash_t
}

/// Get object ID (pointer as integer)
#[inline(always)]
pub unsafe fn PyObject_Id(obj: *mut PyObject) -> usize {
    obj as usize
}

/// Check if object is immutable literal
#[inline(always)]
pub unsafe fn is_immutable_literal(obj: *mut PyObject) -> bool {
    let tp = Py_TYPE(obj);

    // None, True, False, NotImplemented, Ellipsis
    Py_IS_TYPE(obj, &_PyNone_Type as *const _)
        || tp == &mut PyLong_Type
        || tp == &mut PyFloat_Type
        || tp == &mut PyUnicode_Type
        || tp == &mut PyBytes_Type
        || tp == &mut PyBool_Type
}
