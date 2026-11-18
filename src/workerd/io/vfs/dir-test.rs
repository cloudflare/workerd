// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::rc::Rc;

use super::*;
use crate::SymbolicLink;
use crate::tests::TestHost;
use crate::tests::names;
use crate::tests::p;

const CREATE_FILE: OpenOptions = OpenOptions {
    create_as: Some(FsType::File),
    follow_links: true,
};

const NO_FOLLOW: OpenOptions = OpenOptions {
    create_as: None,
    follow_links: false,
};

fn open(host: &TestHost, dir: &Directory, path: &str) -> Option<Result<Node, FsError>> {
    dir.try_open(host, &p(path), OpenOptions::default())
}

fn open_file(host: &TestHost, dir: &Directory, path: &str) -> File {
    match open(host, dir, path) {
        Some(Ok(Node::File(file))) => file,
        _ => panic!("{path} is not a file"),
    }
}

fn open_dir(host: &TestHost, dir: &Directory, path: &str) -> Directory {
    match open(host, dir, path) {
        Some(Ok(Node::Directory(dir))) => dir,
        _ => panic!("{path} is not a directory"),
    }
}

fn readable(bytes: &'static [u8]) -> File {
    File::new_readable(Box::new(bytes))
}

#[test]
fn creating_makes_missing_parents() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let Some(Ok(Node::File(file))) = root.try_open(&host, &p("a/b/c.txt"), CREATE_FILE) else {
        panic!("not created");
    };
    assert!(open_file(&host, &root, "a/b/c.txt").is_same_node(&file));
    assert_eq!(file.stat().size, 0);
    let a = open_dir(&host, &root, "a");
    assert!(a.stat(&host, &p("")).unwrap().unwrap().writable);

    let options = OpenOptions {
        create_as: Some(FsType::Directory),
        follow_links: true,
    };
    assert!(matches!(
        root.try_open(&host, &p("a/d"), options),
        Some(Ok(Node::Directory(_)))
    ));
    let options = OpenOptions {
        create_as: Some(FsType::Symlink),
        follow_links: true,
    };
    assert!(matches!(
        root.try_open(&host, &p("a/e"), options),
        Some(Err(FsError::NotPermitted))
    ));
    // Creating does not pass through a file.
    assert!(
        root.try_open(&host, &p("a/b/c.txt/d"), CREATE_FILE)
            .is_none()
    );
    // The empty path is the directory itself.
    assert!(
        matches!(open(&host, &root, ""), Some(Ok(Node::Directory(d))) if d.is_same_node(&root))
    );
}

#[test]
fn created_nodes_are_charged() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    assert_eq!(host.charged.get(), 0);
    let _ = root.try_open(&host, &p("a/b"), CREATE_FILE);
    let file = open_file(&host, &root, "a/b");
    assert_eq!(
        host.charged.get(),
        root.memory_self_size() + file.memory_self_size()
    );
    drop(file);
    drop(root);
    assert_eq!(host.charged.get(), 0);
}

#[test]
fn stat_count_and_list() {
    let host = TestHost::new();
    let dir = Directory::new_writable();
    dir.add(&host, b"f", Node::File(readable(b"abc"))).unwrap();
    dir.add(&host, b"d", Node::Directory(Directory::new_writable()))
        .unwrap();
    dir.add(
        &host,
        b"l",
        Node::SymbolicLink(SymbolicLink::new(dir.clone(), p("f"))),
    )
    .unwrap();
    assert_eq!(names(&host, &dir), ["f", "d", "l"]);
    assert_eq!(dir.count(&host, None), 3);
    assert_eq!(dir.count(&host, Some(FsType::File)), 1);
    assert_eq!(dir.count(&host, Some(FsType::Symlink)), 1);

    assert_eq!(dir.stat(&host, &p("f")).unwrap().unwrap().size, 3);
    assert_eq!(
        dir.stat(&host, &p("d")).unwrap().unwrap().fs_type,
        FsType::Directory
    );
    // Stat follows links.
    assert_eq!(dir.stat(&host, &p("l")).unwrap().unwrap().size, 3);
    assert!(dir.stat(&host, &p("missing")).is_none());
    assert!(dir.stat(&host, &p("f/x")).is_none());
}

