use pyo3::exceptions::{PyRuntimeError, PyTypeError};
use pyo3::prelude::*;
use pyo3::types::PyFunction;
use pyo3_ffi::*;
use std::ptr;

use crate::types::PyObjectPtr;

const CAPSULE_NAME: *const std::ffi::c_char = b"copium._original_vectorcall\0".as_ptr().cast();

// ══════════════════════════════════════════════════════════════
//  3.12+: swap the vectorcall slot on the function object
// ══════════════════════════════════════════════════════════════

#[cfg(Py_3_12)]
pub(crate) unsafe extern "C" fn copium_deepcopy_vectorcall(
    _callable: *mut PyObject,
    args: *const *mut PyObject,
    nargsf: usize,
    kwnames: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        crate::py_deepcopy(
            ptr::null_mut(),
            args,
            crate::ffi_ext::PyVectorcall_NARGS(nargsf),
            kwnames,
        )
    }
}

#[cfg(Py_3_12)]
unsafe fn is_patched(fn_ptr: *mut PyObject) -> bool {
    unsafe {
        crate::ffi_ext::PyVectorcall_Function(fn_ptr)
            .map(|f| f as usize == copium_deepcopy_vectorcall as *const () as usize)
            .unwrap_or(false)
    }
}

#[cfg(Py_3_12)]
unsafe fn apply_patch(_py: Python<'_>, fn_ptr: *mut PyObject, target: *mut PyObject) -> i32 {
    unsafe {
        let original_vc = match crate::ffi_ext::PyVectorcall_Function(fn_ptr) {
            Some(f) => f,
            None => {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    crate::cstr!("copium.patch: function has no vectorcall slot"),
                );
                return -1;
            }
        };

        let capsule = PyCapsule_New(
            std::mem::transmute::<vectorcallfunc, *mut std::ffi::c_void>(original_vc),
            CAPSULE_NAME,
            None,
        );
        if capsule.is_null() {
            return -1;
        }

        if PyObject_SetAttrString(fn_ptr, crate::cstr!("__copium_original__"), capsule) < 0 {
            capsule.decref();
            return -1;
        }
        capsule.decref();

        if PyObject_SetAttrString(fn_ptr, crate::cstr!("__wrapped__"), target) < 0 {
            PyObject_DelAttrString(fn_ptr, crate::cstr!("__copium_original__"));
            PyErr_Clear();
            return -1;
        }

        crate::ffi_ext::PyFunction_SetVectorcall(fn_ptr, copium_deepcopy_vectorcall);
        1
    }
}

#[cfg(Py_3_12)]
unsafe fn unapply_patch(_py: Python<'_>, fn_ptr: *mut PyObject) -> i32 {
    unsafe {
        let capsule = PyObject_GetAttrString(fn_ptr, crate::cstr!("__copium_original__"));
        if capsule.is_null() {
            PyErr_Clear();
            PyErr_SetString(PyExc_RuntimeError, crate::cstr!("copium.patch: not applied"));
            return -1;
        }

        let raw = PyCapsule_GetPointer(capsule, CAPSULE_NAME);
        capsule.decref();
        if raw.is_null() {
            return -1;
        }
        let original_vc = std::mem::transmute::<*mut std::ffi::c_void, vectorcallfunc>(raw);

        crate::ffi_ext::PyFunction_SetVectorcall(fn_ptr, original_vc);

        PyObject_DelAttrString(fn_ptr, crate::cstr!("__copium_original__"));
        PyErr_Clear();
        PyObject_DelAttrString(fn_ptr, crate::cstr!("__wrapped__"));
        PyErr_Clear();

        0
    }
}

// ══════════════════════════════════════════════════════════════
//  <3.12: swap __code__ on the function object
// ══════════════════════════════════════════════════════════════

#[cfg(not(Py_3_12))]
static mut G_TEMPLATE_CODE: *mut PyObject = ptr::null_mut();

#[cfg(not(Py_3_12))]
unsafe fn is_patched(fn_ptr: *mut PyObject) -> bool {
    unsafe { PyObject_HasAttrString(fn_ptr, crate::cstr!("__copium_original__")) != 0 }
}

