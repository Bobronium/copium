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
                Err(e) => {
                    // Check for special error prefixes
                    if e.starts_with("PYTHON_EXCEPTION:AttributeError") {
                        // Import copy.Error and raise it
                        let copy_module = py.import_bound("copy")?;
                        let error_class = copy_module.getattr("Error")?;
                        let error_type = error_class.downcast::<pyo3::types::PyType>()?;
                        Err(PyErr::from_type_bound(error_type.clone(), "un(deep)copyable object"))
                    } else if let Some(msg) = e.strip_prefix("PYTHON_EXCEPTION:TypeError:") {
                        Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(msg.to_string()))
                    } else if e.starts_with("PYTHON_EXCEPTION:TypeError") {
                        Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(e))
                    } else if e.contains("pickle protocol") {
                        Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(e))
                    } else {
                        Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))
                    }
                }
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
                Err(e) => {
                    // Check for special error prefixes
                    if e.starts_with("PYTHON_EXCEPTION:AttributeError") {
                        // Import copy.Error and raise it
                        let copy_module = py.import_bound("copy")?;
                        let error_class = copy_module.getattr("Error")?;
                        let error_type = error_class.downcast::<pyo3::types::PyType>()?;
                        Err(PyErr::from_type_bound(error_type.clone(), "un(deep)copyable object"))
                    } else if let Some(msg) = e.strip_prefix("PYTHON_EXCEPTION:TypeError:") {
                        Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(msg.to_string()))
                    } else if e.starts_with("PYTHON_EXCEPTION:TypeError") {
                        Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(e))
                    } else if e.contains("pickle protocol") {
                        Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(e))
                    } else {
                        Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))
                    }
                }
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
        // Check for immutable types - return as-is
        if ffi::is_immutable_literal(obj_ptr) {
            return Ok(Py::from_borrowed_ptr(py, ffi::Py_NewRef(obj_ptr)));
        }

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

        // Handle built-in containers with shallow copy
        let tp = pyo3_ffi::Py_TYPE(obj_ptr);

        // List
        if tp == std::ptr::addr_of_mut!(pyo3_ffi::PyList_Type) {
            let size = pyo3_ffi::PyList_Size(obj_ptr);
            let new_list = pyo3_ffi::PyList_New(size);
            if !new_list.is_null() {
                for i in 0..size {
                    let item = pyo3_ffi::PyList_GetItem(obj_ptr, i);
                    pyo3_ffi::Py_IncRef(item);
                    pyo3_ffi::PyList_SetItem(new_list, i, item);
                }
                return Ok(Py::from_owned_ptr(py, new_list));
            }
        }

        // Dict
        if tp == std::ptr::addr_of_mut!(pyo3_ffi::PyDict_Type) {
            let new_dict = pyo3_ffi::PyDict_New();
            if !new_dict.is_null() {
                let mut pos: pyo3_ffi::Py_ssize_t = 0;
                let mut key: *mut pyo3_ffi::PyObject = std::ptr::null_mut();
                let mut value: *mut pyo3_ffi::PyObject = std::ptr::null_mut();

                while pyo3_ffi::PyDict_Next(obj_ptr, &mut pos, &mut key, &mut value) != 0 {
                    pyo3_ffi::PyDict_SetItem(new_dict, key, value);
                }
                return Ok(Py::from_owned_ptr(py, new_dict));
            }
        }

        // Set
        if tp == std::ptr::addr_of_mut!(pyo3_ffi::PySet_Type) {
            let new_set = pyo3_ffi::PySet_New(std::ptr::null_mut());
            if !new_set.is_null() {
                let iter = pyo3_ffi::PyObject_GetIter(obj_ptr);
                if !iter.is_null() {
                    loop {
                        let item = pyo3_ffi::PyIter_Next(iter);
                        if item.is_null() {
                            break;
                        }
                        pyo3_ffi::PySet_Add(new_set, item);
                        pyo3_ffi::Py_DecRef(item);
                    }
                    pyo3_ffi::Py_DecRef(iter);
                    pyo3_ffi::PyErr_Clear();
                }
                return Ok(Py::from_owned_ptr(py, new_set));
            }
        }

        // Tuple - tuples are immutable, return same object
        if tp == std::ptr::addr_of_mut!(pyo3_ffi::PyTuple_Type) {
            return Ok(Py::from_borrowed_ptr(py, ffi::Py_NewRef(obj_ptr)));
        }

        // Bytearray
        if tp == std::ptr::addr_of_mut!(pyo3_ffi::PyByteArray_Type) {
            let bytes = PyBytes_FromObject(obj_ptr);
            if !bytes.is_null() {
                let new_ba = PyByteArray_FromObject(bytes);
                pyo3_ffi::Py_DecRef(bytes);
                if !new_ba.is_null() {
                    return Ok(Py::from_owned_ptr(py, new_ba));
                }
            }
            pyo3_ffi::PyErr_Clear();
        }

        // For everything else, try reduce protocol
        copy_via_reduce(obj_ptr, py)
    }
}

