use pyo3_ffi::*;
use std::ffi::{c_char, CStr};
use std::ptr;

use crate::ffi_ext::PyUnicode_FromFormat;
use crate::memo::{MemoCheckpoint, PyMemoObject};
use crate::state::{OnIncompatible, STATE};
use crate::types::PyObjectPtr;

macro_rules! cleanup_traceback_build {
    ($parts:expr, $traceback_module:expr, $format_exception:expr, $traceback_lines:expr, $empty_string:expr, $caller_string:expr) => {{
        $parts.decref_nullable();
        $traceback_module.decref_nullable();
        $format_exception.decref_nullable();
        $traceback_lines.decref_nullable();
        $empty_string.decref_nullable();
        $caller_string.decref_nullable();
        return ptr::null_mut();
    }};
}

macro_rules! finish_warning_emit {
    ($status:expr, $caller_info:expr, $traceback_string:expr, $full_message:expr, $module_name:expr, $type_name:expr, $deepcopy_qualified_name:expr, $deepcopy_expression:expr, $deepcopy_expression_with_memo:expr) => {{
        $caller_info.decref_nullable();
        $traceback_string.decref_nullable();
        $full_message.decref_nullable();
        $module_name.decref_nullable();
        $type_name.decref_nullable();
        $deepcopy_qualified_name.decref_nullable();
        $deepcopy_expression.decref_nullable();
        $deepcopy_expression_with_memo.decref_nullable();
        return $status;
    }};
}

macro_rules! finish_fallback_retry {
    ($result:expr, $dict_memo:expr, $exception_type:expr, $exception_value:expr, $exception_traceback:expr, $error_identifier:expr) => {{
        $dict_memo.decref_nullable();
        $exception_type.decref_nullable();
        $exception_value.decref_nullable();
        $exception_traceback.decref_nullable();
        $error_identifier.decref_nullable();
        return $result;
    }};
}

unsafe fn unicode_to_string(object: *mut PyObject) -> Option<String> {
    unsafe {
        let utf8 = PyUnicode_AsUTF8(object);
        if utf8.is_null() {
            return None;
        }
        Some(CStr::from_ptr(utf8).to_string_lossy().into_owned())
    }
}

unsafe fn new_unicode_from_string(value: &str) -> *mut PyObject {
    unsafe { PyUnicode_FromStringAndSize(value.as_ptr() as *const c_char, value.len() as isize) }
}

unsafe fn build_error_identifier(
    exception_type: *mut PyObject,
    exception_value: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let mut type_name: *mut PyObject = ptr::null_mut();
        let mut message: *mut PyObject = ptr::null_mut();

        if !exception_type.is_null() && PyType_Check(exception_type) != 0 {
            type_name = PyObject_GetAttrString(exception_type, crate::cstr!("__name__"));
        }

        if type_name.is_null() {
            PyErr_Clear();
            type_name = PyUnicode_FromString(crate::cstr!("Exception"));
            if type_name.is_null() {
                return ptr::null_mut();
            }
        }

        if !exception_value.is_null() {
            message = PyObject_Str(exception_value);
            if message.is_null() {
                PyErr_Clear();
            }
        }

        let result = if !message.is_null() && PyUnicode_GET_LENGTH(message) > 0 {
            PyUnicode_FromFormat(crate::cstr!("%U: %U"), type_name, message)
        } else {
            PyUnicode_FromFormat(crate::cstr!("%U: "), type_name)
        };

        type_name.decref_nullable();
        message.decref_nullable();
        result
    }
}

unsafe fn error_is_ignored(error_identifier: *mut PyObject) -> bool {
    unsafe {
        if error_identifier.is_null() || STATE.ignored_errors.is_null() {
            return false;
        }

        let ignored_error_count = PyTuple_GET_SIZE(STATE.ignored_errors);
        for index in 0..ignored_error_count {
            let suffix = PyTuple_GET_ITEM(STATE.ignored_errors, index);
            let matched = PyUnicode_Tailmatch(error_identifier, suffix, 0, PY_SSIZE_T_MAX, 1);
            if matched == 1 {
                return true;
            }
            if matched == -1 {
                PyErr_Clear();
            }
        }

        false
    }
}

