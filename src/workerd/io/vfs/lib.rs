// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The in-memory virtual file system of a Worker.
//!
//! This is the Rust implementation of the API declared in `workerd/io/worker-fs.h`. Every Worker
//! has its own file system, rooted at a read-only directory that holds:
//!
//! - the bundle root (`/bundle` by default): the Worker's modules, read-only;
//! - the dev root (`/dev`): the device files `null`, `zero`, `full` and `random`;
//! - the temp root (`/tmp`): a writable directory whose contents belong to the current
//!   [`TmpDirStore`], so each request sees, and discards, its own temporary files.
//!
//! # Threading and the isolate lock
//!
//! Nodes are reference counted with [`std::rc::Rc`], so no type here is `Send` or `Sync`. In
//! workerd every operation runs under the isolate lock, and the C++ facade must also drop every
//! handle under that lock, as it does for the C++ implementation's `kj::Refcounted` nodes.
//!
//! # The host
//!
//! Everything the file system needs from the runtime comes through [`VfsHost`], which each
//! operation takes as a parameter: the blob size limit, memory accounting, randomness, logging
//! for stdio, microtasks, and the request's temporary directory. Holding a `&dyn VfsHost` stands
//! for holding the isolate lock.
//!
//! # Names and paths
//!
//! Names are byte strings, as `kj::String` is: a name may be any bytes except NUL and `/`, and
//! need not be UTF-8. A path is a slice of names that has already been evaluated (see [`path`]),
//! so it holds no empty, `.` or `..` components.

mod dir;
mod entries;
mod error;
mod file;
mod host;
mod link;
pub mod path;
mod tmp;
mod vfs;

pub use dir::Builder;
pub use dir::Directory;
pub use dir::ListEntry;
pub use dir::OpenOptions;
pub use dir::RemoveOptions;
pub use error::BuilderError;
pub use error::FsError;
pub use error::FsType;
pub use error::Stat;
pub use file::ExternalBytes;
pub use file::File;
pub use host::FdObserver;
pub use host::MemoryCharge;
pub use host::Microtask;
pub use host::NoopFdObserver;
pub use host::Stdio;
pub use host::VfsHost;
pub use link::LinkGuard;
pub use link::SymbolicLink;
pub use tmp::TmpDirStore;
pub use tmp::TmpDirStoreScope;
pub use tmp::current_tmp_store;
pub use vfs::FdHandle;
pub use vfs::FdOpenOptions;
pub use vfs::LockHandle;
pub use vfs::OpenedFile;
pub use vfs::VirtualFileSystem;
pub use vfs::new_dev_directory;
pub use vfs::new_worker_file_system;

/// A node of the file system: a file, a directory or a symbolic link.
///
/// Cloning a node adds a reference to the same node; it does not copy it.
#[derive(Clone)]
pub enum Node {
    File(File),
    Directory(Directory),
    SymbolicLink(SymbolicLink),
}

impl Node {
    /// The kind of this node.
    pub const fn fs_type(&self) -> FsType {
        match self {
            Self::File(_) => FsType::File,
            Self::Directory(_) => FsType::Directory,
            Self::SymbolicLink(_) => FsType::Symlink,
        }
    }

    /// Returns true if this and `other` are the same node.
    pub fn is_same_node(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::File(a), Self::File(b)) => a.is_same_node(b),
            (Self::Directory(a), Self::Directory(b)) => a.is_same_node(b),
            (Self::SymbolicLink(a), Self::SymbolicLink(b)) => a.is_same_node(b),
            _ => false,
        }
    }
}

impl From<File> for Node {
    fn from(file: File) -> Self {
        Self::File(file)
    }
}

impl From<Directory> for Node {
    fn from(dir: Directory) -> Self {
        Self::Directory(dir)
    }
}

impl From<SymbolicLink> for Node {
    fn from(link: SymbolicLink) -> Self {
        Self::SymbolicLink(link)
    }
}

/// A file or a directory: what a symbolic link resolves to, and what [`Builder`] holds.
#[derive(Clone)]
pub enum FileOrDirectory {
    File(File),
    Directory(Directory),
}

impl From<FileOrDirectory> for Node {
    fn from(node: FileOrDirectory) -> Self {
        match node {
            FileOrDirectory::File(file) => Self::File(file),
            FileOrDirectory::Directory(dir) => Self::Directory(dir),
        }
    }
}

impl From<File> for FileOrDirectory {
    fn from(file: File) -> Self {
        Self::File(file)
    }
}

impl From<Directory> for FileOrDirectory {
    fn from(dir: Directory) -> Self {
        Self::Directory(dir)
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