#[test]
fn add_checks_the_name() {
    let host = TestHost::new();
    let dir = Directory::new_writable();
    let file = || Node::File(File::new_dev_null());
    assert_eq!(dir.add(&host, b"a/b", file()), Err(FsError::InvalidPath));
    assert_eq!(dir.add(&host, b"..", file()), Err(FsError::InvalidPath));
    dir.add(&host, b"a", file()).unwrap();
    assert_eq!(dir.add(&host, b"a", file()), Err(FsError::AlreadyExists));
    // A name with a trailing separator is accepted and kept as given, as in C++.
    dir.add(&host, b"b/", file()).unwrap();
    assert_eq!(names(&host, &dir), ["a", "b/"]);
}

#[test]
fn remove() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let _ = root.try_open(&host, &p("a/b"), CREATE_FILE);
    let _ = root.try_open(&host, &p("c"), CREATE_FILE);
    assert_eq!(
        root.remove(&host, &p("a"), RemoveOptions::default()),
        Err(FsError::NotEmpty)
    );
    assert_eq!(
        root.remove(&host, &p("c/x"), RemoveOptions::default()),
        Err(FsError::NotDirectory)
    );
    assert_eq!(
        root.remove(&host, &p("x"), RemoveOptions::default()),
        Ok(false)
    );
    assert_eq!(
        root.remove(&host, &p(""), RemoveOptions::default()),
        Ok(false)
    );
    assert_eq!(
        root.remove(&host, &p("a/b"), RemoveOptions::default()),
        Ok(true)
    );
    assert_eq!(
        root.remove(&host, &p("a"), RemoveOptions::default()),
        Ok(true)
    );
    let _ = root.try_open(&host, &p("a/b"), CREATE_FILE);
    assert_eq!(
        root.remove(&host, &p("a"), RemoveOptions { recursive: true }),
        Ok(true)
    );
    assert_eq!(names(&host, &root), ["c"]);
}

#[test]
fn removing_a_link_keeps_its_target() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let _ = root.try_open(&host, &p("dir/file"), CREATE_FILE);
    let link = SymbolicLink::new(root.clone(), p("dir"));
    root.add(&host, b"link", Node::SymbolicLink(link)).unwrap();
    assert_eq!(
        root.remove(&host, &p("link/file"), RemoveOptions::default()),
        Err(FsError::NotDirectory)
    );
    assert_eq!(
        root.remove(&host, &p("link"), RemoveOptions::default()),
        Ok(true)
    );
    open_file(&host, &root, "dir/file");
}

#[test]
fn read_only_directories() {
    let host = TestHost::new();
    let mut builder = Builder::default();
    builder.add(b"f", readable(b"x")).unwrap();
    let dir = builder.finish();
    assert!(!dir.stat(&host, &p("")).unwrap().unwrap().writable);
    assert!(matches!(
        dir.try_open(&host, &p("new"), CREATE_FILE),
        Some(Err(FsError::ReadOnly))
    ));
    // Opening an existing path with create_as is fine.
    open_file(&host, &dir, "f");
    assert!(matches!(
        dir.try_open(&host, &p("f"), CREATE_FILE),
        Some(Ok(Node::File(_)))
    ));
    assert_eq!(
        dir.add(&host, b"g", Node::File(File::new_dev_null())),
        Err(FsError::NotPermitted)
    );
    assert_eq!(
        dir.remove(&host, &p("f"), RemoveOptions::default()),
        Err(FsError::ReadOnly)
    );
    assert_eq!(
        dir.remove(&host, &p(""), RemoveOptions::default()),
        Err(FsError::ReadOnly)
    );
    dir.count_towards_isolate_limit(&host);
    assert_eq!(host.charged.get(), 0);
}

