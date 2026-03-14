use pyo3::exceptions::{PyRuntimeError, PyTypeError, PyValueError};
use pyo3::prelude::*;
use pyo3::types::{PyAny, PyDict};
use pyo3_ffi::PyObject;

use crate::state::{MemoMode, OnIncompatible, STATE};
use crate::types::PyObjectPtr;

// ══════════════════════════════════════════════════════════════
//  copium.config.apply(**kwargs)
//
//  Called with no kwargs → reset from environment variables.
//  Called with kwargs    → update only the specified fields.
//
//  suppress_warnings: pass a sequence to set, empty sequence to clear,
//  omit to leave unchanged. (Passing None is treated as omit.)
// ══════════════════════════════════════════════════════════════

#[derive(Clone, Copy, Debug)]
enum PyMemoMode {
    Native,
    Dict,
}

impl<'py> FromPyObject<'py, 'py> for PyMemoMode {
    type Error = PyErr;

    fn extract(obj: Borrowed<'_, 'py, PyAny>) -> Result<Self, Self::Error> {
        let s = obj.extract::<&str>()?;
        match s {
            "native" => Ok(Self::Native),
            "dict" => Ok(Self::Dict),
            other => Err(PyValueError::new_err(format!(
                "memo must be 'native' or 'dict', got '{other}'"
            ))),
        }
    }
}

#[derive(Clone, Copy, Debug)]
enum PyOnIncompatible {
    Warn,
    Raise,
    Silent,
}

impl<'py> FromPyObject<'py, 'py> for PyOnIncompatible {
    type Error = PyErr;

    fn extract(obj: Borrowed<'_, 'py, PyAny>) -> Result<Self, Self::Error> {
        let s = obj.extract::<&str>()?;
        match s {
            "warn" => Ok(Self::Warn),
            "raise" => Ok(Self::Raise),
            "silent" => Ok(Self::Silent),
            other => Err(PyValueError::new_err(format!(
                "on_incompatible must be 'warn', 'raise', or 'silent', got '{other}'"
            ))),
        }
    }
}

//  copium.config.apply()
#[pyfunction]
#[pyo3(signature = (*, memo=None, on_incompatible=None, suppress_warnings=None))]
fn apply(
    py: Python<'_>,
    memo: Option<PyMemoMode>,
    on_incompatible: Option<PyOnIncompatible>,
    suppress_warnings: Option<Bound<'_, PyAny>>,
) -> PyResult<()> {
    if memo.is_none() && on_incompatible.is_none() && suppress_warnings.is_none() {
        if unsafe { crate::state::load_config_from_env() } < 0 {
            return Err(PyErr::take(py)
                .unwrap_or_else(|| PyRuntimeError::new_err("load_config_from_env failed")));
        }
        return Ok(());
    }

    let state = std::ptr::addr_of_mut!(STATE);
    let mut memo_is_dict = false;

    if let Some(memo) = memo {
        unsafe {
            (*state).memo_mode = match memo {
                PyMemoMode::Native => MemoMode::Native,
                PyMemoMode::Dict => {
                    memo_is_dict = true;
                    MemoMode::Dict
                }
            };
        }
    }

    if memo_is_dict && (on_incompatible.is_some() || suppress_warnings.is_some()) {
        return Err(PyTypeError::new_err(
            "when `memo='dict'`, `on_incompatible` and `suppress_warnings` are ambiguous: \
             remove them or use `memo='native'`",
        ));
    }

    if let Some(on_incompatible) = on_incompatible {
        unsafe {
            (*state).on_incompatible = match on_incompatible {
                PyOnIncompatible::Warn => OnIncompatible::Warn,
                PyOnIncompatible::Raise => OnIncompatible::Raise,
                PyOnIncompatible::Silent => OnIncompatible::Silent,
            };
        }
    }

    if let Some(suppress_warnings_object) = suppress_warnings {
        unsafe {
            let new_tuple = if suppress_warnings_object.is_none() {
                pyo3_ffi::PyTuple_New(0)
            } else {
                pyo3_ffi::PySequence_Tuple(suppress_warnings_object.as_ptr())
            };
            if new_tuple.is_null() {
                return Err(PyErr::take(py)
                    .unwrap_or_else(|| PyRuntimeError::new_err("PySequence_Tuple failed")));
            }

            let suppress_warning_count = pyo3_ffi::PyTuple_Size(new_tuple);
            for index in 0..suppress_warning_count {
                let item = pyo3_ffi::PyTuple_GetItem(new_tuple, index);
                if pyo3_ffi::PyUnicode_Check(item) == 0 {
                    let item_type_name = Bound::<PyAny>::from_borrowed_ptr(py, item)
                        .get_type()
                        .name()
                        .map(|name| name.to_string_lossy().into_owned())
                        .unwrap_or_else(|_| "object".to_owned());
                    new_tuple.decref();
                    return Err(PyTypeError::new_err(format!(
                        "on_incompatible[{index}] must be a 'str', got '{item_type_name}'"
                    )));
                }
            }

            if crate::state::update_suppress_warnings(new_tuple) < 0 {
                return Err(PyErr::take(py).unwrap_or_else(|| {
                    PyRuntimeError::new_err("update_suppress_warnings failed")
                }));
            }
        }
    }

    Ok(())
}

//  copium.config.get()
#[pyfunction]
fn get(py: Python<'_>) -> PyResult<Bound<'_, PyDict>> {
    let state_pointer = std::ptr::addr_of!(STATE);
    let memo_mode = unsafe { (*state_pointer).memo_mode };
    let on_incompatible = unsafe { (*state_pointer).on_incompatible };
    let ignored_errors = unsafe { (*state_pointer).ignored_errors };
    let dict = PyDict::new(py);

    dict.set_item(
        "memo",
        match memo_mode {
            MemoMode::Dict => "dict",
            MemoMode::Native => "native",
        },
    )?;

    dict.set_item(
        "on_incompatible",
        match on_incompatible {
            OnIncompatible::Raise => "raise",
            OnIncompatible::Silent => "silent",
            OnIncompatible::Warn => "warn",
        },
    )?;

    let sw = unsafe {
        if !ignored_errors.is_null() {
            ignored_errors.newref()
        } else {
            pyo3_ffi::PyTuple_New(0)
        }
    };
    let sw_obj = unsafe { Bound::from_owned_ptr(py, sw) }.cast_into::<pyo3::types::PyTuple>()?;
    dict.set_item("suppress_warnings", sw_obj)?;

    Ok(dict)
}

pub unsafe fn create_module(parent: *mut PyObject) -> i32 {
    let py = unsafe { Python::assume_attached() };
    let result: PyResult<()> = (|| {
        let m = pyo3::types::PyModule::new(py, "config")?;
        m.add_function(wrap_pyfunction!(apply, &m)?)?;
        m.add_function(wrap_pyfunction!(get, &m)?)?;
        let ptr = m.into_ptr();
        if unsafe { crate::add_submodule(parent, crate::cstr!("config"), ptr) } < 0 {
            return Err(
                PyErr::take(py).unwrap_or_else(|| PyRuntimeError::new_err("add_submodule failed"))
            );
        }
        Ok(())
    })();
    match result {
        Ok(()) => 0,
        Err(e) => {
            e.restore(py);
            -1
        }
    }
}
