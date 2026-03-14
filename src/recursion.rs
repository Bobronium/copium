use pyo3_ffi::*;
use core::hint::{likely,unlikely};
use std::ptr;

const STACKCHECK_STRIDE: u32 = 32;
const STACK_SAFETY_MARGIN: usize = 256 * 1024;

#[thread_local]
static mut DEPTH: u32 = 0;

#[thread_local]
static mut STACK_LOW: *mut u8 = ptr::null_mut();

#[thread_local]
static mut STACK_INITED: i32 = 0;

unsafe fn init_stack_bounds() {
    unsafe {
        STACK_INITED = 1;
    }

    #[cfg(target_os = "macos")]
    unsafe {
        let t = libc::pthread_self();
        let sz = libc::pthread_get_stacksize_np(t);
        let base = libc::pthread_get_stackaddr_np(t);
        let high = base as *mut u8;
        let mut low = high.sub(sz);
        if sz > STACK_SAFETY_MARGIN {
            low = low.add(STACK_SAFETY_MARGIN);
        }
        STACK_LOW = low;
    }

    #[cfg(target_os = "linux")]
    unsafe {
        let mut attr = core::mem::MaybeUninit::<libc::pthread_attr_t>::uninit();
        if libc::pthread_getattr_np(libc::pthread_self(), attr.as_mut_ptr()) == 0 {
            let mut attr = attr.assume_init();
            let mut addr: *mut libc::c_void = ptr::null_mut();
            let mut sz: usize = 0;

            if libc::pthread_attr_getstack(&attr, &mut addr, &mut sz) == 0 && !addr.is_null() && sz != 0
            {
                let mut low = addr as *mut u8;
                if sz > STACK_SAFETY_MARGIN {
                    low = low.add(STACK_SAFETY_MARGIN);
                }
                STACK_LOW = low;
            }

            libc::pthread_attr_destroy(&mut attr);
        }
    }

    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::Foundation::HMODULE;
        use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};

        type GetStackLimitsFn = unsafe extern "system" fn(*mut usize, *mut usize);

        let kernel32_name: [u16; 13] = [
            b'k' as u16,
            b'e' as u16,
            b'r' as u16,
            b'n' as u16,
            b'e' as u16,
            b'l' as u16,
            b'3' as u16,
            b'2' as u16,
            b'.' as u16,
            b'd' as u16,
            b'l' as u16,
            b'l' as u16,
            0,
        ];

        let h_kernel32: HMODULE = GetModuleHandleW(kernel32_name.as_ptr());
        if !h_kernel32.is_null() {
            let proc = GetProcAddress(
                h_kernel32,
                b"GetCurrentThreadStackLimits\0".as_ptr(),
            );
            if let Some(raw) = proc {
                let fn_ptr: GetStackLimitsFn = core::mem::transmute(raw);
                let mut low: usize = 0;
                let mut high: usize = 0;
                fn_ptr(&mut low, &mut high);

                let sz = high.wrapping_sub(low);
                let mut lowc = low as *mut u8;
                if sz > STACK_SAFETY_MARGIN {
                    lowc = lowc.add(STACK_SAFETY_MARGIN);
                }
                STACK_LOW = lowc;
            }
        }
    }
    if STACK_LOW.is_null() {
        let probe = 0u8;
        let sp = (&probe as *const u8) as *mut u8;
        STACK_LOW = sp.sub(STACK_SAFETY_MARGIN);
}

#[inline(always)]
pub unsafe fn enter() -> i32 {
    let d = unsafe {
        DEPTH = DEPTH.wrapping_add(1);
        DEPTH
    };

    if likely(d < STACKCHECK_STRIDE) {
        return 0;
    }

    if unlikely((d & (STACKCHECK_STRIDE - 1)) == 0) {
        if unsafe { unlikely(STACK_INITED == 0) } {
            unsafe { init_stack_bounds() };
        }

        let sp_probe = 0u8;
        let sp = (&sp_probe as *const u8).cast_mut();
        if unlikely(sp <= unsafe { STACK_LOW }) {
            unsafe {
                DEPTH -= 1;
                PyErr_Format(
                    PyExc_RecursionError,
                    crate::cstr!("Stack overflow (depth %u) while deep copying an object"),
                    d,
                );
            }
            return -1;
        }
    }

    0
}

#[inline(always)]
pub fn leave() {
    unsafe {
        if DEPTH > 0 {
            DEPTH -= 1;
        }
    }
}
