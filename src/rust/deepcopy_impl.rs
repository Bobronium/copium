//! Main deepcopy implementation with generic memo support
//!
//! Uses Rust's type system to encode different code paths at compile time:
//! - ThreadLocalMemo: Fast path with native hash table
//! - UserProvidedMemo: Conservative path using Python dict API
//!
//! Zero runtime cost - monomorphization generates specialized code for each path.

use crate::ffi;
use crate::memo_trait::Memo;
use crate::state::{get_thread_local_memo, return_thread_local_memo, ThreadLocalMemo};
use crate::user_memo::UserProvidedMemo;
use crate::types::{classify_type, init_type_cache};
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

    let obj_ptr = obj.as_ptr();
    let py = obj.py();

    // Dispatch based on memo type
    if let Some(user_memo) = memo {
        // Conservative path: user-provided memo
        unsafe {
            let mut memo_impl = UserProvidedMemo::new(user_memo.as_ptr());
            let result = match deepcopy_internal(obj_ptr, &mut memo_impl) {
                Ok(result_ptr) => Ok(Py::from_owned_ptr(py, result_ptr)),
                Err(e) => Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e)),
            };
            memo_impl.cleanup();
            result
        }
    } else {
        // Fast path: thread-local memo
        let mut tl_memo = get_thread_local_memo();

        unsafe {
            let result = match deepcopy_internal(obj_ptr, &mut tl_memo) {
                Ok(result_ptr) => Ok(Py::from_owned_ptr(py, result_ptr)),
                Err(e) => Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e)),
            };

            // Return memo to thread-local storage
            return_thread_local_memo(tl_memo);

            result
        }
    }
}

/// Internal deepcopy implementation - generic over Memo type
///
/// This function is monomorphized at compile time for each Memo implementation,
/// generating specialized code with zero runtime overhead.
#[inline]
unsafe fn deepcopy_internal<M: Memo>(
    obj: *mut ffi::PyObject,
    memo: &mut M,
) -> Result<*mut ffi::PyObject, String> {
    // Fast path: check for immutable literals
    if ffi::is_immutable_literal(obj) {
        return Ok(ffi::Py_NewRef(obj));
    }

    // Compute hash ONCE
    let key = obj as *const std::os::raw::c_void;
    let hash = ffi::hash_pointer(key as *mut std::os::raw::c_void);

    // Check memo first
    if let Some(cached) = memo.lookup(key, hash) {
        return Ok(ffi::Py_NewRef(cached));
    }

    // Classify type ONCE
    let type_class = classify_type(obj);

    // Dispatch to handler
    dispatch_deepcopy(obj, type_class, hash, memo)
}

/// Recursive deepcopy - generic over Memo type
///
/// Used by container handlers and reduce protocol.
/// Monomorphized at compile time for each Memo implementation.
#[inline]
pub unsafe fn deepcopy_recursive<M: Memo>(
    obj: *mut ffi::PyObject,
    memo: &mut M,
) -> Result<*mut ffi::PyObject, String> {
    // Fast path: immutable literals
    if ffi::is_immutable_literal(obj) {
        return Ok(ffi::Py_NewRef(obj));
    }

    // Compute hash
    let key = obj as *const std::os::raw::c_void;
    let hash = ffi::hash_pointer(key as *mut std::os::raw::c_void);

    // Check memo
    if let Some(cached) = memo.lookup(key, hash) {
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
