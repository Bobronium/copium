use std::hint::likely;
use std::ptr;

use crate::py::{PyObject, PyObjectPtr, PyTypeInfo};

pub(crate) const TOMBSTONE: usize = usize::MAX;

const MEMO_RETAIN_MAX_SLOTS: usize = 1 << 17;
const MEMO_RETAIN_SHRINK_TO: usize = 1 << 13;
const KEEP_RETAIN_MAX: usize = 1 << 13;
const KEEP_RETAIN_TARGET: usize = 1 << 10;

pub(crate) struct MemoEntry {
    pub(crate) key: usize,
    pub(crate) value: *mut PyObject,
}

pub struct MemoTable {
    pub(crate) slots: *mut MemoEntry,
    pub(crate) size: usize,
    pub(crate) used: usize,
    pub(crate) filled: usize,
}

#[inline(always)]
pub(crate) fn hash_pointer(ptr: usize) -> usize {
    let mut h = ptr;
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    h
}

impl MemoTable {
    pub(crate) fn new() -> Self {
        Self {
            slots: ptr::null_mut(),
            size: 0,
            used: 0,
            filled: 0,
        }
    }

    fn ensure(&mut self) -> i32 {
        if likely(!self.slots.is_null()) {
            return 0;
        }
        self.resize(1)
    }

    fn resize(&mut self, min_needed: usize) -> i32 {
        let mut new_size = 8usize;
        while new_size < min_needed.saturating_mul(2) {
            new_size = new_size.saturating_mul(2);
        }

        let layout = std::alloc::Layout::array::<MemoEntry>(new_size).unwrap();
        let new_slots = unsafe { std::alloc::alloc_zeroed(layout) as *mut MemoEntry };
        if new_slots.is_null() {
            return -1;
        }

        let old_slots = self.slots;
        let old_size = self.size;

        self.slots = new_slots;
        self.size = new_size;
        self.used = 0;
        self.filled = 0;

        if !old_slots.is_null() {
            for i in 0..old_size {
                let entry = unsafe { &*old_slots.add(i) };
                if entry.key != 0 && entry.key != TOMBSTONE {
                    self.insert_no_grow(entry.key, entry.value);
                }
            }
            let old_layout = std::alloc::Layout::array::<MemoEntry>(old_size).unwrap();
            unsafe { std::alloc::dealloc(old_slots as *mut u8, old_layout) };
        }

        0
    }

    fn insert_no_grow(&mut self, key: usize, value: *mut PyObject) {
        let mask = self.size - 1;
        let mut idx = hash_pointer(key) & mask;

        loop {
            let entry = unsafe { &mut *self.slots.add(idx) };
            if entry.key == 0 {
                entry.key = key;
                entry.value = value;
                unsafe { value.incref() };
                self.used += 1;
                self.filled += 1;
                return;
            }
            if entry.key == key {
                let old = entry.value;
                entry.value = value;
                unsafe {
                    value.incref();
                    old.decref_nullable();
                }
                return;
            }
            idx = (idx + 1) & mask;
        }
    }

    #[inline(always)]
    pub fn lookup_h(&self, key: usize, hash: usize) -> *mut PyObject {
        if std::hint::unlikely(self.slots.is_null()) {
            return ptr::null_mut();
        }

        let mask = self.size - 1;
        let mut idx = hash & mask;

        loop {
            let entry = unsafe { &*self.slots.add(idx) };
            if entry.key == 0 {
                return ptr::null_mut();
            }
            if likely(entry.key != TOMBSTONE) && entry.key == key {
                return entry.value;
            }
            idx = (idx + 1) & mask;
        }
    }

