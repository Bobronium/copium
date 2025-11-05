//! Memo trait for compile-time polymorphism over memo implementations

use crate::ffi::*;
use std::os::raw::c_void;

/// Trait for memo operations - implemented by both ThreadLocalMemo and UserProvidedMemo
///
/// This trait allows the entire deepcopy implementation to be generic over the memo type,
/// enabling compile-time specialization with zero runtime cost.
pub trait Memo {
    /// Look up an object in the memo by its pointer address
    /// Returns Some(copied_object) if found, None otherwise
    unsafe fn lookup(&mut self, key: *const c_void, hash: Py_hash_t) -> Option<*mut PyObject>;

    /// Insert a mapping from original object to copied object
    unsafe fn insert(&mut self, key: *const c_void, value: *mut PyObject, hash: Py_hash_t);

    /// Add an object to the keepalive list to prevent premature GC
    unsafe fn keepalive(&mut self, obj: *mut PyObject);

    /// Clear the memo for reuse (thread-local path) or no-op (user-provided path)
    unsafe fn clear(&mut self);

    /// Cleanup before returning to pool (thread-local path) or no-op (user-provided path)
    unsafe fn cleanup(&mut self);

    /// Check if this is a user-provided memo (affects behavior of __deepcopy__ methods)
    fn is_user_provided(&self) -> bool;
}
