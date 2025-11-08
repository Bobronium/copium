//! Specialized handlers for container types - generic over Memo

use crate::ffi::*;
use crate::memo_trait::Memo;
use crate::deepcopy_impl::deepcopy_recursive;
use std::ptr;

/// Deepcopy dict with mutation detection - generic over Memo
pub unsafe fn deepcopy_dict<M: Memo>(
    dict: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    // Create new dict
    let new_dict = PyDict_New();
    if new_dict.is_null() {
        return Err("Failed to create new dict".to_string());
    }

    // Save to memo before recursing
    let key = dict as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    memo.insert(key, new_dict, hash);

    // Iterate and copy key-value pairs
    let mut pos: Py_ssize_t = 0;
    let mut key_ptr: *mut PyObject = ptr::null_mut();
    let mut value_ptr: *mut PyObject = ptr::null_mut();

    while PyDict_Next(dict, &mut pos, &mut key_ptr, &mut value_ptr) != 0 {
        // Deepcopy key and value
        let new_key = deepcopy_recursive(key_ptr, memo)?;
        let new_value = deepcopy_recursive(value_ptr, memo)?;

        // Insert into new dict
        if PyDict_SetItem(new_dict, new_key, new_value) < 0 {
            Py_DecRef(new_key);
            Py_DecRef(new_value);
            Py_DecRef(new_dict);
            return Err("Failed to insert into new dict".to_string());
        }

        Py_DecRef(new_key);
        Py_DecRef(new_value);
    }

    Ok(new_dict)
}

/// Deepcopy list with dynamic sizing - generic over Memo
pub unsafe fn deepcopy_list<M: Memo>(
    list: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    let size = PyList_Size(list);
    if size < 0 {
        return Err("Failed to get list size".to_string());
    }

    // Create new list
    let new_list = PyList_New(size);
    if new_list.is_null() {
        return Err("Failed to create new list".to_string());
    }

    // Save to memo before recursing
    let key = list as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    memo.insert(key, new_list, hash);

    // Keep original list alive (stdlib behavior for both user and thread-local memos)
    memo.keepalive(list);

    // Copy elements
    for i in 0..size {
        let item = PyList_GetItem(list, i);
        if item.is_null() {
            Py_DecRef(new_list);
            return Err("Failed to get list item".to_string());
        }

        let new_item = deepcopy_recursive(item, memo)?;
        PyList_SetItem(new_list, i, new_item); // Steals reference
    }

    Ok(new_list)
}

/// Deepcopy tuple with immutability optimization - generic over Memo
pub unsafe fn deepcopy_tuple<M: Memo>(
    tuple: *mut PyObject,
    hash: Py_ssize_t,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    let size = PyTuple_Size(tuple);
    if size < 0 {
        return Err("Failed to get tuple size".to_string());
    }

    // Create new tuple
    let new_tuple = PyTuple_New(size);
    if new_tuple.is_null() {
        return Err("Failed to create new tuple".to_string());
    }

    // Track if all elements are identical (immutable optimization)
    let mut all_identical = true;

    // Copy elements
    for i in 0..size {
        let item = PyTuple_GetItem(tuple, i);
        if item.is_null() {
            Py_DecRef(new_tuple);
            return Err("Failed to get tuple item".to_string());
        }

        let new_item = deepcopy_recursive(item, memo)?;

        if new_item != item {
            all_identical = false;
        }

        PyTuple_SetItem(new_tuple, i, new_item); // Steals reference
    }

    // If all elements identical, return original tuple
    if all_identical {
        Py_DecRef(new_tuple);
        return Ok(Py_NewRef(tuple));
    }

    // Check if tuple was copied recursively (self-referential)
    let key = tuple as *const std::os::raw::c_void;
    if let Some(cached) = memo.lookup(key, hash) {
        Py_DecRef(new_tuple);
        return Ok(Py_NewRef(cached));
    }

    // Save to memo
    memo.insert(key, new_tuple, hash);

    Ok(new_tuple)
}

