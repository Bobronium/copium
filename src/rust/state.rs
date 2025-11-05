//! Simplified thread-local state management matching C implementation
//!
//! Pattern:
//! 1. Thread-local MemoObject stored in TLS
//! 2. Check refcount before reuse - if > 1, someone else holds it, create new
//! 3. After call, clear() the memo (doesn't free, just clears contents)
//! 4. Proxy is just the MemoObject itself exposed to Python

use crate::memo::MemoTable;
use crate::keepalive::KeepAlive;
use std::cell::RefCell;

/// Thread-local memo that can be reused or detached
pub struct ThreadLocalMemo {
    pub table: MemoTable,
    pub keepalive: KeepAlive,
}

impl ThreadLocalMemo {
    pub fn new() -> Self {
        Self {
            table: MemoTable::new(),
            keepalive: KeepAlive::new(),
        }
    }

    /// Clear for reuse (doesn't deallocate)
    pub fn clear(&mut self) {
        self.table.clear();
        self.keepalive.clear();
    }

    /// Cleanup after call
    pub fn cleanup(&mut self) {
        self.clear();
        self.table.shrink_if_large();
        self.keepalive.shrink_if_large();
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
                existing.clear();
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
    memo.cleanup();

    THREAD_MEMO.with(|storage| {
        *storage.borrow_mut() = Some(memo);
    });
}
