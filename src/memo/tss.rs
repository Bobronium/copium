use std::ffi::c_void;
use std::hint::{likely, unlikely};
use std::ptr;

use super::native::PyMemoObject;
use super::pytype::Memo_Type;
use crate::py;
use crate::py::PyObjectPtr;

#[thread_local]
static mut TSS_MEMO: *mut PyMemoObject = ptr::null_mut();

pub unsafe fn pymemo_alloc() -> *mut PyMemoObject {
    unsafe {
        let memo = py::gc::new::<PyMemoObject>(ptr::addr_of_mut!(Memo_Type));
        if memo.is_null() {
            return ptr::null_mut();
        }

        (*memo).init_in_place();
        memo
    }
}

#[inline(always)]
pub unsafe fn get_memo() -> (*mut PyMemoObject, bool) {
    unsafe {
        let tss = TSS_MEMO;
        if unlikely(tss.is_null()) {
            let fresh = pymemo_alloc();
            if fresh.is_null() {
                return (ptr::null_mut(), false);
            }
            TSS_MEMO = fresh;
            return (fresh, true);
        }

        if likely(tss.refcount() == 1) {
            return (tss, true);
        }

        (pymemo_alloc(), false)
    }
}

#[inline(always)]
pub unsafe fn cleanup_memo(memo: *mut PyMemoObject, is_tss: bool) {
    unsafe {
        if likely(is_tss && memo.refcount() == 1) {
            (*memo).reset();
            return;
        }

        if is_tss {
            let fresh = pymemo_alloc();
            if fresh.is_null() {
                TSS_MEMO = ptr::null_mut();
            } else {
                TSS_MEMO = fresh;
            }
        }

        py::gc::track(memo as *mut c_void);
        memo.decref();
    }
}