unsafe fn extract_deepcopy_expression(line: *mut PyObject) -> *mut PyObject {
    unsafe {
        if line.is_null() || PyUnicode_Check(line) == 0 {
            return ptr::null_mut();
        }

        let line_text = match unicode_to_string(line) {
            Some(text) => text,
            None => return ptr::null_mut(),
        };

        let deepcopy_start = match line_text.find("deepcopy(") {
            Some(index) => index,
            None => return ptr::null_mut(),
        };

        let line_bytes = line_text.as_bytes();
        let mut expression_start = deepcopy_start;
        while expression_start > 0 {
            let previous = line_bytes[expression_start - 1];
            if previous == b'.' || previous == b'_' || previous.is_ascii_alphanumeric() {
                expression_start -= 1;
            } else {
                break;
            }
        }

        let mut cursor = deepcopy_start + "deepcopy".len();
        if line_bytes.get(cursor) != Some(&b'(') {
            return ptr::null_mut();
        }
        cursor += 1;

        let mut depth = 1i32;
        while cursor < line_bytes.len() && depth > 0 {
            let current = line_bytes[cursor];
            if current == b'(' {
                depth += 1;
                cursor += 1;
                continue;
            }
            if current == b')' {
                depth -= 1;
                if depth == 0 {
                    break;
                }
                cursor += 1;
                continue;
            }
            if current == b'"' || current == b'\'' {
                let quote = current;
                cursor += 1;
                while cursor < line_bytes.len() && line_bytes[cursor] != quote {
                    if line_bytes[cursor] == b'\\' && cursor + 1 < line_bytes.len() {
                        cursor += 1;
                    }
                    cursor += 1;
                }
                if cursor < line_bytes.len() {
                    cursor += 1;
                }
                continue;
            }

            cursor += 1;
        }

        if depth != 0 || cursor >= line_bytes.len() {
            return ptr::null_mut();
        }

        new_unicode_from_string(&line_text[expression_start..=cursor])
    }
}

unsafe fn make_expression_with_memo(expression: *mut PyObject) -> *mut PyObject {
    unsafe {
        if expression.is_null() || PyUnicode_Check(expression) == 0 {
            return ptr::null_mut();
        }

        let expression_text = match unicode_to_string(expression) {
            Some(text) => text,
            None => return ptr::null_mut(),
        };

        if let Some(prefix) = expression_text.strip_suffix(",)") {
            return new_unicode_from_string(&format!("{prefix}, memo={{}})"));
        }

        if let Some(prefix) = expression_text.strip_suffix(')') {
            return new_unicode_from_string(&format!("{prefix}, memo={{}})"));
        }

        ptr::null_mut()
    }
}

