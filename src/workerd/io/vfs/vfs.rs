// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::rc::Weak;

use crate::Builder;
use crate::BuilderError;
use crate::Directory;
use crate::FdObserver;
use crate::File;
use crate::FsError;
use crate::FsType;
use crate::Node;
use crate::OpenOptions;
use crate::Stat;
use crate::Stdio;
use crate::SymbolicLink;
use crate::VfsHost;

/// Options for [`VirtualFileSystem::open_fd`].
#[derive(Clone, Copy, Debug)]
#[expect(clippy::struct_excessive_bools, reason = "these are the open(2) flags")]
pub struct FdOpenOptions {
    pub read: bool,
    pub write: bool,
    /// Write at the end of the file. Ignored without `write`.
    pub append: bool,
    /// Create a missing file. Without it, opening a missing path fails with
    /// [`FsError::NotFound`]. Missing parent directories are never created.
    pub create: bool,
    /// Fail if the path exists. Only meaningful with `create`.
    pub exclusive: bool,
    /// Truncate an existing file to zero length. Ignored without `write`.
    pub truncate: bool,
    /// If the path names a symbolic link, open its target rather than the link.
    pub follow_links: bool,
}

impl Default for FdOpenOptions {
    fn default() -> Self {
        Self {
            read: true,
            write: false,
            append: false,
            create: false,
            exclusive: false,
            truncate: false,
            follow_links: true,
        }
    }
}

/// An open file descriptor. Cloning one adds a reference to the same descriptor, which shares
/// its position.
#[derive(Clone)]
pub struct OpenedFile(Rc<OpenedInner>);

struct OpenedInner {
    fd: i32,
    read: bool,
    write: bool,
    append: bool,
    node: Node,
    position: Cell<u32>,
}

impl OpenedFile {
    fn new(fd: i32, options: FdOpenOptions, node: Node) -> Self {
        Self(Rc::new(OpenedInner {
            fd,
            read: options.read,
            write: options.write,
            append: options.append,
            node,
            position: Cell::new(0),
        }))
    }

    pub fn fd(&self) -> i32 {
        self.0.fd
    }

    pub fn read(&self) -> bool {
        self.0.read
    }

    pub fn write(&self) -> bool {
        self.0.write
    }

    pub fn append(&self) -> bool {
        self.0.append
    }

    /// The file, directory or symbolic link that was opened.
    pub fn node(&self) -> &Node {
        &self.0.node
    }

    /// Where a read or write without an explicit offset starts.
    pub fn position(&self) -> u32 {
        self.0.position.get()
    }

    pub fn set_position(&self, position: u32) {
        self.0.position.set(position);
    }
}

/// A Worker's file system: a root directory and the table of open file descriptors.
///
/// The descriptor table belongs to the whole Worker, not to a request: a descriptor stays open
/// until it is closed or the file system is dropped. Descriptors are never reused, and once
/// `i32::MAX - 1` have been opened, no more can be.
pub struct VirtualFileSystem {
    inner: Rc<VfsInner>,
}

struct VfsInner {
    root: Directory,
    observer: Box<dyn FdObserver>,
    fds: RefCell<FdTable>,
    // The number of holders of each locked locator.
    locks: RefCell<HashMap<String, usize>>,
}

struct FdTable {
    next_fd: i32,
    opened: HashMap<i32, OpenedFile>,
}

impl VirtualFileSystem {
    /// A file system rooted at `root`, reporting file descriptors to `observer`.
    pub fn new(root: Directory, observer: Box<dyn FdObserver>) -> Self {
        Self {
            inner: Rc::new(VfsInner {
                root,
                observer,
                fds: RefCell::new(FdTable {
                    next_fd: Stdio::Err as i32 + 1,
                    opened: HashMap::new(),
                }),
                locks: RefCell::new(HashMap::new()),
            }),
        }
    }

    /// The root directory.
    pub fn root(&self) -> Directory {
        self.inner.root.clone()
    }

