//! Copium: Ultra-fast deepcopy implementation in Rust with compile-time state management
//!
//! This implementation moves state management to compile-time using Rust's type system,
//! ensuring zero redundant operations and maximum performance.

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

mod ffi;
mod memo;
mod keepalive;
mod proxy;
mod state;
mod deepcopy;
mod dispatch;
mod containers;
mod reduce;
mod types;

use pyo3::prelude::*;
use state::ThreadState;

/// Main entry point for deepcopy
#[pyfunction]
#[pyo3(signature = (obj, memo = None))]
fn deepcopy(obj: &Bound<'_, PyAny>, memo: Option<&Bound<'_, PyAny>>) -> PyResult<PyObject> {
    ThreadState::with_state(|state| {
        deepcopy::deepcopy_impl(obj, memo, state)
    })
}

/// Shallow copy implementation
#[pyfunction]
fn copy(obj: &Bound<'_, PyAny>) -> PyResult<PyObject> {
    // Shallow copy via __copy__ or reduce protocol
    deepcopy::copy_impl(obj)
}

/// Batch replication with optimization
#[pyfunction]
#[pyo3(signature = (obj, n, compile_after = 20))]
fn replicate(
    obj: &Bound<'_, PyAny>,
    n: usize,
    compile_after: usize,
) -> PyResult<Vec<PyObject>> {
    deepcopy::replicate_impl(obj, n, compile_after)
}

/// Python module initialization
#[pymodule]
fn copium(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(deepcopy, m)?)?;
    m.add_function(wrap_pyfunction!(copy, m)?)?;
    m.add_function(wrap_pyfunction!(replicate, m)?)?;

    // Add submodules
    let extra = PyModule::new(m.py(), "extra")?;
    extra.add_function(wrap_pyfunction!(replicate, m)?)?;
    m.add_submodule(&extra)?;

    Ok(())
}