/// Copy using reduce protocol
unsafe fn copy_via_reduce(obj: *mut pyo3_ffi::PyObject, py: Python) -> PyResult<Py<PyAny>> {
    // Check copyreg.dispatch_table first
    if let Ok(result) = try_copyreg_dispatch_copy(obj, py) {
        return Ok(result);
    }

    // Try __reduce_ex__(4) for copy (same as stdlib)
    let reduce_ex_str = pyo3_ffi::PyUnicode_InternFromString(b"__reduce_ex__\0".as_ptr() as *const i8);
    if !reduce_ex_str.is_null() {
        let method = pyo3_ffi::PyObject_GetAttr(obj, reduce_ex_str);
        pyo3_ffi::Py_DecRef(reduce_ex_str);

        if !method.is_null() {
            let protocol = pyo3_ffi::PyLong_FromLong(4);
            if !protocol.is_null() {
                let reduced = ffi::call_one_arg(method, protocol);
                pyo3_ffi::Py_DecRef(protocol);
                pyo3_ffi::Py_DecRef(method);

                if !reduced.is_null() {
                    let result = reconstruct_from_reduce_copy(obj, reduced, py);
                    pyo3_ffi::Py_DecRef(reduced);
                    return result;
                } else {
                    // Check if there's a Python exception
                    if !pyo3_ffi::PyErr_Occurred().is_null() {
                        let exc_type = pyo3_ffi::PyErr_Occurred();
                        let type_error = std::ptr::addr_of_mut!(pyo3_ffi::PyExc_TypeError);
                        if pyo3_ffi::PyErr_GivenExceptionMatches(exc_type, *type_error) != 0 {
                            // TypeError - try manual copy for __slots__ or other special cases
                            pyo3_ffi::PyErr_Clear();
                            return copy_slots_object(obj, py);
                        } else {
                            // Other exception (e.g., ValueError from __getstate__) - propagate it
                            return Err(PyErr::fetch(py));
                        }
                    }
                }
            }
        } else {
            // Check if it's AttributeError (e.g., from __getattribute__)
            if !pyo3_ffi::PyErr_Occurred().is_null() {
                let attr_error = std::ptr::addr_of_mut!(pyo3_ffi::PyExc_AttributeError);
                if pyo3_ffi::PyErr_GivenExceptionMatches(pyo3_ffi::PyErr_Occurred(), *attr_error) != 0 {
                    pyo3_ffi::PyErr_Clear();
                    // Import copy.Error and raise it
                    let copy_module = py.import_bound("copy")?;
                    let error_class = copy_module.getattr("Error")?;
                    let error_type = error_class.downcast::<pyo3::types::PyType>()?;
                    return Err(PyErr::from_type_bound(error_type.clone(), ""));
                }
            }
        }
        pyo3_ffi::PyErr_Clear();
    }

    // Try __reduce__
    let reduce_str = pyo3_ffi::PyUnicode_InternFromString(b"__reduce__\0".as_ptr() as *const i8);
    if !reduce_str.is_null() {
        let method = pyo3_ffi::PyObject_GetAttr(obj, reduce_str);
        pyo3_ffi::Py_DecRef(reduce_str);

        if !method.is_null() {
            let reduced = ffi::call_no_args(method);
            pyo3_ffi::Py_DecRef(method);

            if !reduced.is_null() {
                let result = reconstruct_from_reduce_copy(obj, reduced, py);
                pyo3_ffi::Py_DecRef(reduced);
                return result;
            } else {
                // Check if there's a Python exception from __reduce__
                if !pyo3_ffi::PyErr_Occurred().is_null() {
                    // Propagate any exception from __reduce__
                    return Err(PyErr::fetch(py));
                }
            }
        } else {
            // Check if it's AttributeError (e.g., from __getattribute__)
            if !pyo3_ffi::PyErr_Occurred().is_null() {
                let attr_error = std::ptr::addr_of_mut!(pyo3_ffi::PyExc_AttributeError);
                if pyo3_ffi::PyErr_GivenExceptionMatches(pyo3_ffi::PyErr_Occurred(), *attr_error) != 0 {
                    pyo3_ffi::PyErr_Clear();
                    // Import copy.Error and raise it
                    let copy_module = py.import_bound("copy")?;
                    let error_class = copy_module.getattr("Error")?;
                    let error_type = error_class.downcast::<pyo3::types::PyType>()?;
                    return Err(PyErr::from_type_bound(error_type.clone(), ""));
                }
            }
        }
        pyo3_ffi::PyErr_Clear();
    }

    // Try manual copy for __slots__
    copy_slots_object(obj, py)
}