    /// The node at `path`, an evaluated path from the root, or `None` if it does not exist.
    pub fn resolve<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        path: &[P],
        follow_links: bool,
    ) -> Option<Result<Node, FsError>> {
        let options = OpenOptions {
            create_as: None,
            follow_links,
        };
        self.inner.root.try_open(host, path, options)
    }

    /// The metadata of the node at `path`, following symbolic links, or `None` if it does not
    /// exist.
    pub fn resolve_stat<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        path: &[P],
    ) -> Option<Result<Stat, FsError>> {
        self.inner.root.stat(host, path)
    }

    /// A new symbolic link to `target`, an evaluated path from the root, which need not exist.
    pub fn new_symbolic_link(&self, target: Vec<Vec<u8>>) -> SymbolicLink {
        SymbolicLink::new(self.inner.root.clone(), target)
    }

    /// Opens a file descriptor for the node at `path`, an evaluated path from the root.
    ///
    /// # Errors
    ///
    /// - [`FsError::TooManyOpenFiles`] once the Worker has opened as many as it ever may.
    /// - [`FsError::NotFound`] if the parent directory, or without `create` the node, is missing.
    /// - [`FsError::NotDirectory`] if the parent is a file or a symbolic link.
    /// - [`FsError::AlreadyExists`] if `exclusive` and `create` are set and the path exists.
    /// - [`FsError::NotPermitted`] to write a read-only file or a link, and
    ///   [`FsError::NotPermittedOnDirectory`] to write a directory.
    /// - [`FsError::Failed`] if the file could not be created.
    /// - Any error resolving the path.
    pub fn open_fd<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        path: &[P],
        options: FdOpenOptions,
    ) -> Result<OpenedFile, FsError> {
        let next_fd = self.inner.fds.borrow().next_fd;
        if next_fd == i32::MAX {
            let open_fds = self.inner.fds.borrow().opened.len();
            self.inner.observer.on_max_fds(open_fds);
            return Err(FsError::TooManyOpenFiles);
        }

        let root = &self.inner.root;

        // open(2) never creates parent directories.
        if let Some((_, parent)) = path.split_last() {
            match root.try_open(host, parent, OpenOptions::default()) {
                Some(Ok(Node::Directory(_))) => {}
                Some(Ok(Node::File(_) | Node::SymbolicLink(_))) => {
                    return Err(FsError::NotDirectory);
                }
                Some(Err(err)) => return Err(err),
                None => return Err(FsError::NotFound),
            }
        }

        if options.exclusive && options.create {
            match root.stat(host, path) {
                Some(Ok(_)) => return Err(FsError::AlreadyExists),
                Some(Err(err)) => return Err(err),
                None => {}
            }
        }

        let open_options = OpenOptions {
            create_as: options.create.then_some(FsType::File),
            follow_links: options.follow_links,
        };
        let Some(node) = root.try_open(host, path, open_options) else {
            // A missing file that `create` did not create: its directory is likely read-only.
            return Err(if options.create {
                FsError::Failed
            } else {
                FsError::NotFound
            });
        };
        match node? {
            Node::File(file) => {
                if options.write {
                    let stat = file.stat();
                    if !stat.writable {
                        return Err(FsError::NotPermitted);
                    }
                    if options.truncate && stat.size > 0 {
                        file.resize(host, 0)?;
                    }
                }
                Ok(self.add_fd(options, Node::File(file)))
            }
            Node::Directory(dir) => {
                if options.write {
                    return Err(FsError::NotPermittedOnDirectory);
                }
                Ok(self.add_fd(options, Node::Directory(dir)))
            }
            Node::SymbolicLink(link) => {
                if options.write {
                    return Err(FsError::NotPermitted);
                }
                Ok(self.add_fd(options, Node::SymbolicLink(link)))
            }
        }
    }

    fn add_fd(&self, options: FdOpenOptions, node: Node) -> OpenedFile {
        let (opened, open_fds, total_fds) = {
            let mut fds = self.inner.fds.borrow_mut();
            let fd = fds.next_fd;
            fds.next_fd += 1;
            let opened = OpenedFile::new(fd, options, node);
            fds.opened.insert(fd, opened.clone());
            (opened, fds.opened.len(), fds.next_fd)
        };
        self.inner.observer.on_open(open_fds, fd_count(total_fds));
        opened
    }

    /// Closes `fd`. Closing a descriptor that is not open, or one of the stdio descriptors,
    /// does nothing.
    pub fn close_fd(&self, fd: i32) {
        if fd <= Stdio::Err as i32 {
            return;
        }
        self.inner.close_fd(fd);
    }

    /// A handle that closes `fd` when dropped, unless the file system has been dropped first.
    /// Unlike [`close_fd`](Self::close_fd), it also closes a stdio descriptor.
    pub fn wrap_fd(&self, fd: i32) -> FdHandle {
        FdHandle {
            vfs: Rc::downgrade(&self.inner),
            fd,
        }
    }

    /// The open descriptor `fd`, or `None` if it is not open. Descriptors 0, 1 and 2 are always
    /// open: they are the stdio files. Negative descriptors are never open.
    pub fn try_get_fd(&self, fd: i32) -> Option<OpenedFile> {
        if fd < 0 {
            return None;
        }
        if let Some(stdio) = Stdio::from_fd(fd) {
            return Some(self.get_stdio(stdio));
        }
        self.inner.fds.borrow().opened.get(&fd).cloned()
    }

    /// The descriptor of `stdio`, opened for reading, writing and appending on first use.
    pub fn get_stdio(&self, stdio: Stdio) -> OpenedFile {
        let fd = stdio as i32;
        let mut fds = self.inner.fds.borrow_mut();
        fds.opened
            .entry(fd)
            .or_insert_with(|| {
                let options = FdOpenOptions {
                    read: true,
                    write: true,
                    append: true,
                    ..FdOpenOptions::default()
                };
                OpenedFile::new(fd, options, Node::File(File::new_stdio(stdio)))
            })
            .clone()
    }

    /// Locks `locators` until the handle is dropped. While a locator is locked,
    /// [`is_locked`](Self::is_locked) reports it, and the web file system API refuses some
    /// changes to its node. The caller passes a node's locator together with those of all its
    /// parents, so that a directory is locked while any of its descendants is.
    pub fn lock(&self, locators: Vec<String>) -> LockHandle {
        {
            let mut locks = self.inner.locks.borrow_mut();
            for locator in &locators {
                *locks.entry(locator.clone()).or_insert(0) += 1;
            }
        }
        LockHandle {
            vfs: Rc::downgrade(&self.inner),
            locators,
        }
    }

    /// Whether `locator` is locked.
    pub fn is_locked(&self, locator: &str) -> bool {
        self.inner
            .locks
            .borrow()
            .get(locator)
            .is_some_and(|&count| count > 0)
    }
}

