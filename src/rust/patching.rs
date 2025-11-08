//! Patching module for copy.deepcopy replacement
//!
//! Python 3.12+: Uses vectorcall patching (fast)
//! Python 3.10/3.11: Uses code object replacement (legacy)

use pyo3::prelude::*;
use pyo3::ffi;
use std::ptr;

#[cfg(Py_3_12)]
mod vectorcall_patch {
    use super::*;

    /// Apply vectorcall patch to redirect function calls
    #[pyfunction]
    pub fn apply(func: &Bound<'_, PyAny>, target: &Bound<'_, PyAny>) -> PyResult<()> {
        let py = func.py();

        unsafe {
            let func_ptr = func.as_ptr();
            let target_ptr = target.as_ptr();

            // Store target in function's __dict__
            let dict_str = ffi::PyUnicode_InternFromString(b"_copium_target\0".as_ptr() as *const i8);
            if dict_str.is_null() {
                return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Failed to create key"));
            }

            // Get or create func_dict
            let func_obj = func_ptr as *mut ffi::PyFunctionObject;
            if (*func_obj).func_dict.is_null() {
                let new_dict = ffi::PyDict_New();
                if new_dict.is_null() {
                    ffi::Py_DECREF(dict_str);
                    return Err(PyErr::fetch(py));
                }
                (*func_obj).func_dict = new_dict;
            }

            // Store target
            if ffi::PyDict_SetItem((*func_obj).func_dict, dict_str, target_ptr) < 0 {
                ffi::Py_DECREF(dict_str);
                return Err(PyErr::fetch(py));
            }

            // Store original vectorcall if not already saved
            let saved_str = ffi::PyUnicode_InternFromString(b"_copium_saved_vec\0".as_ptr() as *const i8);
            if saved_str.is_null() {
                ffi::Py_DECREF(dict_str);
                return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Failed to create key"));
            }

            let saved = ffi::PyDict_GetItemWithError((*func_obj).func_dict, saved_str);
            if saved.is_null() && !ffi::PyErr_Occurred().is_null() {
                ffi::Py_DECREF(dict_str);
                ffi::Py_DECREF(saved_str);
                return Err(PyErr::fetch(py));
            }

            if saved.is_null() {
                // Save original vectorcall
                let orig_vec = ffi::PyVectorcall_Function(func_ptr);
                if orig_vec.is_none() {
                    ffi::Py_DECREF(dict_str);
                    ffi::Py_DECREF(saved_str);
                    return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("No vectorcall"));
                }

                let cap = ffi::PyCapsule_New(
                    orig_vec.unwrap() as *mut std::ffi::c_void,
                    b"copium.vectorcall\0".as_ptr() as *const i8,
                    None
                );
                if cap.is_null() {
                    ffi::Py_DECREF(dict_str);
                    ffi::Py_DECREF(saved_str);
                    return Err(PyErr::fetch(py));
                }

                if ffi::PyDict_SetItem((*func_obj).func_dict, saved_str, cap) < 0 {
                    ffi::Py_DECREF(cap);
                    ffi::Py_DECREF(dict_str);
                    ffi::Py_DECREF(saved_str);
                    return Err(PyErr::fetch(py));
                }
                ffi::Py_DECREF(cap);
            }

            // Set our forwarding vectorcall
            ffi::PyFunction_SetVectorcall(func_obj, Some(forward_vectorcall));

            ffi::Py_DECREF(dict_str);
            ffi::Py_DECREF(saved_str);
        }