/// Copy object with __slots__ or __dict__ manually
unsafe fn copy_slots_object(obj: *mut pyo3_ffi::PyObject, py: Python) -> PyResult<Py<PyAny>> {
    // Create new instance using type(obj)()
    let obj_type = pyo3_ffi::Py_TYPE(obj);
    let new_obj = pyo3_ffi::PyObject_CallNoArgs(obj_type as *mut pyo3_ffi::PyObject);

    if new_obj.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(
            "cannot copy object"
        ));
    }

    // Copy __dict__ if present
    let dict_str = pyo3_ffi::PyUnicode_InternFromString(b"__dict__\0".as_ptr() as *const i8);
    if !dict_str.is_null() {
        let obj_dict = pyo3_ffi::PyObject_GetAttr(obj, dict_str);
        if !obj_dict.is_null() {
            let new_dict = pyo3_ffi::PyObject_GetAttr(new_obj, dict_str);
            if !new_dict.is_null() {
                pyo3_ffi::PyDict_Update(new_dict, obj_dict);
                pyo3_ffi::Py_DecRef(new_dict);
            }
            pyo3_ffi::Py_DecRef(obj_dict);
        }
        pyo3_ffi::Py_DecRef(dict_str);
        pyo3_ffi::PyErr_Clear();
    }

    // Copy __slots__ attributes
    let slots_str = pyo3_ffi::PyUnicode_InternFromString(b"__slots__\0".as_ptr() as *const i8);
    if !slots_str.is_null() {
        // Get slots from the type
        let slots = pyo3_ffi::PyObject_GetAttr(obj_type as *mut pyo3_ffi::PyObject, slots_str);
        if !slots.is_null() {
            // Iterate over slots
            let iter = pyo3_ffi::PyObject_GetIter(slots);
            if !iter.is_null() {
                loop {
                    let slot_name = pyo3_ffi::PyIter_Next(iter);
                    if slot_name.is_null() {
                        break;
                    }

                    // Try to get attribute from original object
                    let value = pyo3_ffi::PyObject_GetAttr(obj, slot_name);
                    if !value.is_null() {
                        // Set on new object (shallow copy - same reference)
                        pyo3_ffi::PyObject_SetAttr(new_obj, slot_name, value);
                        pyo3_ffi::Py_DecRef(value);
                    }
                    pyo3_ffi::Py_DecRef(slot_name);
                    pyo3_ffi::PyErr_Clear();
                }
                pyo3_ffi::Py_DecRef(iter);
            }
            pyo3_ffi::Py_DecRef(slots);
        }
        pyo3_ffi::Py_DecRef(slots_str);
        pyo3_ffi::PyErr_Clear();
    }

    Ok(Py::from_owned_ptr(py, new_obj))
}

