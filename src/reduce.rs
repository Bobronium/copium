use std::os::raw::c_int;
use std::ptr;

use pyo3_ffi::*;

use crate::deepcopy;
use crate::ffi_ext;
use crate::memo::Memo;
use crate::state::STATE;
use crate::types::*;

macro_rules! bail {
    ($e:expr) => {{
        let p = $e;
        if p.is_null() {
            return ptr::null_mut();
        }
        p
    }};
}

// ── Error chaining ─────────────────────────────────────────

pub(crate) unsafe fn chain_type_error(msg: *mut PyObject) {
    unsafe {
        let mut cause_type: *mut PyObject = ptr::null_mut();
        let mut cause_val: *mut PyObject = ptr::null_mut();
        let mut cause_tb: *mut PyObject = ptr::null_mut();

        #[allow(deprecated)]
        PyErr_Fetch(&mut cause_type, &mut cause_val, &mut cause_tb);

        if !cause_val.is_null() {
            #[allow(deprecated)]
            PyErr_NormalizeException(&mut cause_type, &mut cause_val, &mut cause_tb);
        }

        let new_exc = PyObject_CallOneArg(PyExc_TypeError, msg);
        msg.decref();

        if new_exc.is_null() {
            #[allow(deprecated)]
            PyErr_Restore(cause_type, cause_val, cause_tb);
            return;
        }

        if !cause_val.is_null() {
            PyException_SetCause(cause_val, new_exc);
            #[allow(deprecated)]
            PyErr_Restore(cause_type, cause_val, cause_tb);
            return;
        }

        cause_type.decref_nullable();
        cause_tb.decref_nullable();

        PyErr_SetObject(PyExc_TypeError, new_exc);
        new_exc.decref();
    }
}

// ── Registry & reduce dispatch ─────────────────────────────

pub(crate) unsafe fn try_reduce_via_registry(
    obj: *mut PyObject,
    tp: *mut PyTypeObject,
) -> *mut PyObject {
    unsafe {
        let reducer = STATE.copyreg_dispatch.get_item(tp as *mut PyObject);
        if reducer.is_null() {
            return ptr::null_mut();
        }
        if PyCallable_Check(reducer) == 0 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("copyreg.dispatch_table value is not callable"),
            );
            return ptr::null_mut();
        }
        PyObject_CallOneArg(reducer, obj)
    }
}

pub(crate) unsafe fn call_reduce_method_preferring_ex(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let state_pointer = ptr::addr_of!(STATE);
        let reduce_ex_name = (*state_pointer).s_reduce_ex;
        let reduce_name = (*state_pointer).s_reduce;
        let copy_error = (*state_pointer).copy_error;
        let mut reduce_ex: *mut PyObject = ptr::null_mut();
        let has = obj.get_optional_attr(reduce_ex_name, &mut reduce_ex);
        if has > 0 {
            let four = PyLong_FromLong(4);
            let res = reduce_ex.call_one(four);
            four.decref();
            reduce_ex.decref();
            return res;
        }
        if has < 0 {
            return ptr::null_mut();
        }
        let mut reduce: *mut PyObject = ptr::null_mut();
        let has = obj.get_optional_attr(reduce_name, &mut reduce);
        if has > 0 {
            let res = reduce.call();
            reduce.decref();
            return res;
        }
        if has < 0 {
            return ptr::null_mut();
        }
        PyErr_SetString(
            copy_error,
            crate::cstr!("un(deep)copyable object (no reduce protocol)"),
        );
        ptr::null_mut()
    }
}

// ── Reduce result parsing ──────────────────────────────────

pub(crate) struct ReduceParts {
    pub(crate) callable: *mut PyObject,
    pub(crate) argtup: *mut PyObject,
    pub(crate) state: *mut PyObject,
    pub(crate) listitems: *mut PyObject,
    pub(crate) dictitems: *mut PyObject,
}

pub(crate) enum ReduceKind {
    Error,
    Tuple,
    String,
}