unsafe fn get_caller_frame_info() -> *mut PyObject {
    unsafe {
        let mut result: *mut PyObject = ptr::null_mut();
        let mut linecache_module: *mut PyObject = ptr::null_mut();
        let mut getline: *mut PyObject = ptr::null_mut();
        let mut line_number_object: *mut PyObject = ptr::null_mut();
        let mut line: *mut PyObject = ptr::null_mut();
        let mut stripped: *mut PyObject = ptr::null_mut();
        let mut frame = PyEval_GetFrame();
        let mut code: *mut PyCodeObject = ptr::null_mut();
        let mut filename: *mut PyObject = ptr::null_mut();
        let mut name: *mut PyObject = ptr::null_mut();

        if frame.is_null() {
            return ptr::null_mut();
        }

        (frame as *mut PyObject).incref();

        while !frame.is_null() {
            code = PyFrame_GetCode(frame);
            if code.is_null() {
                let back = PyFrame_GetBack(frame);
                (frame as *mut PyObject).decref();
                frame = back;
                continue;
            }

            filename = PyObject_GetAttrString(code as *mut PyObject, crate::cstr!("co_filename"));
            if filename.is_null() {
                PyErr_Clear();
            }

            name = PyObject_GetAttrString(code as *mut PyObject, crate::cstr!("co_name"));
            if name.is_null() {
                PyErr_Clear();
            }

            if !filename.is_null() && !name.is_null() {
                let line_number = PyFrame_GetLineNumber(frame);

                linecache_module = PyImport_ImportModule(crate::cstr!("linecache"));
                if linecache_module.is_null() {
                    PyErr_Clear();
                    break;
                }

                getline = PyObject_GetAttrString(linecache_module, crate::cstr!("getline"));
                if getline.is_null() {
                    PyErr_Clear();
                    break;
                }

                line_number_object = PyLong_FromLong(line_number as _);
                if line_number_object.is_null() {
                    break;
                }

                line = PyObject_CallFunctionObjArgs(
                    getline,
                    filename,
                    line_number_object,
                    ptr::null_mut::<PyObject>(),
                );
                if line.is_null() {
                    PyErr_Clear();
                    line = PyUnicode_FromString(crate::cstr!(""));
                    if line.is_null() {
                        break;
                    }
                }

                let strip_method = PyObject_GetAttrString(line, crate::cstr!("strip"));
                if strip_method.is_null() {
                    PyErr_Clear();
                    stripped = PyUnicode_FromString(crate::cstr!(""));
                } else {
                    stripped = PyObject_CallNoArgs(strip_method);
                    strip_method.decref();
                    if stripped.is_null() {
                        PyErr_Clear();
                        stripped = PyUnicode_FromString(crate::cstr!(""));
                    }
                }
                if stripped.is_null() {
                    break;
                }

                result = PyTuple_New(4);
                if result.is_null() {
                    break;
                }

                filename.incref();
                if PyTuple_SetItem(result, 0, filename) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                filename = ptr::null_mut();

                if PyTuple_SetItem(result, 1, line_number_object) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                line_number_object = ptr::null_mut();

                name.incref();
                if PyTuple_SetItem(result, 2, name) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                name = ptr::null_mut();

                if PyTuple_SetItem(result, 3, stripped) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                stripped = ptr::null_mut();
                break;
            }

            filename.decref_nullable();
            filename = ptr::null_mut();
            name.decref_nullable();
            name = ptr::null_mut();

            (code as *mut PyObject).decref();
            code = ptr::null_mut();

            let back = PyFrame_GetBack(frame);
            (frame as *mut PyObject).decref();
            frame = back;
        }

        linecache_module.decref_nullable();
        getline.decref_nullable();
        line_number_object.decref_nullable();
        line.decref_nullable();
        stripped.decref_nullable();
        (code as *mut PyObject).decref_nullable();
        filename.decref_nullable();
        name.decref_nullable();
        (frame as *mut PyObject).decref_nullable();
        result
    }
}

