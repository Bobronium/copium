use pyo3_ffi::*;
use std::ptr;

use crate::ffi_ext;
use crate::reduce;
use crate::state::STATE;
use crate::types::*;

pub unsafe fn copy(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let tp = Py_TYPE(obj);

        if tp.is_atomic_immutable() {
            return Py_NewRef(obj);
        }

        if PySlice_Check(obj) != 0 {
            return Py_NewRef(obj);
        }
        if PyFrozenSet_Check(obj) != 0 {
            return Py_NewRef(obj);
        }
        if PyType_IsSubtype(tp, std::ptr::addr_of_mut!(PyType_Type)) != 0 {
            return Py_NewRef(obj);
        }

        // try .copy() for dict/set/list/bytearray
        if tp == std::ptr::addr_of_mut!(PyDict_Type)
            || tp == std::ptr::addr_of_mut!(PySet_Type)
            || tp == std::ptr::addr_of_mut!(PyList_Type)
            || tp == std::ptr::addr_of_mut!(PyByteArray_Type)
        {
            let method = PyObject_GetAttrString(obj, crate::cstr!("copy"));
            if !method.is_null() {
                let out = PyObject_CallNoArgs(method);
                Py_DECREF(method);
                if !out.is_null() {
                    return out;
                }
                return ptr::null_mut();
            }
            PyErr_Clear();
        }

        // try __copy__
        {
            let s = &STATE;
            let cp = PyObject_GetAttrString(obj, crate::cstr!("__copy__"));
            if !cp.is_null() {
                let out = PyObject_CallNoArgs(cp);
                Py_DECREF(cp);
                return out;
            }
            PyErr_Clear();
        }

        // fallback to reduce protocol
        let result = reduce::call_reduce_method_preferring_ex(obj);
        if result.is_null() {
            return ptr::null_mut();
        }

        if PyUnicode_Check(result) != 0 || PyBytes_Check(result) != 0 {
            Py_DECREF(result);
            return Py_NewRef(obj);
        }

        if PyTuple_Check(result) == 0 {
            Py_DECREF(result);
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("__reduce__ must return a tuple or str"),
            );
            return ptr::null_mut();
        }

        let sz = PyTuple_Size(result);
        if sz < 2 || sz > 5 {
            Py_DECREF(result);
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("tuple returned by __reduce__ must contain 2 through 5 elements"),
            );
            return ptr::null_mut();
        }

        let constructor = PyTuple_GetItem(result, 0);
        let args = PyTuple_GetItem(result, 1);

        let out = if PyTuple_Size(args) == 0 {
            PyObject_CallNoArgs(constructor)
        } else {
            PyObject_Call(constructor, args, ptr::null_mut())
        };

        if out.is_null() {
            Py_DECREF(result);
            return ptr::null_mut();
        }

        // apply state if present
        if sz >= 3 {
            let state = PyTuple_GetItem(result, 2);
            if state != Py_None() {
                let setstate = PyObject_GetAttrString(out, crate::cstr!("__setstate__"));
                if !setstate.is_null() {
                    let r = PyObject_CallOneArg(setstate, state);
                    Py_DECREF(setstate);
                    if r.is_null() {
                        Py_DECREF(out);
                        Py_DECREF(result);
                        return ptr::null_mut();
                    }
                    Py_DECREF(r);
                } else {
                    PyErr_Clear();
                    // apply dict state
                    let obj_dict = PyObject_GetAttrString(out, crate::cstr!("__dict__"));
                    if !obj_dict.is_null() {
                        let update = PyObject_GetAttrString(obj_dict, crate::cstr!("update"));
                        Py_DECREF(obj_dict);
                        if !update.is_null() {
                            let r = PyObject_CallOneArg(update, state);
                            Py_DECREF(update);
                            if r.is_null() {
                                Py_DECREF(out);
                                Py_DECREF(result);
                                return ptr::null_mut();
                            }
                            Py_DECREF(r);
                        } else {
                            PyErr_Clear();
                        }
                    } else {
                        PyErr_Clear();
                    }
                }
            }
        }

        Py_DECREF(result);
        out
    }
}