/// Try copyreg.dispatch_table for shallow copy
unsafe fn try_copyreg_dispatch_copy(
    obj: *mut pyo3_ffi::PyObject,
    py: Python,
) -> Result<Py<PyAny>, ()> {
    // Import copyreg module
    let copyreg = pyo3_ffi::PyImport_ImportModule(b"copyreg\0".as_ptr() as *const i8);
    if copyreg.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Get dispatch_table
    let dispatch_table = pyo3_ffi::PyObject_GetAttrString(copyreg, b"dispatch_table\0".as_ptr() as *const i8);
    pyo3_ffi::Py_DecRef(copyreg);
    if dispatch_table.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Look up the type in dispatch_table
    let obj_type = pyo3_ffi::Py_TYPE(obj) as *mut pyo3_ffi::PyObject;
    let reducer = pyo3_ffi::PyObject_GetItem(dispatch_table, obj_type);
    pyo3_ffi::Py_DecRef(dispatch_table);

    if reducer.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Call the reducer
    let reduced = ffi::call_one_arg(reducer, obj);
    pyo3_ffi::Py_DecRef(reducer);

    if reduced.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Reconstruct from the reduced form (shallow copy - no deepcopy of args)
    let result = reconstruct_from_reduce_copy(obj, reduced, py);
    pyo3_ffi::Py_DecRef(reduced);
    result.map_err(|_| ())
}

/// Reconstruct object from reduce for shallow copy
unsafe fn reconstruct_from_reduce_copy(
    _original: *mut pyo3_ffi::PyObject,
    reduced: *mut pyo3_ffi::PyObject,
    py: Python,
) -> PyResult<Py<PyAny>> {
    // Check if it's a string (return original unchanged)
    if pyo3_ffi::Py_TYPE(reduced) == std::ptr::addr_of_mut!(pyo3_ffi::PyUnicode_Type) {
        return Ok(Py::from_owned_ptr(py, ffi::Py_NewRef(_original)));
    }

    // Must be a tuple
    if pyo3_ffi::Py_TYPE(reduced) != std::ptr::addr_of_mut!(pyo3_ffi::PyTuple_Type) {
        return Ok(Py::from_owned_ptr(py, ffi::Py_NewRef(_original)));
    }

    let size = pyo3_ffi::PyTuple_Size(reduced);
    if size < 2 {
        return Ok(Py::from_owned_ptr(py, ffi::Py_NewRef(_original)));
    }

    // Valid reduce formats are 2-5 tuples only
    if size > 5 {
        return Err(PyErr::new::<pyo3::exceptions::PyTypeError, _>(
            "pickle protocol expects at most 5-tuple"
        ));
    }

    let callable = pyo3_ffi::PyTuple_GetItem(reduced, 0);
    let args = pyo3_ffi::PyTuple_GetItem(reduced, 1);

    if callable.is_null() || args.is_null() {
        return Ok(Py::from_owned_ptr(py, ffi::Py_NewRef(_original)));
    }

    // Call constructor (no deep copying of args for shallow copy)
    let new_obj = pyo3_ffi::PyObject_CallObject(callable, args);
    if new_obj.is_null() {
        // Propagate the Python exception (don't clear it)
        return Err(PyErr::fetch(py));
    }

    // Handle state if present (index 2) - shallow copy the state
    if size > 2 {
        let obj_state = pyo3_ffi::PyTuple_GetItem(reduced, 2);
        if !obj_state.is_null() && obj_state != pyo3_ffi::Py_None() {
            let _ = set_object_state_shallow(new_obj, obj_state);
        }
    }

    // Handle list items if present (index 3) - shallow copy
    if size > 3 {
        let list_items = pyo3_ffi::PyTuple_GetItem(reduced, 3);
        if !list_items.is_null() && list_items != pyo3_ffi::Py_None() {
            if let Err(_) = populate_list_items_shallow(new_obj, list_items) {
                pyo3_ffi::Py_DecRef(new_obj);
                return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Failed to populate list items"));
            }
        }
    }

    // Handle dict items if present (index 4) - shallow copy
    if size > 4 {
        let dict_items = pyo3_ffi::PyTuple_GetItem(reduced, 4);
        if !dict_items.is_null() && dict_items != pyo3_ffi::Py_None() {
            if let Err(_) = populate_dict_items_shallow(new_obj, dict_items) {
                pyo3_ffi::Py_DecRef(new_obj);
                return Err(PyErr::new::<pyo3::exceptions::PyRuntimeError, _>("Failed to populate dict items"));
            }
        }
    }

    Ok(Py::from_owned_ptr(py, new_obj))
}

