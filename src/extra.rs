//! Extra utilities: replicate and repeatcall

use crate::deepcopy::deepcopy_impl;
use pyo3::prelude::*;

#[pyfunction]
#[pyo3(signature = (obj, n, *, compile_after=20))]
pub fn replicate(py: Python, obj: &PyAny, n: usize, compile_after: usize) -> PyResult<PyObject> {
    let list = pyo3::types::PyList::empty(py);

    for _ in 0..n {
        let copied = deepcopy_impl(py, obj, None)?;
        list.append(copied)?;
    }

    Ok(list.into())
}

#[pyfunction]
pub fn repeatcall(py: Python, function: &PyAny, size: usize) -> PyResult<PyObject> {
    let list = pyo3::types::PyList::empty(py);

    for _ in 0..size {
        let result = function.call0()?;
        list.append(result)?;
    }

    Ok(list.into())
}
