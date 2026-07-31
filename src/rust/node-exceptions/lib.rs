// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Utilities for creating Node.js-style exceptions.
//!
//! This is a Rust port of the C++ `workerd::api::node` exception helpers
//! (`src/workerd/api/node/exceptions.{h,c++}`). It is gated behind the
//! `NODEJS_EXCEPTIONS_RUST` autogate; when the gate is off the C++
//! implementation is used instead. The two implementations are intended to be
//! behaviorally identical.

use jsg::Lock;
use jsg::v8;
use jsg::v8::ToLocalValue;
use kj_rs::KjMaybe;
use nix::libc;

#[cxx::bridge(namespace = "workerd::rust::node_exceptions")]
mod ffi {
    /// Most Node.js exceptions are represented as either Error, TypeError, or
    /// RangeError. This is a cxx *extern* enum: the C++ `api::node::JsErrorType`
    /// is the single source of truth, and cxx generates static-asserts that the
    /// variants below match it. `#[repr(u8)]` matches the C++ enum's `uint8_t`
    /// underlying type.
    ///
    /// The paired `type JsErrorType;` declaration below marks this as an extern
    /// enum. cxx references the C++ type by this bridge's namespace, so the
    /// `js-error-type.h` shim aliases `api::node::JsErrorType` into
    /// `workerd::rust::node_exceptions`.
    #[repr(u8)]
    enum JsErrorType {
        Error,
        TypeError,
        RangeError,
    }

    unsafe extern "C++" {
        include!("workerd/rust/node-exceptions/js-error-type.h");

        type JsErrorType;
    }

    /// Node.js Exception Codes. Mirrors the C++ `NodeExceptionCode` enum
    /// (generated from `NODE_EXCEPTION_CODE_LIST`); the variant order must stay
    /// in sync with `api::node::NodeExceptionCode`.
    enum NodeExceptionCode {
        ErrFsCpEexist,
        ErrFsCpDirToNonDir,
        ErrFsCpEinval,
        ErrFsCpNonDirToDir,
        ErrFsEisdir,
    }

    extern "Rust" {
        /// Creates a Node.js-style exception: a JS error with a string "code"
        /// property. Returns a `v8::Local` handle (the reused `jsg::v8::ffi::Local`
        /// shared struct), which the C++ caller reconstitutes into a
        /// `jsg::JsValue` via `local_from_ffi`.
        ///
        /// An absent `message` (`kj::none`) means the default message for the
        /// given `code` is used. `message` is passed as raw bytes because
        /// exception messages may hold arbitrary, potentially non-UTF-8 byte
        /// strings; V8 renders invalid UTF-8 lossily rather than rejecting it.
        unsafe fn create_node_exception(
            isolate: *mut Isolate,
            code: NodeExceptionCode,
            error_type: JsErrorType,
            message: KjMaybe<&[u8]>,
        ) -> Local;

        /// Creates a Node.js-style "UVException": an ordinary Error object with
        /// additional `code`, `syscall`, `path`, and `dest` properties. Returns
        /// a `v8::Local` handle, as for `create_node_exception`.
        ///
        /// The `message`, `path`, and `dest` values map to nullable
        /// `kj::StringPtr` arguments on the C++ side; an absent value is
        /// `kj::none`. They are passed as raw bytes (not `&str`) because paths
        /// may be arbitrary, potentially non-UTF-8 byte strings; V8 renders
        /// invalid UTF-8 lossily rather than rejecting it. `syscall` is always a
        /// fixed ASCII string, so it stays a `&str`.
        unsafe fn create_uv_exception(
            isolate: *mut Isolate,
            errorno: i32,
            syscall: &str,
            message: KjMaybe<&[u8]>,
            path: KjMaybe<&[u8]>,
            dest: KjMaybe<&[u8]>,
        ) -> Local;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;

        // Reuse the `Local` shared struct defined by the jsg crate's `v8.rs`
        // bridge instead of round-tripping the handle through a bare `usize`.
        // The alias makes this the same Rust/C++ type as `jsg::v8::ffi::Local`,
        // so the handle can be returned by value across the bridge.
        type Local = jsg::v8::ffi::Local;
    }
}

// ======================================================================================
// Node.js exceptions

