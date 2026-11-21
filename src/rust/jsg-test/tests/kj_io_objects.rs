// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A resource that holds a KJ I/O object other than through an `IoOwn` aborts the process when it
//! is destroyed, as a C++ JSG object does.
//!
//! Each test runs its case in a child process, which it expects to abort.

use std::process::Command;

use jsg::ToJS;
use jsg_macros::jsg_resource;
use kj_rs::KjOwn;

use crate::ffi;

const CHILD_ENV: &str = "WORKERD_JSG_TEST_KJ_IO_OBJECT_CHILD";
const REASON: &str = "JavaScript heap objects must not contain KJ I/O objects without a IoOwn";

#[jsg_resource(custom_trace)]
struct HoldsAsyncObject {
    _object: KjOwn<ffi::TestAsyncObject>,
}

impl jsg::Traced for HoldsAsyncObject {
    fn trace(&self, _visitor: &mut jsg::v8::GcVisitor) {}
}

#[jsg_resource]
impl HoldsAsyncObject {}

fn holds_async_object(lock: &mut jsg::Lock<'_>) -> jsg::Rc<HoldsAsyncObject> {
    jsg::Rc::new(
        lock,
        HoldsAsyncObject {
            _object: ffi::new_test_async_object(),
        },
    )
}

/// Runs `case` in a child process when this is one, and otherwise runs the test named `test` in a
/// child and checks that it aborted for the reason a JavaScript heap object's destruction gives.
fn expect_abort_in_child(test: &str, case: impl FnOnce()) {
    if std::env::var_os(CHILD_ENV).is_some() {
        case();
        return;
    }
    // A test's name is its path without the crate.
    let module = module_path!().split_once("::").map_or("", |(_, rest)| rest);
    let output = Command::new(std::env::current_exe().expect("test executable path"))
        .args(["--exact", &format!("{module}::{test}")])
        .env(CHILD_ENV, "1")
        .output()
        .expect("spawn child test process");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        !output.status.success() && stderr.contains(REASON),
        "the child did not abort for a KJ I/O object in a resource: {:?}\n{stderr}",
        output.status,
    );
}

#[test]
fn releasing_the_last_rc_under_the_lock_aborts() {
    expect_abort_in_child("releasing_the_last_rc_under_the_lock_aborts", || {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, _ctx| {
            drop(holds_async_object(lock));
            Ok(())
        });
    });
}

#[test]
fn releasing_the_last_rc_queued_for_the_lock_aborts() {
    expect_abort_in_child("releasing_the_last_rc_queued_for_the_lock_aborts", || {
        let harness = crate::Harness::new();
        let mut kept = None;
        harness.run_in_context(|lock, _ctx| {
            kept = Some(holds_async_object(lock));
            Ok(())
        });
        // Without the lock, the release waits for the next one.
        drop(kept);
        harness.run_in_context(|_lock, _ctx| Ok(()));
    });
}

#[test]
fn collecting_the_wrapper_aborts() {
    expect_abort_in_child("collecting_the_wrapper_aborts", || {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let _ = holds_async_object(lock).to_js(lock)?;
            Ok(())
        });
        harness.run_in_context(|lock, _ctx| {
            crate::Harness::request_gc(lock);
            Ok(())
        });
    });
}
