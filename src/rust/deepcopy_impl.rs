//! Main deepcopy implementation - simplified to match C pattern
//!
//! Pattern from C:
//! 1. Get thread-local memo (creates new if refcount > 1)
//! 2. Do deepcopy with that memo
//! 3. Clear memo for reuse
//! 4. Return memo to thread-local storage

use crate::ffi;
use crate::state::{get_thread_local_memo, return_thread_local_memo, ThreadLocalMemo};
use crate::types::{classify_type, init_type_cache, TypeClass};
use crate::dispatch::dispatch_deepcopy;
use pyo3::prelude::*;
use pyo3::ffi as pyo3_ffi;

/// Main deepcopy entry point
pub fn deepcopy_impl(
    obj: &Bound<'_, PyAny>,
    memo: Option<&Bound<'_, PyAny>>,
) -> PyResult<Py<PyAny>> {
    // Initialize type cache if needed
    init_type_cache();

    // Get raw pointer
    let obj_ptr = obj.as_ptr();

    // Handle user-provided memo (conservative path)
    if let Some(_user_memo) = memo {
        return Err(PyErr::new::<pyo3::exceptions::PyNotImplementedError, _>(
            "User-provided memo not yet implemented in Rust version",
        ));
    }

    // Get thread-local memo
    let mut tl_memo = get_thread_local_memo();

    // Fast path: native memo
    unsafe {
        let result = match deepcopy_internal(obj_ptr, &mut tl_memo) {
            Ok(result_ptr) => {
                let py = obj.py();
                Ok(Py::from_owned_ptr(py, result_ptr))
            }
            Err(e) => Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e)),
        };

        // Return memo to thread-local storage
        return_thread_local_memo(tl_memo);

        result
    }
}

/// Internal deepcopy with thread-local memo
unsafe fn deepcopy_internal(
    obj: *mut ffi::PyObject,
    memo: &mut ThreadLocalMemo,
) -> Result<*mut ffi::PyObject, String> {
    // Fast path: check for immutable literals
    if ffi::is_immutable_literal(obj) {
        return Ok(ffi::Py_NewRef(obj));
    }

    // Compute hash ONCE
    let key = obj as *const std::os::raw::c_void;
    let hash = ffi::hash_pointer(key as *mut std::os::raw::c_void);

    // Check memo first
    if let Some(cached) = memo.table.lookup(key, hash) {
        return Ok(ffi::Py_NewRef(cached));
    }

    // Classify type ONCE
    let type_class = classify_type(obj);

    // Dispatch to handler
    dispatch_deepcopy(obj, type_class, hash, memo)
}

/// Recursive deepcopy (used by container handlers)
#[inline]
pub unsafe fn deepcopy_recursive(
    obj: *mut ffi::PyObject,
    memo: &mut ThreadLocalMemo,
) -> Result<*mut ffi::PyObject, String> {
    // Fast path: immutable literals
    if ffi::is_immutable_literal(obj) {
        return Ok(ffi::Py_NewRef(obj));
    }

    // Compute hash
    let key = obj as *const std::os::raw::c_void;
    let hash = ffi::hash_pointer(key as *mut std::os::raw::c_void);

    // Check memo
    if let Some(cached) = memo.table.lookup(key, hash) {
        return Ok(ffi::Py_NewRef(cached));
    }

    // Classify and dispatch
    let type_class = classify_type(obj);
    dispatch_deepcopy(obj, type_class, hash, memo)
}

/// Shallow copy implementation
pub fn copy_impl(obj: &Bound<'_, PyAny>) -> PyResult<Py<PyAny>> {
    let py = obj.py();
    let obj_ptr = obj.as_ptr();

    unsafe {
        // Try __copy__ method first
        let copy_str = pyo3_ffi::PyUnicode_InternFromString(b"__copy__\0".as_ptr() as *const i8);
        if !copy_str.is_null() {
            let method = pyo3_ffi::PyObject_GetAttr(obj_ptr, copy_str);
            pyo3_ffi::Py_DecRef(copy_str);

            if !method.is_null() {
                let result = pyo3_ffi::PyObject_CallNoArgs(method);
                pyo3_ffi::Py_DecRef(method);

                if !result.is_null() {
                    return Ok(Py::from_owned_ptr(py, result));
                }
                pyo3_ffi::PyErr_Clear();
            } else {
                pyo3_ffi::PyErr_Clear();
            }
        }

        // Fall back to just returning new ref (simplified)
        Ok(Py::from_borrowed_ptr(py, ffi::Py_NewRef(obj_ptr)))
    }
}

/// Batch replication with optimization
pub fn replicate_impl(
    obj: &Bound<'_, PyAny>,
    n: usize,
    _compile_after: usize,
) -> PyResult<Vec<Py<PyAny>>> {
    let mut results = Vec::with_capacity(n);

    // Simple loop for now - optimization would compile after threshold
    for _ in 0..n {
        let copied = deepcopy_impl(obj, None)?;
        results.push(copied);
    }

    Ok(results)
}