/// Creates a JS error object of the given type. Mirrors C++ `createJsError`.
///
/// `message` is raw UTF-8 bytes: exception messages may embed arbitrary,
/// potentially non-UTF-8 byte strings (e.g. filesystem paths), which V8
/// renders lossily (U+FFFD) rather than rejecting.
fn create_js_error<'a>(
    lock: &mut Lock,
    error_type: ffi::JsErrorType,
    message: &[u8],
) -> v8::Local<'a, v8::Object> {
    match error_type {
        ffi::JsErrorType::TypeError => lock.type_error_from_bytes(message),
        ffi::JsErrorType::RangeError => lock.range_error_from_bytes(message),
        // JsErrorType::Error, plus any unknown value, fall back to Error.
        _ => lock.error_from_bytes(message),
    }
}

/// Creates a V8 string value from raw UTF-8 bytes, used for exception
/// properties (`path`, `dest`) that may hold arbitrary, potentially non-UTF-8
/// byte strings. Invalid sequences are replaced with U+FFFD by V8.
fn bytes_to_value<'a>(lock: &mut Lock, data: &[u8]) -> v8::Local<'a, v8::Value> {
    v8::String::new_from_utf8(lock, data).unwrap(lock).into()
}

/// Returns the default message for a Node.js exception code. Mirrors C++
/// `getMessage`.
fn node_default_message(code: ffi::NodeExceptionCode) -> &'static str {
    match code {
        ffi::NodeExceptionCode::ErrFsCpEexist => "File already exists",
        ffi::NodeExceptionCode::ErrFsCpDirToNonDir => "Cannot copy directory to non-directory",
        ffi::NodeExceptionCode::ErrFsCpEinval => "Invalid cp operation",
        ffi::NodeExceptionCode::ErrFsCpNonDirToDir => "Cannot copy non-directory to directory",
        ffi::NodeExceptionCode::ErrFsEisdir => "Expected a file but found a directory",
        _ => "Unknown Node.js exception",
    }
}

/// Returns the string "code" value for a Node.js exception code. Mirrors C++
/// `getCode` (the stringified enumerator name).
fn node_code_name(code: ffi::NodeExceptionCode) -> &'static str {
    match code {
        ffi::NodeExceptionCode::ErrFsCpEexist => "ERR_FS_CP_EEXIST",
        ffi::NodeExceptionCode::ErrFsCpDirToNonDir => "ERR_FS_CP_DIR_TO_NON_DIR",
        ffi::NodeExceptionCode::ErrFsCpEinval => "ERR_FS_CP_EINVAL",
        ffi::NodeExceptionCode::ErrFsCpNonDirToDir => "ERR_FS_CP_NON_DIR_TO_DIR",
        ffi::NodeExceptionCode::ErrFsEisdir => "ERR_FS_EISDIR",
        _ => "UNKNOWN",
    }
}

/// Mirrors C++ `createNodeException`.
fn create_node_exception_impl<'a>(
    lock: &mut Lock,
    code: ffi::NodeExceptionCode,
    error_type: ffi::JsErrorType,
    message: Option<&[u8]>,
) -> v8::Local<'a, v8::Object> {
    let message = message.unwrap_or_else(|| node_default_message(code).as_bytes());
    let mut err = create_js_error(lock, error_type, message);
    // The "code" value is always a fixed ASCII enumerator name.
    let code_value = node_code_name(code).to_local(lock);
    err.set(lock, "code", code_value);
    err
}

// ======================================================================================
// UV exceptions

/// This is an intentionally truncated list of the error codes that Node.js/libuv
/// uses. We won't need all of the error codes that libuv uses.
///
/// Mirrors the C++ `UV_ERRNO_MAP` X-macro list. Each entry is
/// `(errorno, name, default message)`, where `errorno` mirrors the C++
/// `UV_##code = -code` constant. Using `libc` errno constants keeps the numeric
/// values in sync with the C++ side across platforms.
macro_rules! uv_errno_map {
    ($($name:ident => $message:literal),* $(,)?) => {
        const UV_ERRNO_MAP: &[(i32, &str, &str)] = &[
            $((-libc::$name, stringify!($name), $message)),*
        ];
    };
}

uv_errno_map! {
    EACCES => "permission denied",
    EBADF => "bad file descriptor",
    EEXIST => "file already exists",
    EFBIG => "file too large",
    EINVAL => "invalid argument",
    EISDIR => "illegal operation on a directory",
    ELOOP => "too many symbolic links encountered",
    EMFILE => "too many open files",
    ENAMETOOLONG => "name too long",
    ENFILE => "file table overflow",
    ENOBUFS => "no buffer space available",
    ENODEV => "no such device",
    ENOENT => "no such file or directory",
    ENOMEM => "not enough memory",
    ENOSPC => "no space left on device",
    ENOSYS => "function not implemented",
    ENOTDIR => "not a directory",
    ENOTEMPTY => "directory not empty",
    EPERM => "operation not permitted",
    EMLINK => "too many links",
    EIO => "input/output error",
}