#[cfg(not(Py_3_12))]
unsafe fn init_template() -> i32 {
    unsafe {
        if !G_TEMPLATE_CODE.is_null() {
            return 0;
        }

        let src = b"def deepcopy(x, memo=None, _nil=[]):\n    return \"copium.deepcopy\"(x, memo)\n\0";

        let globals = PyDict_New();
        if globals.is_null() {
            return -1;
        }

        let builtins = PyEval_GetBuiltins();
        if !builtins.is_null()
            && PyDict_SetItemString(globals, crate::cstr!("__builtins__"), builtins) < 0
        {
            globals.decref();
            return -1;
        }

        let warnings = PyImport_ImportModule(crate::cstr!("warnings"));
        let mut filters_copy: *mut PyObject = ptr::null_mut();

        if !warnings.is_null() {
            let old_filters = PyObject_GetAttrString(warnings, crate::cstr!("filters"));
            if !old_filters.is_null() {
                filters_copy = PySequence_List(old_filters);
                old_filters.decref();
            } else {
                PyErr_Clear();
            }
            let ignore = PyObject_CallMethod(
                warnings,
                crate::cstr!("simplefilter"),
                crate::cstr!("sO"),
                crate::cstr!("ignore"),
                PyExc_SyntaxWarning,
            );
            if !ignore.is_null() {
                ignore.decref();
            } else {
                PyErr_Clear();
            }
        } else {
            PyErr_Clear();
        }

        let res = PyRun_StringFlags(
            src.as_ptr().cast(),
            Py_file_input,
            globals,
            globals,
            ptr::null_mut(),
        );

        if !warnings.is_null() && !filters_copy.is_null() {
            if PyObject_SetAttrString(warnings, crate::cstr!("filters"), filters_copy) < 0 {
                PyErr_Clear();
            }
        }
        filters_copy.decref_nullable();
        warnings.decref_nullable();

        if res.is_null() {
            globals.decref();
            return -1;
        }
        res.decref();

        let fn_obj = PyDict_GetItemString(globals, crate::cstr!("deepcopy"));
        if fn_obj.is_null() {
            globals.decref();
            PyErr_SetString(
                PyExc_RuntimeError,
                crate::cstr!("copium.patch: template creation failed"),
            );
            return -1;
        }

        G_TEMPLATE_CODE = PyObject_GetAttrString(fn_obj, crate::cstr!("__code__"));
        globals.decref();
        if G_TEMPLATE_CODE.is_null() { -1 } else { 0 }
    }
}

#[cfg(not(Py_3_12))]
unsafe fn build_patched_code(target: *mut PyObject) -> *mut PyObject {
    unsafe {
        let template_consts =
            PyObject_GetAttrString(G_TEMPLATE_CODE, crate::cstr!("co_consts"));
        if template_consts.is_null() {
            return ptr::null_mut();
        }

        let n = PyTuple_Size(template_consts);
        if n < 0 {
            template_consts.decref();
            return ptr::null_mut();
        }

        let mut sentinel_idx: Py_ssize_t = -1;
        for i in 0..n {
            let item = PyTuple_GetItem(template_consts, i);
            if !item.is_null()
                && PyUnicode_Check(item) != 0
                && PyUnicode_CompareWithASCIIString(item, crate::cstr!("copium.deepcopy")) == 0
            {
                sentinel_idx = i;
                break;
            }
        }

        if sentinel_idx < 0 {
            template_consts.decref();
            PyErr_SetString(
                PyExc_RuntimeError,
                crate::cstr!("copium.patch: sentinel not found"),
            );
            return ptr::null_mut();
        }

        let new_consts = PyList_New(n);
        if new_consts.is_null() {
            template_consts.decref();
            return ptr::null_mut();
        }

        for j in 0..n {
            let item = if j == sentinel_idx {
                target
            } else {
                PyTuple_GetItem(template_consts, j)
            };
            if item.is_null() {
                new_consts.decref();
                template_consts.decref();
                return ptr::null_mut();
            }
            if PyList_SetItem(new_consts, j, item.newref()) < 0 {
                new_consts.decref();
                template_consts.decref();
                return ptr::null_mut();
            }
        }
        template_consts.decref();

        let consts_tuple = PyList_AsTuple(new_consts);
        new_consts.decref();
        if consts_tuple.is_null() {
            return ptr::null_mut();
        }

        let replace = PyObject_GetAttrString(G_TEMPLATE_CODE, crate::cstr!("replace"));
        if replace.is_null() {
            consts_tuple.decref();
            return ptr::null_mut();
        }

        let kwargs = PyDict_New();
        if kwargs.is_null() {
            replace.decref();
            consts_tuple.decref();
            return ptr::null_mut();
        }
        if PyDict_SetItemString(kwargs, crate::cstr!("co_consts"), consts_tuple) < 0 {
            kwargs.decref();
            replace.decref();
            consts_tuple.decref();
            return ptr::null_mut();
        }
        consts_tuple.decref();

        let empty = PyTuple_New(0);
        if empty.is_null() {
            kwargs.decref();
            replace.decref();
            return ptr::null_mut();
        }
        let new_code = PyObject_Call(replace, empty, kwargs);
        empty.decref();
        replace.decref();
        kwargs.decref();
        new_code
    }
}

#[cfg(not(Py_3_12))]
unsafe fn cleanup_patch_attrs(fn_ptr: *mut PyObject) {
    unsafe {
        PyObject_DelAttrString(fn_ptr, crate::cstr!("__copium_original__"));
        PyErr_Clear();
        PyObject_DelAttrString(fn_ptr, crate::cstr!("__wrapped__"));
        PyErr_Clear();
    }
}

