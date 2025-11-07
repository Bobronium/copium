//! Function patching (simplified stub)

use pyo3::prelude::*;

#[pyfunction]
pub fn apply(_func: &PyAny, _target: &PyAny) -> PyResult<()> {
    Err(pyo3::exceptions::PyNotImplementedError::new_err(
        "Patching not yet implemented in Rust version",
    ))
}

#[pyfunction]
pub fn unapply(_func: &PyAny) -> PyResult<()> {
    Err(pyo3::exceptions::PyNotImplementedError::new_err(
        "Patching not yet implemented in Rust version",
    ))
}

#[pyfunction]
pub fn applied(_func: &PyAny) -> PyResult<bool> {
    Ok(false)
}

#[pyfunction]
pub fn get_vectorcall_ptr(_func: &PyAny) -> PyResult<usize> {
    Err(pyo3::exceptions::PyNotImplementedError::new_err(
        "Patching not yet implemented in Rust version",
    ))
}
