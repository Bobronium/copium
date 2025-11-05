//! Copium: Ultra-fast deepcopy implementation in Rust with thread-local state management
//!
//! Simplified to match C implementation pattern:
//! - Thread-local memo reused across calls  
//! - Detaches if Python holds reference (refcount > 1)
//! - Single hash computation per object
//! - Direct FFI for hot paths

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

mod ffi;
mod memo;
mod keepalive;
mod proxy;
mod state;
mod deepcopy_impl;
mod dispatch;
mod containers;
mod reduce;
mod types;

use pyo3::prelude::*;
use pyo3::types::PyModule as PyModuleType;

/// Main entry point for deepcopy
#[pyfunction]
#[pyo3(signature = (obj, memo = None))]
fn deepcopy(obj: &Bound<'_, PyAny>, memo: Option<&Bound<'_, PyAny>>) -> PyResult<Py<PyAny>> {
    deepcopy_impl::deepcopy_impl(obj, memo)
}

/// Shallow copy implementation
#[pyfunction]
fn copy(obj: &Bound<'_, PyAny>) -> PyResult<Py<PyAny>> {
    deepcopy_impl::copy_impl(obj)
}

/// Batch replication with optimization
#[pyfunction]
#[pyo3(signature = (obj, n, compile_after = 20))]
fn replicate(
    obj: &Bound<'_, PyAny>,
    n: usize,
    compile_after: usize,
) -> PyResult<Vec<Py<PyAny>>> {
    deepcopy_impl::replicate_impl(obj, n, compile_after)
}

/// Python module initialization
#[pymodule]
fn copium(m: &Bound<'_, PyModuleType>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(deepcopy, m)?)?;
    m.add_function(wrap_pyfunction!(copy, m)?)?;
    m.add_function(wrap_pyfunction!(replicate, m)?)?;

    // Add submodules
    let extra = PyModuleType::new(m.py(), "extra")?;
    extra.add_function(wrap_pyfunction!(replicate, m)?)?;
    m.add_submodule(&extra)?;

    Ok(())
}
