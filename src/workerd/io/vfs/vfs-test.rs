// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::RefCell;
use std::rc::Rc;

use super::*;
use crate::NoopFdObserver;
use crate::tests::TestHost;
use crate::tests::clear;
use crate::tests::names;
use crate::tests::p;

#[derive(Clone, Default)]
struct Events(Rc<RefCell<Vec<String>>>);

impl FdObserver for Events {
    fn on_open(&self, open_fds: usize, total_fds: usize) {
        self.0
            .borrow_mut()
            .push(format!("open {open_fds} {total_fds}"));
    }

    fn on_close(&self, open_fds: usize, total_fds: usize) {
        self.0
            .borrow_mut()
            .push(format!("close {open_fds} {total_fds}"));
    }

    fn on_max_fds(&self, open_fds: usize) {
        self.0.borrow_mut().push(format!("max {open_fds}"));
    }
}

impl Events {
    fn take(&self) -> Vec<String> {
        std::mem::take(&mut *self.0.borrow_mut())
    }
}

fn bundle() -> Directory {
    let mut builder = Builder::default();
    builder
        .add(
            b"worker.js",
            File::new_readable(Box::new(&b"export {}"[..])),
        )
        .unwrap();
    builder.finish()
}

fn worker_fs(events: &Events) -> VirtualFileSystem {
    new_worker_file_system(
        &p("bundle"),
        &p("tmp"),
        &p("dev"),
        bundle(),
        Box::new(events.clone()),
    )
    .unwrap()
}

const WRITE: FdOpenOptions = FdOpenOptions {
    read: true,
    write: true,
    append: false,
    create: false,
    exclusive: false,
    truncate: false,
    follow_links: true,
};

const CREATE: FdOpenOptions = FdOpenOptions {
    create: true,
    ..WRITE
};

#[test]
fn worker_layout() {
    let host = TestHost::new();
    let vfs = worker_fs(&Events::default());
    let root = vfs.root();
    assert_eq!(names(&host, &root), ["bundle", "tmp", "dev"]);
    let Some(Ok(Node::Directory(dev))) = vfs.resolve(&host, &p("dev"), true) else {
        panic!("no /dev");
    };
    assert_eq!(names(&host, &dev), ["null", "zero", "full", "random"]);
    assert!(!root.stat(&host, &p("")).unwrap().unwrap().writable);
    assert!(
        vfs.resolve_stat(&host, &p("tmp"))
            .unwrap()
            .unwrap()
            .writable
    );
    assert_eq!(
        vfs.resolve_stat(&host, &p("bundle/worker.js"))
            .unwrap()
            .unwrap()
            .size,
        9
    );
}

#[test]
fn custom_roots_nest_and_must_not_collide() {
    let host = TestHost::new();
    let vfs = new_worker_file_system(
        &p("mything/bundle"),
        &p("mything/temp"),
        &p("dev"),
        bundle(),
        Box::new(NoopFdObserver),
    )
    .unwrap();
    assert_eq!(names(&host, &vfs.root()), ["mything", "dev"]);
    assert!(vfs.resolve(&host, &p("mything/temp"), true).is_some());

    let collision = new_worker_file_system(
        &p("same"),
        &p("same"),
        &p("dev"),
        bundle(),
        Box::new(NoopFdObserver),
    );
    assert!(matches!(collision, Err(BuilderError::AlreadyExists(_))));
    let empty = new_worker_file_system(
        &p(""),
        &p("tmp"),
        &p("dev"),
        bundle(),
        Box::new(NoopFdObserver),
    );
    assert!(matches!(empty, Err(BuilderError::EmptyPath)));
}

#[test]
fn open_fd_errors() {
    let host = TestHost::new();
    let vfs = worker_fs(&Events::default());
    let open = |path: &str, options| vfs.open_fd(&host, &p(path), options).err();
    assert_eq!(open("tmp/missing/f", CREATE), Some(FsError::NotFound));
    assert_eq!(open("tmp/f", WRITE), Some(FsError::NotFound));
    assert_eq!(
        open("bundle/worker.js/f", CREATE),
        Some(FsError::NotDirectory)
    );
    assert_eq!(open("bundle/worker.js", WRITE), Some(FsError::NotPermitted));
    assert_eq!(open("tmp", WRITE), Some(FsError::NotPermittedOnDirectory));
    assert_eq!(open("bundle/new", CREATE), Some(FsError::ReadOnly));
    assert_eq!(open("new", CREATE), Some(FsError::ReadOnly));
    vfs.open_fd(&host, &p("tmp/f"), CREATE).unwrap();
    let exclusive = FdOpenOptions {
        exclusive: true,
        ..CREATE
    };
    assert_eq!(open("tmp/f", exclusive), Some(FsError::AlreadyExists));
    // Exclusive matters only with create.
    assert!(
        vfs.open_fd(
            &host,
            &p("tmp/f"),
            FdOpenOptions {
                exclusive: true,
                ..WRITE
            }
        )
        .is_ok()
    );
}

#[test]
fn open_fd_creates_and_truncates() {
    let host = TestHost::new();
    let vfs = worker_fs(&Events::default());
    let opened = vfs.open_fd(&host, &p("tmp/f"), CREATE).unwrap();
    assert!(opened.read() && opened.write() && !opened.append());
    let Node::File(file) = opened.node() else {
        panic!("not a file");
    };
    file.write_all(&host, b"abc").unwrap();
    opened.set_position(3);
    assert_eq!(opened.position(), 3);

    vfs.open_fd(&host, &p("tmp/f"), WRITE).unwrap();
    assert_eq!(file.stat().size, 3);
    let truncate = FdOpenOptions {
        truncate: true,
        ..WRITE
    };
    vfs.open_fd(&host, &p("tmp/f"), truncate).unwrap();
    assert_eq!(file.stat().size, 0);
}

