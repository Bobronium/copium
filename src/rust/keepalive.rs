//! KeepAlive: Vector for preventing premature garbage collection
//!
//! Holds strong references to objects during deepcopy to prevent GC issues
//! with reduce protocol reconstruction.

use crate::ffi::{PyObject, Py_DecRef, Py_IncRef};
use std::ptr;

/// KeepAlive vector
pub struct KeepAlive {
    items: Vec<*mut PyObject>,
}

// Retention policy
const MAX_CAPACITY: usize = 1 << 13; // 8,192 elements
const SHRINK_TO: usize = 1 << 10;    // 1,024 elements

impl KeepAlive {
    /// Create new keepalive vector
    pub fn new() -> Self {
        Self {
            items: Vec::with_capacity(16),
        }
    }

    /// Append object to keepalive
    #[inline(always)]
    pub fn append(&mut self, obj: *mut PyObject) {
        if !obj.is_null() {
            unsafe { Py_IncRef(obj) };
            self.items.push(obj);
        }
    }

    /// Clear all items
    pub fn clear(&mut self) {
        for &obj in &self.items {
            if !obj.is_null() {
                unsafe { Py_DecRef(obj) };
            }
        }
        self.items.clear();
    }

    /// Shrink if grew too large
    pub fn shrink_if_large(&mut self) {
        if self.items.capacity() > MAX_CAPACITY {
            self.items.shrink_to(SHRINK_TO);
        }
    }

    /// Get current size
    #[allow(dead_code)]
    pub fn len(&self) -> usize {
        self.items.len()
    }

    /// Check if empty
    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// Get items as slice (for proxy)
    pub fn as_slice(&self) -> &[*mut PyObject] {
        &self.items
    }

    /// Get mutable items (for proxy)
    pub fn as_mut_vec(&mut self) -> &mut Vec<*mut PyObject> {
        &mut self.items
    }
}

impl Drop for KeepAlive {
    fn drop(&mut self) {
        self.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_keepalive_basic() {
        let mut ka = KeepAlive::new();
        assert_eq!(ka.len(), 0);
        assert!(ka.is_empty());
    }

    #[test]
    fn test_keepalive_capacity() {
        let ka = KeepAlive::new();
        assert!(ka.items.capacity() >= 16);
    }
}
