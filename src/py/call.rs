#![allow(unused_macros)]

use std::ffi::CStr;
use std::os::raw::c_char;

pub trait IntoPythonCallArgument {
    type ForeignType;

    fn into_python_call_argument(self) -> Self::ForeignType;
}

impl IntoPythonCallArgument for &CStr {
    type ForeignType = *const c_char;

    #[inline(always)]
    fn into_python_call_argument(self) -> Self::ForeignType {
        self.as_ptr()
    }
}

impl<T> IntoPythonCallArgument for *mut T {
    type ForeignType = *mut T;

    #[inline(always)]
    fn into_python_call_argument(self) -> Self::ForeignType {
        self
    }
}

impl<T> IntoPythonCallArgument for *const T {
    type ForeignType = *const T;

    #[inline(always)]
    fn into_python_call_argument(self) -> Self::ForeignType {
        self
    }
}

macro_rules! impl_into_python_call_argument_for_numbers {
    ($($number_type:ty),+ $(,)?) => {
        $(
            impl IntoPythonCallArgument for $number_type {
                type ForeignType = $number_type;

                #[inline(always)]
                fn into_python_call_argument(self) -> Self::ForeignType {
                    self
                }
            }
        )+
    };
}

impl_into_python_call_argument_for_numbers!(
    i8,
    u8,
    i16,
    u16,
    i32,
    u32,
    i64,
    u64,
    isize,
    usize,
    f32,
    f64,
);

macro_rules! call_function {
    ($callable:expr, $format_string:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyObject_CallFunction(
                ($callable) as *mut $crate::py::PyObject,
                ($format_string).as_ptr()
                $(, $crate::py::call::IntoPythonCallArgument::into_python_call_argument($argument) )*
            )
        }
    }};
}

pub(crate) use call_function;

macro_rules! call_function_obj_args {
    ($callable:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyObject_CallFunctionObjArgs(
                ($callable) as *mut $crate::py::PyObject
                $(, $crate::py::call::IntoPythonCallArgument::into_python_call_argument($argument) )*
                , std::ptr::null_mut::<$crate::py::PyObject>()
            )
        }
    }};
}

pub(crate) use call_function_obj_args;

macro_rules! call_method_obj_args {
    ($callable:expr, $name:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyObject_CallMethodObjArgs(
                ($callable) as *mut $crate::py::PyObject,
                $crate::py::call::IntoPythonCallArgument::into_python_call_argument($name)
                    as *mut $crate::py::PyObject
                $(, $crate::py::call::IntoPythonCallArgument::into_python_call_argument($argument) )*
                , std::ptr::null_mut::<$crate::py::PyObject>()
            )
        }
    }};
}

pub(crate) use call_method_obj_args;
pub(crate) use call_function as function;
pub(crate) use call_function_obj_args as function_obj_args;
pub(crate) use call_method_obj_args as method_obj_args;