        Ok(())
    }

    /// Unapply vectorcall patch
    #[pyfunction]
    pub fn unapply(func: &Bound<'_, PyAny>) -> PyResult<()> {
        let py = func.py();

        unsafe {
            let func_ptr = func.as_ptr();
            let func_obj = func_ptr as *mut ffi::PyFunctionObject;

            if (*func_obj).func_dict.is_null() {
                return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Not patched"));
            }

            let saved_str = ffi::PyUnicode_InternFromString(b"_copium_saved_vec\0".as_ptr() as *const i8);
            if saved_str.is_null() {
                return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Failed to create key"));
            }

            let cap = ffi::PyDict_GetItemWithError((*func_obj).func_dict, saved_str);
            if cap.is_null() {
                ffi::Py_DECREF(saved_str);
                if ffi::PyErr_Occurred().is_null() {
                    return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Not patched"));
                }
                return Err(PyErr::fetch(py));
            }

            let orig_ptr = ffi::PyCapsule_GetPointer(cap, b"copium.vectorcall\0".as_ptr() as *const i8);
            if orig_ptr.is_null() {
                ffi::Py_DECREF(saved_str);
                return Err(PyErr::fetch(py));
            }

            let orig_vec = std::mem::transmute(orig_ptr);
            ffi::PyFunction_SetVectorcall(func_obj, orig_vec);

            // Clean up dict entries
            let target_str = ffi::PyUnicode_InternFromString(b"_copium_target\0".as_ptr() as *const i8);
            if !target_str.is_null() {
                ffi::PyDict_DelItem((*func_obj).func_dict, target_str);
                ffi::Py_DECREF(target_str);
            }
            ffi::PyDict_DelItem((*func_obj).func_dict, saved_str);
            ffi::Py_DECREF(saved_str);
            ffi::PyErr_Clear();
        }

        Ok(())
    }

    /// Check if patch is applied
    #[pyfunction]
    pub fn applied(func: &Bound<'_, PyAny>) -> PyResult<bool> {
        unsafe {
            let func_ptr = func.as_ptr();
            let func_obj = func_ptr as *mut ffi::PyFunctionObject;

            if (*func_obj).func_dict.is_null() {
                return Ok(false);
            }

            let vec = ffi::PyVectorcall_Function(func_ptr);
            if vec.is_none() {
                return Ok(false);
            }

            // Check if vectorcall points to our forward function
            Ok(vec.unwrap() as *const() == forward_vectorcall as *const())
        }
    }

    /// Forward vectorcall to target
    unsafe extern "C" fn forward_vectorcall(
        callable: *mut ffi::PyObject,
        args: *const *mut ffi::PyObject,
        nargsf: usize,
        kwnames: *mut ffi::PyObject
    ) -> *mut ffi::PyObject {
        let func_obj = callable as *mut ffi::PyFunctionObject;
        if (*func_obj).func_dict.is_null() {
            ffi::PyErr_SetString(
                ptr::addr_of_mut!(ffi::PyExc_RuntimeError),
                b"copium: func_dict missing\0".as_ptr() as *const i8
            );
            return ptr::null_mut();
        }

        let target_str = ffi::PyUnicode_InternFromString(b"_copium_target\0".as_ptr() as *const i8);
        if target_str.is_null() {
            return ptr::null_mut();
        }

        let target = ffi::PyDict_GetItemWithError((*func_obj).func_dict, target_str);
        ffi::Py_DECREF(target_str);

        if target.is_null() {
            if ffi::PyErr_Occurred().is_null() {
                ffi::PyErr_SetString(
                    ptr::addr_of_mut!(ffi::PyExc_RuntimeError),
                    b"copium: target not found\0".as_ptr() as *const i8
                );
            }
            return ptr::null_mut();
        }

        ffi::_PyObject_Vectorcall(target, args, nargsf, kwnames)
    }
}

#[cfg(not(Py_3_12))]
mod code_replace_patch {
    use super::*;

    // TODO: Implement code object replacement for Python 3.10/3.11
    #[pyfunction]
    pub fn apply(_func: &Bound<'_, PyAny>, _target: &Bound<'_, PyAny>) -> PyResult<()> {
        Err(PyErr::new::<pyo3::exceptions::PyNotImplementedError, _>(
            "Patching not yet implemented for Python < 3.12"
        ))
    }

    #[pyfunction]
    pub fn unapply(_func: &Bound<'_, PyAny>) -> PyResult<()> {
        Err(PyErr::new::<pyo3::exceptions::PyNotImplementedError, _>(
            "Patching not yet implemented for Python < 3.12"
        ))
    }

    #[pyfunction]
    pub fn applied(_func: &Bound<'_, PyAny>) -> PyResult<bool> {
        Ok(false)
    }
}

// Re-export the appropriate implementation
#[cfg(Py_3_12)]
pub use vectorcall_patch::{apply, unapply, applied};

#[cfg(not(Py_3_12))]
pub use code_replace_patch::{apply, unapply, applied};

// High-level enable/disable/enabled API (works for both versions)
use pyo3::prelude::*;

/// Enable patching of copy.deepcopy to use copium.deepcopy
#[pyfunction]
pub fn enable(py: Python) -> PyResult<bool> {
    let copy_mod = py.import_bound("copy")?;
    let deepcopy_fn = copy_mod.getattr("deepcopy")?;

    // Check if already applied
    if applied(&deepcopy_fn)? {
        return Ok(false);
    }

    // Get copium.deepcopy as target
    let copium_mod = py.import_bound("copium")?;
    let copium_deepcopy = copium_mod.getattr("deepcopy")?;

    // Apply the patch
    apply(&deepcopy_fn, &copium_deepcopy)?;
    Ok(true)
}

/// Disable patching of copy.deepcopy
#[pyfunction]
pub fn disable(py: Python) -> PyResult<bool> {
    let copy_mod = py.import_bound("copy")?;
    let deepcopy_fn = copy_mod.getattr("deepcopy")?;

    // Check if not applied
    if !applied(&deepcopy_fn)? {
        return Ok(false);
    }

    // Unapply the patch
    unapply(&deepcopy_fn)?;
    Ok(true)
}

/// Check if copy.deepcopy is patched
#[pyfunction]
pub fn enabled(py: Python) -> PyResult<bool> {
    let copy_mod = py.import_bound("copy")?;
    let deepcopy_fn = copy_mod.getattr("deepcopy")?;
    applied(&deepcopy_fn)
}