#[test]
fn builder() {
    let host = TestHost::new();
    let mut builder = Builder::default();
    builder.add_path(&p("a/b/one"), readable(b"1")).unwrap();
    builder.add_path(&p("a/two"), readable(b"2")).unwrap();
    builder.add(b"c", Directory::new_writable()).unwrap();
    let mut nested = Builder::default();
    nested.add(b"three", readable(b"3")).unwrap();
    builder.add_builder(b"n", nested).unwrap();

    assert_eq!(
        builder.add(b"c", readable(b"")),
        Err(BuilderError::AlreadyExists(b"c".to_vec()))
    );
    assert_eq!(
        builder.add(b"x/y", readable(b"")),
        Err(BuilderError::InvalidName(b"x/y".to_vec()))
    );
    assert_eq!(
        builder.add_path(&p("c/d"), readable(b"")),
        Err(BuilderError::PathIsDirectory(b"c".to_vec()))
    );
    assert_eq!(
        builder.add_path(&p("a/two/x"), readable(b"")),
        Err(BuilderError::PathIsFile(b"two".to_vec()))
    );
    assert_eq!(
        builder.add_path::<Vec<u8>>(&[], readable(b"")),
        Err(BuilderError::EmptyPath)
    );

    let root = builder.finish();
    assert_eq!(names(&host, &root), ["a", "c", "n"]);
    assert_eq!(names(&host, &open_dir(&host, &root, "a")), ["b", "two"]);
    open_file(&host, &root, "a/b/one");
    open_file(&host, &root, "n/three");
    // Built directories are read-only; added ones keep their own writability.
    assert!(
        !open_dir(&host, &root, "a")
            .stat(&host, &p(""))
            .unwrap()
            .unwrap()
            .writable
    );
    assert!(
        open_dir(&host, &root, "c")
            .stat(&host, &p(""))
            .unwrap()
            .unwrap()
            .writable
    );
}

#[test]
fn a_failed_lazy_load_fails_the_operation_and_is_retried() {
    let host = TestHost::new();
    let loads = Rc::new(Cell::new(0));
    let counter = loads.clone();
    let lazy = Directory::new_lazy(Box::new(move || {
        counter.set(counter.get() + 1);
        // Fails until the ninth call.
        if counter.get() < 9 {
            return Err(HostError);
        }
        let mut builder = Builder::default();
        builder.add(b"f", readable(b"x")).unwrap();
        Ok(builder.finish())
    }));
    let file = || Node::File(File::new_dev_null());
    assert_eq!(lazy.stat(&host, &p("f")), Some(Err(FsError::Failed)));
    assert_eq!(lazy.count(&host, None), 0);
    assert!(lazy.list(&host).is_empty());
    assert!(matches!(
        open(&host, &lazy, "f"),
        Some(Err(FsError::Failed))
    ));
    assert_eq!(lazy.add(&host, b"g", file()), Err(FsError::Failed));
    assert_eq!(
        lazy.remove(&host, &p("f"), RemoveOptions::default()),
        Err(FsError::Failed)
    );
    assert_eq!(lazy.unique_id(&host), None);
    assert_eq!(loads.get(), 7);

    // So does a path through it, until it loads; then it stays loaded.
    let root = Directory::new_writable();
    root.add(&host, b"lazy", Node::Directory(lazy.clone()))
        .unwrap();
    assert_eq!(root.stat(&host, &p("lazy/f")), Some(Err(FsError::Failed)));
    assert_eq!(loads.get(), 8);
    open_file(&host, &root, "lazy/f");
    assert_eq!(names(&host, &lazy), ["f"]);
    assert_eq!(loads.get(), 9);
}

