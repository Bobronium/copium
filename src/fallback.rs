use std::ptr;

use crate::memo::{MemoCheckpoint, PyMemoObject};
use crate::py::{self, *};
use crate::state::{OnIncompatible, STATE};

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
        if object.is_null() || !object.is_unicode() {
            return None;
        }
        Some(py::unicode::as_utf8(object).to_string_lossy().into_owned())
    }
}

unsafe fn new_unicode_from_string(value: &str) -> *mut PyObject {
    unsafe { py::unicode::from_str_and_size(value).as_object() }
}

unsafe fn build_error_identifier(
    exception_type: *mut PyObject,
    exception_value: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let mut type_name: *mut PyObject = ptr::null_mut();
        let mut message: *mut PyObject = ptr::null_mut();

        if !exception_type.is_null() && exception_type.is_type() {
            type_name = exception_type.getattr_cstr(crate::cstr!("__name__"));
        }

        if type_name.is_null() {
            py::err::clear();
            type_name = py::unicode::from_cstr(crate::cstr!("Exception")).as_object();
            if type_name.is_null() {
                return ptr::null_mut();
            }
        }

        if !exception_value.is_null() {
            message = exception_value.str_().as_object();
            if message.is_null() {
                py::err::clear();
            }
        }

        let result = if !message.is_null() && py::unicode::byte_length(message) > 0 {
            py::unicode::from_format!(crate::cstr!("%U: %U"), type_name, message).as_object()
        } else {
            py::unicode::from_format!(crate::cstr!("%U: "), type_name).as_object()
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

        let ignored_error_count = py::tuple::size(STATE.ignored_errors);
        for index in 0..ignored_error_count {
            let suffix = py::tuple::get_item(STATE.ignored_errors, index);
            if py::unicode::tailmatch(error_identifier, suffix, 0, PY_SSIZE_T_MAX, 1) {
                return true;
            }
            if !py::err::occurred().is_null() {
                py::err::clear();
            }
        }

        false
    }
}

