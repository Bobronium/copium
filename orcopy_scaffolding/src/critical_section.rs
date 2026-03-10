//! Wrappers for the Python critical section API
//!
//! [Critical Sections](https://docs.python.org/3/c-api/init.html#python-critical-section-api) allow
//! access to the [`PyMutex`](https://docs.python.org/3/c-api/init.html#c.PyMutex) lock attached to
//! each Python object in the free-threaded build. They are no-ops on the GIL-enabled build.
//!
//! Provides weaker locking guarantees than traditional locks, but can in some cases be used to
//! provide guarantees similar to the GIL without the risk of deadlocks associated with traditional
//! locks.
//!
//! # Usage Notes
//!
//! The calling thread locks the per-object mutex when it enters the critical section and holds it
//! until exiting the critical section unless the critical section is suspended. Any call into the
//! CPython C API may cause the critical section to be suspended. Creating an inner critical
//! section, for example by accessing an item in a Python list or dict, will cause the outer
//! critical section to be relased while the inner critical section is active.
//!
//! As a consequence, it is only possible to lock one or two objects at a time. If you need two lock
//! two objects, you should use the variants that accept two arguments. The outer critical section
//! is suspended if you create an outer an inner critical section on two objects using the
//! single-argument variants.
//!
//! It is not currently possible to lock more than two objects simultaneously using this mechanism.
//! Taking a critical section on a container object does not lock the objects stored in the
//! container.
//!
//! Many CPython C API functions do not lock the per-object mutex on objects passed to Python. You
//! should not expect critical sections applied to built-in types to prevent concurrent
//! modification. This API is most useful for user-defined types with full control over how the
//! internal state for the type is managed.
//!
//! The caller must ensure the closure cannot implicitly release the critical section. If a
//! multithreaded program calls back into the Python interpreter in a manner that would cause the
//! critical section to be released, the per-object mutex will be unlocked and the state of the
//! object may be read from or modified by another thread. Concurrent modifications are impossible,
//! but races are possible and the state of an object may change "underneath" a suspended thread in
//! possibly surprising ways.

use pyo3_ffi::*;


#[cfg(Py_GIL_DISABLED)]
struct CSGuard(pyo3_ffi::PyCriticalSection);

#[cfg(Py_GIL_DISABLED)]
impl Drop for CSGuard {
    fn drop(&mut self) {
        unsafe {
            pyo3_ffi::PyCriticalSection_End(&mut self.0);
        }
    }
}


#[cfg(Py_GIL_DISABLED)]
struct CS2Guard(pyo3_ffi::PyCriticalSection2);

#[cfg(Py_GIL_DISABLED)]
impl Drop for CS2Guard {
    fn drop(&mut self) {
        unsafe {
            pyo3_ffi::PyCriticalSection2_End(&mut self.0);
        }
    }
}

#[inline(always)]
pub fn with_critical_section_raw<F, R>(object: *mut PyObject, f: F) -> R
where
    F: FnOnce() -> R,
{
    #[cfg(Py_GIL_DISABLED)]
    {
        let mut guard = CSGuard(unsafe { std::mem::zeroed() });
        unsafe { pyo3_ffi::PyCriticalSection_Begin(&mut guard.0, object) };
        f()
    }
    #[cfg(not(Py_GIL_DISABLED))]
    {
        f()
    }
}