unsafe fn format_combined_traceback(
    caller_info: *mut PyObject,
    exception_value: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let mut parts: *mut PyObject = ptr::null_mut();
        let traceback_module = PyImport_ImportModule(crate::cstr!("traceback"));
        let mut format_exception: *mut PyObject = ptr::null_mut();
        let mut traceback_lines: *mut PyObject = ptr::null_mut();
        let mut empty_string: *mut PyObject = ptr::null_mut();
        let mut caller_string: *mut PyObject = ptr::null_mut();

        if traceback_module.is_null() {
            cleanup_traceback_build!(
                parts,
                traceback_module,
                format_exception,
                traceback_lines,
                empty_string,
                caller_string
            );
        }

        format_exception =
            PyObject_GetAttrString(traceback_module, crate::cstr!("format_exception"));
        if format_exception.is_null() {
            cleanup_traceback_build!(
                parts,
                traceback_module,
                format_exception,
                traceback_lines,
                empty_string,
                caller_string
            );
        }

        traceback_lines = PyObject_CallOneArg(format_exception, exception_value);
        if traceback_lines.is_null() || PyList_Check(traceback_lines) == 0 {
            cleanup_traceback_build!(
                parts,
                traceback_module,
                format_exception,
                traceback_lines,
                empty_string,
                caller_string
            );
        }

        empty_string = PyUnicode_FromString(crate::cstr!(""));
        if empty_string.is_null() {
            cleanup_traceback_build!(
                parts,
                traceback_module,
                format_exception,
                traceback_lines,
                empty_string,
                caller_string
            );
        }

        parts = PyList_New(0);
        if parts.is_null() {
            cleanup_traceback_build!(
                parts,
                traceback_module,
                format_exception,
                traceback_lines,
                empty_string,
                caller_string
            );
        }

        if !caller_info.is_null()
            && PyTuple_Check(caller_info) != 0
            && PyTuple_GET_SIZE(caller_info) == 4
        {
            let filename = PyTuple_GET_ITEM(caller_info, 0);
            let line_number = PyTuple_GET_ITEM(caller_info, 1);
            let function_name = PyTuple_GET_ITEM(caller_info, 2);
            let line = PyTuple_GET_ITEM(caller_info, 3);

            caller_string = PyUnicode_FromFormat(
                crate::cstr!("  File \"%U\", line %S, in %U\n    %U\n"),
                filename,
                line_number,
                function_name,
                line,
            );
            if caller_string.is_null() {
                PyErr_Clear();
            }
        }

        let traceback_line_count = PyList_GET_SIZE(traceback_lines);
        let mut found_traceback_header = false;
        let mut caller_inserted = false;

        for index in 0..traceback_line_count {
            let line = PyList_GET_ITEM(traceback_lines, index);

            if !found_traceback_header && PyUnicode_Check(line) != 0 {
                if let Some(line_text) = unicode_to_string(line) {
                    if line_text.starts_with("Traceback") {
                        found_traceback_header = true;
                        if PyList_Append(parts, line) < 0 {
                            cleanup_traceback_build!(
                                parts,
                                traceback_module,
                                format_exception,
                                traceback_lines,
                                empty_string,
                                caller_string
                            );
                        }
                        if !caller_string.is_null() {
                            if PyList_Append(parts, caller_string) < 0 {
                                cleanup_traceback_build!(
                                    parts,
                                    traceback_module,
                                    format_exception,
                                    traceback_lines,
                                    empty_string,
                                    caller_string
                                );
                            }
                            caller_inserted = true;
                        }
                        continue;
                    }
                }
            }

            if PyList_Append(parts, line) < 0 {
                cleanup_traceback_build!(
                    parts,
                    traceback_module,
                    format_exception,
                    traceback_lines,
                    empty_string,
                    caller_string
                );
            }
        }

        if !found_traceback_header && !caller_string.is_null() && !caller_inserted {
            let header = PyUnicode_FromString(crate::cstr!("Traceback (most recent call last):\n"));
            if !header.is_null() {
                if PyList_Insert(parts, 0, header) == 0 {
                    let _ = PyList_Insert(parts, 1, caller_string);
                }
                header.decref();
            }
        }

        let result = PyUnicode_Join(empty_string, parts);

        parts.decref_nullable();
        traceback_module.decref_nullable();
        format_exception.decref_nullable();
        traceback_lines.decref_nullable();
        empty_string.decref_nullable();
        caller_string.decref_nullable();
        result
    }
}

