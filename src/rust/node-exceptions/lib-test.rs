// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
        let err = create_uv_exception_impl(lock, -libc::ENOENT, "open", None, Some(bad_path), None);

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