// The number of file descriptors ever opened, given the next descriptor, which is never negative.
fn fd_count(next_fd: i32) -> usize {
    usize::try_from(next_fd).unwrap_or(0)
}

impl VfsInner {
    fn close_fd(&self, fd: i32) {
        let (open_fds, total_fds) = {
            let mut fds = self.fds.borrow_mut();
            fds.opened.remove(&fd);
            (fds.opened.len(), fds.next_fd)
        };
        self.observer.on_close(open_fds, fd_count(total_fds));
    }
}

/// Closes a file descriptor when dropped. See [`VirtualFileSystem::wrap_fd`].
pub struct FdHandle {
    vfs: Weak<VfsInner>,
    fd: i32,
}

impl Drop for FdHandle {
    fn drop(&mut self) {
        if let Some(vfs) = self.vfs.upgrade() {
            vfs.close_fd(self.fd);
        }
    }
}

/// Holds locks on locators until dropped. See [`VirtualFileSystem::lock`].
pub struct LockHandle {
    vfs: Weak<VfsInner>,
    locators: Vec<String>,
}

impl Drop for LockHandle {
    fn drop(&mut self) {
        let Some(vfs) = self.vfs.upgrade() else {
            return;
        };
        let mut locks = vfs.locks.borrow_mut();
        for locator in &self.locators {
            if let Some(count) = locks.get_mut(locator) {
                *count -= 1;
                if *count == 0 {
                    locks.remove(locator);
                }
            }
        }
    }
}

/// A new `/dev` directory holding `null`, `zero`, `full` and `random`.
pub fn new_dev_directory() -> Directory {
    let mut builder = Builder::default();
    for (name, file) in [
        (&b"null"[..], File::new_dev_null()),
        (b"zero", File::new_dev_zero()),
        (b"full", File::new_dev_full()),
        (b"random", File::new_dev_random()),
    ] {
        if builder.add(name, file).is_err() {
            unreachable!("the device names are distinct single components");
        }
    }
    builder.finish()
}

/// A Worker's file system: a read-only root holding `bundle` at `bundle_path`, the `/tmp`
/// directory at `tmp_path` and the `/dev` directory at `dev_path`, each an evaluated path from
/// the root.
///
/// # Errors
///
/// A [`BuilderError`] if a path is empty or if two paths collide.
pub fn new_worker_file_system<P: AsRef<[u8]>>(
    bundle_path: &[P],
    tmp_path: &[P],
    dev_path: &[P],
    bundle: Directory,
    observer: Box<dyn FdObserver>,
) -> Result<VirtualFileSystem, BuilderError> {
    let mut builder = Builder::default();
    builder.add_path(bundle_path, bundle)?;
    builder.add_path(tmp_path, Directory::new_tmp())?;
    builder.add_path(dev_path, new_dev_directory())?;
    Ok(VirtualFileSystem::new(builder.finish(), observer))
}

#[cfg(test)]
#[path = "vfs-test.rs"]
mod tests;
