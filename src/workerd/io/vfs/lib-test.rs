// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A [`VfsHost`] for the unit tests, shared by every module's tests.

use std::cell::Cell;
use std::cell::RefCell;
use std::rc::Rc;

use super::*;

/// A host that records what the file system asks of it.
pub struct TestHost {
    pub blob_size_limit: Cell<usize>,
    /// The bytes currently charged to the isolate's memory limit.
    pub charged: Rc<Cell<usize>>,
    /// Whether a request is current: randomness and UUIDs are available only within one.
    pub in_request: Cell<bool>,
    uuids: Cell<u32>,
    pub logged: RefCell<Vec<(Stdio, Vec<u8>)>>,
    microtasks: RefCell<Vec<Microtask>>,
    pub tmp_store: RefCell<Option<TmpDirStore>>,
    /// Called after each line is logged, to run "JavaScript" from `console.log`.
    #[expect(clippy::type_complexity, reason = "a test hook")]
    pub on_log: RefCell<Option<Box<dyn Fn(&Self)>>>,
    /// The number of lines logged after which logging fails, as a throwing `console.log` does.
    pub fail_log_after: Cell<Option<usize>>,
}

impl TestHost {
    /// A host within a request, with its own temporary directory store and a 1 MiB blob limit.
    pub fn new() -> Self {
        Self {
            blob_size_limit: Cell::new(1 << 20),
            charged: Rc::new(Cell::new(0)),
            in_request: Cell::new(true),
            uuids: Cell::new(0),
            logged: RefCell::new(Vec::new()),
            microtasks: RefCell::new(Vec::new()),
            tmp_store: RefCell::new(Some(TmpDirStore::new())),
            on_log: RefCell::new(None),
            fail_log_after: Cell::new(None),
        }
    }

    /// A host outside any request: no randomness, no UUIDs and no request store.
    pub fn outside_request() -> Self {
        let host = Self::new();
        host.in_request.set(false);
        *host.tmp_store.borrow_mut() = None;
        host
    }

    /// Runs the queued microtasks, including any they queue.
    pub fn run_microtasks(&self) {
        loop {
            let tasks = std::mem::take(&mut *self.microtasks.borrow_mut());
            if tasks.is_empty() {
                return;
            }
            for task in tasks {
                task(self);
            }
        }
    }

    pub fn pending_microtasks(&self) -> usize {
        self.microtasks.borrow().len()
    }

    /// The lines logged so far, as text.
    pub fn logged_lines(&self) -> Vec<(Stdio, String)> {
        self.logged
            .borrow()
            .iter()
            .map(|(stdio, line)| (*stdio, String::from_utf8_lossy(line).into_owned()))
            .collect()
    }
}

struct TestCharge {
    amount: usize,
    total: Rc<Cell<usize>>,
}

impl MemoryCharge for TestCharge {
    fn set(&mut self, bytes: usize) {
        self.total.set(self.total.get() - self.amount + bytes);
        self.amount = bytes;
    }
}

impl Drop for TestCharge {
    fn drop(&mut self) {
        self.total.set(self.total.get() - self.amount);
    }
}

impl VfsHost for TestHost {
    fn blob_size_limit(&self) -> usize {
        self.blob_size_limit.get()
    }

    fn charge_memory(&self, bytes: usize) -> Box<dyn MemoryCharge> {
        self.charged.set(self.charged.get() + bytes);
        Box::new(TestCharge {
            amount: bytes,
            total: self.charged.clone(),
        })
    }

    fn fill_random(&self, buffer: &mut [u8]) -> bool {
        if !self.in_request.get() {
            return false;
        }
        buffer.fill(9);
        true
    }

    fn random_uuid(&self) -> Option<String> {
        if !self.in_request.get() {
            return None;
        }
        let n = self.uuids.get() + 1;
        self.uuids.set(n);
        Some(format!("uuid-{n}"))
    }

    fn has_io_context(&self) -> bool {
        self.in_request.get()
    }

    fn log_stdio(&self, stdio: Stdio, line: &[u8]) -> Result<(), HostError> {
        if self.fail_log_after.get() == Some(self.logged.borrow().len()) {
            return Err(HostError);
        }
        self.logged.borrow_mut().push((stdio, line.to_vec()));
        if let Some(hook) = &*self.on_log.borrow() {
            hook(self);
        }
        Ok(())
    }

    fn enqueue_microtask(&self, task: Microtask) {
        self.microtasks.borrow_mut().push(task);
    }

    fn io_context_tmp_store(&self) -> Option<TmpDirStore> {
        self.tmp_store.borrow().clone()
    }
}

/// The names of a directory's entries, as text, in listing order.
pub fn names(host: &dyn VfsHost, dir: &Directory) -> Vec<String> {
    dir.list(host)
        .into_iter()
        .map(|entry| String::from_utf8_lossy(&entry.name).into_owned())
        .collect()
}

/// Removes everything in `dir`. A test that adds to a directory a link that resolves from it calls
/// this at its end: the link holds the directory, so otherwise the two keep each other alive.
pub fn clear(host: &dyn VfsHost, dir: &Directory) {
    for entry in dir.list(host) {
        dir.remove(host, &[entry.name], RemoveOptions { recursive: true })
            .unwrap();
    }
}

/// `text` evaluated as a path from the root.
pub fn p(text: &str) -> Vec<Vec<u8>> {
    path::eval(text.as_bytes()).unwrap()
}

#[test]
fn node_identity() {
    let file = File::new_dev_null();
    let a = Node::File(file.clone());
    assert!(a.is_same_node(&Node::File(file)));
    assert!(!a.is_same_node(&Node::File(File::new_dev_null())));
    assert!(!a.is_same_node(&Node::Directory(Directory::new_writable())));
    assert_eq!(a.fs_type(), FsType::File);
}
