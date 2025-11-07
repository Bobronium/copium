//! Lightweight hash table for memo
//! - Computes hash key ONCE from pointer to PyObject
//! - Incref on original for the lifetime of the copying process
//! - Never exposes internal table to Python (use Proxy instead)

use crate::ffi::{self, PyObject};
use crate::keepalive::KeepVector;
use std::ptr;

const TOMBSTONE: *const std::os::raw::c_void = !0 as *const _;
const INITIAL_SIZE: usize = 8;
const LOAD_FACTOR_NUM: usize = 7;
const LOAD_FACTOR_DEN: usize = 10;

/// Maximum size to retain between calls (131072 slots)
const RETAIN_MAX_SLOTS: usize = 1 << 17;
/// Shrink to this size (8192 slots)
const RETAIN_SHRINK_TO: usize = 1 << 13;

#[repr(C)]
pub struct MemoEntry {
    pub key: *const std::os::raw::c_void,
    pub value: *mut PyObject,
}

pub struct MemoTable {
    slots: *mut MemoEntry,
    size: usize,
    used: usize,
    filled: usize,
}

impl MemoTable {
    #[inline(always)]
    pub fn new() -> Self {
        Self {
            slots: ptr::null_mut(),
            size: 0,
            used: 0,
            filled: 0,
        }
    }

    /// Ensure table is allocated
    #[inline]
    fn ensure_allocated(&mut self) -> Result<(), ()> {
        if !self.slots.is_null() {
            return Ok(());
        }
        self.resize(INITIAL_SIZE)?;
        Ok(())
    }

    /// Resize table to new size (must be power of 2)
    fn resize(&mut self, new_size: usize) -> Result<(), ()> {
        let new_slots = unsafe {
            libc::calloc(new_size, std::mem::size_of::<MemoEntry>()) as *mut MemoEntry
        };
        if new_slots.is_null() {
            return Err(());
        }

        // Migrate existing entries
        if !self.slots.is_null() {
            for i in 0..self.size {
                let entry = unsafe { &*self.slots.add(i) };
                if !entry.key.is_null() && entry.key != TOMBSTONE {
                    // Rehash and insert into new table
                    let hash = ffi::hash_pointer(entry.key);
                    let mask = new_size - 1;
                    let mut idx = (hash as usize) & mask;

                    loop {
                        let slot = unsafe { &mut *new_slots.add(idx) };
                        if slot.key.is_null() {
                            slot.key = entry.key;
                            slot.value = entry.value; // Transfer ownership
                            break;
                        }
                        idx = (idx + 1) & mask;
                    }
                }
            }
            // Free old slots
            unsafe { libc::free(self.slots as *mut _) };
        }

        self.slots = new_slots;
        self.size = new_size;
        self.filled = self.used; // No tombstones in new table
        Ok(())
    }

    /// Lookup with precomputed hash (hot path)
    #[inline(always)]
    pub fn lookup_with_hash(&self, key: *const std::os::raw::c_void, hash: isize) -> *mut PyObject {
        if self.slots.is_null() {
            return ptr::null_mut();
        }

        let mask = self.size - 1;
        let mut idx = (hash as usize) & mask;

        loop {
            let slot = unsafe { &*self.slots.add(idx) };
            if slot.key.is_null() {
                return ptr::null_mut();
            }
            if slot.key != TOMBSTONE && slot.key == key {
                return slot.value;
            }
            idx = (idx + 1) & mask;
        }
    }