#[cfg(not(Py_3_12))]
unsafe fn apply_patch(_py: Python<'_>, fn_ptr: *mut PyObject, target: *mut PyObject) -> i32 {
    unsafe {
        if init_template() < 0 {
            return -1;
        }

        let current_code = PyObject_GetAttrString(fn_ptr, crate::cstr!("__code__"));
        if current_code.is_null() {
            return -1;
        }

        if PyObject_SetAttrString(fn_ptr, crate::cstr!("__copium_original__"), current_code) < 0 {
            current_code.decref();
            return -1;
        }
        current_code.decref();

        if PyObject_SetAttrString(fn_ptr, crate::cstr!("__wrapped__"), target) < 0 {
            cleanup_patch_attrs(fn_ptr);
            return -1;
        }

        let new_code = build_patched_code(target);
        if new_code.is_null() {
            cleanup_patch_attrs(fn_ptr);
            return -1;
        }

        if PyObject_SetAttrString(fn_ptr, crate::cstr!("__code__"), new_code) < 0 {
            new_code.decref();
            cleanup_patch_attrs(fn_ptr);
            return -1;
        }
        new_code.decref();
        1
    }
}

#[cfg(not(Py_3_12))]
unsafe fn unapply_patch(_py: Python<'_>, fn_ptr: *mut PyObject) -> i32 {
    unsafe {
        let original_code =
            PyObject_GetAttrString(fn_ptr, crate::cstr!("__copium_original__"));
        if original_code.is_null() {
            PyErr_Clear();
            PyErr_SetString(PyExc_RuntimeError, crate::cstr!("copium.patch: not applied"));
            return -1;
        }

        if PyObject_SetAttrString(fn_ptr, crate::cstr!("__code__"), original_code) < 0 {
            original_code.decref();
            return -1;
        }
        original_code.decref();

        cleanup_patch_attrs(fn_ptr);
        0
    }
}

// ══════════════════════════════════════════════════════════════
//  PyO3 wrappers — cold path, boilerplate handled by macros
// ══════════════════════════════════════════════════════════════

fn require_py_function<'py>(obj: Bound<'py, PyAny>) -> PyResult<Bound<'py, PyAny>> {
    if !obj.is_instance_of::<PyFunction>() {
        return Err(PyTypeError::new_err("copy.deepcopy is not a Python function"));
    }
    Ok(obj)
}

fn take_py_err(py: Python<'_>) -> PyErr {
    PyErr::take(py)
        .unwrap_or_else(|| PyRuntimeError::new_err("unexpected null error state"))
}

#[pyfunction]
fn enable(py: Python<'_>) -> PyResult<bool> {
    let copy_mod = py.import("copy")?;
    let stdlib_dc = require_py_function(copy_mod.getattr("deepcopy")?)?;
    let fn_ptr = stdlib_dc.as_ptr();

    unsafe {
        if is_patched(fn_ptr) {
            return Ok(false);
        }
        let copium_mod = py.import("copium")?;
        let target = copium_mod.getattr("deepcopy")?;
        match apply_patch(py, fn_ptr, target.as_ptr()) {
            r if r >= 0 => Ok(true),
            _ => Err(take_py_err(py)),
        }
    }
}

#[pyfunction]
fn disable(py: Python<'_>) -> PyResult<bool> {
    let copy_mod = py.import("copy")?;
    let stdlib_dc = require_py_function(copy_mod.getattr("deepcopy")?)?;
    let fn_ptr = stdlib_dc.as_ptr();

    unsafe {
        if !is_patched(fn_ptr) {
            return Ok(false);
        }
        match unapply_patch(py, fn_ptr) {
            0 => Ok(true),
            _ => Err(take_py_err(py)),
        }
    }
}

#[pyfunction]
fn enabled(py: Python<'_>) -> PyResult<bool> {
    let copy_mod = py.import("copy")?;
    let stdlib_dc = require_py_function(copy_mod.getattr("deepcopy")?)?;
    Ok(unsafe { is_patched(stdlib_dc.as_ptr()) })
}

// ══════════════════════════════════════════════════════════════
//  Submodule registration
// ══════════════════════════════════════════════════════════════

pub unsafe fn create_module(parent: *mut PyObject) -> i32 {
    let py = unsafe { Python::assume_attached() };
    let result: PyResult<()> = (|| {
        let m = pyo3::types::PyModule::new(py, "patch")?;
        m.add_function(wrap_pyfunction!(enable, &m)?)?;
        m.add_function(wrap_pyfunction!(disable, &m)?)?;
        m.add_function(wrap_pyfunction!(enabled, &m)?)?;
        let ptr = m.into_ptr();
        if unsafe { crate::add_submodule(parent, crate::cstr!("patch"), ptr) } < 0 {
            return Err(PyErr::take(py)
                .unwrap_or_else(|| PyRuntimeError::new_err("add_submodule failed")));
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
