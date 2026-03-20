use super::{PyObject, PyTypeInfo};

#[cfg(Py_GIL_DISABLED)]
#[inline(always)]
pub unsafe fn begin<T: PyTypeInfo>(critical_section: &mut PyCriticalSection, object: *mut T) {
    pyo3_ffi::PyCriticalSection_Begin(critical_section, object as *mut PyObject)
}

#[cfg(Py_GIL_DISABLED)]
pub use pyo3_ffi::{PyCriticalSection, PyCriticalSection2};

#[cfg(not(Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn begin<T: PyTypeInfo>(critical_section: &mut (), object: *mut T) {
    let _ = critical_section;
    let _ = object;
}

#[cfg(Py_GIL_DISABLED)]
#[inline(always)]
pub unsafe fn end(critical_section: &mut PyCriticalSection) {
    pyo3_ffi::PyCriticalSection_End(critical_section)
}

#[cfg(not(Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn end(critical_section: &mut ()) {
    let _ = critical_section;
}

#[cfg(Py_GIL_DISABLED)]
struct CriticalSectionGuard(PyCriticalSection);

#[cfg(Py_GIL_DISABLED)]
impl Drop for CriticalSectionGuard {
    fn drop(&mut self) {
        unsafe {
            end(&mut self.0);
        }
    }
}

#[inline(always)]
pub fn enter<F, R, T: PyTypeInfo>(object: *mut T, f: F) -> R
where
    F: FnOnce() -> R,
{
    #[cfg(Py_GIL_DISABLED)]
    {
        let mut guard = CriticalSectionGuard(unsafe { std::mem::zeroed() });
        unsafe { begin(&mut guard.0, object) };
        f()
    }
    #[cfg(not(Py_GIL_DISABLED))]
    {
        let _ = object;
        f()
    }
}
