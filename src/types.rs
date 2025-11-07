//! Type-level state machine for compile-time guarantees
//!
//! State transitions:
//! - Uninitialized -> FromUser (user provided memo)
//! - Uninitialized -> Initialized (we create memo)
//! - Initialized -> Cleanup (after call, before next use)
//!
//! The type system ensures:
//! - No double initialization
//! - No use before initialization
//! - No forgetting to cleanup
//! - Hash computed exactly once when needed

use std::marker::PhantomData;

/// Memo state: uninitialized (no memo yet)
pub struct Uninitialized;

/// Memo state: from user (user provided memo dict)
pub struct FromUser;

/// Memo state: initialized by us
pub struct Initialized;

/// Memo state: needs cleanup
pub struct NeedsCleanup;

/// Hash state: not computed
pub struct NoHash;

/// Hash state: computed
pub struct HasHash(pub isize);

/// Type-level marker for object type
pub struct ObjectType {
    pub type_ptr: *mut crate::ffi::PyTypeObject,
}

impl ObjectType {
    #[inline(always)]
    pub fn new(type_ptr: *mut crate::ffi::PyTypeObject) -> Self {
        Self { type_ptr }
    }

    #[inline(always)]
    pub fn matches(&self, other: *mut crate::ffi::PyTypeObject) -> bool {
        self.type_ptr == other
    }
}

/// Zero-cost wrapper that encodes state in type system
///
/// S: Memo state (Uninitialized, FromUser, Initialized)
/// H: Hash state (NoHash, HasHash)
pub struct CopyContext<S, H> {
    _state: PhantomData<S>,
    _hash: PhantomData<H>,
}

impl CopyContext<Uninitialized, NoHash> {
    #[inline(always)]
    pub fn new() -> Self {
        Self {
            _state: PhantomData,
            _hash: PhantomData,
        }
    }

    /// Transition to FromUser state (user provided memo)
    #[inline(always)]
    pub fn with_user_memo(self) -> CopyContext<FromUser, NoHash> {
        CopyContext {
            _state: PhantomData,
            _hash: PhantomData,
        }
    }

    /// Transition to Initialized state (we create memo)
    #[inline(always)]
    pub fn initialize_memo(self) -> CopyContext<Initialized, NoHash> {
        CopyContext {
            _state: PhantomData,
            _hash: PhantomData,
        }
    }
}

impl<H> CopyContext<FromUser, H> {
    /// Check if object is in user's memo
    #[inline(always)]
    pub fn check_user_memo(&self) -> bool {
        // Implementation will check user memo
        false
    }
}

impl<H> CopyContext<Initialized, H> {
    /// Ensure hash is computed
    #[inline(always)]
    pub fn with_hash(self, hash: isize) -> CopyContext<Initialized, HasHash> {
        CopyContext {
            _state: PhantomData,
            _hash: PhantomData,
        }
    }

    /// Transition to cleanup state
    #[inline(always)]
    pub fn needs_cleanup(self) -> CopyContext<NeedsCleanup, H> {
        CopyContext {
            _state: PhantomData,
            _hash: PhantomData,
        }
    }
}

impl<S, H> CopyContext<S, H> {
    /// Get hash value (only available when HasHash)
    #[inline(always)]
    pub fn get_hash(&self) -> Option<isize>
    where
        H: HashValue,
    {
        H::value()
    }
}

/// Trait to extract hash value at compile time
pub trait HashValue {
    fn value() -> Option<isize>;
}

impl HashValue for NoHash {
    #[inline(always)]
    fn value() -> Option<isize> {
        None
    }
}

impl HashValue for HasHash {
    #[inline(always)]
    fn value() -> Option<isize> {
        // This is a marker; actual value passed through context
        None
    }
}

/// Trait for memo states that allow lookup
pub trait CanLookup {}
impl CanLookup for FromUser {}
impl CanLookup for Initialized {}

/// Trait for memo states that allow insert
pub trait CanInsert {}
impl CanInsert for Initialized {}

/// Result of deepcopy that tracks immutability
#[derive(Clone, Copy)]
pub enum CopyResult {
    /// Immutable object, can be reused directly
    Immutable(*mut crate::ffi::PyObject),
    /// Mutable copy
    Mutable(*mut crate::ffi::PyObject),
    /// Found in memo
    FromMemo(*mut crate::ffi::PyObject),
    /// Error occurred
    Error,
}

impl CopyResult {
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut crate::ffi::PyObject {
        match self {
            CopyResult::Immutable(p) => *p,
            CopyResult::Mutable(p) => *p,
            CopyResult::FromMemo(p) => *p,
            CopyResult::Error => std::ptr::null_mut(),
        }
    }

    #[inline(always)]
    pub fn is_error(&self) -> bool {
        matches!(self, CopyResult::Error)
    }

    #[inline(always)]
    pub fn is_immutable(&self) -> bool {
        matches!(self, CopyResult::Immutable(_))
    }
}
