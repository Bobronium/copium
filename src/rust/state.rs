//! Type-state pattern for compile-time memo lifecycle management
//!
//! This module uses Rust's type system to enforce correct state transitions at compile-time:
//! - Uninitialized: No memo/keepalive allocated
//! - Initialized: Memo table and keepalive vector ready
//! - Proxied: Python proxy exposed (detached after call)
//!
//! This ensures zero redundant operations - each state transition happens exactly once.

use crate::memo::MemoTable;
use crate::keepalive::KeepAlive;
use crate::proxy::MemoProxy;
use std::cell::RefCell;
use std::marker::PhantomData;

/// Marker types for state machine
pub struct Uninitialized;
pub struct Initialized;
pub struct Proxied;

/// Thread-local state with type-state pattern
pub struct MemoState<State = Uninitialized> {
    table: Option<MemoTable>,
    keepalive: Option<KeepAlive>,
    proxy: Option<MemoProxy>,
    _state: PhantomData<State>,
}

/// Global thread-local storage
thread_local! {
    static THREAD_STATE: RefCell<ThreadState> = RefCell::new(ThreadState::new());
}

/// Thread state manager
pub struct ThreadState {
    memo_state: MemoState<Uninitialized>,
    recursion_depth: usize,
}

impl ThreadState {
    pub fn new() -> Self {
        Self {
            memo_state: MemoState::new(),
            recursion_depth: 0,
        }
    }

    /// Access thread-local state
    pub fn with_state<F, R>(f: F) -> R
    where
        F: FnOnce(&mut ThreadState) -> R,
    {
        THREAD_STATE.with(|state| f(&mut state.borrow_mut()))
    }

    /// Get current memo state (uninitialized)
    pub fn uninitialized(&mut self) -> &mut MemoState<Uninitialized> {
        &mut self.memo_state
    }

    /// Initialize memo and keepalive
    pub fn initialize(&mut self) -> &mut MemoState<Initialized> {
        let state = std::mem::replace(&mut self.memo_state, MemoState::new());
        let initialized = state.initialize();
        // SAFETY: We're transmuting the state type
        unsafe {
            self.memo_state = std::mem::transmute(initialized);
            std::mem::transmute(&mut self.memo_state)
        }
    }

    /// Clean up after deepcopy call
    pub fn cleanup(&mut self) {
        self.memo_state.reset();
        self.recursion_depth = 0;
    }
}

impl<State> MemoState<State> {
    /// Create new uninitialized state
    fn new() -> MemoState<Uninitialized> {
        MemoState {
            table: None,
            keepalive: None,
            proxy: None,
            _state: PhantomData,
        }
    }
}

impl MemoState<Uninitialized> {
    /// Transition to initialized state
    pub fn initialize(mut self) -> MemoState<Initialized> {
        self.table = Some(MemoTable::new());
        self.keepalive = Some(KeepAlive::new());

        MemoState {
            table: self.table,
            keepalive: self.keepalive,
            proxy: None,
            _state: PhantomData,
        }
    }

    /// Check if we need initialization (always false for Uninitialized)
    #[inline(always)]
    pub fn needs_init(&self) -> bool {
        true
    }
}

impl MemoState<Initialized> {
    /// Get mutable reference to table
    #[inline(always)]
    pub fn table_mut(&mut self) -> &mut MemoTable {
        self.table.as_mut().unwrap()
    }

    /// Get reference to table
    #[inline(always)]
    pub fn table(&self) -> &MemoTable {
        self.table.as_ref().unwrap()
    }

    /// Get mutable reference to keepalive
    #[inline(always)]
    pub fn keepalive_mut(&mut self) -> &mut KeepAlive {
        self.keepalive.as_mut().unwrap()
    }

    /// Lookup in memo with precomputed hash
    #[inline(always)]
    pub fn memo_lookup(&self, key: *const std::os::raw::c_void, hash: isize) -> Option<*mut crate::ffi::PyObject> {
        self.table().lookup(key, hash)
    }

    /// Insert into memo with precomputed hash
    #[inline(always)]
    pub fn memo_insert(&mut self, key: *const std::os::raw::c_void, value: *mut crate::ffi::PyObject, hash: isize) {
        self.table_mut().insert(key, value, hash);
    }

    /// Append to keepalive
    #[inline(always)]
    pub fn keepalive_append(&mut self, obj: *mut crate::ffi::PyObject) {
        self.keepalive_mut().append(obj);
    }

    /// Create proxy for Python __deepcopy__ calls
    pub fn create_proxy(mut self) -> MemoState<Proxied> {
        let proxy = MemoProxy::new(
            self.table.as_mut().unwrap(),
            self.keepalive.as_mut().unwrap(),
        );

        MemoState {
            table: self.table,
            keepalive: self.keepalive,
            proxy: Some(proxy),
            _state: PhantomData,
        }
    }
}

impl MemoState<Proxied> {
    /// Get proxy for Python
    pub fn get_proxy(&self) -> &MemoProxy {
        self.proxy.as_ref().unwrap()
    }

    /// Detach proxy if referenced
    pub fn detach_proxy(&mut self) {
        if let Some(proxy) = &mut self.proxy {
            proxy.detach();
        }
    }
}

impl<State> MemoState<State> {
    /// Reset to uninitialized (cleanup)
    fn reset(&mut self) {
        // Clear and shrink table
        if let Some(table) = &mut self.table {
            table.clear();
            table.shrink_if_large();
        }

        // Clear and shrink keepalive
        if let Some(keepalive) = &mut self.keepalive {
            keepalive.clear();
            keepalive.shrink_if_large();
        }

        // Detach proxy if needed
        if let Some(proxy) = &mut self.proxy {
            if proxy.is_referenced() {
                proxy.detach();
            }
        }
        self.proxy = None;
    }
}

/// Helper trait to work with different states
pub trait MemoStateAccess {
    fn is_initialized(&self) -> bool;
}

impl MemoStateAccess for MemoState<Uninitialized> {
    #[inline(always)]
    fn is_initialized(&self) -> bool {
        false
    }
}

impl MemoStateAccess for MemoState<Initialized> {
    #[inline(always)]
    fn is_initialized(&self) -> bool {
        true
    }
}

impl MemoStateAccess for MemoState<Proxied> {
    #[inline(always)]
    fn is_initialized(&self) -> bool {
        true
    }
}