/// Returns the error name for an errno value. Mirrors C++ `uv_err_name`.
fn uv_err_name(errorno: i32) -> &'static str {
    for &(code, name, _) in UV_ERRNO_MAP {
        if code == errorno {
            return name;
        }
    }
    "UNKNOWN"
}

/// Returns the default message for an errno value, matching the C++ default
/// message lookup in `createUVException`.
fn uv_default_message(errorno: i32) -> String {
    for &(code, _, message) in UV_ERRNO_MAP {
        if code == errorno {
            return message.to_owned();
        }
    }
    format!("unknown error: {errorno}")
}

/// Mirrors C++ `createUVException`.
fn create_uv_exception_impl<'a>(
    lock: &mut Lock,
    errorno: i32,
    syscall: &str,
    message: Option<&[u8]>,
    path: Option<&[u8]>,
    dest: Option<&[u8]>,
) -> v8::Local<'a, v8::Object> {
    debug_assert!(!syscall.is_empty(), "syscall must not be null");

    // Format the message to match Node.js format:
    // "ENOENT: no such file or directory, open 'path'"
    //
    // `path` may be arbitrary bytes, so the message is assembled as bytes and
    // handed to V8 (which renders invalid UTF-8 lossily) rather than forced
    // through a Rust `&str`.
    let formatted_message: Vec<u8> = if let Some(message) = message {
        message.to_vec()
    } else {
        let mut msg = uv_default_message(errorno).into_bytes();
        if let Some(path) = path {
            msg.extend_from_slice(b", ");
            msg.extend_from_slice(syscall.as_bytes());
            msg.extend_from_slice(b" '");
            msg.extend_from_slice(path);
            msg.push(b'\'');
        }
        msg
    };

    let mut obj = lock.error_from_bytes(&formatted_message);

    // `syscall` and the `code` name are always fixed ASCII strings.
    let syscall_value = syscall.to_local(lock);
    obj.set(lock, "syscall", syscall_value);
    let code_value = uv_err_name(errorno).to_local(lock);
    obj.set(lock, "code", code_value);

    if let Some(path) = path {
        let path_value = bytes_to_value(lock, path);
        obj.set(lock, "path", path_value);
    }
    if let Some(dest) = dest {
        let dest_value = bytes_to_value(lock, dest);
        obj.set(lock, "dest", dest_value);
    }

    obj
}

// ======================================================================================
// FFI wrappers
//
// These are thin adapters over the `*_impl` functions: they build a `Lock` from
// the raw isolate pointer, convert the nullable `KjMaybe` arguments to
// `Option`, and return the `v8::Local` handle for the C++ caller.

/// # Safety
/// `isolate` must be a valid pointer to a locked `v8::Isolate`.
unsafe fn create_node_exception(
    isolate: *mut ffi::Isolate,
    code: ffi::NodeExceptionCode,
    error_type: ffi::JsErrorType,
    message: KjMaybe<&[u8]>,
) -> ffi::Local {
    // SAFETY: isolate is valid and locked — called from C++ exception helpers.
    let mut lock = unsafe { Lock::from_isolate_ptr(isolate) };
    let err = create_node_exception_impl(&mut lock, code, error_type, message.into());
    // SAFETY: the handle is returned synchronously and stays within the caller's
    // HandleScope; C++ immediately reconstitutes it via local_from_ffi.
    unsafe { err.into_ffi() }
}

/// # Safety
/// `isolate` must be a valid pointer to a locked `v8::Isolate`.
unsafe fn create_uv_exception(
    isolate: *mut ffi::Isolate,
    errorno: i32,
    syscall: &str,
    message: KjMaybe<&[u8]>,
    path: KjMaybe<&[u8]>,
    dest: KjMaybe<&[u8]>,
) -> ffi::Local {
    // SAFETY: isolate is valid and locked — called from C++ exception helpers.
    let mut lock = unsafe { Lock::from_isolate_ptr(isolate) };
    let obj = create_uv_exception_impl(
        &mut lock,
        errorno,
        syscall,
        message.into(),
        path.into(),
        dest.into(),
    );
    // SAFETY: the handle is returned synchronously and stays within the caller's
    // HandleScope; C++ immediately reconstitutes it via local_from_ffi.
    unsafe { obj.into_ffi() }
}

#[cfg(test)]
mod tests {
    use jsg::FromJS;
    use jsg_test::Harness;

    use super::*;