pub(crate) unsafe fn validate_reduce_tuple(
    reduce_result: *mut PyObject,
    reducing_type: *mut PyTypeObject,
) -> (ReduceKind, ReduceParts) {
    unsafe {
        let empty = ReduceParts {
            callable: ptr::null_mut(),
            argtup: ptr::null_mut(),
            state: ptr::null_mut(),
            listitems: ptr::null_mut(),
            dictitems: ptr::null_mut(),
        };

        if !reduce_result.is_tuple() {
            if reduce_result.is_unicode() || reduce_result.is_bytes() {
                return (ReduceKind::String, empty);
            }
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("__reduce__ must return a tuple or str"),
            );
            return (ReduceKind::Error, empty);
        }

        let tup = reduce_result as *mut PyTupleObject;
        let size = tup.length();
        if size < 2 || size > 5 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("tuple returned by __reduce__ must contain 2 through 5 elements"),
            );
            return (ReduceKind::Error, empty);
        }

        let callable = tup.get_borrowed_unchecked(0);
        let mut argtup = tup.get_borrowed_unchecked(1);
        let none = ffi_ext::Py_None();
        let state_raw = if size >= 3 {
            tup.get_borrowed_unchecked(2)
        } else {
            none
        };
        let list_raw = if size >= 4 {
            tup.get_borrowed_unchecked(3)
        } else {
            none
        };
        let dict_raw = if size == 5 {
            tup.get_borrowed_unchecked(4)
        } else {
            none
        };

        if !argtup.is_tuple() {
            let coerced = PySequence_Tuple(argtup);
            if coerced.is_null() {
                let msg = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "second element of the tuple returned by %s.__reduce__ must be a tuple, not %.200s"
                    ),
                    (*reducing_type).tp_name,
                    (*argtup.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg);
                }
                return (ReduceKind::Error, empty);
            }
            let old = argtup;
            tup.set_slot_steal_unchecked(1, coerced);
            old.decref();
            argtup = coerced;
        }

        (
            ReduceKind::Tuple,
            ReduceParts {
                callable,
                argtup,
                state: if state_raw == none {
                    ptr::null_mut()
                } else {
                    state_raw
                },
                listitems: if list_raw == none {
                    ptr::null_mut()
                } else {
                    list_raw
                },
                dictitems: if dict_raw == none {
                    ptr::null_mut()
                } else {
                    dict_raw
                },
            },
        )
    }
}

// ── Instance reconstruction ────────────────────────────────

unsafe fn call_tp_new(
    cls: *mut PyTypeObject,
    args: *mut PyObject,
    kwargs: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        match (*cls).tp_new {
            Some(f) => f(cls, args, kwargs),
            None => {
                ffi_ext::PyErr_Format(
                    PyExc_TypeError,
                    crate::cstr!("cannot create '%.200s' instances: tp_new is NULL"),
                    (*cls).tp_name,
                );
                ptr::null_mut()
            }
        }
    }
}

unsafe fn reconstruct_newobj<M: Memo>(argtup: *mut PyObject, memo: &mut M) -> *mut PyObject {
    unsafe {
        let tup = argtup as *mut PyTupleObject;
        let nargs = tup.length();
        if nargs < 1 {
            PyErr_SetString(
                PyExc_TypeError,
                crate::cstr!("__newobj__ requires at least 1 argument"),
            );
            return ptr::null_mut();
        }

        let cls = tup.get_borrowed_unchecked(0);
        if !cls.is_type() {
            ffi_ext::PyErr_Format(
                PyExc_TypeError,
                crate::cstr!("__newobj__ arg 1 must be a type, not %.200s"),
                (*cls.class()).tp_name,
            );
            return ptr::null_mut();
        }

        let args = bail!(PyTuple_New(nargs - 1));
        let args_tup = args as *mut PyTupleObject;

        for i in 1..nargs {
            let arg = tup.get_borrowed_unchecked(i);
            let copied = deepcopy::deepcopy(arg, memo);
            if copied.is_error() {
                args.decref();
                return ptr::null_mut();
            }
            args_tup.set_slot_steal_unchecked(i - 1, copied.into_raw());
        }

        let instance = call_tp_new(cls as *mut PyTypeObject, args, ptr::null_mut());
        args.decref();
        instance
    }
}