    /// Insert with precomputed hash (hot path)
    #[inline]
    pub fn insert_with_hash(
        &mut self,
        key: *const std::os::raw::c_void,
        value: *mut PyObject,
        hash: isize,
    ) -> Result<(), ()> {
        self.ensure_allocated()?;

        // Check if resize needed
        if self.filled * LOAD_FACTOR_DEN >= self.size * LOAD_FACTOR_NUM {
            let new_size = if self.size == 0 {
                INITIAL_SIZE
            } else {
                self.size * 2
            };
            self.resize(new_size)?;
        }

        let mask = self.size - 1;
        let mut idx = (hash as usize) & mask;
        let mut first_tomb = None;

        loop {
            let slot = unsafe { &mut *self.slots.add(idx) };
            if slot.key.is_null() {
                // Found empty slot
                let insert_idx = first_tomb.unwrap_or(idx);
                let insert_slot = unsafe { &mut *self.slots.add(insert_idx) };
                insert_slot.key = key;
                unsafe { ffi::incref(value) };
                insert_slot.value = value;
                self.used += 1;
                self.filled += 1;
                return Ok(());
            }
            if slot.key == TOMBSTONE {
                if first_tomb.is_none() {
                    first_tomb = Some(idx);
                }
            } else if slot.key == key {
                // Update existing
                let old_value = slot.value;
                unsafe { ffi::incref(value) };
                slot.value = value;
                unsafe { ffi::decref(old_value) };
                return Ok(());
            }
            idx = (idx + 1) & mask;
        }
    }

    /// Clear all entries (but keep capacity for reuse)
    pub fn clear(&mut self) {
        if self.slots.is_null() {
            return;
        }

        for i in 0..self.size {
            let slot = unsafe { &mut *self.slots.add(i) };
            if !slot.key.is_null() && slot.key != TOMBSTONE {
                unsafe { ffi::decref(slot.value) };
            }
            slot.key = ptr::null();
            slot.value = ptr::null_mut();
        }
        self.used = 0;
        self.filled = 0;
    }

    /// Shrink if table grew too large
    pub fn shrink_if_large(&mut self) {
        if self.size > RETAIN_MAX_SLOTS {
            let target = RETAIN_SHRINK_TO;
            if self.resize(target).is_err() {
                // If resize fails, keep larger buffer (correctness preserved)
            }
        }
    }

    /// Get iteration info for Python dict protocol
    pub fn iter_info(&self) -> (usize, usize) {
        (self.used, self.size)
    }

    /// Iterate entries
    pub fn iter<F>(&self, mut f: F)
    where
        F: FnMut(*const std::os::raw::c_void, *mut PyObject),
    {
        if self.slots.is_null() {
            return;
        }

        for i in 0..self.size {
            let slot = unsafe & *self.slots.add(i) };
            if !slot.key.is_null() && slot.key != TOMBSTONE {
                f(slot.key, slot.value);
            }
        }
    }
}

impl Drop for MemoTable {
    fn drop(&mut self) {
        self.clear();
        if !self.slots.is_null() {
            unsafe { libc::free(self.slots as *mut _) };
        }
    }
}

/// Thread-local memo state
pub struct ThreadMemo {
    pub table: MemoTable,
    pub keepalive: KeepVector,
    pub current_proxy: *mut PyObject, // Nullable
}

impl ThreadMemo {
    pub fn new() -> Self {
        Self {
            table: MemoTable::new(),
            keepalive: KeepVector::new(),
            current_proxy: ptr::null_mut(),
        }
    }

    /// Reset for next call
    pub fn reset(&mut self) {
        self.table.clear();
        self.keepalive.clear();
        self.table.shrink_if_large();
        self.keepalive.shrink_if_large();
        if !self.current_proxy.is_null() {
            unsafe { ffi::decref(self.current_proxy) };
            self.current_proxy = ptr::null_mut();
        }
    }

    /// Initialize memo and keepalive for first use
    #[inline(always)]
    pub fn initialize(&mut self) -> Result<(), ()> {
        self.table.ensure_allocated()
    }
}

impl Drop for ThreadMemo {
    fn drop(&mut self) {
        self.reset();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_hash_pointer() {
        let ptr1 = 0x1234 as *const std::os::raw::c_void;
        let ptr2 = 0x5678 as *const std::os::raw::c_void;
        let h1 = ffi::hash_pointer(ptr1);
        let h2 = ffi::hash_pointer(ptr2);
        assert_ne!(h1, h2);
        // Hash should be deterministic
        assert_eq!(h1, ffi::hash_pointer(ptr1));
    }
}