#[test]
fn a_lazy_directory_used_by_its_own_loader_fails_that_use() {
    let host = Rc::new(TestHost::new());
    let slot: Rc<RefCell<Option<Directory>>> = Rc::default();
    let inner = slot.clone();
    let loader_host = host.clone();
    let lazy = Directory::new_lazy(Box::new(move || {
        let this = inner.borrow().clone().unwrap();
        // The loader's own use of its directory fails rather than recursing.
        assert_eq!(this.count(&*loader_host, None), 0);
        assert_eq!(
            this.stat(&*loader_host, &p("f")),
            Some(Err(FsError::Failed))
        );
        Ok(Directory::new_writable())
    }));
    *slot.borrow_mut() = Some(lazy.clone());
    // The outer use loads the directory.
    assert_eq!(lazy.count(&*host, None), 0);
    assert!(
        lazy.add(&*host, b"f", Node::File(File::new_dev_null()))
            .is_ok()
    );
    assert_eq!(names(&*host, &lazy), ["f"]);
    // Break the cycle through the slot.
    slot.borrow_mut().take();
}

#[test]
fn lazy_directories_load_once_on_first_use() {
    let host = TestHost::new();
    let loads = Rc::new(Cell::new(0));
    let counter = loads.clone();
    let lazy = Directory::new_lazy(Box::new(move || {
        counter.set(counter.get() + 1);
        let mut builder = Builder::default();
        builder.add(b"f", readable(b"x")).unwrap();
        Ok(builder.finish())
    }));
    assert_eq!(lazy.memory_name(), c"LazyDirectory");
    assert_eq!(loads.get(), 0);
    assert_eq!(names(&host, &lazy), ["f"]);
    open_file(&host, &lazy, "f");
    assert_eq!(lazy.count(&host, None), 1);
    assert_eq!(loads.get(), 1);
    // The lazy directory has its contents' identifier, and the empty path opens the contents.
    let contents = open_dir(&host, &lazy, "");
    assert!(!contents.is_same_node(&lazy));
    assert_eq!(lazy.unique_id(&host), contents.unique_id(&host));
}

#[test]
fn links_are_followed_unless_asked_not_to() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let _ = root.try_open(&host, &p("dir/file"), CREATE_FILE);
    let to_dir = SymbolicLink::new(root.clone(), p("dir"));
    let to_file = SymbolicLink::new(root.clone(), p("dir/file"));
    let dangling = SymbolicLink::new(root.clone(), p("nowhere"));
    root.add(&host, b"to_dir", Node::SymbolicLink(to_dir.clone()))
        .unwrap();
    root.add(&host, b"to_file", Node::SymbolicLink(to_file))
        .unwrap();
    root.add(&host, b"dangling", Node::SymbolicLink(dangling))
        .unwrap();

    let file = open_file(&host, &root, "dir/file");
    assert!(open_file(&host, &root, "to_file").is_same_node(&file));
    assert!(open_file(&host, &root, "to_dir/file").is_same_node(&file));
    assert!(open(&host, &root, "to_file/x").is_none());
    assert!(open(&host, &root, "dangling").is_none());
    assert!(matches!(
        root.try_open(&host, &p("to_dir"), NO_FOLLOW),
        Some(Ok(Node::SymbolicLink(link))) if link.is_same_node(&to_dir)
    ));
    // Without following, a link cannot be passed through.
    assert!(root.try_open(&host, &p("to_dir/file"), NO_FOLLOW).is_none());
    // A link to a file stats as the file, whatever follows it in the path.
    assert_eq!(
        root.stat(&host, &p("to_file/x/y")).unwrap().unwrap().size,
        0
    );
    assert!(root.stat(&host, &p("dangling")).is_none());
    // A file is created through a link to a directory.
    assert!(matches!(
        root.try_open(&host, &p("to_dir/new"), CREATE_FILE),
        Some(Ok(Node::File(_)))
    ));
    open_file(&host, &root, "dir/new");
}

