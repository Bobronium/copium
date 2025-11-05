//! Reduce protocol handling for generic object deepcopy

use crate::ffi::*;
use crate::memo_trait::Memo;
use crate::deepcopy_impl::deepcopy_recursive;

/// Deepcopy via reduce protocol
pub unsafe fn deepcopy_via_reduce<M: Memo>(
    obj: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    // Try __reduce_ex__(4) first
    let reduce_ex_result = try_reduce_ex(obj, memo);
    if let Ok(result) = reduce_ex_result {
        return Ok(result);
    }

    // Try __reduce__ as fallback
    try_reduce(obj, memo)
}

unsafe fn try_reduce_ex<M: Memo>(
    obj: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    let reduce_ex_str = PyUnicode_InternFromString(b"__reduce_ex__\0".as_ptr() as *const i8);
    if reduce_ex_str.is_null() {
        return Err("Failed to create __reduce_ex__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, reduce_ex_str);
    Py_DECREF(reduce_ex_str);

    if method.is_null() {
        PyErr_Clear();
        return Err("No __reduce_ex__".to_string());
    }

    let protocol = PyLong_FromLong(4);
    if protocol.is_null() {
        Py_DECREF(method);
        return Err("Failed to create protocol number".to_string());
    }

    let reduced = crate::ffi::call_one_arg(method, protocol);
    Py_DECREF(method);
    Py_DECREF(protocol);

    if reduced.is_null() {
        // Check if it's a TypeError about unpicklable object
        if !PyErr_Occurred().is_null() {
            let exc_type = PyErr_Occurred();

            // Check if it's TypeError
            if !exc_type.is_null() {
                let type_error = std::ptr::addr_of_mut!(PyExc_TypeError);
                if PyErr_GivenExceptionMatches(exc_type, *type_error) != 0 {
                    // It's a TypeError - check if it's about cannot pickle
                    // For now, just treat as uncopyable and return original
                    PyErr_Clear();
                    return Ok(Py_NewRef(obj));
                }
            }
        }

        PyErr_Clear();
        return Err("__reduce_ex__ failed".to_string());
    }

    // Check if it's a string (stdlib returns original object unchanged)
    if Py_TYPE(reduced) == std::ptr::addr_of_mut!(PyUnicode_Type) {
        Py_DECREF(reduced);
        return Ok(Py_NewRef(obj));
    }

    reconstruct_from_reduce(obj, reduced, memo)
}

unsafe fn try_reduce<M: Memo>(
    obj: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    let reduce_str = PyUnicode_InternFromString(b"__reduce__\0".as_ptr() as *const i8);
    if reduce_str.is_null() {
        return Err("Failed to create __reduce__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, reduce_str);
    Py_DECREF(reduce_str);

    if method.is_null() {
        PyErr_Clear();
        return Err("No __reduce__".to_string());
    }

    let reduced = crate::ffi::call_no_args(method);
    Py_DECREF(method);

    if reduced.is_null() {
        PyErr_Clear();
        return Err("__reduce__ failed".to_string());
    }

    // Check if it's a string (stdlib returns original object unchanged)
    if Py_TYPE(reduced) == std::ptr::addr_of_mut!(PyUnicode_Type) {
        Py_DECREF(reduced);
        return Ok(Py_NewRef(obj));
    }

    reconstruct_from_reduce(obj, reduced, memo)
}

unsafe fn reconstruct_from_reduce<M: Memo>(
    original: *mut PyObject,
    reduced: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    // Check if it's a tuple
    if Py_TYPE(reduced) != std::ptr::addr_of_mut!(PyTuple_Type) {
        Py_DECREF(reduced);
        // If not a tuple, return original unchanged (like stdlib)
        return Ok(Py_NewRef(original));
    }

    let size = PyTuple_Size(reduced);
    if size < 2 {
        Py_DECREF(reduced);
        return Ok(Py_NewRef(original));
    }

    // Valid reduce formats are 2-5 tuples only
    if size > 5 {
        Py_DECREF(reduced);
        return Err("pickle protocol expects at most 5-tuple".to_string());
    }

    let callable = PyTuple_GetItem(reduced, 0);
    let args = PyTuple_GetItem(reduced, 1);

    if callable.is_null() || args.is_null() {
        Py_DECREF(reduced);
        return Ok(Py_NewRef(original));
    }

    // Deepcopy args
    let new_args = deepcopy_recursive(args, memo)?;

    // Call constructor
    let new_obj = PyObject_CallObject(callable, new_args);
    Py_DECREF(new_args);

    if new_obj.is_null() {
        Py_DECREF(reduced);
        // Check if there's a Python exception to propagate
        if !PyErr_Occurred().is_null() {
            // Get the exception info to create a better error message
            let exc_type = PyErr_Occurred();
            let type_error = std::ptr::addr_of_mut!(PyExc_TypeError);
            if PyErr_GivenExceptionMatches(exc_type, *type_error) != 0 {
                // Fetch the error message and propagate it
                return Err("PYTHON_EXCEPTION:TypeError".to_string());
            }
        }
        return Err("Failed to reconstruct object".to_string());
    }

    // Save to memo before setting state
    let key = original as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    memo.insert(key, new_obj, hash);
    memo.keepalive(new_obj);

    // Handle state if present (index 2)
    if size > 2 {
        let obj_state = PyTuple_GetItem(reduced, 2);
        if !obj_state.is_null() && obj_state != Py_None() {
            let new_state = deepcopy_recursive(obj_state, memo)?;
            let _ = set_object_state::<M>(new_obj, new_state);
            Py_DECREF(new_state);
        }
    }

    // Handle list items if present (index 3)
    if size > 3 {
        let list_items = PyTuple_GetItem(reduced, 3);
        if !list_items.is_null() && list_items != Py_None() {
            let _ = populate_list_items::<M>(new_obj, list_items, memo);
        }
    }

    // Handle dict items if present (index 4)
    if size > 4 {
        let dict_items = PyTuple_GetItem(reduced, 4);
        if !dict_items.is_null() && dict_items != Py_None() {
            let _ = populate_dict_items::<M>(new_obj, dict_items, memo);
        }
    }

    Py_DECREF(reduced);
    Ok(new_obj)
}

unsafe fn set_object_state<M: Memo>(
    obj: *mut PyObject,
    state: *mut PyObject,
) -> Result<(), String> {
    let setstate_str = PyUnicode_InternFromString(b"__setstate__\0".as_ptr() as *const i8);
    if setstate_str.is_null() {
        return Ok(());
    }

    let method = PyObject_GetAttr(obj, setstate_str);
    Py_DECREF(setstate_str);

    if !method.is_null() {
        // Object has __setstate__, call it
        let result = crate::ffi::call_one_arg(method, state);
        Py_DECREF(method);

        if result.is_null() {
            PyErr_Clear();
            return Ok(());
        }

        Py_DECREF(result);
        return Ok(());
    }

    PyErr_Clear();

    // No __setstate__ - handle state based on type
    if Py_TYPE(state) == std::ptr::addr_of_mut!(PyDict_Type) {
        // Simple dict state - update __dict__
        let dict_str = PyUnicode_InternFromString(b"__dict__\0".as_ptr() as *const i8);
        if dict_str.is_null() {
            return Ok(());
        }

        let obj_dict = PyObject_GetAttr(obj, dict_str);
        Py_DECREF(dict_str);

        if obj_dict.is_null() {
            PyErr_Clear();
            return Ok(());
        }

        // Update obj.__dict__ with state
        if PyDict_Update(obj_dict, state) < 0 {
            Py_DECREF(obj_dict);
            PyErr_Clear();
            return Ok(());
        }

        Py_DECREF(obj_dict);
    } else if Py_TYPE(state) == std::ptr::addr_of_mut!(PyTuple_Type) && PyTuple_Size(state) == 2 {
        // Tuple state (for __slots__): (dict_state, slots_state)
        let dict_state = PyTuple_GetItem(state, 0);
        let slots_state = PyTuple_GetItem(state, 1);

        // Restore __dict__ if present
        if !dict_state.is_null() && dict_state != Py_None() {
            if Py_TYPE(dict_state) == std::ptr::addr_of_mut!(PyDict_Type) {
                let dict_str = PyUnicode_InternFromString(b"__dict__\0".as_ptr() as *const i8);
                if !dict_str.is_null() {
                    let obj_dict = PyObject_GetAttr(obj, dict_str);
                    Py_DECREF(dict_str);

                    if !obj_dict.is_null() {
                        let _ = PyDict_Update(obj_dict, dict_state);
                        Py_DECREF(obj_dict);
                        PyErr_Clear();
                    } else {
                        PyErr_Clear();
                    }
                }
            }
        }

        // Restore __slots__ if present
        if !slots_state.is_null() && slots_state != Py_None() {
            if Py_TYPE(slots_state) == std::ptr::addr_of_mut!(PyDict_Type) {
                // Iterate over slots_state dict and setattr each item
                let mut pos: Py_ssize_t = 0;
                let mut key: *mut PyObject = std::ptr::null_mut();
                let mut value: *mut PyObject = std::ptr::null_mut();

                while PyDict_Next(slots_state, &mut pos, &mut key, &mut value) != 0 {
                    if !key.is_null() && !value.is_null() {
                        let _ = PyObject_SetAttr(obj, key, value);
                        PyErr_Clear();
                    }
                }
            }
        }
    }

    Ok(())
}

unsafe fn populate_list_items<M: Memo>(
    obj: *mut PyObject,
    list_items: *mut PyObject,
    memo: &mut M,
) -> Result<(), String> {
    // Get iterator from list_items
    let iterator = PyObject_GetIter(list_items);
    if iterator.is_null() {
        PyErr_Clear();
        return Ok(());
    }

    // Iterate and append each item
    loop {
        let item = PyIter_Next(iterator);
        if item.is_null() {
            if PyErr_Occurred().is_null() {
                // Iterator exhausted normally
                break;
            } else {
                // Error occurred
                PyErr_Clear();
                break;
            }
        }

        // Deepcopy the item
        let new_item = match deepcopy_recursive(item, memo) {
            Ok(new_item) => new_item,
            Err(_) => {
                Py_DECREF(item);
                continue;
            }
        };
        Py_DECREF(item);

        // Append to object (works for lists and list subclasses)
        let append_str = PyUnicode_InternFromString(b"append\0".as_ptr() as *const i8);
        if !append_str.is_null() {
            let append_method = PyObject_GetAttr(obj, append_str);
            Py_DECREF(append_str);

            if !append_method.is_null() {
                let result = crate::ffi::call_one_arg(append_method, new_item);
                Py_DECREF(append_method);
                if !result.is_null() {
                    Py_DECREF(result);
                } else {
                    PyErr_Clear();
                }
            } else {
                PyErr_Clear();
            }
        }

        Py_DECREF(new_item);
    }

    Py_DECREF(iterator);
    Ok(())
}

unsafe fn populate_dict_items<M: Memo>(
    obj: *mut PyObject,
    dict_items: *mut PyObject,
    memo: &mut M,
) -> Result<(), String> {
    // Get iterator from dict_items
    let iterator = PyObject_GetIter(dict_items);
    if iterator.is_null() {
        PyErr_Clear();
        return Ok(());
    }

    // Iterate and set each key-value pair
    loop {
        let item = PyIter_Next(iterator);
        if item.is_null() {
            if PyErr_Occurred().is_null() {
                // Iterator exhausted normally
                break;
            } else {
                // Error occurred
                PyErr_Clear();
                break;
            }
        }

        // Item should be a 2-tuple (key, value)
        if PyTuple_Check(item) != 0 && PyTuple_Size(item) == 2 {
            let key = PyTuple_GetItem(item, 0);
            let value = PyTuple_GetItem(item, 1);

            if !key.is_null() && !value.is_null() {
                // Deepcopy key and value
                let new_key = match deepcopy_recursive(key, memo) {
                    Ok(k) => k,
                    Err(_) => {
                        Py_DECREF(item);
                        continue;
                    }
                };

                let new_value = match deepcopy_recursive(value, memo) {
                    Ok(v) => v,
                    Err(_) => {
                        Py_DECREF(new_key);
                        Py_DECREF(item);
                        continue;
                    }
                };

                // Set item in dict (works for dicts and dict subclasses)
                if PyObject_SetItem(obj, new_key, new_value) < 0 {
                    PyErr_Clear();
                }

                Py_DECREF(new_key);
                Py_DECREF(new_value);
            }
        }

        Py_DECREF(item);
    }

    Py_DECREF(iterator);
    Ok(())
}
