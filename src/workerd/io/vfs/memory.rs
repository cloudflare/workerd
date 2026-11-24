// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Heap snapshots: what the file system reports to the runtime's memory tracker.

use std::ffi::CStr;

use crate::FileOrDirectory;

/// A heap snapshot in progress, as the file system reports to it (C++ `jsg::MemoryTracker`).
///
/// Every file and directory is a node of its own, identified by the address of the state its
/// handles share, so a node that several directories or handles reach is reported, and counted,
/// once, and a directory that contains itself ends the walk. Names are literals because the
/// snapshot reads them after reporting ends.
///
/// Reporting reads the tree but changes nothing in it, and runs under the isolate lock. A
/// directory reports each file and directory in it, as a node of its own, but not its symbolic
/// links or the contents of `/tmp`, which belong to a request; a lazy directory reports what its
/// contents hold once they are loaded. A file reports the contents of a writable file only; a
/// read-only file's contents belong to the runtime.
pub trait MemoryTracker {
    /// Adds an edge named `edge` from the current node to the node `id` identifies. If that node
    /// is new, adds it, named `name` and of `self_size` bytes, makes it the current node, and
    /// returns true: the caller reports what it retains and then calls [`Self::pop_node`].
    /// Otherwise returns false.
    fn push_node(
        &mut self,
        edge: &'static CStr,
        id: usize,
        name: &'static CStr,
        self_size: usize,
    ) -> bool;

    /// Ends the node of the last [`Self::push_node`] that returned true.
    fn pop_node(&mut self);

    /// Adds a node of `size` bytes named `name`, with an edge named `edge` from the current
    /// node, or nothing if `size` is 0.
    fn track_bytes(&mut self, edge: &'static CStr, name: &'static CStr, size: usize);
}

enum Step {
    Report(&'static CStr, FileOrDirectory),
    Pop,
}

/// Reports `node`, with an edge named `edge` from the current node, and everything in it. The walk
/// keeps its own stack, so a tree of any depth is reported in full, as C++ reports its own.
pub fn track(tracker: &mut dyn MemoryTracker, edge: &'static CStr, node: FileOrDirectory) {
    let mut steps = vec![Step::Report(edge, node)];
    while let Some(step) = steps.pop() {
        let (edge, node) = match step {
            Step::Report(edge, node) => (edge, node),
            Step::Pop => {
                tracker.pop_node();
                continue;
            }
        };
        match node {
            FileOrDirectory::File(file) => {
                if tracker.push_node(
                    edge,
                    file.memory_id(),
                    file.memory_name(),
                    file.memory_self_size(),
                ) {
                    // Named as C++ names the `kj::Array` that holds a writable file's contents.
                    tracker.track_bytes(c"owned", c"kj::Array<T>", file.memory_tracked_size());
                    tracker.pop_node();
                }
            }
            FileOrDirectory::Directory(dir) => {
                if tracker.push_node(
                    edge,
                    dir.memory_id(),
                    dir.memory_name(),
                    dir.memory_self_size(),
                ) {
                    steps.push(Step::Pop);
                    // Reversed, so that they are reported in order.
                    steps.extend(dir.memory_children().into_iter().rev().map(|child| {
                        let edge = match child {
                            FileOrDirectory::File(_) => c"file",
                            FileOrDirectory::Directory(_) => c"directory",
                        };
                        Step::Report(edge, child)
                    }));
                }
            }
        }
    }
}

#[cfg(test)]
#[path = "memory-test.rs"]
mod tests;