unsafe fn reconstruct_newobj_ex<M: Memo>(
    argtup: *mut PyObject,
    memo: &mut M,
    reducing_type: *mut PyTypeObject,
) -> *mut PyObject {
    unsafe {
        let tup = argtup as *mut PyTupleObject;
        if tup.length() != 3 {
            ffi_ext::PyErr_Format(
                PyExc_TypeError,
                crate::cstr!("__newobj_ex__ requires 3 arguments, got %zd"),
                tup.length(),
            );
            return ptr::null_mut();
        }

        let cls = tup.get_borrowed_unchecked(0);
        let mut args = tup.get_borrowed_unchecked(1);
        let mut kwargs = tup.get_borrowed_unchecked(2);

        if !cls.is_type() {
            ffi_ext::PyErr_Format(
                PyExc_TypeError,
                crate::cstr!("__newobj_ex__ arg 1 must be a type, not %.200s"),
                (*cls.class()).tp_name,
            );
            return ptr::null_mut();
        }

        let mut coerced_args: *mut PyObject = ptr::null_mut();
        let mut coerced_kwargs: *mut PyObject = ptr::null_mut();

        if !args.is_tuple() {
            coerced_args = PySequence_Tuple(args);
            if coerced_args.is_null() {
                let msg = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "__newobj_ex__ args in %s.__reduce__ result must be a tuple, not %.200s"
                    ),
                    (*reducing_type).tp_name,
                    (*args.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg);
                }
                return ptr::null_mut();
            }
            args = coerced_args;
        }

        if !kwargs.is_dict() {
            coerced_kwargs = PyDict_New();
            if coerced_kwargs.is_null() {
                coerced_args.decref_nullable();
                return ptr::null_mut();
            }
            if PyDict_Merge(coerced_kwargs, kwargs, 1) < 0 {
                let msg = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "__newobj_ex__ kwargs in %s.__reduce__ result must be a dict, not %.200s"
                    ),
                    (*reducing_type).tp_name,
                    (*kwargs.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg);
                }
                coerced_args.decref_nullable();
                coerced_kwargs.decref();
                return ptr::null_mut();
            }
            kwargs = coerced_kwargs;
        }

        let copied_args = deepcopy::deepcopy(args, memo);
        coerced_args.decref_nullable();
        if copied_args.is_error() {
            coerced_kwargs.decref_nullable();
            return ptr::null_mut();
        }

        let copied_kwargs = deepcopy::deepcopy(kwargs, memo);
        coerced_kwargs.decref_nullable();
        if copied_kwargs.is_error() {
            copied_args.into_raw().decref();
            return ptr::null_mut();
        }

        let ca = copied_args.into_raw();
        let ck = copied_kwargs.into_raw();
        let instance = call_tp_new(cls as *mut PyTypeObject, ca, ck);
        ca.decref();
        ck.decref();
        instance
    }
}

unsafe fn reconstruct_callable<M: Memo>(
    callable: *mut PyObject,
    argtup: *mut PyObject,
    memo: &mut M,
) -> *mut PyObject {
    unsafe {
        let tup = argtup as *mut PyTupleObject;
        let nargs = tup.length();

        if nargs == 0 {
            return callable.call();
        }

        let copied_args = bail!(PyTuple_New(nargs));
        let copied_tup = copied_args as *mut PyTupleObject;

        for i in 0..nargs {
            let arg = tup.get_borrowed_unchecked(i);
            let copied = deepcopy::deepcopy(arg, memo);
            if copied.is_error() {
                copied_args.decref();
                return ptr::null_mut();
            }
            copied_tup.set_slot_steal_unchecked(i, copied.into_raw());
        }

        let instance = callable.call_with(copied_args);
        copied_args.decref();
        instance
    }
}

// ── State application ──────────────────────────────────────

/// Returns 1 if __setstate__ found and applied, 0 if not found, -1 on error.
unsafe fn apply_setstate<M: Memo>(
    instance: *mut PyObject,
    state: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        let mut setstate: *mut PyObject = ptr::null_mut();
        if instance.get_optional_attr(STATE.s_setstate, &mut setstate) < 0 {
            return -1;
        }
        if setstate.is_null() {
            return 0;
        }

        let copied = deepcopy::deepcopy(state, memo);
        if copied.is_error() {
            setstate.decref();
            return -1;
        }

        let cs = copied.into_raw();
        let result = setstate.call_one(cs);
        cs.decref();
        setstate.decref();

        if result.is_null() {
            return -1;
        }
        result.decref();
        1
    }
}

unsafe fn apply_dict_state<M: Memo>(
    instance: *mut PyObject,
    dict_state: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        if dict_state.is_null() || dict_state.is_none() {
            return 0;
        }

        let copied = deepcopy::deepcopy(dict_state, memo);
        if copied.is_error() {
            return -1;
        }
        let copied = copied.into_raw();

        if !copied.is_dict() {
            let instance_dict = instance.getattr(STATE.s_dict);
            if instance_dict.is_null() {
                copied.decref();
                return -1;
            }
            let ret = PyDict_Merge(instance_dict, copied, 1);
            instance_dict.decref();
            if ret < 0 {
                let msg = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "dict state from %s.__reduce__ must be a dict or mapping, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*copied.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg);
                }
            }
            copied.decref();
            return ret;
        }

        let instance_dict = instance.getattr(STATE.s_dict);
        if instance_dict.is_null() {
            copied.decref();
            return -1;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut pos: Py_ssize_t = 0;
        let mut ret: c_int = 0;

        while PyDict_Next(copied, &mut pos, &mut key, &mut value) != 0 {
            if PyObject_SetItem(instance_dict, key, value) < 0 {
                ret = -1;
                break;
            }
        }

        instance_dict.decref();
        copied.decref();
        ret
    }
}

