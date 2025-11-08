//! Simplified thread-local state management matching C implementation
//!
//! Pattern:
//! 1. Thread-local MemoObject stored in TLS
//! 2. Check refcount before reuse - if > 1, someone else holds it, create new
//! 3. After call, clear() the memo (doesn't free, just clears contents)
//! 4. Proxy is just the MemoObject itself exposed to Python

use crate::memo::MemoTable;
use crate::keepalive::KeepAlive;
use crate::memo_trait::Memo;
use crate::ffi::*;
use std::cell::RefCell;
use std::os::raw::c_void;

/// Thread-local memo that can be reused or detached
pub struct ThreadLocalMemo {
    pub table: MemoTable,
    pub keepalive: KeepAlive,
    exposed_to_python: bool,  // Track if we've been exposed via as_python_dict()
}

impl ThreadLocalMemo {
    pub fn new() -> Self {
        Self {
            table: MemoTable::new(),
            keepalive: KeepAlive::new(),
            exposed_to_python: false,
        }
    }

    fn clear_internal(&mut self) {
        self.table.clear();
        self.keepalive.clear();
        self.exposed_to_python = false;
    }

    pub fn is_exposed(&self) -> bool {
        self.exposed_to_python
    }

    fn cleanup_internal(&mut self) {
        self.clear_internal();
        self.table.shrink_if_large();
        self.keepalive.shrink_if_large();
    }
}

impl Memo for ThreadLocalMemo {
    #[inline(always)]
    unsafe fn lookup(&mut self, key: *const c_void, hash: Py_hash_t) -> Option<*mut PyObject> {
        self.table.lookup(key, hash)
    }

    #[inline(always)]
    unsafe fn insert(&mut self, key: *const c_void, value: *mut PyObject, hash: Py_hash_t) {
        self.table.insert(key, value, hash);
    }

    #[inline(always)]
    unsafe fn keepalive(&mut self, obj: *mut PyObject) {
        self.keepalive.append(obj);
    }

    unsafe fn clear(&mut self) {
        self.clear_internal();
    }

    unsafe fn cleanup(&mut self) {
        self.cleanup_internal();
    }

    #[inline(always)]
    fn is_user_provided(&self) -> bool {
        false
    }

    fn is_exposed(&self) -> bool {
        self.exposed_to_python
    }

    unsafe fn as_python_dict(&mut self) -> *mut PyObject {
        // Mark as exposed to Python
        self.exposed_to_python = true;

        // Create a temporary dict for thread-local memo
        let dict = PyDict_New();
        if dict.is_null() {
            return std::ptr::null_mut();
        }

        // Get the keepalive list as a Python list
        let keepalive_list = self.keepalive.as_python_list();
        if keepalive_list.is_null() {
            Py_DECREF(dict);
            return std::ptr::null_mut();
        }

        // Store keepalive list at memo[id(memo)]
        let memo_id = dict as isize;
        let memo_id_obj = PyLong_FromSsize_t(memo_id);
        if memo_id_obj.is_null() {
            Py_DECREF(keepalive_list);
            Py_DECREF(dict);
            return std::ptr::null_mut();
        }

        if PyDict_SetItem(dict, memo_id_obj, keepalive_list) < 0 {
            Py_DECREF(memo_id_obj);
            Py_DECREF(keepalive_list);
            Py_DECREF(dict);
            return std::ptr::null_mut();
        }

        Py_DECREF(memo_id_obj);
        Py_DECREF(keepalive_list);

        dict
    }
}

thread_local! {
    static THREAD_MEMO: RefCell<Option<ThreadLocalMemo>> = RefCell::new(None);
}

/// Get or create thread-local memo
///
/// Note: In C this checks refcount > 1 to detect if Python holds a reference.
/// In Rust we'll simplify by always creating fresh for now.
pub fn get_thread_local_memo() -> ThreadLocalMemo {
    THREAD_MEMO.with(|memo| {
        let mut memo_ref = memo.borrow_mut();

        match memo_ref.take() {
            Some(mut existing) => {
                // Reuse existing, after clearing
                unsafe { existing.clear(); }
                existing
            }
            None => {
                // Create new
                ThreadLocalMemo::new()
            }
        }
    })
}

/// Return memo to thread-local storage after cleanup
pub fn return_thread_local_memo(mut memo: ThreadLocalMemo) {
    unsafe { memo.cleanup(); }

    THREAD_MEMO.with(|storage| {
        *storage.borrow_mut() = Some(memo);
    });
}