#[test]
fn link_cycles_are_detected() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    root.add(
        &host,
        b"a",
        Node::SymbolicLink(SymbolicLink::new(root.clone(), p("b"))),
    )
    .unwrap();
    root.add(
        &host,
        b"b",
        Node::SymbolicLink(SymbolicLink::new(root.clone(), p("a"))),
    )
    .unwrap();
    assert!(matches!(
        open(&host, &root, "a"),
        Some(Err(FsError::SymlinkDepthExceeded))
    ));
    assert!(matches!(
        root.stat(&host, &p("b/x")),
        Some(Err(FsError::SymlinkDepthExceeded))
    ));

    // Passing through the same link twice in one path counts as a cycle.
    let _ = root.try_open(&host, &p("d/f"), CREATE_FILE);
    root.add(
        &host,
        b"self",
        Node::SymbolicLink(SymbolicLink::new(root.clone(), p(""))),
    )
    .unwrap();
    open_file(&host, &root, "self/d/f");
    assert!(matches!(
        open(&host, &root, "self/self/d/f"),
        Some(Err(FsError::SymlinkDepthExceeded))
    ));
}

#[test]
fn a_chain_of_257_links_is_too_deep() {
    let host = TestHost::new();
    let root = Directory::new_writable();
    let _ = root.try_open(&host, &p("target"), CREATE_FILE);
    let mut previous = "target".to_owned();
    for i in 0..=256 {
        let name = format!("l{i}");
        let link = SymbolicLink::new(root.clone(), p(&previous));
        root.add(&host, name.as_bytes(), Node::SymbolicLink(link))
            .unwrap();
        previous = name;
    }
    open_file(&host, &root, "l255");
    assert!(matches!(
        open(&host, &root, "l256"),
        Some(Err(FsError::SymlinkDepthExceeded))
    ));
}

#[test]
fn a_link_back_to_an_ancestor_can_create() {
    // Creating through a link that leads back to a directory still being walked must not trip
    // over that directory's borrow.
    let host = TestHost::new();
    let root = Directory::new_writable();
    let _ = root.try_open(&host, &p("d/x"), CREATE_FILE);
    let d = open_dir(&host, &root, "d");
    d.add(
        &host,
        b"up",
        Node::SymbolicLink(SymbolicLink::new(root.clone(), p("d"))),
    )
    .unwrap();
    assert!(matches!(
        root.try_open(&host, &p("d/up/new"), CREATE_FILE),
        Some(Ok(Node::File(_)))
    ));
    assert_eq!(names(&host, &d), ["x", "up", "new"]);
    assert_eq!(
        root.remove(&host, &p("d"), RemoveOptions { recursive: true }),
        Ok(true)
    );
}

#[test]
fn tmp_delegates_to_the_current_store() {
    let host = TestHost::new();
    let tmp = Directory::new_tmp();
    let store = host.tmp_store.borrow().clone().unwrap();
    let _ = tmp.try_open(&host, &p("f"), CREATE_FILE);
    assert_eq!(names(&host, &store.directory()), ["f"]);
    assert_eq!(tmp.count(&host, None), 1);
    tmp.add(&host, b"g", Node::File(File::new_dev_null()))
        .unwrap();
    assert_eq!(names(&host, &tmp), ["f", "g"]);
    assert_eq!(
        tmp.remove(&host, &p("g"), RemoveOptions::default()),
        Ok(true)
    );

    // With no store current, /tmp is an empty writable directory that refuses entries.
    let outside = TestHost::outside_request();
    assert!(tmp.stat(&outside, &p("")).unwrap().unwrap().writable);
    assert!(tmp.stat(&outside, &p("f")).is_none());
    assert!(tmp.list(&outside).is_empty());
    assert_eq!(tmp.count(&outside, None), 0);
    assert!(tmp.try_open(&outside, &p("f"), CREATE_FILE).is_none());
    assert_eq!(
        tmp.add(&outside, b"h", Node::File(File::new_dev_null())),
        Err(FsError::NotPermitted)
    );
    assert_eq!(
        tmp.remove(&outside, &p("f"), RemoveOptions::default()),
        Ok(false)
    );
    // The directory's identifier is its own, not the store's.
    assert_ne!(tmp.unique_id(&host), store.directory().unique_id(&host));
}