    #[inline(always)]
    pub fn insert_h(&mut self, key: usize, value: *mut PyObject, hash: usize) -> i32 {
        if std::hint::unlikely(self.ensure() < 0) {
            return -1;
        }
        if std::hint::unlikely(self.filled * 10 >= self.size * 7) {
            if self.resize(self.used + 1) < 0 {
                return -1;
            }
        }

        let mask = self.size - 1;
        let mut idx = hash & mask;
        let mut first_tomb: Option<usize> = None;

        loop {
            let entry = unsafe { &mut *self.slots.add(idx) };
            if likely(entry.key == 0) {
                let at = first_tomb.unwrap_or(idx);
                let slot = unsafe { &mut *self.slots.add(at) };
                slot.key = key;
                unsafe { value.incref() };
                slot.value = value;
                self.used += 1;
                self.filled += 1;
                return 0;
            }
            if std::hint::unlikely(entry.key == TOMBSTONE) {
                if first_tomb.is_none() {
                    first_tomb = Some(idx);
                }
            } else if std::hint::unlikely(entry.key == key) {
                let old = entry.value;
                unsafe {
                    value.incref();
                    entry.value = value;
                    old.decref_nullable();
                }
                return 0;
            }
            idx = (idx + 1) & mask;
        }
    }

    pub fn remove_h(&mut self, key: usize, hash: usize) -> i32 {
        if self.slots.is_null() {
            return -1;
        }

        let mask = self.size - 1;
        let mut idx = hash & mask;

        loop {
            let entry = unsafe { &mut *self.slots.add(idx) };
            if entry.key == 0 {
                return -1;
            }
            if entry.key != TOMBSTONE && entry.key == key {
                entry.key = TOMBSTONE;
                unsafe { entry.value.decref_nullable() };
                entry.value = ptr::null_mut();
                self.used -= 1;
                return 0;
            }
            idx = (idx + 1) & mask;
        }
    }

    pub fn clear(&mut self) {
        if self.slots.is_null() {
            return;
        }

        for i in 0..self.size {
            let entry = unsafe { &*self.slots.add(i) };
            if likely(entry.key != 0 && entry.key != TOMBSTONE) {
                unsafe { entry.value.decref_nullable() };
            }
        }
        unsafe { ptr::write_bytes(self.slots, 0, self.size) };
        self.used = 0;
        self.filled = 0;
    }

    pub fn reset(&mut self) {
        self.clear();
        if self.size > MEMO_RETAIN_MAX_SLOTS {
            let _ = self.resize(MEMO_RETAIN_SHRINK_TO / 2);
        }
    }
}

impl Drop for MemoTable {
    fn drop(&mut self) {
        if self.slots.is_null() {
            return;
        }

        for i in 0..self.size {
            let entry = unsafe { &*self.slots.add(i) };
            if entry.key != 0 && entry.key != TOMBSTONE {
                unsafe { entry.value.decref_nullable() };
            }
        }

        let layout = std::alloc::Layout::array::<MemoEntry>(self.size).unwrap();
        unsafe { std::alloc::dealloc(self.slots as *mut u8, layout) };
    }
}

// ── KeepaliveVec ───────────────────────────────────────────

pub struct KeepaliveVec {
    pub(crate) items: Vec<*mut PyObject>,
}

impl KeepaliveVec {
    pub(crate) fn new() -> Self {
        Self { items: Vec::new() }
    }

    pub fn append<T: PyTypeInfo>(&mut self, obj: *mut T) {
        unsafe { obj.incref() };
        self.items.push(obj as _);
    }

    pub fn clear(&mut self) {
        for &item in &self.items {
            unsafe { item.decref() };
        }
        self.items.clear();
    }

    pub fn shrink_if_large(&mut self) {
        if self.items.capacity() > KEEP_RETAIN_MAX {
            self.items.shrink_to(KEEP_RETAIN_TARGET);
        }
    }
}

impl Drop for KeepaliveVec {
    fn drop(&mut self) {
        self.clear();
    }
}

// ── UndoLog ────────────────────────────────────────────────

pub struct UndoLog {
    pub(super) keys: Vec<usize>,
}

impl UndoLog {
    pub(crate) fn new() -> Self {
        Self { keys: Vec::new() }
    }

    pub(super) fn append(&mut self, key: usize) {
        self.keys.push(key);
    }

    pub fn clear(&mut self) {
        self.keys.clear();
    }

    pub fn shrink_if_large(&mut self) {
        if self.keys.capacity() > KEEP_RETAIN_MAX {
            self.keys.shrink_to(KEEP_RETAIN_TARGET);
        }
    }
}