unsafe fn apply_slot_state<M: Memo>(
    instance: *mut PyObject,
    slotstate: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        if slotstate.is_null() || slotstate.is_none() {
            return 0;
        }

        let copied = deepcopy::deepcopy(slotstate, memo);
        if copied.is_error() {
            return -1;
        }
        let copied = copied.into_raw();

        if !copied.is_dict() {
            let items_attr = PyObject_GetAttrString(copied, crate::cstr!("items"));
            if items_attr.is_null() {
                let msg = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "slot state from %s.__reduce__ must be a dict or have an items() method, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*copied.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg);
                }
                copied.decref();
                return -1;
            }
            let items = items_attr.call();
            items_attr.decref();
            copied.decref();
            if items.is_null() {
                return -1;
            }

            let iterator = items.get_iter();
            items.decref();
            if iterator.is_null() {
                return -1;
            }

            let mut ret: c_int = 0;
            loop {
                let pair = PyIter_Next(iterator);
                if pair.is_null() {
                    break;
                }
                let seq = ffi_ext::PySequence_Fast(pair, crate::cstr!("items() must return pairs"));
                pair.decref();
                if seq.is_null() {
                    ret = -1;
                    break;
                }
                if ffi_ext::PySequence_Fast_GET_SIZE(seq) != 2 {
                    seq.decref();
                    if PyErr_Occurred().is_null() {
                        PyErr_SetString(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack"),
                        );
                    }
                    ret = -1;
                    break;
                }
                let k = ffi_ext::PySequence_Fast_GET_ITEM(seq, 0);
                let v = ffi_ext::PySequence_Fast_GET_ITEM(seq, 1);
                let rc = instance.set_attr(k, v);
                seq.decref();
                if rc < 0 {
                    ret = -1;
                    break;
                }
            }
            if ret == 0 && !PyErr_Occurred().is_null() {
                ret = -1;
            }
            iterator.decref();
            return ret;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut pos: Py_ssize_t = 0;
        let mut ret: c_int = 0;

        while PyDict_Next(copied, &mut pos, &mut key, &mut value) != 0 {
            if instance.set_attr(key, value) < 0 {
                ret = -1;
                break;
            }
        }

        copied.decref();
        ret
    }
}

unsafe fn apply_state_tuple<M: Memo>(
    instance: *mut PyObject,
    state: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        let mut dict_state = state;
        let mut slotstate: *mut PyObject = ptr::null_mut();

        if state.is_tuple() && (state as *mut PyTupleObject).length() == 2 {
            let tup = state as *mut PyTupleObject;
            dict_state = tup.get_borrowed_unchecked(0);
            slotstate = tup.get_borrowed_unchecked(1);
        }

        if apply_dict_state(instance, dict_state, memo) < 0 {
            return -1;
        }
        if apply_slot_state(instance, slotstate, memo) < 0 {
            return -1;
        }
        0
    }
}

unsafe fn apply_listitems<M: Memo>(
    instance: *mut PyObject,
    listitems: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        if listitems.is_null() {
            return 0;
        }

        let append = instance.getattr(STATE.s_append);
        if append.is_null() {
            return -1;
        }

        let iterator = listitems.get_iter();
        if iterator.is_null() {
            append.decref();
            return -1;
        }

        let mut ret: c_int = 0;
        loop {
            let item = PyIter_Next(iterator);
            if item.is_null() {
                break;
            }
            let copied = deepcopy::deepcopy(item, memo);
            item.decref();
            if copied.is_error() {
                ret = -1;
                break;
            }
            let ci = copied.into_raw();
            let result = append.call_one(ci);
            ci.decref();
            if result.is_null() {
                ret = -1;
                break;
            }
            result.decref();
        }

        if ret == 0 && !PyErr_Occurred().is_null() {
            ret = -1;
        }
        iterator.decref();
        append.decref();
        ret
    }
}