/// Set object state for shallow copy (no deep copying)
unsafe fn set_object_state_shallow(
    obj: *mut pyo3_ffi::PyObject,
    state: *mut pyo3_ffi::PyObject,
) -> Result<(), ()> {
    // Try __setstate__
    let setstate_str = pyo3_ffi::PyUnicode_InternFromString(b"__setstate__\0".as_ptr() as *const i8);
    if !setstate_str.is_null() {
        let method = pyo3_ffi::PyObject_GetAttr(obj, setstate_str);
        pyo3_ffi::Py_DecRef(setstate_str);

        if !method.is_null() {
            let result = ffi::call_one_arg(method, state);
            pyo3_ffi::Py_DecRef(method);
            if !result.is_null() {
                pyo3_ffi::Py_DecRef(result);
                return Ok(());
            }
            pyo3_ffi::PyErr_Clear();
            return Ok(());
        }
        pyo3_ffi::PyErr_Clear();
    }

    // No __setstate__ - handle state manually
    // State can be either a dict or a 2-tuple (dict_state, slot_state)
    let mut dict_state = state;
    let mut slot_state: *mut pyo3_ffi::PyObject = std::ptr::null_mut();

    if pyo3_ffi::Py_TYPE(state) == std::ptr::addr_of_mut!(pyo3_ffi::PyTuple_Type) {
        let size = pyo3_ffi::PyTuple_Size(state);
        if size == 2 {
            dict_state = pyo3_ffi::PyTuple_GetItem(state, 0);
            slot_state = pyo3_ffi::PyTuple_GetItem(state, 1);
        }
    }

    // Update __dict__ if present
    if !dict_state.is_null() && dict_state != pyo3_ffi::Py_None()
        && pyo3_ffi::Py_TYPE(dict_state) == std::ptr::addr_of_mut!(pyo3_ffi::PyDict_Type) {
        let dict_str = pyo3_ffi::PyUnicode_InternFromString(b"__dict__\0".as_ptr() as *const i8);
        if !dict_str.is_null() {
            let obj_dict = pyo3_ffi::PyObject_GetAttr(obj, dict_str);
            pyo3_ffi::Py_DecRef(dict_str);

            if !obj_dict.is_null() {
                pyo3_ffi::PyDict_Update(obj_dict, dict_state);
                pyo3_ffi::Py_DecRef(obj_dict);
                pyo3_ffi::PyErr_Clear();
            } else {
                pyo3_ffi::PyErr_Clear();
            }
        }
    }

    // Update __slots__ if present
    if !slot_state.is_null() && slot_state != pyo3_ffi::Py_None()
        && pyo3_ffi::Py_TYPE(slot_state) == std::ptr::addr_of_mut!(pyo3_ffi::PyDict_Type) {
        let mut pos: pyo3_ffi::Py_ssize_t = 0;
        let mut key: *mut pyo3_ffi::PyObject = std::ptr::null_mut();
        let mut value: *mut pyo3_ffi::PyObject = std::ptr::null_mut();

        while pyo3_ffi::PyDict_Next(slot_state, &mut pos, &mut key, &mut value) != 0 {
            pyo3_ffi::PyObject_SetAttr(obj, key, value);
            pyo3_ffi::PyErr_Clear();
        }
    }

    Ok(())
}

