#![allow(unused_macros)]

macro_rules! function {
    ($callable:expr, $format_string:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyObject_CallFunction(
                ($callable) as *mut ::pyo3_ffi::PyObject,
                ($format_string).as_ptr()
                $(, $argument )*
            )
        }
    }};
}

pub(crate) use function;

macro_rules! function_obj_args {
    ($callable:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyObject_CallFunctionObjArgs(
                ($callable) as *mut ::pyo3_ffi::PyObject
                $(, $argument )*
            )
        }
    }};
}

pub(crate) use function_obj_args;

macro_rules! method_obj_args {
    ($callable:expr, $name:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyObject_CallMethodObjArgs(
                ($callable) as *mut ::pyo3_ffi::PyObject,
                ($name) as *mut ::pyo3_ffi::PyObject
                $(, $argument )*
            )
        }
    }};
}

pub(crate) use method_obj_args;
