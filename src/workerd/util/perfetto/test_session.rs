// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An in-process tracing session of the "workerd" category for the `perfetto` crate's tests.

#![allow(
    clippy::unwrap_used,
    clippy::missing_panics_doc,
    reason = "test support, where a session that fails to start or stop fails the test"
)]

use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;

#[cxx::bridge(namespace = "workerd::perfetto_test")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/util/perfetto/test-session.h");

        fn compiled_in() -> bool;
        fn flush_thread();
        fn start_session(path: &str) -> Result<()>;
        fn stop_session(path: &str) -> Result<Vec<String>>;
    }
}

/// Whether perfetto is compiled in. Without it, a session records nothing.
pub fn compiled_in() -> bool {
    ffi::compiled_in()
}

/// Commits the calling thread's buffered events to the running session. [`Session::stop`] does
/// it for the thread that calls it.
pub fn flush_thread() {
    ffi::flush_thread();
}

/// Serializes the tests that depend on whether a session is running.
static SESSION_LOCK: Mutex<()> = Mutex::new(());

/// Holds off sessions, for a test that needs none to be running.
pub fn no_session() -> MutexGuard<'static, ()> {
    SESSION_LOCK.lock().unwrap_or_else(PoisonError::into_inner)
}

/// A tracing session writing to a file in the test's temporary directory. Sessions run one at a
/// time.
pub struct Session {
    path: String,
    _lock: MutexGuard<'static, ()>,
}

impl Session {
    pub fn start() -> Self {
        let lock = no_session();
        let dir = std::env::var("TEST_TMPDIR")
            .unwrap_or_else(|_| std::env::temp_dir().display().to_string());
        let path = format!("{dir}/perfetto-test-{}.pftrace", std::process::id());
        ffi::start_session(&path).unwrap();
        Self { path, _lock: lock }
    }

    /// Stops the session, and returns the slice events it recorded, `"B <track> <name>"` and
    /// `"E <track>"`, in the order of each thread's sequence, the sequences in the order they were
    /// committed. `<track>` is `thread` for the thread's track, and
    /// otherwise `t1`, `t2`, ... in order of first appearance.
    pub fn stop(self) -> Vec<String> {
        ffi::stop_session(&self.path).unwrap()
    }
}
