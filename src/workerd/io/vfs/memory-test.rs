// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashSet;
use std::ffi::CStr;

use super::*;
use crate::Builder;
use crate::Directory;
use crate::File;
use crate::Node;
use crate::SymbolicLink;
use crate::tests::TestHost;
use crate::tests::p;

/// Writes what is reported as an outline, one line per edge, indented by depth.
#[derive(Default)]
struct Recorder {
    seen: HashSet<usize>,
    depth: usize,
    lines: Vec<String>,
}

impl Recorder {
    fn line(&mut self, text: &str) {
        self.lines
            .push(format!("{}{text}", "  ".repeat(self.depth)));
    }
}

fn text(name: &CStr) -> &str {
    name.to_str().unwrap()
}

impl MemoryTracker for Recorder {
    fn push_node(
        &mut self,
        edge: &'static CStr,
        id: usize,
        name: &'static CStr,
        self_size: usize,
    ) -> bool {
        let new = self.seen.insert(id);
        let seen = if new { "" } else { " (seen)" };
        self.line(&format!("{}: {} {self_size}{seen}", text(edge), text(name)));
        if new {
            self.depth += 1;
        }
        new
    }

    fn pop_node(&mut self) {
        self.depth -= 1;
    }

    fn track_bytes(&mut self, edge: &'static CStr, name: &'static CStr, size: usize) {
        if size > 0 {
            self.line(&format!("{}: {} {size}", text(edge), text(name)));
        }
    }
}

fn report(dir: &Directory) -> Vec<String> {
    let mut recorder = Recorder::default();
    dir.track_memory(&mut recorder, c"root");
    assert_eq!(recorder.depth, 0);
    recorder.lines
}

fn readable(bytes: &'static [u8]) -> File {
    File::new_readable(Box::new(bytes.to_vec()))
}

#[test]
fn reports_each_node_with_what_it_retains() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let sub = Directory::new_writable();
    let written = File::new_writable(&host, None);
    written.write(&host, 0, b"abcd").unwrap();
    sub.add(&host, b"w", Node::File(written.clone())).unwrap();
    root.add(&host, b"r", Node::File(readable(b"xyz"))).unwrap();
    root.add(&host, b"sub", Node::Directory(sub)).unwrap();
    root.add(
        &host,
        b"link",
        Node::SymbolicLink(SymbolicLink::new(root.clone(), p("r"))),
    )
    .unwrap();

    let dir = root.memory_self_size();
    let file = written.memory_self_size();
    // A read-only file's contents and symbolic links are not counted.
    assert_eq!(
        report(&root),
        [
            format!("root: Directory {dir}"),
            format!("  file: File {file}"),
            format!("  directory: Directory {dir}"),
            format!("    file: File {file}"),
            "      owned: kj::Array<T> 4".to_owned(),
        ]
    );
}

#[test]
fn reports_a_node_reached_twice_once() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let shared = File::new_writable(&host, None);
    shared.write(&host, 0, b"ab").unwrap();
    root.add(&host, b"a", Node::File(shared.clone())).unwrap();
    root.add(&host, b"b", Node::File(shared.clone())).unwrap();
    // A directory that contains itself ends the walk.
    root.add(&host, b"self", Node::Directory(root.clone()))
        .unwrap();

    let dir = root.memory_self_size();
    let file = shared.memory_self_size();
    assert_eq!(
        report(&root),
        [
            format!("root: Directory {dir}"),
            format!("  file: File {file}"),
            "    owned: kj::Array<T> 2".to_owned(),
            format!("  file: File {file} (seen)"),
            format!("  directory: Directory {dir} (seen)"),
        ]
    );

    // Reports through different handles of one node are one node.
    let mut recorder = Recorder::default();
    shared.track_memory(&mut recorder, c"handle");
    shared.clone().track_memory(&mut recorder, c"handle");
    assert_eq!(
        recorder.lines,
        [
            format!("handle: File {file}"),
            "  owned: kj::Array<T> 2".to_owned(),
            format!("handle: File {file} (seen)"),
        ]
    );
}

#[test]
fn a_lazy_directory_reports_its_contents_once_loaded() {
    let host = TestHost::new();
    let lazy = Directory::new_lazy(Box::new(|| {
        let mut builder = Builder::default();
        builder.add(b"f", readable(b"x")).unwrap();
        builder.finish()
    }));
    let dir = lazy.memory_self_size();
    assert_eq!(report(&lazy), [format!("root: LazyDirectory {dir}")]);

    assert_eq!(lazy.count(&host, None), 1);
    let file = readable(b"").memory_self_size();
    // The contents' entries are the lazy directory's, as in C++.
    assert_eq!(
        report(&lazy),
        [
            format!("root: LazyDirectory {dir}"),
            format!("  file: File {file}"),
        ]
    );
}

#[test]
fn tmp_contents_are_not_reported() {
    let host = TestHost::new();
    let tmp = Directory::new_tmp();
    tmp.add(&host, b"f", Node::File(File::new_writable(&host, Some(3))))
        .unwrap();
    assert_eq!(tmp.count(&host, None), 1);
    assert_eq!(
        report(&tmp),
        [format!("root: TmpDirectory {}", tmp.memory_self_size())]
    );
}

/// Counts nodes and how deep they nest.
#[derive(Default)]
struct Counter {
    nodes: usize,
    depth: usize,
    max_depth: usize,
}

impl MemoryTracker for Counter {
    fn push_node(&mut self, _: &'static CStr, _: usize, _: &'static CStr, _: usize) -> bool {
        self.nodes += 1;
        self.depth += 1;
        self.max_depth = self.max_depth.max(self.depth);
        true
    }

    fn pop_node(&mut self) {
        self.depth -= 1;
    }

    fn track_bytes(&mut self, _: &'static CStr, _: &'static CStr, _: usize) {}
}

#[test]
fn reports_a_deep_tree_in_full() {
    const DEPTH: usize = 100_000;
    let host = TestHost::new();
    let mut dirs = vec![Directory::new_writable()];
    for _ in 0..DEPTH {
        let sub = Directory::new_writable();
        dirs.last()
            .unwrap()
            .add(&host, b"d", Node::Directory(sub.clone()))
            .unwrap();
        dirs.push(sub);
    }
    let mut counter = Counter::default();
    dirs[0].track_memory(&mut counter, c"root");
    assert_eq!(counter.nodes, DEPTH + 1);
    assert_eq!(counter.max_depth, DEPTH + 1);
    assert_eq!(counter.depth, 0);

    // Dropping the tree whole would recurse once per level.
    for dir in dirs.iter().rev() {
        let _ = dir.remove(&host, &p("d"), crate::RemoveOptions::default());
    }
}