unsafe fn emit_fallback_warning(
    exception_value: *mut PyObject,
    object: *mut PyObject,
    error_identifier: *mut PyObject,
) -> i32 {
    unsafe {
        let mut status = 0;
        let caller_info = get_caller_frame_info();
        let mut traceback_string = format_combined_traceback(caller_info, exception_value);
        let mut full_message: *mut PyObject = ptr::null_mut();
        let type_object = object.class() as *mut PyObject;
        let mut module_name = PyObject_GetAttrString(type_object, crate::cstr!("__module__"));
        let mut type_name = PyObject_GetAttrString(type_object, crate::cstr!("__name__"));
        let mut deepcopy_qualified_name: *mut PyObject = ptr::null_mut();
        let mut deepcopy_expression: *mut PyObject = ptr::null_mut();
        let mut deepcopy_expression_with_memo: *mut PyObject = ptr::null_mut();

        if traceback_string.is_null() {
            PyErr_Clear();
            traceback_string = PyUnicode_FromString(crate::cstr!("[traceback unavailable]\n"));
            if traceback_string.is_null() {
                status = -1;
                finish_warning_emit!(
                    status,
                    caller_info,
                    traceback_string,
                    full_message,
                    module_name,
                    type_name,
                    deepcopy_qualified_name,
                    deepcopy_expression,
                    deepcopy_expression_with_memo
                );
            }
        }

        if module_name.is_null() {
            PyErr_Clear();
            module_name = PyUnicode_FromString(crate::cstr!("__main__"));
            if module_name.is_null() {
                status = -1;
                finish_warning_emit!(
                    status,
                    caller_info,
                    traceback_string,
                    full_message,
                    module_name,
                    type_name,
                    deepcopy_qualified_name,
                    deepcopy_expression,
                    deepcopy_expression_with_memo
                );
            }
        }

        if type_name.is_null() {
            PyErr_Clear();
            type_name = PyUnicode_FromString(crate::cstr!("?"));
            if type_name.is_null() {
                status = -1;
                finish_warning_emit!(
                    status,
                    caller_info,
                    traceback_string,
                    full_message,
                    module_name,
                    type_name,
                    deepcopy_qualified_name,
                    deepcopy_expression,
                    deepcopy_expression_with_memo
                );
            }
        }

        deepcopy_qualified_name =
            PyUnicode_FromFormat(crate::cstr!("%U.%U.__deepcopy__"), module_name, type_name);
        if deepcopy_qualified_name.is_null() {
            status = -1;
            finish_warning_emit!(
                status,
                caller_info,
                traceback_string,
                full_message,
                module_name,
                type_name,
                deepcopy_qualified_name,
                deepcopy_expression,
                deepcopy_expression_with_memo
            );
        }

        if !caller_info.is_null()
            && PyTuple_Check(caller_info) != 0
            && PyTuple_GET_SIZE(caller_info) == 4
        {
            let line = PyTuple_GET_ITEM(caller_info, 3);
            deepcopy_expression = extract_deepcopy_expression(line);
        }

        if !deepcopy_expression.is_null() {
            deepcopy_expression_with_memo = make_expression_with_memo(deepcopy_expression);
            if deepcopy_expression_with_memo.is_null() {
                PyErr_Clear();
            }
        }

        if deepcopy_expression.is_null() {
            deepcopy_expression = PyUnicode_FromFormat(crate::cstr!("deepcopy(%U())"), type_name);
            if deepcopy_expression.is_null() {
                status = -1;
                finish_warning_emit!(
                    status,
                    caller_info,
                    traceback_string,
                    full_message,
                    module_name,
                    type_name,
                    deepcopy_qualified_name,
                    deepcopy_expression,
                    deepcopy_expression_with_memo
                );
            }
        }

        if deepcopy_expression_with_memo.is_null() {
            deepcopy_expression_with_memo = make_expression_with_memo(deepcopy_expression);
            if deepcopy_expression_with_memo.is_null() {
                PyErr_Clear();
                deepcopy_expression_with_memo =
                    PyUnicode_FromFormat(crate::cstr!("deepcopy(%U(), memo={})"), type_name);
                if deepcopy_expression_with_memo.is_null() {
                    status = -1;
                    finish_warning_emit!(
                        status,
                        caller_info,
                        traceback_string,
                        full_message,
                        module_name,
                        type_name,
                        deepcopy_qualified_name,
                        deepcopy_expression,
                        deepcopy_expression_with_memo
                    );
                }
            }
        }

        full_message = PyUnicode_FromFormat(
            crate::cstr!(
                "\n\nSeems like 'copium.memo' was rejected inside '%U':\n\n%U\ncopium was able to recover from this error, but this is slow.\n\nFix:\n\n  Per Python docs, '%U' should treat memo as an opaque object.\n  See: https://docs.python.org/3/library/copy.html#object.__deepcopy__\n\nWorkarounds:\n\n     local  change %U to %U\n            -> copium uses dict memo in this call (recommended)\n\n    global  `copium.config.apply(memo=\"dict\")` or export COPIUM_USE_DICT_MEMO=1\n            -> copium uses dict memo everywhere (~1.3-2x slowdown, still faster than stdlib)\n\n explosive  `copium.config.apply(on_incompatible=\"raise\")` or export COPIUM_NO_MEMO_FALLBACK=1\n            -> '%U' raises the error above. Useful if you want to handle it yourself.\n\n    silent  `copium.config.apply(suppress_warnings=[%R])` or export COPIUM_NO_MEMO_FALLBACK_WARNING='%U'\n            -> disables this warning for '%U', it stays slow to deepcopy\n"
            ),
            deepcopy_qualified_name,
            traceback_string,
            deepcopy_qualified_name,
            deepcopy_expression,
            deepcopy_expression_with_memo,
            deepcopy_expression,
            error_identifier,
            error_identifier,
            deepcopy_expression
        );
        if full_message.is_null() {
            status = -1;
            finish_warning_emit!(
                status,
                caller_info,
                traceback_string,
                full_message,
                module_name,
                type_name,
                deepcopy_qualified_name,
                deepcopy_expression,
                deepcopy_expression_with_memo
            );
        }

        if PyErr_WarnEx(PyExc_UserWarning, PyUnicode_AsUTF8(full_message), 1) < 0 {
            status = -1;
        } else {
            PyErr_Clear();
        }

        finish_warning_emit!(
            status,
            caller_info,
            traceback_string,
            full_message,
            module_name,
            type_name,
            deepcopy_qualified_name,
            deepcopy_expression,
            deepcopy_expression_with_memo
        );
    }
}

