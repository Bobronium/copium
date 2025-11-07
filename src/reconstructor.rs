//! Specialized reconstructors for common types
//! - dict, list, set, frozenset, tuple
//! - Optimized paths with inline deepcopy on children

use crate::deepcopy::dispatch_copy;
use crate::ffi::{self, PyObject};
use crate::proxy::get_thread_memo;
use crate::types::CopyResult;
use pyo3::prelude::*;
use std::ptr;

/// Copy dict
pub unsafe fn copy_dict(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    // Create new dict
    let new_dict = ffi::PyDict_New();
    if new_dict.is_null() {
        return CopyResult::Error;
    }

    // Save to memo before recursing
    if use_thread_memo {
        let memo = get_thread_memo();
        let hash = ffi::hash_pointer(obj as *const _);
        if memo.initialize().is_err() {
            ffi::decref(new_dict);
            return CopyResult::Error;
        }
        if memo
            .table
            .insert_with_hash(obj as *const _, new_dict, hash)
            .is_err()
        {
            ffi::decref(new_dict);
            return CopyResult::Error;
        }
        // Keep alive
        if memo.keepalive.append(new_dict).is_err() {
            ffi::decref(new_dict);
            return CopyResult::Error;
        }
    } else if let Some(user_memo) = user_memo {
        let key = ffi::PyLong_FromVoidPtr(obj as *const _);
        ffi::PyDict_SetItem(user_memo, key, new_dict);
        ffi::decref(key);
    }

    // Copy items
    let mut pos = 0isize;
    let mut key: *mut PyObject = ptr::null_mut();
    let mut value: *mut PyObject = ptr::null_mut();

    while ffi::PyDict_Next(obj, &mut pos, &mut key, &mut value) != 0 {
        let key_tp = ffi::py_type(key);
        let value_tp = ffi::py_type(value);

        let copied_key = match dispatch_copy(py, key, key_tp, user_memo, use_thread_memo) {
            CopyResult::Immutable(p) | CopyResult::Mutable(p) | CopyResult::FromMemo(p) => p,
            CopyResult::Error => {
                ffi::decref(new_dict);
                return CopyResult::Error;
            }
        };

        let copied_value = match dispatch_copy(py, value, value_tp, user_memo, use_thread_memo) {
            CopyResult::Immutable(p) | CopyResult::Mutable(p) | CopyResult::FromMemo(p) => p,
            CopyResult::Error => {
                ffi::decref(copied_key);
                ffi::decref(new_dict);
                return CopyResult::Error;
            }
        };

        if ffi::PyDict_SetItem(new_dict, copied_key, copied_value) < 0 {
            ffi::decref(copied_key);
            ffi::decref(copied_value);
            ffi::decref(new_dict);
            return CopyResult::Error;
        }

        ffi::decref(copied_key);
        ffi::decref(copied_value);
    }

    CopyResult::Mutable(new_dict)
}

/// Copy list
pub unsafe fn copy_list(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    let size = ffi::PyDict_Size(obj); // Works for sequences too
    let new_list = ffi::PyList_New(size);
    if new_list.is_null() {
        return CopyResult::Error;
    }

    // Save to memo before recursing
    if use_thread_memo {
        let memo = get_thread_memo();
        let hash = ffi::hash_pointer(obj as *const _);
        if memo.initialize().is_err() {
            ffi::decref(new_list);
            return CopyResult::Error;
        }
        if memo
            .table
            .insert_with_hash(obj as *const _, new_list, hash)
            .is_err()
        {
            ffi::decref(new_list);
            return CopyResult::Error;
        }
        if memo.keepalive.append(new_list).is_err() {
            ffi::decref(new_list);
            return CopyResult::Error;
        }
    } else if let Some(user_memo) = user_memo {
        let key = ffi::PyLong_FromVoidPtr(obj as *const _);
        ffi::PyDict_SetItem(user_memo, key, new_list);
        ffi::decref(key);
    }

    // Copy items
    for i in 0..size {
        let item = ffi::PyList_GET_ITEM(obj, i);
        let item_tp = ffi::py_type(item);

        let copied = match dispatch_copy(py, item, item_tp, user_memo, use_thread_memo) {
            CopyResult::Immutable(p) | CopyResult::Mutable(p) | CopyResult::FromMemo(p) => p,
            CopyResult::Error => {
                ffi::decref(new_list);
                return CopyResult::Error;
            }
        };

        ffi::PyList_SET_ITEM(new_list, i, copied); // Steals reference
    }

    CopyResult::Mutable(new_list)
}

/// Copy set (simplified - would need proper Set API)
pub unsafe fn copy_set(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    // For now, use reduce protocol
    // Full implementation would use _PySet_NextEntry
    crate::deepcopy::copy_via_reduce(py, obj, user_memo, use_thread_memo)
}

/// Copy frozenset
pub unsafe fn copy_frozenset(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    // Frozensets are immutable, but might contain mutable items
    // For correctness, need to deep copy
    crate::deepcopy::copy_via_reduce(py, obj, user_memo, use_thread_memo)
}

/// Copy tuple with optimization for all-immutable case
pub unsafe fn copy_tuple(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    let size = ffi::PyDict_Size(obj);
    let new_tuple = ffi::PyTuple_New(size);
    if new_tuple.is_null() {
        return CopyResult::Error;
    }

    let mut all_immutable = true;

    // Copy items
    for i in 0..size {
        let item = ffi::PyTuple_GET_ITEM(obj, i);
        let item_tp = ffi::py_type(item);

        let result = dispatch_copy(py, item, item_tp, user_memo, use_thread_memo);

        if !result.is_immutable() {
            all_immutable = false;
        }

        let copied = match result {
            CopyResult::Immutable(p) | CopyResult::Mutable(p) | CopyResult::FromMemo(p) => p,
            CopyResult::Error => {
                ffi::decref(new_tuple);
                return CopyResult::Error;
            }
        };

        ffi::PyTuple_SET_ITEM(new_tuple, i, copied); // Steals reference
    }

    // If all children are immutable, return original
    if all_immutable {
        ffi::decref(new_tuple);
        ffi::incref(obj);
        return CopyResult::Immutable(obj);
    }

    // Check if we copied this in recursion
    if use_thread_memo {
        let memo = get_thread_memo();
        let hash = ffi::hash_pointer(obj as *const _);
        let found = memo.table.lookup_with_hash(obj as *const _, hash);
        if !found.is_null() {
            ffi::decref(new_tuple);
            ffi::incref(found);
            return CopyResult::FromMemo(found);
        }

        // Save to memo
        if memo.initialize().is_err() {
            ffi::decref(new_tuple);
            return CopyResult::Error;
        }
        if memo
            .table
            .insert_with_hash(obj as *const _, new_tuple, hash)
            .is_err()
        {
            ffi::decref(new_tuple);
            return CopyResult::Error;
        }
        if memo.keepalive.append(new_tuple).is_err() {
            ffi::decref(new_tuple);
            return CopyResult::Error;
        }
    } else if let Some(user_memo) = user_memo {
        let key = ffi::PyLong_FromVoidPtr(obj as *const _);
        ffi::PyDict_SetItem(user_memo, key, new_tuple);
        ffi::decref(key);
    }

    CopyResult::Mutable(new_tuple)
}
