// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::TmpDirStore;

/// One of the three standard streams. The values are also their file descriptors.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Stdio {
    In = 0,
    Out = 1,
    Err = 2,
}

impl Stdio {
    /// The stream whose file descriptor is `fd`, if `fd` is 0, 1 or 2.
    pub const fn from_fd(fd: i32) -> Option<Self> {
        match fd {
            0 => Some(Self::In),
            1 => Some(Self::Out),
            2 => Some(Self::Err),
            _ => None,
        }
    }
}

/// An amount of memory charged to the isolate's limit, released when dropped.
///
/// In workerd this is `jsg::ExternalMemoryAdjustment`.
pub trait MemoryCharge {
    /// Changes the amount charged to `bytes`.
    fn set(&mut self, bytes: usize);
}

/// A task to run as a microtask. It receives the host that is current when it runs.
pub type Microtask = Box<dyn FnOnce(&dyn VfsHost)>;

/// What the file system needs from the runtime.
///
/// Every operation takes a host, and a host exists only while the isolate lock is held. A host
/// may run JavaScript in [`log_stdio`](Self::log_stdio), and that JavaScript may call back into
/// the file system, so the file system holds no borrow of its own state across that call.
pub trait VfsHost {
    /// The largest size, in bytes, a file may have.
    fn blob_size_limit(&self) -> usize;

    /// Charges `bytes` to the isolate's memory limit until the returned charge is dropped.
    fn charge_memory(&self, bytes: usize) -> Box<dyn MemoryCharge>;

    /// Fills `buffer` with random bytes and returns true, or returns false if randomness is
    /// unavailable, as it is outside a request.
    fn fill_random(&self, buffer: &mut [u8]) -> bool;

    /// A new random UUID, or `None` outside a request, where there is no source of randomness.
    fn random_uuid(&self) -> Option<String>;

    /// Whether a request is current.
    fn has_io_context(&self) -> bool;

    /// Logs one line written to stdout or stderr, without its trailing newline. In workerd this
    /// calls `console.log` with the isolate's prefix for the stream, so it can run JavaScript.
    fn log_stdio(&self, stdio: Stdio, line: &[u8]);

    /// Enqueues `task` on the current context's microtask queue.
    fn enqueue_microtask(&self, task: Microtask);

    /// The current request's temporary directory store, if a request is current.
    fn io_context_tmp_store(&self) -> Option<TmpDirStore>;
}

/// Observes file descriptors being opened and closed, so the runtime can watch how many a
/// Worker holds. Every method does nothing by default.
pub trait FdObserver {
    /// A file descriptor was opened. `open_fds` counts the open file descriptors including the
    /// new one; `total_fds` counts every file descriptor ever opened.
    fn on_open(&self, open_fds: usize, total_fds: usize) {
        let _ = (open_fds, total_fds);
    }

    /// A file descriptor was closed. `open_fds` counts the open file descriptors excluding the
    /// closed one; `total_fds` counts every file descriptor ever opened.
    fn on_close(&self, open_fds: usize, total_fds: usize) {
        let _ = (open_fds, total_fds);
    }

    /// The Worker has opened as many file descriptors as it ever may, and no more will open.
    fn on_max_fds(&self, open_fds: usize) {
        let _ = open_fds;
    }
}

/// An [`FdObserver`] that observes nothing.
pub struct NoopFdObserver;

impl FdObserver for NoopFdObserver {}
