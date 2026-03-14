use pyo3_ffi::*;
use std::ptr;

use crate::deepcopy;
use crate::types::{PyObjectPtr, PyTypeObjectPtr};

unsafe extern "C" fn py_replicate(
    _self: *mut PyObject,
    args: *const *mut PyObject,
    nargs: Py_ssize_t,
    kwnames: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        if nargs != 2 {
            PyErr_SetString(PyExc_TypeError, crate::cstr!("replicate(obj, n, /)"));
            return ptr::null_mut();
        }
        if !kwnames.is_null() && PyTuple_Size(kwnames) > 0 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("replicate() does not accept keyword arguments"),
            );
            return ptr::null_mut();
        }

        let obj = *args;
        let n = PyLong_AsLong(*args.add(1));
        if n == -1 && !PyErr_Occurred().is_null() {
            return ptr::null_mut();
        }
        if n < 0 {
            PyErr_SetString(PyExc_ValueError, crate::cstr!("n must be >= 0"));
            return ptr::null_mut();
        }

        if n == 0 {
            return PyList_New(0);
        }

        let type_pointer = obj.class();
        if type_pointer.is_atomic_immutable() {
            let out = PyList_New(n as Py_ssize_t);
            if out.is_null() {
                return ptr::null_mut();
            }
            for i in 0..n as Py_ssize_t {
                PyList_SET_ITEM(out, i, obj.newref());
            }
            return out;
        }

        let out = PyList_New(n as Py_ssize_t);
        if out.is_null() {
            return ptr::null_mut();
        }

        for i in 0..n as Py_ssize_t {
            let memo = crate::memo::pymemo_alloc();
            let copy = deepcopy::deepcopy(obj, &mut *memo);
            (*memo).reset();
            if copy.is_error() {
                out.decref();
                return ptr::null_mut();
            }
            PyList_SetItem(out, i, copy.into_raw());
        }
        out
    }
}

unsafe extern "C" fn py_repeatcall(
    _self: *mut PyObject,
    args: *const *mut PyObject,
    nargs: Py_ssize_t,
    kwnames: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        if nargs != 2 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("repeatcall(function, size, /)"),
            );
            return ptr::null_mut();
        }
        if !kwnames.is_null() && PyTuple_Size(kwnames) > 0 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("repeatcall() takes no keyword arguments"),
            );
            return ptr::null_mut();
        }

        let func = *args;
        if PyCallable_Check(func) == 0 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("function must be callable"),
            );
            return ptr::null_mut();
        }

        let n = PyLong_AsLong(*args.add(1));
        if n == -1 && !PyErr_Occurred().is_null() {
            return ptr::null_mut();
        }
        if n < 0 {
            PyErr_SetString(PyExc_ValueError, crate::cstr!("size must be >= 0"));
            return ptr::null_mut();
        }

        let out = PyList_New(n as Py_ssize_t);
        if out.is_null() {
            return ptr::null_mut();
        }

        for i in 0..n as Py_ssize_t {
            let item = PyObject_CallNoArgs(func);
            if item.is_null() {
                out.decref();
                return ptr::null_mut();
            }
            PyList_SetItem(out, i, item);
        }
        out
    }
}

static mut EXTRA_METHODS: [PyMethodDef; 3] = [PyMethodDef::zeroed(); 3];

static mut EXTRA_MODULE_DEF: PyModuleDef = PyModuleDef {
    m_base: PyModuleDef_HEAD_INIT,
    m_name: b"orcopium.extra\0".as_ptr().cast(),
    m_doc: b"Batch copying utilities for orcopium.\0".as_ptr().cast(),
    m_size: -1,
    m_methods: ptr::null_mut(),
    m_slots: ptr::null_mut(),
    m_traverse: None,
    m_clear: None,
    m_free: None,
};

pub unsafe fn create_module(parent: *mut PyObject) -> i32 {
    unsafe {
        EXTRA_METHODS[0] = PyMethodDef {
            ml_name: crate::cstr!("replicate"),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFastWithKeywords: py_replicate,
            },
            ml_flags: METH_FASTCALL | METH_KEYWORDS,
            ml_doc: crate::cstr!("replicate(obj, n, /)\n--\n\nReturns n deep copies of the object in a list."),
        };
        EXTRA_METHODS[1] = PyMethodDef {
            ml_name: crate::cstr!("repeatcall"),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFastWithKeywords: py_repeatcall,
            },
            ml_flags: METH_FASTCALL | METH_KEYWORDS,
            ml_doc: crate::cstr!("repeatcall(function, size, /)\n--\n\nCall function repeatedly size times."),
        };
        EXTRA_METHODS[2] = PyMethodDef::zeroed();

        EXTRA_MODULE_DEF.m_methods = ptr::addr_of_mut!(EXTRA_METHODS).cast::<PyMethodDef>();

        let module = PyModule_Create(std::ptr::addr_of_mut!(EXTRA_MODULE_DEF));
        if module.is_null() {
            return -1;
        }

        crate::add_submodule(parent, crate::cstr!("extra"), module)
    }
}