pub unsafe fn maybe_retry_with_dict_memo(
    object: *mut PyObject,
    dunder_deepcopy: *mut PyObject,
    memo: &mut PyMemoObject,
    checkpoint: MemoCheckpoint,
) -> *mut PyObject {
    unsafe {
        let mut result: *mut PyObject = ptr::null_mut();
        let mut exception_type: *mut PyObject = ptr::null_mut();
        let mut exception_value: *mut PyObject = ptr::null_mut();
        let mut exception_traceback: *mut PyObject = ptr::null_mut();
        let mut error_identifier: *mut PyObject = ptr::null_mut();

        if PyErr_ExceptionMatches(PyExc_TypeError) == 0
            && PyErr_ExceptionMatches(PyExc_AssertionError) == 0
        {
            return ptr::null_mut();
        }

        if STATE.on_incompatible == OnIncompatible::Raise {
            return ptr::null_mut();
        }

        #[allow(deprecated)]
        PyErr_Fetch(
            &mut exception_type,
            &mut exception_value,
            &mut exception_traceback,
        );
        #[allow(deprecated)]
        PyErr_NormalizeException(
            &mut exception_type,
            &mut exception_value,
            &mut exception_traceback,
        );

        memo.rollback(checkpoint);

        let dict_memo = memo.to_dict();
        if dict_memo.is_null() {
            finish_fallback_retry!(
                result,
                dict_memo,
                exception_type,
                exception_value,
                exception_traceback,
                error_identifier
            );
        }

        let dict_size_before = PyDict_Size(dict_memo);
        result = PyObject_CallOneArg(dunder_deepcopy, dict_memo);
        if result.is_null() {
            finish_fallback_retry!(
                result,
                dict_memo,
                exception_type,
                exception_value,
                exception_traceback,
                error_identifier
            );
        }

        if memo.sync_from_dict(dict_memo, dict_size_before) < 0 {
            result.decref_nullable();
            result = ptr::null_mut();
            finish_fallback_retry!(
                result,
                dict_memo,
                exception_type,
                exception_value,
                exception_traceback,
                error_identifier
            );
        }

        error_identifier = build_error_identifier(exception_type, exception_value);
        if STATE.on_incompatible == OnIncompatible::Warn
            && !error_identifier.is_null()
            && !error_is_ignored(error_identifier)
        {
            if !exception_traceback.is_null() && !exception_value.is_null() {
                let _ = PyException_SetTraceback(exception_value, exception_traceback);
            }

            if emit_fallback_warning(exception_value, object, error_identifier) < 0 {
                result.decref_nullable();
                result = ptr::null_mut();
            }
        }

        finish_fallback_retry!(
            result,
            dict_memo,
            exception_type,
            exception_value,
            exception_traceback,
            error_identifier
        );
    }
}
