use std::os::raw::c_int;
use std::ptr;

use crate::deepcopy;
use crate::memo::Memo;
use crate::py::{self, *};
use crate::py_obj;
use crate::py_str;

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
        let (mut cause_type, mut cause_val, mut cause_tb) = py::err::fetch();

        if !cause_val.is_null() {
            py::err::normalize(&mut cause_type, &mut cause_val, &mut cause_tb);
        }

        let new_exc = PyExc_TypeError.call_one(msg);
        msg.decref();

        if new_exc.is_null() {
            py::err::restore(cause_type, cause_val, cause_tb);
            return;
        }

        if !cause_val.is_null() {
            py::err::set_cause(cause_val, new_exc);
            py::err::restore(cause_type, cause_val, cause_tb);
            return;
        }

        cause_type.decref_nullable();
        cause_tb.decref_nullable();

        py::err::set(PyExc_TypeError, new_exc);
        new_exc.decref();
    }
}

// ── Registry & reduce dispatch ─────────────────────────────

pub(crate) unsafe fn try_reduce_via_registry(
    obj: *mut PyObject,
    tp: *mut PyTypeObject,
) -> *mut PyObject {
    unsafe {
        let reducer = py_obj!(PyDictObject, "copyreg.dispatch_table").get_item(tp as *mut PyObject);
        if reducer.is_null() {
            return ptr::null_mut();
        }
        if !reducer.is_callable() {
            py::err::set_string(
                PyExc_TypeError,
                crate::cstr!("copyreg.dispatch_table value is not callable"),
            );
            return ptr::null_mut();
        }
        reducer.call_one(obj)
    }
}

