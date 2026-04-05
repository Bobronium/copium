use pyo3_ffi::*;
use std::ffi::CStr;
use std::os::raw::c_int;

use super::PyTypeInfo;

pub unsafe trait PyModulePtr {
    unsafe fn add_module_object<V: PyTypeInfo>(self, name: &CStr, object: *mut V) -> c_int;
    unsafe fn add_module_string_constant(self, name: &CStr, value: &CStr) -> c_int;
    unsafe fn module_name(self) -> *mut PyUnicodeObject;
}

unsafe impl<T: PyTypeInfo> PyModulePtr for *mut T {
    #[inline(always)]
    unsafe fn add_module_object<V: PyTypeInfo>(self, name: &CStr, object: *mut V) -> c_int {
        pyo3_ffi::PyModule_AddObject(
            self as *mut PyObject,
            name.as_ptr(),
            object as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn add_module_string_constant(self, name: &CStr, value: &CStr) -> c_int {
        pyo3_ffi::PyModule_AddStringConstant(self as *mut PyObject, name.as_ptr(), value.as_ptr())
    }

    #[inline(always)]
    unsafe fn module_name(self) -> *mut PyUnicodeObject {
        pyo3_ffi::PyModule_GetNameObject(self as *mut PyObject) as *mut PyUnicodeObject
    }
}

#[inline(always)]
pub unsafe fn create(definition: *mut PyModuleDef) -> *mut PyObject {
    pyo3_ffi::PyModule_Create(definition)
}

#[inline(always)]
pub unsafe fn def_init(definition: *mut PyModuleDef) -> *mut PyObject {
    pyo3_ffi::PyModuleDef_Init(definition)
}

#[inline(always)]
pub unsafe fn import(name: &CStr) -> *mut PyObject {
    pyo3_ffi::PyImport_ImportModule(name.as_ptr())
}

#[inline(always)]
pub unsafe fn get_module_dict() -> *mut PyDictObject {
    pyo3_ffi::PyImport_GetModuleDict() as *mut PyDictObject
}
