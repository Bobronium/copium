//! Raw FFI bindings to CPython C API
//!
//! This module re-exports PyO3 FFI and adds our custom helpers

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

// Re-export PyO3 FFI types
pub use pyo3::ffi::*;

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

/// Check if object is immutable literal
#[inline(always)]
pub unsafe fn is_immutable_literal(obj: *mut PyObject) -> bool {
    let tp = Py_TYPE(obj);

    // None, True, False, int, float, str, bytes, bool
    // Note: In production we'd check against actual type objects
    // For now, simplified check
    tp == &mut _PyNone_Type
        || tp == &mut PyLong_Type
        || tp == &mut PyFloat_Type
        || tp == &mut PyUnicode_Type
        || tp == &mut PyBytes_Type
        || tp == &mut PyBool_Type
}
