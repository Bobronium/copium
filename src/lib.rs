//! Copium - Fast deepcopy implementation in Rust
//! Rewrite with type-level state management and minimal PyO3 overhead

mod deepcopy;
mod extra;
mod ffi;
mod keepalive;
mod memo;
mod patching;
mod pinning;
mod proxy;
mod reconstructor;
mod types;

use deepcopy::{cleanup_after_call, deepcopy_impl};
use pyo3::prelude::*;
use pyo3::types::PyModule;

/// Shallow copy
#[pyfunction]
fn copy(py: Python, obj: &PyAny) -> PyResult<PyObject> {
    // Shallow copy: just call copy.copy from stdlib
    let copy_module = PyModule::import(py, "copy")?;
    let copy_fn = copy_module.getattr("copy")?;
    copy_fn.call1((obj,))
}

/// Deep copy with optional memo
#[pyfunction]
#[pyo3(signature = (x, memo=None))]
fn deepcopy(py: Python, x: &PyAny, memo: Option<&PyAny>) -> PyResult<PyObject> {
    let result = deepcopy_impl(py, x, memo);
    cleanup_after_call();
    result
}

/// Replace (Python 3.13+)
#[pyfunction]
#[pyo3(signature = (obj, **changes))]
fn replace(py: Python, obj: &PyAny, changes: Option<&PyAny>) -> PyResult<PyObject> {
    // Use dataclasses.replace
    let dataclasses = PyModule::import(py, "dataclasses")?;
    let replace_fn = dataclasses.getattr("replace")?;

    if let Some(changes) = changes {
        replace_fn.call((obj,), Some(changes))
    } else {
        replace_fn.call1((obj,))
    }
}

/// Main module
#[pymodule]
fn copium(py: Python, m: &PyModule) -> PyResult<()> {
    // Main functions
    m.add_function(wrap_pyfunction!(copy, m)?)?;
    m.add_function(wrap_pyfunction!(deepcopy, m)?)?;
    m.add_function(wrap_pyfunction!(replace, m)?)?;

    // Add Memo and KeepList proxies
    m.add_class::<proxy::MemoProxy>()?;
    m.add_class::<proxy::KeepListProxy>()?;

    // Extra submodule
    let extra = PyModule::new(py, "extra")?;
    extra.add_function(wrap_pyfunction!(extra::replicate, extra)?)?;
    extra.add_function(wrap_pyfunction!(extra::repeatcall, extra)?)?;
    m.add_submodule(extra)?;

    // Patch submodule
    let patch = PyModule::new(py, "patch")?;
    patch.add_function(wrap_pyfunction!(patching::apply, patch)?)?;
    patch.add_function(wrap_pyfunction!(patching::unapply, patch)?)?;
    patch.add_function(wrap_pyfunction!(patching::applied, patch)?)?;
    patch.add_function(wrap_pyfunction!(patching::get_vectorcall_ptr, patch)?)?;

    // Add enable/disable/enabled (these will be implemented in Python wrapper)
    m.add_submodule(patch)?;

    // Experimental pinning submodule
    let experimental = PyModule::new(py, "_experimental")?;
    experimental.add_function(wrap_pyfunction!(pinning::pin, experimental)?)?;
    experimental.add_function(wrap_pyfunction!(pinning::unpin, experimental)?)?;
    experimental.add_function(wrap_pyfunction!(pinning::pinned, experimental)?)?;
    experimental.add_function(wrap_pyfunction!(pinning::clear_pins, experimental)?)?;
    experimental.add_function(wrap_pyfunction!(pinning::get_pins, experimental)?)?;
    m.add_submodule(experimental)?;

    // Version info
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;

    Ok(())
}
