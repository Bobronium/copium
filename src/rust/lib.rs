//! Copium: Ultra-fast deepcopy implementation in Rust

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

mod ffi;
mod memo;
mod memo_trait;
mod keepalive;
mod proxy;
mod state;
mod user_memo;
mod deepcopy_impl;
mod dispatch;
mod containers;
mod reduce;
mod types;
mod patching;

use pyo3::prelude::*;
use pyo3::types::PyModule;

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

/// Batch replication (for copium.extra)
#[pyfunction]
#[pyo3(signature = (obj, n, compile_after = 20), name = "replicate")]
fn extra_replicate(
    obj: &Bound<'_, PyAny>,
    n: usize,
    compile_after: usize,
) -> PyResult<Vec<Py<PyAny>>> {
    deepcopy_impl::replicate_impl(obj, n, compile_after)
}

/// Call function n times
#[pyfunction]
fn extra_repeatcall(func: &Bound<'_, PyAny>, n: usize) -> PyResult<Vec<Py<PyAny>>> {
    let mut results = Vec::with_capacity(n);
    for _ in 0..n {
        let result = func.call0()?;
        results.push(result.unbind());
    }
    Ok(results)
}

/// Python module initialization
#[pymodule]
fn copium(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(deepcopy, m)?)?;
    m.add_function(wrap_pyfunction!(copy, m)?)?;
    m.add_function(wrap_pyfunction!(replicate, m)?)?;

    // Import copy.Error and add it to our module
    let py = m.py();
    let copy_module = PyModule::import_bound(py, "copy")?;
    let error = copy_module.getattr("Error")?;
    m.add("Error", error)?;

    // Add patching functions directly to copium module
    m.add_function(wrap_pyfunction!(patching::apply, m)?)?;
    m.add_function(wrap_pyfunction!(patching::unapply, m)?)?;
    m.add_function(wrap_pyfunction!(patching::applied, m)?)?;

    // Create copium.patch submodule
    let patch_module = PyModule::new_bound(py, "patch")?;
    patch_module.add_function(wrap_pyfunction!(patching::apply, &patch_module)?)?;
    patch_module.add_function(wrap_pyfunction!(patching::unapply, &patch_module)?)?;
    patch_module.add_function(wrap_pyfunction!(patching::applied, &patch_module)?)?;
    patch_module.add_function(wrap_pyfunction!(patching::enable, &patch_module)?)?;
    patch_module.add_function(wrap_pyfunction!(patching::disable, &patch_module)?)?;
    patch_module.add_function(wrap_pyfunction!(patching::enabled, &patch_module)?)?;
    m.add_submodule(&patch_module)?;
    py.import_bound("sys")?
        .getattr("modules")?
        .set_item("copium.patch", patch_module)?;

    // Create copium.extra submodule
    let extra_module = PyModule::new_bound(py, "extra")?;
    extra_module.add_function(wrap_pyfunction!(replicate, &extra_module)?)?;
    extra_module.add_function(wrap_pyfunction!(extra_replicate, &extra_module)?)?;
    extra_module.add_function(wrap_pyfunction!(extra_repeatcall, &extra_module)?)?;
    m.add_submodule(&extra_module)?;
    py.import_bound("sys")?
        .getattr("modules")?
        .set_item("copium.extra", extra_module)?;

    Ok(())
}
