//! Keepalive vector
//! - Owns memory buffer that can grow
//! - Doesn't malloc/free unless buffer is grown beyond certain point
//! - Returns to baseline if too large
//! - Never exposes to Python code directly, instead construct Proxy if needed

use crate::ffi::{self, PyObject};
use std::ptr;

/// Maximum capacity to retain (8192 elements)
const RETAIN_MAX: usize = 1 << 13;
/// Target capacity after shrink (1024 elements)
const RETAIN_TARGET: usize = 1 << 10;
/// Initial capacity
const INITIAL_CAPACITY: usize = 8;

pub struct KeepVector {
    items: *mut *mut PyObject,
    size: usize,
    capacity: usize,
}

impl KeepVector {
    #[inline(always)]
    pub fn new() -> Self {
        Self {
            items: ptr::null_mut(),
            size: 0,
            capacity: 0,
        }
    }

    /// Grow to at least min_capacity
    fn grow(&mut self, min_capacity: usize) -> Result<(), ()> {
        let mut new_cap = if self.capacity == 0 {
            INITIAL_CAPACITY
        } else {
            self.capacity
        };

        while new_cap < min_capacity {
            new_cap = new_cap.checked_mul(2).ok_or(())?;
        }

        let new_items = unsafe {
            libc::realloc(
                self.items as *mut _,
                new_cap * std::mem::size_of::<*mut PyObject>(),
            ) as *mut *mut PyObject
        };

        if new_items.is_null() {
            return Err(());
        }

        self.items = new_items;
        self.capacity = new_cap;
        Ok(())
    }

    /// Append object to vector
    #[inline]
    pub fn append(&mut self, obj: *mut PyObject) -> Result<(), ()> {
        if self.size >= self.capacity {
            self.grow(self.size + 1)?;
        }

        unsafe {
            ffi::incref(obj);
            *self.items.add(self.size) = obj;
        }
        self.size += 1;
        Ok(())
    }

    /// Clear all items
    pub fn clear(&mut self) {
        for i in 0..self.size {
            unsafe {
                let item = *self.items.add(i);
                ffi::decref(item);
            }
        }
        self.size = 0;
    }

    /// Shrink capacity if it ballooned past the cap
    pub fn shrink_if_large(&mut self) {
        if self.items.is_null() || self.capacity <= RETAIN_MAX {
            return;
        }

        let target = if self.size > RETAIN_TARGET {
            self.size
        } else {
            RETAIN_TARGET
        };

        let new_items = unsafe {
            libc::realloc(
                self.items as *mut _,
                target * std::mem::size_of::<*mut PyObject>(),
            ) as *mut *mut PyObject
        };

        if !new_items.is_null() {
            self.items = new_items;
            self.capacity = target;
        }
        // If realloc fails, keep larger buffer (correctness preserved)
    }

    /// Get item at index (for Python list protocol)
    #[inline(always)]
    pub fn get(&self, index: usize) -> Option<*mut PyObject> {
        if index < self.size {
            Some(unsafe { *self.items.add(index) })
        } else {
            None
        }
    }

    /// Get size
    #[inline(always)]
    pub fn len(&self) -> usize {
        self.size
    }

    /// Check if empty
    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        self.size == 0
    }
}

impl Drop for KeepVector {
    fn drop(&mut self) {
        self.clear();
        if !self.items.is_null() {
            unsafe { libc::free(self.items as *mut _) };
        }
    }
}
