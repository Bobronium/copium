//! MemoProxy: Python-facing dict-like interface for memo
//!
//! Provides the same interface as the C implementation:
//! - memo[id(obj)] for lookups
//! - memo[id(memo)] returns keepalive list proxy

use crate::memo::MemoTable;
use crate::keepalive::KeepAlive;
use std::sync::atomic::{AtomicBool, Ordering};

/// Proxy object that exposes memo to Python
pub struct MemoProxy {
    table_ref: *mut MemoTable,
    keepalive_ref: *mut KeepAlive,
    detached: AtomicBool,
    referenced: AtomicBool,
}

impl MemoProxy {
    /// Create new proxy pointing to table and keepalive
    pub fn new(table: &mut MemoTable, keepalive: &mut KeepAlive) -> Self {
        Self {
            table_ref: table as *mut MemoTable,
            keepalive_ref: keepalive as *mut KeepAlive,
            detached: AtomicBool::new(false),
            referenced: AtomicBool::new(false),
        }
    }

    /// Check if proxy is still attached
    pub fn is_attached(&self) -> bool {
        !self.detached.load(Ordering::Acquire)
    }

    /// Check if proxy is referenced by Python code
    pub fn is_referenced(&self) -> bool {
        self.referenced.load(Ordering::Acquire)
    }

    /// Mark as referenced
    pub fn mark_referenced(&self) {
        self.referenced.store(true, Ordering::Release);
    }

    /// Detach proxy from memo/keepalive
    pub fn detach(&mut self) {
        if self.is_referenced() {
            // Move references out before detaching
            self.move_refs();
        }
        self.detached.store(true, Ordering::Release);
        self.table_ref = std::ptr::null_mut();
        self.keepalive_ref = std::ptr::null_mut();
    }

    /// Move references when detaching (if proxy is kept by Python code)
    fn move_refs(&mut self) {
        // In the C implementation, this would transfer ownership
        // For now, we'll just mark as detached
        // The actual implementation would need to create a real Python dict
    }

    /// Get table reference (unsafe - caller must ensure still attached)
    pub unsafe fn table(&self) -> Option<&MemoTable> {
        if self.is_attached() && !self.table_ref.is_null() {
            Some(&*self.table_ref)
        } else {
            None
        }
    }

    /// Get mutable table reference (unsafe - caller must ensure still attached)
    pub unsafe fn table_mut(&mut self) -> Option<&mut MemoTable> {
        if self.is_attached() && !self.table_ref.is_null() {
            Some(&mut *self.table_ref)
        } else {
            None
        }
    }

    /// Get keepalive reference (unsafe - caller must ensure still attached)
    pub unsafe fn keepalive(&self) -> Option<&KeepAlive> {
        if self.is_attached() && !self.keepalive_ref.is_null() {
            Some(&*self.keepalive_ref)
        } else {
            None
        }
    }
}

unsafe impl Send for MemoProxy {}
unsafe impl Sync for MemoProxy {}