/// Populate list items for shallow copy (no deep copying)
unsafe fn populate_list_items_shallow(
    obj: *mut pyo3_ffi::PyObject,
    list_items: *mut pyo3_ffi::PyObject,
) -> Result<(), ()> {
    // Get iterator
    let iter = pyo3_ffi::PyObject_GetIter(list_items);
    if iter.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Get append method
    let append_str = pyo3_ffi::PyUnicode_InternFromString(b"append\0".as_ptr() as *const i8);
    if append_str.is_null() {
        pyo3_ffi::Py_DecRef(iter);
        return Err(());
    }

    let append_method = pyo3_ffi::PyObject_GetAttr(obj, append_str);
    pyo3_ffi::Py_DecRef(append_str);

    if append_method.is_null() {
        pyo3_ffi::Py_DecRef(iter);
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Iterate and append items (shallow - no copying)
    loop {
        let item = pyo3_ffi::PyIter_Next(iter);
        if item.is_null() {
            break;
        }

        let result = ffi::call_one_arg(append_method, item);
        pyo3_ffi::Py_DecRef(item);

        if result.is_null() {
            pyo3_ffi::Py_DecRef(append_method);
            pyo3_ffi::Py_DecRef(iter);
            pyo3_ffi::PyErr_Clear();
            return Err(());
        }
        pyo3_ffi::Py_DecRef(result);
    }

    pyo3_ffi::Py_DecRef(append_method);
    pyo3_ffi::Py_DecRef(iter);
    pyo3_ffi::PyErr_Clear();

    Ok(())
}

/// Populate dict items for shallow copy (no deep copying)
unsafe fn populate_dict_items_shallow(
    obj: *mut pyo3_ffi::PyObject,
    dict_items: *mut pyo3_ffi::PyObject,
) -> Result<(), ()> {
    // Get iterator
    let iter = pyo3_ffi::PyObject_GetIter(dict_items);
    if iter.is_null() {
        pyo3_ffi::PyErr_Clear();
        return Err(());
    }

    // Iterate and setitem (shallow - no copying)
    loop {
        let item = pyo3_ffi::PyIter_Next(iter);
        if item.is_null() {
            break;
        }

        // Each item should be a (key, value) tuple
        if pyo3_ffi::Py_TYPE(item) == std::ptr::addr_of_mut!(pyo3_ffi::PyTuple_Type) {
            let size = pyo3_ffi::PyTuple_Size(item);
            if size == 2 {
                let key = pyo3_ffi::PyTuple_GetItem(item, 0);
                let value = pyo3_ffi::PyTuple_GetItem(item, 1);

                if !key.is_null() && !value.is_null() {
                    pyo3_ffi::PyObject_SetItem(obj, key, value);
                    pyo3_ffi::PyErr_Clear();
                }
            }
        }

        pyo3_ffi::Py_DecRef(item);
    }

    pyo3_ffi::Py_DecRef(iter);
    pyo3_ffi::PyErr_Clear();

    Ok(())
}

extern "C" {
    fn PyBytes_FromObject(o: *mut pyo3_ffi::PyObject) -> *mut pyo3_ffi::PyObject;
    fn PyByteArray_FromObject(o: *mut pyo3_ffi::PyObject) -> *mut pyo3_ffi::PyObject;
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