pub(crate) unsafe fn call_reduce_method_preferring_ex(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let mut reduce_ex: *mut PyObject = ptr::null_mut();
        let has = obj.get_optional_attr(py_str!("__reduce_ex__"), &mut reduce_ex);
        if has > 0 {
            let four = py::long::from_i64(4).as_object();
            let res = reduce_ex.call_one(four);
            four.decref();
            reduce_ex.decref();
            return res;
        }
        if has < 0 {
            return ptr::null_mut();
        }
        let mut reduce: *mut PyObject = ptr::null_mut();
        let has = obj.get_optional_attr(py_str!("__reduce__"), &mut reduce);
        if has > 0 {
            let res = reduce.call();
            reduce.decref();
            return res;
        }
        if has < 0 {
            return ptr::null_mut();
        }
        py::err::set_string(
            py_obj!("copy.Error"),
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
            py::err::set_string(
                PyExc_TypeError,
                crate::cstr!("__reduce__ must return a tuple or str"),
            );
            return (ReduceKind::Error, empty);
        }

        let tup = reduce_result as *mut PyTupleObject;
        let size = tup.length();
        if size < 2 || size > 5 {
            py::err::set_string(
                PyExc_TypeError,
                crate::cstr!("tuple returned by __reduce__ must contain 2 through 5 elements"),
            );
            return (ReduceKind::Error, empty);
        }

        let callable = tup.get_borrowed_unchecked(0);
        let mut argtup = tup.get_borrowed_unchecked(1);
        let none = py::NoneObject;
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
            let coerced = py::seq::to_tuple(argtup).as_object();
            if coerced.is_null() {
                let msg = py::unicode::from_format!(
                    crate::cstr!(
                        "second element of the tuple returned by %s.__reduce__ must be a tuple, not %.200s"
                    ),
                    (*reducing_type).tp_name,
                    (*argtup.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg.as_object());
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
                py::err::format!(
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
            py::err::set_string(
                PyExc_TypeError,
                crate::cstr!("__newobj__ requires at least 1 argument"),
            );
            return ptr::null_mut();
        }

        let cls = tup.get_borrowed_unchecked(0);
        if !cls.is_type() {
            py::err::format!(
                PyExc_TypeError,
                crate::cstr!("__newobj__ arg 1 must be a type, not %.200s"),
                (*cls.class()).tp_name,
            );
            return ptr::null_mut();
        }

        let args = bail!(py::tuple::new(nargs - 1));

        for i in 1..nargs {
            let arg = tup.get_borrowed_unchecked(i);
            let copied = deepcopy::deepcopy(arg, memo);
            if copied.is_error() {
                args.decref();
                return ptr::null_mut();
            }
            args.set_slot_steal_unchecked(i - 1, copied.into_raw());
        }

        let instance = call_tp_new(cls as *mut PyTypeObject, args.as_object(), ptr::null_mut());
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
            py::err::format!(
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
            py::err::format!(
                PyExc_TypeError,
                crate::cstr!("__newobj_ex__ arg 1 must be a type, not %.200s"),
                (*cls.class()).tp_name,
            );
            return ptr::null_mut();
        }

        let mut coerced_args: *mut PyObject = ptr::null_mut();
        let mut coerced_kwargs: *mut PyDictObject = ptr::null_mut();

        if !args.is_tuple() {
            coerced_args = py::seq::to_tuple(args).as_object();
            if coerced_args.is_null() {
                let msg = py::unicode::from_format!(
                    crate::cstr!(
                        "__newobj_ex__ args in %s.__reduce__ result must be a tuple, not %.200s"
                    ),
                    (*reducing_type).tp_name,
                    (*args.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg.as_object());
                }
                return ptr::null_mut();
            }
            args = coerced_args;
        }

        if !kwargs.is_dict() {
            coerced_kwargs = py::dict::new_presized(0);
            if coerced_kwargs.is_null() {
                coerced_args.decref_nullable();
                return ptr::null_mut();
            }
            if coerced_kwargs.merge(kwargs, true) < 0 {
                let msg = py::unicode::from_format!(
                    crate::cstr!(
                        "__newobj_ex__ kwargs in %s.__reduce__ result must be a dict, not %.200s"
                    ),
                    (*reducing_type).tp_name,
                    (*kwargs.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg.as_object());
                }
                coerced_args.decref_nullable();
                coerced_kwargs.decref();
                return ptr::null_mut();
            }
            kwargs = coerced_kwargs.as_object();
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

        let copied_args = bail!(py::tuple::new(nargs));

        for i in 0..nargs {
            let arg = tup.get_borrowed_unchecked(i);
            let copied = deepcopy::deepcopy(arg, memo);
            if copied.is_error() {
                copied_args.decref();
                return ptr::null_mut();
            }
            copied_args.set_slot_steal_unchecked(i, copied.into_raw());
        }

        let instance = callable.call_with(copied_args.as_object());
        copied_args.decref();
        instance
    }
}

// ── State application ──────────────────────────────────────

unsafe fn apply_setstate<M: Memo>(
    instance: *mut PyObject,
    state: *mut PyObject,
    memo: &mut M,
) -> c_int {
    unsafe {
        let mut setstate: *mut PyObject = ptr::null_mut();
        if instance.get_optional_attr(py_str!("__setstate__"), &mut setstate) < 0 {
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
            let instance_dict = instance.getattr(py_str!("__dict__"));
            if instance_dict.is_null() {
                copied.decref();
                return -1;
            }
            let ret = (instance_dict as *mut PyDictObject).merge(copied, true);
            instance_dict.decref();
            if ret < 0 {
                let msg = py::unicode::from_format!(
                    crate::cstr!(
                        "dict state from %s.__reduce__ must be a dict or mapping, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*copied.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg.as_object());
                }
            }
            copied.decref();
            return ret;
        }

        let instance_dict = instance.getattr(py_str!("__dict__"));
        if instance_dict.is_null() {
            copied.decref();
            return -1;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut pos: Py_ssize_t = 0;
        let mut ret: c_int = 0;

        while (copied as *mut PyDictObject).dict_next(&mut pos, &mut key, &mut value) {
            if instance_dict.setitem(key, value) < 0 {
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
            let items_attr = copied.getattr_cstr(crate::cstr!("items"));
            if items_attr.is_null() {
                let msg = py::unicode::from_format!(
                    crate::cstr!(
                        "slot state from %s.__reduce__ must be a dict or have an items() method, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*copied.class()).tp_name,
                );
                if !msg.is_null() {
                    chain_type_error(msg.as_object());
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
                let pair = iterator.iter_next();
                if pair.is_null() {
                    break;
                }
                let seq = py::seq::fast(pair, crate::cstr!("items() must return pairs"));
                pair.decref();
                if seq.is_null() {
                    ret = -1;
                    break;
                }
                if py::seq::fast_length(seq) != 2 {
                    seq.decref();
                    if py::err::occurred().is_null() {
                        py::err::set_string(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack"),
                        );
                    }
                    ret = -1;
                    break;
                }
                let k = py::seq::fast_borrow_item_unchecked(seq, 0);
                let v = py::seq::fast_borrow_item_unchecked(seq, 1);
                let rc = instance.set_attr(k, v);
                seq.decref();
                if rc < 0 {
                    ret = -1;
                    break;
                }
            }
            if ret == 0 && !py::err::occurred().is_null() {
                ret = -1;
            }
            iterator.decref();
            return ret;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut pos: Py_ssize_t = 0;
        let mut ret: c_int = 0;

        while (copied as *mut PyDictObject).dict_next(&mut pos, &mut key, &mut value) {
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

        let append = instance.getattr(py_str!("append"));
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
            let item = iterator.iter_next();
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

        if ret == 0 && !py::err::occurred().is_null() {
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
            let pair = iterator.iter_next();
            if pair.is_null() {
                break;
            }

            let (mut key, mut value);
            if pair.is_tuple() && (pair as *mut PyTupleObject).length() == 2 {
                let ptup = pair as *mut PyTupleObject;
                key = ptup.get_borrowed_unchecked(0).newref();
                value = ptup.get_borrowed_unchecked(1).newref();
            } else {
                let seq = py::seq::fast(pair, crate::cstr!("cannot unpack non-sequence"));
                if seq.is_null() {
                    pair.decref();
                    ret = -1;
                    break;
                }
                let n = py::seq::fast_length(seq);
                if n != 2 {
                    seq.decref();
                    pair.decref();
                    if n < 2 {
                        py::err::format!(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack (expected 2, got %zd)"),
                            n,
                        );
                    } else {
                        py::err::format!(
                            PyExc_ValueError,
                            crate::cstr!("too many values to unpack (expected 2, got %zd)"),
                            n,
                        );
                    }
                    ret = -1;
                    break;
                }
                key = py::seq::fast_borrow_item_unchecked(seq, 0).newref();
                value = py::seq::fast_borrow_item_unchecked(seq, 1).newref();
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

            let status = instance.setitem(key, value);
            key.decref();
            value.decref();
            if status < 0 {
                ret = -1;
                break;
            }
        }

        if ret == 0 && !py::err::occurred().is_null() {
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
        let mut reduce_result = try_reduce_via_registry(original, tp);
        if reduce_result.is_null() {
            if !py::err::occurred().is_null() {
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

        let instance = if parts.callable == py_obj!("copyreg.__newobj__") {
            reconstruct_newobj(parts.argtup, memo)
        } else if parts.callable == py_obj!("copyreg.__newobj_ex__") {
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