/// Deepcopy set with snapshot - generic over Memo
pub unsafe fn deepcopy_set<M: Memo>(
    set: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    // Create snapshot as tuple to avoid concurrent modification
    let snapshot = PySequence_Tuple(set);
    if snapshot.is_null() {
        return Err("Failed to create set snapshot".to_string());
    }

    // Create new set
    let new_set = PySet_New(ptr::null_mut());
    if new_set.is_null() {
        Py_DecRef(snapshot);
        return Err("Failed to create new set".to_string());
    }

    // Save to memo before recursing
    let key = set as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    memo.insert(key, new_set, hash);

    // Copy elements from snapshot
    let size = PyTuple_Size(snapshot);
    for i in 0..size {
        let item = PyTuple_GetItem(snapshot, i);
        if item.is_null() {
            Py_DecRef(snapshot);
            Py_DecRef(new_set);
            return Err("Failed to get set item".to_string());
        }

        let new_item = deepcopy_recursive(item, memo)?;

        if PySet_Add(new_set, new_item) < 0 {
            Py_DecRef(new_item);
            Py_DecRef(snapshot);
            Py_DecRef(new_set);
            return Err("Failed to add to new set".to_string());
        }

        Py_DecRef(new_item);
    }

    Py_DecRef(snapshot);
    Ok(new_set)
}

/// Deepcopy frozenset - generic over Memo
pub unsafe fn deepcopy_frozenset<M: Memo>(
    fset: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    // Similar to set but creates frozenset
    let snapshot = PySequence_Tuple(fset);
    if snapshot.is_null() {
        return Err("Failed to create frozenset snapshot".to_string());
    }

    // Copy elements into list first
    let temp_list = PyList_New(0);
    if temp_list.is_null() {
        Py_DecRef(snapshot);
        return Err("Failed to create temp list".to_string());
    }

    let size = PyTuple_Size(snapshot);
    for i in 0..size {
        let item = PyTuple_GetItem(snapshot, i);
        if item.is_null() {
            Py_DecRef(snapshot);
            Py_DecRef(temp_list);
            return Err("Failed to get frozenset item".to_string());
        }

        let new_item = deepcopy_recursive(item, memo)?;

        if PyList_Append(temp_list, new_item) < 0 {
            Py_DecRef(new_item);
            Py_DecRef(snapshot);
            Py_DecRef(temp_list);
            return Err("Failed to append to temp list".to_string());
        }

        Py_DecRef(new_item);
    }

    Py_DecRef(snapshot);

    // Create frozenset from list
    let new_fset = PyFrozenSet_New(temp_list);
    Py_DecRef(temp_list);

    if new_fset.is_null() {
        return Err("Failed to create frozenset".to_string());
    }

    // Save to memo
    let key = fset as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    memo.insert(key, new_fset, hash);

    Ok(new_fset)
}

/// Deepcopy bytearray - generic over Memo
pub unsafe fn deepcopy_bytearray<M: Memo>(
    ba: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    // Bytearray is mutable, so we create a new one
    let bytes = PyBytes_FromObject(ba);
    if bytes.is_null() {
        return Err("Failed to convert bytearray to bytes".to_string());
    }

    let new_ba = PyByteArray_FromObject(bytes);
    Py_DecRef(bytes);

    if new_ba.is_null() {
        return Err("Failed to create new bytearray".to_string());
    }

    // Save to memo
    let key = ba as *const std::os::raw::c_void;
    let hash = hash_pointer(key as *mut std::os::raw::c_void);
    memo.insert(key, new_ba, hash);

    Ok(new_ba)
}

extern "C" {
    fn PySequence_Tuple(o: *mut PyObject) -> *mut PyObject;
    fn PyBytes_FromObject(o: *mut PyObject) -> *mut PyObject;
    fn PyByteArray_FromObject(o: *mut PyObject) -> *mut PyObject;
}