unsafe fn extract_deepcopy_expression(line: *mut PyObject) -> *mut PyObject {
    unsafe {
        if line.is_null() || !line.is_unicode() {
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
        if expression.is_null() || !expression.is_unicode() {
            return ptr::null_mut();
        }

        let expression_text = match unicode_to_string(expression) {
            Some(text) => text,
            None => return ptr::null_mut(),
        };

        if let Some(prefix) = expression_text.strip_suffix(",)") {
            return new_unicode_from_string(&std::format!("{prefix}, memo={{}})"));
        }

        if let Some(prefix) = expression_text.strip_suffix(')') {
            return new_unicode_from_string(&std::format!("{prefix}, memo={{}})"));
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
        let mut frame = py::eval::current_frame();
        let mut code: *mut PyCodeObject = ptr::null_mut();
        let mut filename: *mut PyObject = ptr::null_mut();
        let mut name: *mut PyObject = ptr::null_mut();

        if frame.is_null() {
            return ptr::null_mut();
        }

        (frame as *mut PyObject).incref();

        while !frame.is_null() {
            code = frame.code();
            if code.is_null() {
                let back = frame.back();
                (frame as *mut PyObject).decref();
                frame = back;
                continue;
            }

            filename = (code as *mut PyObject).getattr_cstr(c"co_filename");
            if filename.is_null() {
                py::err::clear();
            }

            name = (code as *mut PyObject).getattr_cstr(crate::cstr!("co_name"));
            if name.is_null() {
                py::err::clear();
            }

            if !filename.is_null() && !name.is_null() {
                let line_number = frame.line_number();

                linecache_module = py::module::import(crate::cstr!("linecache"));
                if linecache_module.is_null() {
                    py::err::clear();
                    break;
                }

                getline = linecache_module.getattr_cstr(crate::cstr!("getline"));
                if getline.is_null() {
                    py::err::clear();
                    break;
                }

                line_number_object = py::long::from_i64(line_number as i64).as_object();
                if line_number_object.is_null() {
                    break;
                }

                line = py::call::function_obj_args!(getline, filename, line_number_object);
                if line.is_null() {
                    py::err::clear();
                    line = py::unicode::from_cstr(crate::cstr!("")).as_object();
                    if line.is_null() {
                        break;
                    }
                }

                let strip_method = line.getattr_cstr(crate::cstr!("strip"));
                if strip_method.is_null() {
                    py::err::clear();
                    stripped = py::unicode::from_cstr(crate::cstr!("")).as_object();
                } else {
                    stripped = strip_method.call();
                    strip_method.decref();
                    if stripped.is_null() {
                        py::err::clear();
                        stripped = py::unicode::from_cstr(crate::cstr!("")).as_object();
                    }
                }
                if stripped.is_null() {
                    break;
                }

                result = py::tuple::new(4).as_object();
                if result.is_null() {
                    break;
                }

                filename.incref();
                if py::tuple::set_item(result, 0, filename) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                filename = ptr::null_mut();

                if py::tuple::set_item(result, 1, line_number_object) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                line_number_object = ptr::null_mut();

                name.incref();
                if py::tuple::set_item(result, 2, name) < 0 {
                    result.decref();
                    result = ptr::null_mut();
                    break;
                }
                name = ptr::null_mut();

                if py::tuple::set_item(result, 3, stripped) < 0 {
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

            let back = frame.back();
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
        let traceback_module = py::module::import(crate::cstr!("traceback"));
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

        format_exception = traceback_module.getattr_cstr(crate::cstr!("format_exception"));
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

        traceback_lines = format_exception.call_one(exception_value);
        if traceback_lines.is_null() || !py::list::check(traceback_lines) {
            cleanup_traceback_build!(
                parts,
                traceback_module,
                format_exception,
                traceback_lines,
                empty_string,
                caller_string
            );
        }

        empty_string = py::unicode::from_cstr(crate::cstr!("")).as_object();
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

        parts = py::list::new(0).as_object();
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
            && py::tuple::check(caller_info)
            && py::tuple::size(caller_info) == 4
        {
            let filename = py::tuple::get_item(caller_info, 0);
            let line_number = py::tuple::get_item(caller_info, 1);
            let function_name = py::tuple::get_item(caller_info, 2);
            let line = py::tuple::get_item(caller_info, 3);

            caller_string = py::unicode::from_format!(
                crate::cstr!("  File \"%U\", line %S, in %U\n    %U\n"),
                filename,
                line_number,
                function_name,
                line,
            )
            .as_object();
            if caller_string.is_null() {
                py::err::clear();
            }
        }

        let traceback_line_count = py::list::size(traceback_lines);
        let mut found_traceback_header = false;
        let mut caller_inserted = false;

        for index in 0..traceback_line_count {
            let line = py::list::borrow_item(traceback_lines, index);

            if !found_traceback_header && line.is_unicode() {
                if let Some(line_text) = unicode_to_string(line) {
                    if line_text.starts_with("Traceback") {
                        found_traceback_header = true;
                        if py::list::append(parts, line) < 0 {
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
                            if py::list::append(parts, caller_string) < 0 {
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

            if py::list::append(parts, line) < 0 {
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
            let header =
                py::unicode::from_cstr(crate::cstr!("Traceback (most recent call last):\n"))
                    .as_object();
            if !header.is_null() {
                if py::list::insert(parts, 0, header) == 0 {
                    let _ = py::list::insert(parts, 1, caller_string);
                }
                header.decref();
            }
        }

        let result = py::unicode::join(empty_string, parts).as_object();

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
        let mut module_name = type_object.getattr_cstr(crate::cstr!("__module__"));
        let mut type_name = type_object.getattr_cstr(crate::cstr!("__name__"));
        let mut deepcopy_qualified_name: *mut PyObject = ptr::null_mut();
        let mut deepcopy_expression: *mut PyObject = ptr::null_mut();
        let mut deepcopy_expression_with_memo: *mut PyObject = ptr::null_mut();

        if traceback_string.is_null() {
            py::err::clear();
            traceback_string =
                py::unicode::from_cstr(crate::cstr!("[traceback unavailable]\n")).as_object();
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
            py::err::clear();
            module_name = py::unicode::from_cstr(crate::cstr!("__main__")).as_object();
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
            py::err::clear();
            type_name = py::unicode::from_cstr(crate::cstr!("?")).as_object();
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
            py::unicode::from_format!(crate::cstr!("%U.%U.__deepcopy__"), module_name, type_name)
                .as_object();
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
            && py::tuple::check(caller_info)
            && py::tuple::size(caller_info) == 4
        {
            let line = py::tuple::get_item(caller_info, 3);
            deepcopy_expression = extract_deepcopy_expression(line);
        }

        if !deepcopy_expression.is_null() {
            deepcopy_expression_with_memo = make_expression_with_memo(deepcopy_expression);
            if deepcopy_expression_with_memo.is_null() {
                py::err::clear();
            }
        }

        if deepcopy_expression.is_null() {
            deepcopy_expression =
                py::unicode::from_format!(crate::cstr!("deepcopy(%U())"), type_name).as_object();
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
                py::err::clear();
                deepcopy_expression_with_memo =
                    py::unicode::from_format!(crate::cstr!("deepcopy(%U(), memo={})"), type_name)
                        .as_object();
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

        full_message = py::unicode::from_format!(
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
            deepcopy_expression,
        )
        .as_object();
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

        if py::err::warn(PyExc_UserWarning, py::unicode::as_utf8(full_message), 1) < 0 {
            status = -1;
        } else {
            py::err::clear();
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
        let mut error_identifier: *mut PyObject = ptr::null_mut();

        if !py::err::matches_current(PyExc_TypeError)
            && !py::err::matches_current(PyExc_AssertionError)
        {
            return ptr::null_mut();
        }

        if STATE.on_incompatible == OnIncompatible::Raise {
            return ptr::null_mut();
        }

        let (mut exception_type, mut exception_value, mut exception_traceback) = py::err::fetch();
        py::err::normalize(
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

        let dict_size_before = py::dict::size(dict_memo);
        result = dunder_deepcopy.call_one(dict_memo);
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
                let _ = py::err::set_traceback(exception_value, exception_traceback);
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
