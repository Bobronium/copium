//! Reduce protocol handling for generic object deepcopy

use crate::ffi::*;
use crate::state::{MemoState, Initialized};
use crate::deepcopy::deepcopy_recursive;
use std::ptr;

/// Deepcopy via reduce protocol
pub unsafe fn deepcopy_via_reduce(
    obj: *mut PyObject,
    state: &mut MemoState<Initialized>,
) -> Result<*mut PyObject, String> {
    // Try __reduce_ex__(4) first
    let reduce_ex = get_reduce_ex(obj)?;
    let protocol = PyLong_FromSsize_t(4);
    if protocol.is_null() {
        return Err("Failed to create protocol number".to_string());
    }

    let reduced = PyObject_CallOneArg(reduce_ex, protocol);
    Py_DecRef(reduce_ex);
    Py_DecRef(protocol);

    if reduced.is_null() {
        // Clear error and try __reduce__
        PyErr_Clear();
        return try_reduce(obj, state);
    }

    reconstruct_from_reduce(obj, reduced, state)
}

/// Get __reduce_ex__ method
unsafe fn get_reduce_ex(obj: *mut PyObject) -> Result<*mut PyObject, String> {
    let reduce_ex_str = PyUnicode_InternFromString(b"__reduce_ex__\0".as_ptr() as *const i8);
    if reduce_ex_str.is_null() {
        return Err("Failed to create __reduce_ex__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, reduce_ex_str);
    Py_DecRef(reduce_ex_str);

    if method.is_null() {
        Err("Object has no __reduce_ex__ method".to_string())
    } else {
        Ok(method)
    }
}

/// Try __reduce__ as fallback
unsafe fn try_reduce(
    obj: *mut PyObject,
    state: &mut MemoState<Initialized>,
) -> Result<*mut PyObject, String> {
    let reduce_str = PyUnicode_InternFromString(b"__reduce__\0".as_ptr() as *const i8);
    if reduce_str.is_null() {
        return Err("Failed to create __reduce__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, reduce_str);
    Py_DecRef(reduce_str);

    if method.is_null() {
        return Err("Object has no __reduce__ method".to_string());
    }

    let reduced = PyObject_CallNoArgs(method);
    Py_DecRef(method);

    if reduced.is_null() {
        return Err("__reduce__ call failed".to_string());
    }

    reconstruct_from_reduce(obj, reduced, state)
}

/// Reconstruct object from reduce tuple
unsafe fn reconstruct_from_reduce(
    original: *mut PyObject,
    reduced: *mut PyObject,
    state: &mut MemoState<Initialized>,
) -> Result<*mut PyObject, String> {
    // Reduce returns a tuple: (callable, args, state?, listitems?, dictitems?)
    if !Py_IS_TYPE(reduced, &PyTuple_Type) {
        Py_DecRef(reduced);
        return Err("__reduce__ did not return a tuple".to_string());
    }

    let size = PyTuple_Size(reduced);
    if size < 2 {
        Py_DecRef(reduced);
        return Err("__reduce__ tuple too small".to_string());
    }

    // Get callable and args
    let callable = PyTuple_GetItem(reduced, 0);
    let args = PyTuple_GetItem(reduced, 1);

    if callable.is_null() || args.is_null() {
        Py_DecRef(reduced);
        return Err("Failed to extract callable/args from reduce tuple".to_string());
    }

    // Deepcopy args
    let new_args = deepcopy_recursive(args, state)?;

    // Call constructor
    let new_obj = PyObject_Call(callable, new_args, ptr::null_mut());
    Py_DecRef(new_args);

    if new_obj.is_null() {
        Py_DecRef(reduced);
        return Err("Failed to reconstruct object".to_string());
    }

    // Save to memo before setting state
    let key = original as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    state.memo_insert(key, new_obj, hash);

    // Add to keepalive
    state.keepalive_append(new_obj);

    // Handle state if present (index 2)
    if size > 2 {
        let obj_state = PyTuple_GetItem(reduced, 2);
        if !obj_state.is_null() && obj_state != get_none() {
            let new_state = deepcopy_recursive(obj_state, state)?;
            set_object_state(new_obj, new_state)?;
            Py_DecRef(new_state);
        }
    }

    Py_DecRef(reduced);
    Ok(new_obj)
}

/// Set object state via __setstate__
unsafe fn set_object_state(
    obj: *mut PyObject,
    state: *mut PyObject,
) -> Result<(), String> {
    let setstate_str = PyUnicode_InternFromString(b"__setstate__\0".as_ptr() as *const i8);
    if setstate_str.is_null() {
        return Err("Failed to create __setstate__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, setstate_str);
    Py_DecRef(setstate_str);

    if method.is_null() {
        // No __setstate__, try __dict__ update
        PyErr_Clear();
        return Ok(());
    }

    let result = PyObject_CallOneArg(method, state);
    Py_DecRef(method);

    if result.is_null() {
        Err("__setstate__ call failed".to_string())
    } else {
        Py_DecRef(result);
        Ok(())
    }
}

/// Get None singleton
unsafe fn get_none() -> *mut PyObject {
    static mut NONE_PTR: *mut PyObject = ptr::null_mut();
    if NONE_PTR.is_null() {
        // Get None from Python
        NONE_PTR = Py_None();
    }
    NONE_PTR
}

extern "C" {
    fn PyUnicode_InternFromString(s: *const i8) -> *mut PyObject;
    fn PyObject_CallNoArgs(callable: *mut PyObject) -> *mut PyObject;
    fn Py_None() -> *mut PyObject;
}
