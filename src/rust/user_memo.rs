//! User-provided memo implementation (conservative path)

use crate::ffi::*;
use crate::memo_trait::Memo;
use std::os::raw::c_void;

/// User-provided memo that wraps a Python dict
///
/// This is the "conservative path" that uses Python's dict API instead of
/// our custom hash table. Used when the user passes their own memo dict.
pub struct UserProvidedMemo {
    dict: *mut PyObject,
    keepalive_list: *mut PyObject,
}

impl UserProvidedMemo {
    /// Create from a Python dict object
    ///
    /// Safety: dict must be a valid PyDict object with a valid reference
    pub unsafe fn new(dict: *mut PyObject) -> Self {
        // Create keepalive list
        let keepalive_list = PyList_New(0);

        // Store keepalive list in memo dict under id(memo)
        // This matches stdlib behavior: memo[id(memo)] = [keepalive_objects]
        if !keepalive_list.is_null() {
            let memo_id = dict as isize;
            let memo_id_obj = PyLong_FromSsize_t(memo_id);
            if !memo_id_obj.is_null() {
                PyDict_SetItem(dict, memo_id_obj, keepalive_list);
                Py_DECREF(memo_id_obj);
            }
        }

        Self {
            dict,
            keepalive_list,
        }
    }

    /// Get the Python dict (for passing to __deepcopy__ methods)
    pub unsafe fn as_dict(&self) -> *mut PyObject {
        self.dict
    }
}

impl Memo for UserProvidedMemo {
    unsafe fn lookup(&mut self, key: *const c_void, _hash: Py_hash_t) -> Option<*mut PyObject> {
        // Convert pointer to Python int as key
        let key_obj = PyLong_FromVoidPtr(key as *mut c_void);
        if key_obj.is_null() {
            PyErr_Clear();
            return None;
        }

        let value = PyDict_GetItem(self.dict, key_obj);
        Py_DECREF(key_obj);

        if value.is_null() {
            PyErr_Clear();
            None
        } else {
            // PyDict_GetItem returns borrowed reference, need to incref
            Some(Py_NewRef(value))
        }
    }

    unsafe fn insert(&mut self, key: *const c_void, value: *mut PyObject, _hash: Py_hash_t) {
        // Convert pointer to Python int as key
        let key_obj = PyLong_FromVoidPtr(key as *mut c_void);
        if key_obj.is_null() {
            PyErr_Clear();
            return;
        }

        if PyDict_SetItem(self.dict, key_obj, value) < 0 {
            PyErr_Clear();
        }
        Py_DECREF(key_obj);

        // Add original object to keepalive (matches stdlib behavior)
        let original = key as *mut PyObject;
        if !self.keepalive_list.is_null() && !original.is_null() {
            if PyList_Append(self.keepalive_list, original) < 0 {
                PyErr_Clear();
            }
        }
    }

    unsafe fn keepalive(&mut self, obj: *mut PyObject) {
        if !self.keepalive_list.is_null() {
            if PyList_Append(self.keepalive_list, obj) < 0 {
                PyErr_Clear();
            }
        }
    }

    unsafe fn clear(&mut self) {
        // User-provided memo - don't clear it (they might want to inspect it)
    }

    unsafe fn cleanup(&mut self) {
        // Clean up keepalive list
        if !self.keepalive_list.is_null() {
            Py_DECREF(self.keepalive_list);
            self.keepalive_list = std::ptr::null_mut();
        }
    }

    #[inline(always)]
    fn is_user_provided(&self) -> bool {
        true
    }
}

impl Drop for UserProvidedMemo {
    fn drop(&mut self) {
        unsafe {
            if !self.keepalive_list.is_null() {
                Py_DECREF(self.keepalive_list);
            }
        }
    }
}
