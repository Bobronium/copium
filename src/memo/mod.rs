mod any;
mod dict;
mod native;
mod pytype;
mod table;
mod tss;

use pyo3_ffi::*;
use std::ptr;

pub use any::AnyMemo;
pub use dict::DictMemo;
pub use native::PyMemoObject;
pub use pytype::{memo_ready_type, Memo_Type};
pub use table::{KeepaliveVec, MemoTable, UndoLog};
pub use tss::{cleanup_memo, get_memo, pymemo_alloc};

pub type MemoCheckpoint = usize;

pub trait Memo: Sized {
    type Probe;

    const RECALL_CAN_ERROR: bool;

    unsafe fn recall(&mut self, object: *mut PyObject) -> (Self::Probe, *mut PyObject);

    unsafe fn recall_probed(
        &mut self,
        object: *mut PyObject,
        probe: &Self::Probe,
    ) -> *mut PyObject {
        let _ = probe;
        self.recall(object).1
    }

    unsafe fn memoize(
        &mut self,
        original: *mut PyObject,
        copy: *mut PyObject,
        probe: &Self::Probe,
    ) -> i32;

    unsafe fn forget(&mut self, original: *mut PyObject, probe: &Self::Probe);

    unsafe fn as_call_arg(&mut self) -> *mut PyObject;

    #[inline(always)]
    unsafe fn ensure_memo_is_valid(&mut self) -> i32 {
        0
    }

    #[inline(always)]
    unsafe fn checkpoint(&mut self) -> Option<MemoCheckpoint> {
        None
    }

    #[inline(always)]
    unsafe fn as_native_memo(&mut self) -> *mut PyMemoObject {
        ptr::null_mut()
    }
}
