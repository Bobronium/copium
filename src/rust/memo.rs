//! MemoTable: High-performance hash table for memoization
//!
//! Uses open addressing with linear probing and tombstone-based deletion.
//! Hash is computed ONCE using SplitMix64 and reused throughout.

use crate::ffi::{hash_pointer, PyObject, Py_DecRef, Py_IncRef, Py_ssize_t, Py_XNewRef};
use std::os::raw::c_void;
use std::ptr;

/// Tombstone marker for deleted entries
const TOMBSTONE: *const c_void = usize::MAX as *const c_void;

/// Memo table entry
#[derive(Clone, Copy)]
#[repr(C)]
struct MemoEntry {
    key: *const c_void,
    value: *mut PyObject,
}

/// High-performance memo table
pub struct MemoTable {
    slots: Vec<MemoEntry>,
    size: usize,      // Power of two capacity
    used: usize,      // Live entries
    filled: usize,    // Live + tombstones
}

// Retention policy
const MAX_SLOTS: usize = 1 << 17; // 131,072 slots
const SHRINK_TO: usize = 1 << 13; // 8,192 slots

impl MemoTable {
    /// Create new memo table
    pub fn new() -> Self {
        Self::with_capacity(16)
    }

    /// Create with specific capacity (must be power of 2)
    fn with_capacity(capacity: usize) -> Self {
        let size = capacity.next_power_of_two();
        let slots = vec![
            MemoEntry {
                key: ptr::null(),
                value: ptr::null_mut(),
            };
            size
        ];

        Self {
            slots,
            size,
            used: 0,
            filled: 0,
        }
    }

    /// Lookup with precomputed hash
    #[inline(always)]
    pub fn lookup(&self, key: *const c_void, hash: Py_ssize_t) -> Option<*mut PyObject> {
        if self.size == 0 {
            return None;
        }

        let mask = self.size - 1;
        let mut idx = (hash as usize) & mask;

        loop {
            let entry = &self.slots[idx];

            if entry.key.is_null() {
                // Empty slot - key not found
                return None;
            }

            if entry.key == key {
                // Found!
                return Some(entry.value);
            }

            // Continue linear probe (skip tombstones)
            idx = (idx + 1) & mask;
        }
    }

    /// Insert with precomputed hash
    #[inline]
    pub fn insert(&mut self, key: *const c_void, value: *mut PyObject, hash: Py_ssize_t) {
        // Resize if needed (70% load factor)
        if self.filled * 10 >= self.size * 7 {
            let new_capacity = if self.size == 0 {
                16
            } else {
                self.size * 2
            };
            self.resize(new_capacity);
        }

        self.insert_unchecked(key, value, hash);
    }

    /// Insert without checking capacity
    #[inline(always)]
    fn insert_unchecked(&mut self, key: *const c_void, value: *mut PyObject, hash: Py_ssize_t) {
        let mask = self.size - 1;
        let mut idx = (hash as usize) & mask;

        loop {
            let entry = &mut self.slots[idx];

            if entry.key.is_null() || entry.key == TOMBSTONE {
                // Empty or tombstone - insert here
                if entry.key.is_null() {
                    self.filled += 1;
                }
                entry.key = key;
                entry.value = value;
                unsafe { Py_IncRef(value) };
                self.used += 1;
                return;
            }

            if entry.key == key {
                // Update existing
                unsafe {
                    Py_IncRef(value);
                    Py_DecRef(entry.value);
                }
                entry.value = value;
                return;
            }

            // Continue linear probe
            idx = (idx + 1) & mask;
        }
    }

    /// Resize table
    fn resize(&mut self, new_capacity: usize) {
        let old_slots = std::mem::replace(
            &mut self.slots,
            vec![
                MemoEntry {
                    key: ptr::null(),
                    value: ptr::null_mut(),
                };
                new_capacity
            ],
        );

        self.size = new_capacity;
        self.used = 0;
        self.filled = 0;

        // Rehash all entries
        for entry in old_slots {
            if !entry.key.is_null() && entry.key != TOMBSTONE {
                let hash = hash_pointer(entry.key as *mut c_void);
                self.insert_unchecked(entry.key, entry.value, hash);
                // Don't incref - we're transferring ownership
                unsafe { Py_DecRef(entry.value) };
            }
        }
    }

    /// Clear all entries
    pub fn clear(&mut self) {
        for entry in &mut self.slots {
            if !entry.key.is_null() && entry.key != TOMBSTONE {
                unsafe {
                    Py_DecRef(entry.value);
                }
            }
            entry.key = ptr::null();
            entry.value = ptr::null_mut();
        }
        self.used = 0;
        self.filled = 0;
    }

    /// Shrink if table grew too large
    pub fn shrink_if_large(&mut self) {
        if self.size > MAX_SLOTS {
            self.resize(SHRINK_TO);
        }
    }

    /// Get current usage stats
    #[allow(dead_code)]
    pub fn stats(&self) -> (usize, usize, usize) {
        (self.size, self.used, self.filled)
    }
}

impl Drop for MemoTable {
    fn drop(&mut self) {
        self.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_memo_basic() {
        let mut table = MemoTable::new();

        // Test empty lookup
        let key = 0x1000 as *const c_void;
        let hash = hash_pointer(key as *mut c_void);
        assert!(table.lookup(key, hash).is_none());

        // Note: Can't actually test with real PyObjects without Python runtime
        // This would need integration tests
    }

    #[test]
    fn test_memo_capacity() {
        let table = MemoTable::new();
        assert!(table.size > 0);
        assert_eq!(table.used, 0);
    }
}
