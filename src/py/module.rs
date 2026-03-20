use pyo3_ffi::*;
use std::ffi::CStr;
use std::os::raw::c_int;

use super::PyTypeInfo;

#[inline(always)]
pub unsafe fn create(definition: *mut PyModuleDef) -> *mut PyObject {
    pyo3_ffi::PyModule_Create(definition)
}

#[inline(always)]
pub unsafe fn def_init(definition: *mut PyModuleDef) -> *mut PyObject {
    pyo3_ffi::PyModuleDef_Init(definition)
}

#[inline(always)]
pub unsafe fn add_object<M: PyTypeInfo, V: PyTypeInfo>(
    module: *mut M,
    name: &CStr,
    object: *mut V,
) -> c_int {
    pyo3_ffi::PyModule_AddObject(module as *mut PyObject, name.as_ptr(), object as *mut PyObject)
}

#[inline(always)]
pub unsafe fn add_string_constant<M: PyTypeInfo>(
    module: *mut M,
    name: &CStr,
    value: &CStr,
) -> c_int {
    pyo3_ffi::PyModule_AddStringConstant(module as *mut PyObject, name.as_ptr(), value.as_ptr())
}

#[inline(always)]
pub unsafe fn get_name<M: PyTypeInfo>(module: *mut M) -> *mut PyUnicodeObject {
    pyo3_ffi::PyModule_GetNameObject(module as *mut PyObject) as *mut PyUnicodeObject
}

#[inline(always)]
pub unsafe fn import(name: &CStr) -> *mut PyObject {
    pyo3_ffi::PyImport_ImportModule(name.as_ptr())
}

#[inline(always)]
pub unsafe fn get_module_dict() -> *mut PyDictObject {
    pyo3_ffi::PyImport_GetModuleDict() as *mut PyDictObject
}