    fn get_string(lock: &mut Lock, obj: &v8::Local<v8::Object>, key: &str) -> Option<String> {
        obj.get(lock, key)
            .and_then(|value| String::from_js(lock, value).ok())
    }

    #[test]
    fn node_exception_uses_default_message_and_code() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let err = create_node_exception_impl(
                lock,
                ffi::NodeExceptionCode::ErrFsEisdir,
                ffi::JsErrorType::Error,
                None,
            );
            let value: v8::Local<v8::Value> = err.clone().into();
            assert!(value.is_native_error());
            assert_eq!(
                get_string(lock, &err, "message").as_deref(),
                Some("Expected a file but found a directory")
            );
            assert_eq!(
                get_string(lock, &err, "code").as_deref(),
                Some("ERR_FS_EISDIR")
            );
            Ok(())
        });
    }

    #[test]
    fn node_exception_uses_explicit_message() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let err = create_node_exception_impl(
                lock,
                ffi::NodeExceptionCode::ErrFsCpEexist,
                ffi::JsErrorType::TypeError,
                Some(b"custom message".as_slice()),
            );
            assert_eq!(
                get_string(lock, &err, "message").as_deref(),
                Some("custom message")
            );
            assert_eq!(
                get_string(lock, &err, "code").as_deref(),
                Some("ERR_FS_CP_EEXIST")
            );
            Ok(())
        });
    }

    #[test]
    fn uv_exception_formats_default_message_with_path() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let err = create_uv_exception_impl(
                lock,
                -libc::ENOENT,
                "open",
                None,
                Some(b"/tmp/missing".as_slice()),
                None,
            );
            assert_eq!(
                get_string(lock, &err, "message").as_deref(),
                Some("no such file or directory, open '/tmp/missing'")
            );
            assert_eq!(get_string(lock, &err, "code").as_deref(), Some("ENOENT"));
            assert_eq!(get_string(lock, &err, "syscall").as_deref(), Some("open"));
            assert_eq!(
                get_string(lock, &err, "path").as_deref(),
                Some("/tmp/missing")
            );
            assert_eq!(get_string(lock, &err, "dest"), None);
            Ok(())
        });
    }

    #[test]
    fn uv_exception_uses_explicit_message_and_dest() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let err = create_uv_exception_impl(
                lock,
                -libc::EEXIST,
                "link",
                Some(b"File already exists".as_slice()),
                Some(b"/a".as_slice()),
                Some(b"/b".as_slice()),
            );
            assert_eq!(
                get_string(lock, &err, "message").as_deref(),
                Some("File already exists")
            );
            assert_eq!(get_string(lock, &err, "code").as_deref(), Some("EEXIST"));
            assert_eq!(get_string(lock, &err, "syscall").as_deref(), Some("link"));
            assert_eq!(get_string(lock, &err, "path").as_deref(), Some("/a"));
            assert_eq!(get_string(lock, &err, "dest").as_deref(), Some("/b"));
            Ok(())
        });
    }

    #[test]
    fn uv_exception_unknown_errno() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let err = create_uv_exception_impl(lock, 12345, "stat", None, None, None);
            assert_eq!(
                get_string(lock, &err, "message").as_deref(),
                Some("unknown error: 12345")
            );
            assert_eq!(get_string(lock, &err, "code").as_deref(), Some("UNKNOWN"));
            Ok(())
        });
    }

    // A non-UTF-8 path (valid on POSIX filesystems) must not throw; V8 renders
    // the invalid bytes lossily as U+FFFD in both the message and the `path`
    // property. Regression test for the rust::Str UTF-8-validation hazard: the
    // arbitrary byte arguments are passed as &[u8], never through a Rust &str.
    #[test]
    fn uv_exception_non_utf8_path_is_lossy_not_a_panic() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            // 0x80 is a lone continuation byte — invalid UTF-8.
            let bad_path: &[u8] = b"/tmp/\x80bad";
            let err =
                create_uv_exception_impl(lock, -libc::ENOENT, "open", None, Some(bad_path), None);

            // U+FFFD (the Unicode replacement character) stands in for 0x80.
            let expected_path = "/tmp/\u{FFFD}bad";
            assert_eq!(
                get_string(lock, &err, "path").as_deref(),
                Some(expected_path)
            );
            assert_eq!(
                get_string(lock, &err, "message").as_deref(),
                Some(format!("no such file or directory, open '{expected_path}'").as_str())
            );
            assert_eq!(get_string(lock, &err, "code").as_deref(), Some("ENOENT"));
            Ok(())
        });
    }
}