#[test]
fn open_fd_on_links() {
    let host = TestHost::new();
    let vfs = worker_fs(&Events::default());
    vfs.open_fd(&host, &p("tmp/f"), CREATE).unwrap();
    let link = vfs.new_symbolic_link(p("tmp/f"));
    let Some(Ok(Node::Directory(tmp))) = vfs.resolve(&host, &p("tmp"), true) else {
        panic!("no /tmp");
    };
    tmp.add(&host, b"l", Node::SymbolicLink(link)).unwrap();
    let no_follow = FdOpenOptions {
        follow_links: false,
        ..FdOpenOptions::default()
    };
    let opened = vfs.open_fd(&host, &p("tmp/l"), no_follow).unwrap();
    assert_eq!(opened.node().fs_type(), FsType::Symlink);
    assert_eq!(
        vfs.open_fd(
            &host,
            &p("tmp/l"),
            FdOpenOptions {
                follow_links: false,
                ..WRITE
            }
        )
        .err(),
        Some(FsError::NotPermitted)
    );
    let opened = vfs.open_fd(&host, &p("tmp/l"), WRITE).unwrap();
    assert_eq!(opened.node().fs_type(), FsType::File);
    clear(&host, &tmp);
}

#[test]
fn descriptors_are_counted_and_never_reused() {
    let host = TestHost::new();
    let events = Events::default();
    let vfs = worker_fs(&events);
    let a = vfs.open_fd(&host, &p("tmp/a"), CREATE).unwrap();
    let b = vfs.open_fd(&host, &p("tmp/a"), WRITE).unwrap();
    assert_eq!((a.fd(), b.fd()), (3, 4));
    assert!(vfs.try_get_fd(3).unwrap().node().is_same_node(a.node()));
    vfs.close_fd(3);
    assert!(vfs.try_get_fd(3).is_none());
    // Closing a descriptor that is not open still reports.
    vfs.close_fd(3);
    let c = vfs.open_fd(&host, &p("tmp/a"), WRITE).unwrap();
    assert_eq!(c.fd(), 5);
    assert_eq!(
        events.take(),
        ["open 1 4", "open 2 5", "close 1 5", "close 1 5", "open 2 6"]
    );
}

#[test]
fn running_out_of_descriptors() {
    let host = TestHost::new();
    let events = Events::default();
    let vfs = worker_fs(&events);
    vfs.inner.fds.borrow_mut().next_fd = i32::MAX - 1;
    let last = vfs.open_fd(&host, &p("tmp/a"), CREATE).unwrap();
    assert_eq!(last.fd(), i32::MAX - 1);
    assert_eq!(
        vfs.open_fd(&host, &p("tmp/a"), WRITE).err(),
        Some(FsError::TooManyOpenFiles)
    );
    assert_eq!(events.take().last().unwrap(), "max 1");
}

#[test]
fn stdio_descriptors() {
    let host = TestHost::new();
    let vfs = worker_fs(&Events::default());
    let out = vfs.try_get_fd(1).unwrap();
    assert!(out.read() && out.write() && out.append());
    assert!(vfs.get_stdio(Stdio::Out).node().is_same_node(out.node()));
    assert!(!vfs.get_stdio(Stdio::Err).node().is_same_node(out.node()));
    // Stdio descriptors can't be closed, and negative ones are never open.
    vfs.close_fd(1);
    assert!(vfs.try_get_fd(1).unwrap().node().is_same_node(out.node()));
    assert!(vfs.try_get_fd(-1).is_none());

    let Node::File(file) = out.node() else {
        panic!("not a file");
    };
    file.write(&host, 0, b"hello\n").unwrap();
    assert_eq!(host.logged_lines(), [(Stdio::Out, "hello".to_owned())]);
}

#[test]
fn wrapped_descriptors_close_when_dropped() {
    let host = TestHost::new();
    let events = Events::default();
    let vfs = worker_fs(&events);
    let opened = vfs.open_fd(&host, &p("tmp/a"), CREATE).unwrap();
    let handle = vfs.wrap_fd(opened.fd());
    drop(handle);
    assert!(vfs.try_get_fd(opened.fd()).is_none());
    // A wrapped stdio descriptor is closed too, and opened again on next use.
    let out = vfs.get_stdio(Stdio::Out);
    drop(vfs.wrap_fd(1));
    assert!(!vfs.get_stdio(Stdio::Out).node().is_same_node(out.node()));
    // A handle that outlives the file system does nothing.
    let handle = vfs.wrap_fd(3);
    drop(vfs);
    drop(handle);
}

#[test]
fn locks_cover_parents_and_count_holders() {
    let vfs = worker_fs(&Events::default());
    let locators = |names: &[&str]| names.iter().map(|&n| n.to_owned()).collect();
    let first = vfs.lock(locators(&[
        "file:///tmp/a/b",
        "file:///tmp/a",
        "file:///tmp",
    ]));
    let second = vfs.lock(locators(&["file:///tmp/a", "file:///tmp"]));
    assert!(vfs.is_locked("file:///tmp/a/b"));
    assert!(vfs.is_locked("file:///tmp"));
    assert!(!vfs.is_locked("file:///tmp/c"));
    drop(first);
    assert!(!vfs.is_locked("file:///tmp/a/b"));
    assert!(vfs.is_locked("file:///tmp/a"));
    drop(second);
    assert!(!vfs.is_locked("file:///tmp"));
    assert!(vfs.inner.locks.borrow().is_empty());
    // A lock that outlives the file system does nothing when dropped.
    let lock = vfs.lock(locators(&["file:///tmp"]));
    drop(vfs);
    drop(lock);
}