unsafe fn apply_dictitems<M: Memo>(
    instance: *mut PyObject,
    dictitems: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        if dictitems.is_null() {
            return 0;
        }

        let iterator = dictitems.get_iter();
        if iterator.is_null() {
            return -1;
        }

        let mut ret: c_int = 0;
        loop {
            let pair = PyIter_Next(iterator);
            if pair.is_null() {
                break;
            }

            let (mut key, mut value);
            if pair.is_tuple() && (pair as *mut PyTupleObject).length() == 2 {
                let ptup = pair as *mut PyTupleObject;
                key = ptup.get_borrowed_unchecked(0).newref();
                value = ptup.get_borrowed_unchecked(1).newref();
            } else {
                let seq =
                    ffi_ext::PySequence_Fast(pair, crate::cstr!("cannot unpack non-sequence"));
                if seq.is_null() {
                    pair.decref();
                    ret = -1;
                    break;
                }
                let n = ffi_ext::PySequence_Fast_GET_SIZE(seq);
                if n != 2 {
                    seq.decref();
                    pair.decref();
                    if n < 2 {
                        ffi_ext::PyErr_Format(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack (expected 2, got %zd)"),
                            n,
                        );
                    } else {
                        ffi_ext::PyErr_Format(
                            PyExc_ValueError,
                            crate::cstr!("too many values to unpack (expected 2, got %zd)"),
                            n,
                        );
                    }
                    ret = -1;
                    break;
                }
                key = ffi_ext::PySequence_Fast_GET_ITEM(seq, 0).newref();
                value = ffi_ext::PySequence_Fast_GET_ITEM(seq, 1).newref();
                seq.decref();
            }
            pair.decref();

            let key_copy = deepcopy::deepcopy(key, memo);
            key.decref();
            if key_copy.is_error() {
                value.decref();
                ret = -1;
                break;
            }
            key = key_copy.into_raw();

            let val_copy = deepcopy::deepcopy(value, memo);
            value.decref();
            if val_copy.is_error() {
                key.decref();
                ret = -1;
                break;
            }
            value = val_copy.into_raw();

            let status = PyObject_SetItem(instance, key, value);
            key.decref();
            value.decref();
            if status < 0 {
                ret = -1;
                break;
            }
        }

        if ret == 0 && !PyErr_Occurred().is_null() {
            ret = -1;
        }
        iterator.decref();
        ret
    }
}

// ── Main entry point ───────────────────────────────────────

pub unsafe fn reconstruct<M: Memo>(
    original: *mut PyObject,
    tp: *mut PyTypeObject,
    memo: &mut M,
    probe: M::Probe,
) -> *mut PyObject {
    unsafe {
        let state_pointer = ptr::addr_of!(STATE);
        let copyreg_new_object = (*state_pointer).copyreg_newobj;
        let copyreg_new_object_with_state = (*state_pointer).copyreg_newobj_ex;

        let mut reduce_result = try_reduce_via_registry(original, tp);
        if reduce_result.is_null() {
            if !PyErr_Occurred().is_null() {
                return ptr::null_mut();
            }
            reduce_result = call_reduce_method_preferring_ex(original);
            if reduce_result.is_null() {
                return ptr::null_mut();
            }
        }

        let (kind, parts) = validate_reduce_tuple(reduce_result, tp);

        match kind {
            ReduceKind::Error => {
                reduce_result.decref();
                return ptr::null_mut();
            }
            ReduceKind::String => {
                reduce_result.decref();
                return original.newref();
            }
            ReduceKind::Tuple => {}
        }

        let instance = if parts.callable == copyreg_new_object {
            reconstruct_newobj(parts.argtup, memo)
        } else if parts.callable == copyreg_new_object_with_state {
            reconstruct_newobj_ex(parts.argtup, memo, tp)
        } else {
            reconstruct_callable(parts.callable, parts.argtup, memo)
        };

        if instance.is_null() {
            reduce_result.decref();
            return ptr::null_mut();
        }

        if memo.memoize(original, instance, &probe) < 0 {
            instance.decref();
            reduce_result.decref();
            return ptr::null_mut();
        }

        if !parts.state.is_null() {
            let applied = apply_setstate(instance, parts.state, memo);
            if applied < 0 {
                memo.forget(original, &probe);
                instance.decref();
                reduce_result.decref();
                return ptr::null_mut();
            }
            if applied == 0 && apply_state_tuple(instance, parts.state, memo) < 0 {
                memo.forget(original, &probe);
                instance.decref();
                reduce_result.decref();
                return ptr::null_mut();
            }
        }

        if apply_listitems(instance, parts.listitems, memo) < 0 {
            memo.forget(original, &probe);
            instance.decref();
            reduce_result.decref();
            return ptr::null_mut();
        }

        if apply_dictitems(instance, parts.dictitems, memo) < 0 {
            memo.forget(original, &probe);
            instance.decref();
            reduce_result.decref();
            return ptr::null_mut();
        }

        reduce_result.decref();
        instance
    }
}
